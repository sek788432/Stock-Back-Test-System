import argparse
import os
import re
import sys
import threading
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path
from typing import Dict, List, Optional, Tuple

import databento as db
import duckdb
import pandas as pd
from databento.common.error import BentoClientError

# ---------------------------------------------------------------------------
# Environment variables: names and defaults. CollectData.sh sources repo
# `.env` before fetch; export vars yourself if you run Python directly.
# DATABENTO_API_KEY is read again inside loadFrameFromInput() so `.env` edits
# picked up by the shell before a long run remain visible without reloading.
# ---------------------------------------------------------------------------
_ENV_STOCK_REPO_ROOT = "STOCK_REPO_ROOT"
_ENV_SCHEMA_NAME = "SCHEMA_NAME"
_ENV_START_TS = "START_TS"
_ENV_END_TS = "END_TS"
_ENV_SYMBOL_BATCH_SIZE = "SYMBOL_BATCH_SIZE"
_ENV_PARALLEL_BATCHES = "PARALLEL_BATCHES"
_ENV_DATABENTO_API_KEY = "DATABENTO_API_KEY"

_FALLBACK_SCHEMA_NAME = "ohlcv-1h"
_FALLBACK_START_TS = "2018-05-01T00:00:00Z"
_FALLBACK_END_TS = "2026-05-06T00:00:00Z"
_FALLBACK_SYMBOL_BATCH_SIZE = 50
_FALLBACK_PARALLEL_BATCHES = 4
_402_MIN_TIME_SLICE = pd.Timedelta(hours=1)
_SAFE_SYMBOL_PATTERN = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_.-]*$")

# Worker threads' notes/warnings can interleave; serialize their stderr/stdout writes.
_PRINT_LOCK = threading.Lock()


def _logLine(message: str, *, file=sys.stdout) -> None:
    """Thread-safe single-line log helper for code reachable from worker threads."""
    with _PRINT_LOCK:
        print(message, file=file, flush=True)


def resolveStockRepoRoot(scriptDir: Path) -> Path:
    envRoot = os.environ.get(_ENV_STOCK_REPO_ROOT, "").strip()
    if envRoot:
        root = Path(envRoot).expanduser().resolve()
        if not root.is_dir():
            raise FileNotFoundError(f"{_ENV_STOCK_REPO_ROOT} is not a directory: {root}")
        if not (root / "StockData").is_dir():
            raise FileNotFoundError(f"{_ENV_STOCK_REPO_ROOT} points to root with no StockData/ folder: {root}")
        return root
    for candidate in [scriptDir, *scriptDir.parents]:
        marker = candidate / "StockData" / "Symbols.txt"
        if marker.is_file():
            return candidate
    raise FileNotFoundError(
        "Cannot find project root (look for StockData/Symbols.txt when walking upward from "
        f"{scriptDir}). Move that file back under StockData/, or export {_ENV_STOCK_REPO_ROOT}=/abs/path/repo."
    )


_SCRIPT_DIR = Path(__file__).resolve().parent
_REPO_ROOT = resolveStockRepoRoot(_SCRIPT_DIR)

STOCK_DATA_DIR = "StockData"
DEFAULT_DB_PATH = str(_REPO_ROOT / STOCK_DATA_DIR / "MarketData.duckdb")
DEFAULT_SYMBOL_LIST = str(_REPO_ROOT / STOCK_DATA_DIR / "Symbols.txt")

DATASET = "XNAS.ITCH"
# Native Databento OHLCV: ohlcv-1s, ohlcv-1m, ohlcv-1h, ohlcv-1d, ohlcv-eod (no ohlcv-30min).
DEFAULT_SCHEMA_NAME = os.environ.get(_ENV_SCHEMA_NAME, _FALLBACK_SCHEMA_NAME)
DEFAULT_START_TS = os.environ.get(_ENV_START_TS, _FALLBACK_START_TS)
# Bump _FALLBACK_END_TS as needed (or pass --end-ts); keep at or before XNAS available_end for 422 avoidance.
DEFAULT_END_TS = os.environ.get(_ENV_END_TS, _FALLBACK_END_TS)


def _envInt(name: str, default: int) -> int:
    raw = os.environ.get(name, "").strip()
    if not raw:
        return default
    return int(raw)


def _chunkSymbols(symbols: list[str], batchSize: int) -> list[list[str]]:
    if batchSize <= 0:
        return [symbols]
    return [symbols[i : i + batchSize] for i in range(0, len(symbols), batchSize)]


def _schemaBarTimedelta(schemaName: str) -> pd.Timedelta:
    preset = {
        "ohlcv-1s": pd.Timedelta(seconds=1),
        "ohlcv-1m": pd.Timedelta(minutes=1),
        "ohlcv-1h": pd.Timedelta(hours=1),
        "ohlcv-1d": pd.Timedelta(days=1),
        "ohlcv-eod": pd.Timedelta(days=1),
    }
    return preset.get(schemaName, pd.Timedelta(hours=1))


def _utcTimestamp(value: object) -> pd.Timestamp:
    """Normalize to UTC pandas Timestamp (DuckDB timestamps may arrive as naive or aware)."""
    ts = pd.to_datetime(value, utc=True)
    if ts.tzinfo is None:
        return ts.tz_localize("UTC")
    return ts.tz_convert("UTC")


def loadCoverageForRange(
    dbPath: str,
    schemaName: str,
    symbols: list[str],
    windowStartIso: str,
    windowEndIso: str,
) -> Dict[str, Tuple[pd.Timestamp, pd.Timestamp, int]]:
    """Min/max ts and row count per symbol for stored Databento rows inside [start, end] (inclusive)."""
    if not symbols or not Path(dbPath).is_file():
        return {}
    connection = duckdb.connect(dbPath)
    placeholders = ",".join(["?" for _ in symbols])
    params: list = [schemaName, windowStartIso, windowEndIso, *symbols]
    try:
        frame = connection.execute(
            f"""
            SELECT
                symbol::VARCHAR AS symbol,
                MIN(ts) AS min_ts,
                MAX(ts) AS max_ts,
                COUNT(*)::BIGINT AS cnt
            FROM hourlyBars
            WHERE source = 'databento'
              AND schemaName = ?
              AND ts >= CAST(? AS TIMESTAMPTZ)
              AND ts <= CAST(? AS TIMESTAMPTZ)
              AND symbol IN ({placeholders})
            GROUP BY symbol
            """,
            params,
        ).df()
    except duckdb.Error:
        frame = pd.DataFrame()
    finally:
        connection.close()

    if frame.empty:
        return {}
    out: Dict[str, Tuple[pd.Timestamp, pd.Timestamp, int]] = {}
    for _, row in frame.iterrows():
        out[str(row["symbol"])] = (
            _utcTimestamp(row["min_ts"]),
            _utcTimestamp(row["max_ts"]),
            int(row["cnt"]),
        )
    return out


def coverageSlackFor(schemaName: str) -> Tuple[pd.Timedelta, pd.Timedelta]:
    """Return ``(startSlack, endSlack)`` used to decide if a symbol is already covered.

    Markets don't trade overnight, so the symbol's earliest stored bar typically lags
    the request's calendar window-start by hours (or a weekend). A generous start
    slack avoids re-fetching everything on each run. The end slack stays tight so
    that extending ``--end-ts`` reliably triggers a fresh fetch of the new tail.
    """
    bar_td = _schemaBarTimedelta(schemaName)
    startSlack = max(bar_td * 48, pd.Timedelta(days=2))
    endSlack = max(bar_td * 2, pd.Timedelta(minutes=5))
    return startSlack, endSlack


def symbolAlreadySpansWindow(
    stats: Optional[Tuple[pd.Timestamp, pd.Timestamp, int]],
    windowStart: pd.Timestamp,
    windowEnd: pd.Timestamp,
    startSlack: pd.Timedelta,
    endSlack: pd.Timedelta,
) -> bool:
    """True if DuckDB already has bars covering both ends of the requested window."""
    if stats is None:
        return False
    min_ts, max_ts, cnt = stats
    if cnt <= 0:
        return False
    return min_ts <= windowStart + startSlack and max_ts >= windowEnd - endSlack


def parseWindowBounds(startTs: str, endTs: str) -> Tuple[pd.Timestamp, pd.Timestamp]:
    windowStart = pd.Timestamp(startTs)
    windowEnd = pd.Timestamp(endTs)
    if windowStart.tzinfo is None:
        windowStart = windowStart.tz_localize("UTC")
    else:
        windowStart = windowStart.tz_convert("UTC")
    if windowEnd.tzinfo is None:
        windowEnd = windowEnd.tz_localize("UTC")
    else:
        windowEnd = windowEnd.tz_convert("UTC")
    return windowStart, windowEnd


def _exclusiveEndRangeBounds(startTs: str, endTs: str) -> Tuple[pd.Timestamp, pd.Timestamp]:
    """Inclusive start / exclusive end in UTC as Databento ``timeseries.get_range`` interprets ``end``."""
    startUtc, endUtc = parseWindowBounds(startTs, endTs)
    if endUtc <= startUtc:
        raise ValueError(
            "Databento requires start strictly before exclusive end; "
            f"got start={startTs!r}, end={endTs!r}"
        )
    return startUtc, endUtc


def _utcTsToApiIso(ts: pd.Timestamp) -> str:
    return ts.tz_convert("UTC").strftime("%Y-%m-%dT%H:%M:%SZ")


def loadSymbols(symbolFilePath: str) -> list[str]:
    path = Path(symbolFilePath)
    if not path.is_file():
        raise FileNotFoundError(f"Symbol list not found: {path.resolve()}")
    symbols: list[str] = []
    for line in path.read_text(encoding="utf-8").splitlines():
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        if not _SAFE_SYMBOL_PATTERN.match(stripped):
            raise ValueError(
                f"Unsafe symbol line (must start with alphanumeric; allowed chars: letters, digits, ._-): {stripped!r}"
            )
        symbols.append(stripped)
    if not symbols:
        raise ValueError(f"No symbols found in {path}")
    return symbols


def normalizeFrame(rawFrame: pd.DataFrame) -> pd.DataFrame:
    frame = rawFrame.copy()
    if "ts_event" not in frame.columns and frame.index.name == "ts_event":
        frame = frame.reset_index()

    requiredColumns = ["ts_event", "symbol", "open", "high", "low", "close", "volume"]
    missingColumns = [column for column in requiredColumns if column not in frame.columns]
    if missingColumns:
        raise ValueError(f"Missing required columns: {missingColumns}")

    frame = frame[requiredColumns].copy()
    frame["ts"] = pd.to_datetime(frame["ts_event"], utc=True)
    for column in ("open", "high", "low", "close", "volume"):
        frame[column] = pd.to_numeric(frame[column], errors="coerce")
    frame = frame.dropna(subset=["ts", "symbol", "open", "high", "low", "close", "volume"])
    frame = frame.sort_values(["symbol", "ts"])
    frame = frame.drop_duplicates(subset=["symbol", "ts"], keep="last")

    validateOhlc(frame)
    return frame


def validateOhlc(frame: pd.DataFrame) -> None:
    """Reject bars that violate basic OHLC invariants.

    The roundtrip check only verifies that what we sent equals what came back;
    it does not detect bad source data. This guard runs before we ingest.
    Assumes ``frame`` has already been numeric-coerced and NaN-dropped.
    """
    if frame.empty:
        return

    nonPositivePrice = frame[(frame[["open", "high", "low", "close"]] <= 0).any(axis=1)]
    if not nonPositivePrice.empty:
        raise ValueError(
            f"Non-positive prices detected ({len(nonPositivePrice)} row(s)). First 3:\n{nonPositivePrice.head(3)}"
        )

    negativeVolume = frame[frame["volume"] < 0]
    if not negativeVolume.empty:
        raise ValueError(
            f"Negative volume detected ({len(negativeVolume)} row(s)). First 3:\n{negativeVolume.head(3)}"
        )

    rowMax = frame[["open", "close", "low"]].max(axis=1)
    rowMin = frame[["open", "close", "high"]].min(axis=1)
    invalid = frame[(frame["high"] < rowMax) | (frame["low"] > rowMin)]
    if not invalid.empty:
        raise ValueError(
            f"OHLC range violations detected ({len(invalid)} row(s)). First 3:\n{invalid.head(3)}"
        )


def saveToDuckdb(frame: pd.DataFrame, dbPath: str, schemaName: str) -> dict:
    os.makedirs(os.path.dirname(dbPath) or ".", exist_ok=True)
    connection = duckdb.connect(dbPath)
    # Table name is legacy; bar interval is defined by the schemaName column.
    connection.execute(
        """
        CREATE TABLE IF NOT EXISTS hourlyBars (
            symbol TEXT NOT NULL,
            ts TIMESTAMPTZ NOT NULL,
            open DOUBLE NOT NULL,
            high DOUBLE NOT NULL,
            low DOUBLE NOT NULL,
            close DOUBLE NOT NULL,
            volume DOUBLE NOT NULL,
            source TEXT NOT NULL,
            schemaName TEXT NOT NULL,
            ingestedAt TIMESTAMPTZ NOT NULL,
            PRIMARY KEY(symbol, ts, source, schemaName)
        )
        """
    )

    loadFrame = frame.copy()
    loadFrame["source"] = "databento"
    loadFrame["schemaName"] = schemaName
    loadFrame["ingestedAt"] = pd.Timestamp.now(tz="UTC")
    connection.register("incomingBars", loadFrame)

    rowsBefore = connection.execute("SELECT COUNT(*) FROM hourlyBars").fetchone()[0]
    incomingRows = connection.execute("SELECT COUNT(*) FROM incomingBars").fetchone()[0]
    newKeyCount = connection.execute(
        """
        SELECT COUNT(*) FROM incomingBars i
        WHERE NOT EXISTS (
            SELECT 1 FROM hourlyBars h
            WHERE h.symbol = i.symbol
              AND h.ts = i.ts
              AND h.source = i.source
              AND h.schemaName = i.schemaName
        )
        """
    ).fetchone()[0]

    connection.execute(
        """
        INSERT INTO hourlyBars (symbol, ts, open, high, low, close, volume, source, schemaName, ingestedAt)
        SELECT symbol, ts, open, high, low, close, volume, source, schemaName, ingestedAt
        FROM incomingBars
        ON CONFLICT(symbol, ts, source, schemaName) DO UPDATE SET
            open = excluded.open,
            high = excluded.high,
            low = excluded.low,
            close = excluded.close,
            volume = excluded.volume,
            ingestedAt = excluded.ingestedAt
        """
    )
    rowsAfter = connection.execute("SELECT COUNT(*) FROM hourlyBars").fetchone()[0]
    connection.close()

    expectedAfter = rowsBefore + newKeyCount
    if rowsAfter != expectedAfter:
        raise RuntimeError(
            "DuckDB invariant violated after upsert: "
            f"rowsBefore={rowsBefore}, newKeys={newKeyCount}, expectedAfter={expectedAfter}, actualAfter={rowsAfter}."
        )

    print(
        f"DuckDB safeguard: rows before={rowsBefore}, incoming={incomingRows}, "
        f"new keys={newKeyCount}, updated keys={incomingRows - newKeyCount}, rows after={rowsAfter}"
    )
    return {
        "rowsBefore": rowsBefore,
        "incomingRows": incomingRows,
        "newKeyCount": newKeyCount,
        "rowsAfter": rowsAfter,
    }


def roundtripCheck(frame: pd.DataFrame, dbPath: str, schemaName: str) -> None:
    if frame.empty:
        raise RuntimeError("Roundtrip check failed: input frame is empty.")

    minTs = frame["ts"].min().isoformat()
    maxTs = frame["ts"].max().isoformat()
    symbols = sorted(frame["symbol"].astype(str).unique().tolist())
    placeholders = ",".join(["?" for _ in symbols])

    connection = duckdb.connect(dbPath)
    params: list = [schemaName, *symbols, minTs, maxTs]
    dbFrame = connection.execute(
        f"""
        SELECT symbol, ts, open, high, low, close, volume
        FROM hourlyBars
        WHERE source = 'databento'
          AND schemaName = ?
          AND symbol IN ({placeholders})
          AND ts >= CAST(? AS TIMESTAMPTZ)
          AND ts <= CAST(? AS TIMESTAMPTZ)
        ORDER BY symbol, ts
        """,
        params,
    ).df()
    connection.close()

    inputFrame = frame[["symbol", "ts", "open", "high", "low", "close", "volume"]].copy()
    inputFrame = inputFrame.sort_values(["symbol", "ts"]).reset_index(drop=True)
    dbFrame = dbFrame.sort_values(["symbol", "ts"]).reset_index(drop=True)

    if len(dbFrame) < len(inputFrame):
        raise RuntimeError(
            f"Roundtrip check failed: database has fewer rows ({len(dbFrame)}) than input ({len(inputFrame)})."
        )

    merged = inputFrame.merge(
        dbFrame,
        on=["symbol", "ts"],
        how="left",
        suffixes=("_input", "_db"),
    )
    if merged[["open_db", "high_db", "low_db", "close_db", "volume_db"]].isna().any().any():
        raise RuntimeError("Roundtrip check failed: some input timestamps are missing in DuckDB.")

    for column in ["open", "high", "low", "close", "volume"]:
        delta = (merged[f"{column}_input"] - merged[f"{column}_db"]).abs().max()
        if pd.isna(delta):
            raise RuntimeError(f"Roundtrip check failed: column {column} has invalid values.")
        if float(delta) > 1e-9:
            raise RuntimeError(
                f"Roundtrip check failed: column {column} mismatch max delta={float(delta)}."
            )

    print(f"Roundtrip check passed: validated {len(inputFrame)} rows against DuckDB.")


def _detailCase(exc: BentoClientError) -> str:
    body = exc.json_body
    if isinstance(body, dict):
        detail = body.get("detail")
        if isinstance(detail, dict):
            return str(detail.get("case", "") or "")
    return ""


def _combinedErrorText(exc: BentoClientError) -> str:
    parts: List[str] = []
    if exc.message:
        parts.append(str(exc.message))
    if exc.http_body:
        parts.append(str(exc.http_body))
    return "\n".join(parts)


def _looksLikeUnknownOrInvalidSymbolBatchError(exc: BentoClientError) -> bool:
    """True when bisecting the symbol list might isolate unknown/invalid tickers."""
    if exc.http_status == 402:
        return False
    case_lc = (_detailCase(exc)).lower()
    fatal_cases = frozenset(
        {
            "account_insufficient_funds",
            "data_end_after_available_end",
            "data_start_before_available_start",
            "subscription_required",
            "not_authorized",
            "authentication_required",
        }
    )
    if case_lc and case_lc in fatal_cases:
        return False

    haystack = (_combinedErrorText(exc) + " " + case_lc).lower()
    fatal_phrases = (
        "insufficient funds",
        "account_insufficient",
        "data_end_after_available",
        "data start before available",
    )
    if any(p in haystack for p in fatal_phrases):
        return False

    hint_terms = (
        "symbol",
        "symbology",
        "stype",
        "resolve",
        "instrument",
    )
    is_4xx = 400 <= exc.http_status < 500
    has_hint = is_4xx and any(t in haystack for t in hint_terms)
    # Only trust 4xx with an actual symbology hint — a generic 422 might be a malformed
    # schema, deprecated endpoint, or new error the SDK doesn't enumerate; bisecting
    # such errors silently drops every symbol and hides the real cause.
    return has_hint


def _getRangeDataFrame(client: db.Historical, symbols: list[str], schemaName: str, startTs: str, endTs: str) -> pd.DataFrame:
    data = client.timeseries.get_range(
        dataset=DATASET,
        symbols=symbols,
        schema=schemaName,
        start=startTs,
        end=endTs,
    )
    frame = data.to_df()
    # Databento returns a DataFrame indexed by ts_event. Materialize it as a column so
    # downstream concat (after symbol/time bisection) doesn't lose the timestamps when
    # ignore_index=True resets the RangeIndex.
    if "ts_event" not in frame.columns and frame.index.name == "ts_event":
        frame = frame.reset_index()
    return frame


class _PerSymbolBudgetExhausted(Exception):
    """One symbol cannot be afforded across the requested window even at the time floor.

    Raised from the single-symbol time-bisection leaf when ``span <= _402_MIN_TIME_SLICE``.
    Propagates up through nested time-bisection calls and is caught at the multi-symbol
    bisection layer (or in ``loadFrameFromInput``) where it converts into a warn-skip-empty
    so the rest of the run keeps going. Matches the user-facing semantics of the
    unknown-symbol skip path.
    """

    def __init__(self, symbol: str, startTs: str, endTs: str, span: pd.Timedelta) -> None:
        self.symbol = symbol
        self.startTs = startTs
        self.endTs = endTs
        self.span = span
        super().__init__(symbol)


def _warnSkippedSymbol(symbol: str, exc: BentoClientError) -> None:
    _logLine(
        f"Warning: skipping unknown or invalid symbol {symbol!r}: {exc}",
        file=sys.stderr,
    )


def _warnSymbolBudgetExhausted(exc: _PerSymbolBudgetExhausted) -> None:
    _logLine(
        f"Warning: skipping {exc.symbol!r} — Databento 402 insufficient_funds across "
        f"{exc.startTs} .. {exc.endTs} (slice down to {exc.span}); already at or below the "
        f"{_402_MIN_TIME_SLICE} floor. Add credits, switch to a coarser --schema-name, "
        f"or narrow --start-ts/--end-ts to recover this symbol. "
        f"See https://databento.com/docs/portal/billing",
        file=sys.stderr,
    )


def _concatNonEmptyFrames(left: pd.DataFrame, right: pd.DataFrame) -> pd.DataFrame:
    frames = [f for f in (left, right) if not f.empty]
    if not frames:
        return pd.DataFrame()
    return pd.concat(frames, ignore_index=True)


def _fetchPartitioningBadSymbols(
    client: db.Historical,
    symbols: list[str],
    schemaName: str,
    startTs: str,
    endTs: str,
    *,
    strictSymbols: bool,
    insufficientFundsSplitDepth: int = 0,
    timeSplit402Depth: int = 0,
) -> pd.DataFrame:
    if not symbols:
        return pd.DataFrame()

    try:
        return _getRangeDataFrame(client, symbols, schemaName, startTs, endTs)
    except BentoClientError as exc:
        if exc.http_status == 402:
            if len(symbols) == 1:
                startUtc, endUtc = _exclusiveEndRangeBounds(startTs, endTs)
                span = endUtc - startUtc
                if span <= _402_MIN_TIME_SLICE:
                    # Don't abort the run; let the multi-symbol layer catch this and warn-skip
                    # the symbol so the rest of the universe continues to fetch.
                    raise _PerSymbolBudgetExhausted(symbols[0], startTs, endTs, span) from exc
                if timeSplit402Depth == 0:
                    _logLine(
                        f"Note: 402 insufficient_funds for {symbols[0]!r} across this entire range — "
                        "bisecting the date window into smaller requests and merging bars.",
                        file=sys.stderr,
                    )
                midUtc = startUtc + span / 2
                startIso = _utcTsToApiIso(startUtc)
                midIso = _utcTsToApiIso(midUtc)
                endIso = _utcTsToApiIso(endUtc)
                nextTs = timeSplit402Depth + 1
                left_df = _fetchPartitioningBadSymbols(
                    client,
                    symbols,
                    schemaName,
                    startIso,
                    midIso,
                    strictSymbols=strictSymbols,
                    insufficientFundsSplitDepth=insufficientFundsSplitDepth,
                    timeSplit402Depth=nextTs,
                )
                right_df = _fetchPartitioningBadSymbols(
                    client,
                    symbols,
                    schemaName,
                    midIso,
                    endIso,
                    strictSymbols=strictSymbols,
                    insufficientFundsSplitDepth=insufficientFundsSplitDepth,
                    timeSplit402Depth=nextTs,
                )
                return _concatNonEmptyFrames(left_df, right_df)
            if insufficientFundsSplitDepth == 0:
                _logLine(
                    f"Note: 402 insufficient_funds for {len(symbols)} symbol(s) "
                    f"({_formatSymbolPreview(symbols, 6)}) — splitting into smaller groups.",
                    file=sys.stderr,
                )
            mid = max(1, len(symbols) // 2)
            left_syms = symbols[:mid]
            right_syms = symbols[mid:]
            next_depth = insufficientFundsSplitDepth + 1

            def _fetchHalfWithBudgetCatch(half_syms: list[str]) -> pd.DataFrame:
                try:
                    return _fetchPartitioningBadSymbols(
                        client,
                        half_syms,
                        schemaName,
                        startTs,
                        endTs,
                        strictSymbols=strictSymbols,
                        insufficientFundsSplitDepth=next_depth,
                        timeSplit402Depth=timeSplit402Depth,
                    )
                except _PerSymbolBudgetExhausted as exhausted:
                    _warnSymbolBudgetExhausted(exhausted)
                    return pd.DataFrame()

            left_df = _fetchHalfWithBudgetCatch(left_syms)
            right_df = _fetchHalfWithBudgetCatch(right_syms)
            return _concatNonEmptyFrames(left_df, right_df)

        if strictSymbols:
            raise

        if not _looksLikeUnknownOrInvalidSymbolBatchError(exc):
            raise
        if len(symbols) == 1:
            _warnSkippedSymbol(symbols[0], exc)
            return pd.DataFrame()

        mid = max(1, len(symbols) // 2)
        left_syms = symbols[:mid]
        right_syms = symbols[mid:]
        left_df = _fetchPartitioningBadSymbols(
            client,
            left_syms,
            schemaName,
            startTs,
            endTs,
            strictSymbols=False,
            timeSplit402Depth=timeSplit402Depth,
        )
        right_df = _fetchPartitioningBadSymbols(
            client,
            right_syms,
            schemaName,
            startTs,
            endTs,
            strictSymbols=False,
            timeSplit402Depth=timeSplit402Depth,
        )
        return _concatNonEmptyFrames(left_df, right_df)


def loadFrameFromInput(
    symbols: list[str],
    schemaName: str,
    startTs: str,
    endTs: str,
    *,
    strictSymbols: bool = False,
) -> pd.DataFrame:
    # Read at call time so running under collectData.sh after sourcing .env is reliable,
    # and strip CR/LF quirks from edited .env files on Windows-style line endings.
    apiKey = os.environ.get(_ENV_DATABENTO_API_KEY, "").strip()
    if not apiKey:
        raise RuntimeError(
            f"{_ENV_DATABENTO_API_KEY} is not set. Add it to repo-root .env (use collectData.sh) or "
            f"export {_ENV_DATABENTO_API_KEY}='...' in your shell."
        )

    client = db.Historical(apiKey)
    try:
        return _fetchPartitioningBadSymbols(
            client, symbols, schemaName, startTs, endTs, strictSymbols=strictSymbols
        )
    except _PerSymbolBudgetExhausted as exhausted:
        # Reached when the caller fed a single symbol that exhausts before any
        # multi-symbol catch could fire. Warn and let main()'s empty-frame guard skip.
        _warnSymbolBudgetExhausted(exhausted)
        return pd.DataFrame()


def _formatSymbolPreview(symbols: list[str], limit: int = 20) -> str:
    if len(symbols) <= limit:
        return ", ".join(symbols)
    return ", ".join(symbols[:limit]) + f", … ({len(symbols)} total)"


def parseArgs() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Fetch Databento OHLCV bars for symbols in a list; save to DuckDB."
    )
    parser.add_argument(
        "--symbol-file",
        default=DEFAULT_SYMBOL_LIST,
        help=f"Path to symbol list (default: {DEFAULT_SYMBOL_LIST})",
    )
    parser.add_argument(
        "--db-path",
        default=DEFAULT_DB_PATH,
        help=f"DuckDB file (default: {DEFAULT_DB_PATH})",
    )
    parser.add_argument(
        "--schema-name",
        default=DEFAULT_SCHEMA_NAME,
        help=(
            "Databento OHLCV schema (ohlcv-1s, ohlcv-1m, ohlcv-1h, ohlcv-1d, ohlcv-eod). "
            f"Default: {DEFAULT_SCHEMA_NAME} (env: {_ENV_SCHEMA_NAME})."
        ),
    )
    parser.add_argument(
        "--start-ts",
        default=DEFAULT_START_TS,
        help=f"ISO-8601 UTC start (default: {DEFAULT_START_TS}, env: {_ENV_START_TS}).",
    )
    parser.add_argument(
        "--end-ts",
        default=DEFAULT_END_TS,
        help=f"ISO-8601 UTC end (default: {DEFAULT_END_TS}, env: {_ENV_END_TS}).",
    )
    parser.add_argument(
        "--symbol-batch-size",
        type=int,
        default=_envInt(_ENV_SYMBOL_BATCH_SIZE, _FALLBACK_SYMBOL_BATCH_SIZE),
        metavar="N",
        help=(
            "Symbols per Databento request "
            f"(default: {_FALLBACK_SYMBOL_BATCH_SIZE}, env: {_ENV_SYMBOL_BATCH_SIZE}). "
            "Use smaller values (e.g. 10–25) if you still hit 402; requests may auto-split on 402 as well. "
            "0 = one request for all symbols (may be expensive / rejected)."
        ),
    )
    parser.add_argument(
        "--strict-symbols",
        action="store_true",
        help=(
            "Do not tolerate unknown or invalid symbols: fail on symbology-style batch errors "
            "(default: bisect and skip bad tickers with a warning). "
            "Does not disable automatic splitting on 402 insufficient_funds."
        ),
    )
    parser.add_argument(
        "--force-full",
        action="store_true",
        help=(
            "Request every symbol from Databento even if DuckDB already has bars spanning "
            "--start-ts through --end-ts (same schema/source). Default: skip those symbols to save "
            "credits when re-running after a partial fetch (e.g. 402)."
        ),
    )
    parser.add_argument(
        "--parallel-batches",
        type=int,
        default=_envInt(_ENV_PARALLEL_BATCHES, _FALLBACK_PARALLEL_BATCHES),
        metavar="N",
        help=(
            f"Fetch up to N batches concurrently from Databento (default: {_FALLBACK_PARALLEL_BATCHES}, "
            f"env: {_ENV_PARALLEL_BATCHES}). 1 = serial (legacy). DuckDB writes always remain serialized. "
            "Reduce if you hit HTTP 429/connection errors; raise if your network and Databento "
            "rate limits allow more concurrency."
        ),
    )
    return parser.parse_args()


def _fetchBatchToFrame(
    batch: list[str],
    schemaName: str,
    startTs: str,
    endTs: str,
    *,
    strictSymbols: bool,
) -> Optional[pd.DataFrame]:
    """Worker-side: fetch one batch from Databento and run normalize+validate.

    Returns ``None`` if the batch yields no usable rows (all symbols skipped, empty
    response, or fully invalid data). Network and CPU only — no DB writes.
    """
    rawFrame = loadFrameFromInput(
        batch, schemaName, startTs, endTs, strictSymbols=strictSymbols,
    )
    if rawFrame.empty:
        return None
    frame = normalizeFrame(rawFrame)
    if frame.empty:
        return None
    return frame


def _resolvePendingBatches(
    args: argparse.Namespace,
    batches: list[list[str]],
    windowStart: pd.Timestamp,
    windowEnd: pd.Timestamp,
    startSlack: pd.Timedelta,
    endSlack: pd.Timedelta,
    allSymbols: list[str],
) -> list[list[str]]:
    """Apply resume coverage to every batch and return only batches that still need fetching.

    Coverage is loaded once for the entire universe (single DuckDB query) instead of per-batch
    so the parallel workers don't re-read the DB and so log lines stay coherent.
    """
    if args.force_full:
        _logLine("--force-full: not skipping symbols that already span the requested window.")
        return [list(batch) for batch in batches]

    coverage = loadCoverageForRange(
        args.db_path, args.schema_name, allSymbols, args.start_ts, args.end_ts
    )
    pending: list[list[str]] = []
    skippedTotal = 0
    for batch in batches:
        to_fetch = [
            symbol
            for symbol in batch
            if not symbolAlreadySpansWindow(
                coverage.get(symbol), windowStart, windowEnd, startSlack, endSlack,
            )
        ]
        skippedTotal += len(batch) - len(to_fetch)
        if to_fetch:
            pending.append(to_fetch)
    if skippedTotal:
        _logLine(
            f"Resume: skipping {skippedTotal} symbol(s) already stored for this schema "
            f"(start slack {startSlack}, end slack {endSlack})."
        )
    return pending


def _runBatchesParallel(
    args: argparse.Namespace, pending: list[list[str]],
) -> Tuple[int, Optional[pd.DataFrame]]:
    """Drive concurrent fetches and serialized DuckDB writes.

    Worker threads run ``_fetchBatchToFrame`` (network + CPU). The main thread iterates
    ``as_completed`` and performs ``saveToDuckdb`` + ``roundtripCheck`` one frame at a
    time, so DuckDB never sees concurrent writers. Any unexpected exception cancels
    not-yet-started futures and propagates to ``main`` (matches serial fail-fast semantics).
    """
    parallelism = max(1, args.parallel_batches)
    totalBatches = len(pending)
    totalRows = 0
    lastNonEmptyFrame: Optional[pd.DataFrame] = None

    if parallelism == 1:
        # Serial path keeps the legacy log layout; useful for debugging.
        for idx, batch in enumerate(pending, start=1):
            _logLine(
                f"--- Batch {idx}/{totalBatches} ({len(batch)} symbol(s)): "
                f"{_formatSymbolPreview(batch, 12)} ---"
            )
            frame = _fetchBatchToFrame(
                batch, args.schema_name, args.start_ts, args.end_ts,
                strictSymbols=args.strict_symbols,
            )
            if frame is None:
                _logLine(
                    f"Batch {idx}/{totalBatches}: no rows (all symbols skipped or no data)."
                )
                continue
            saveToDuckdb(frame, args.db_path, args.schema_name)
            roundtripCheck(frame, args.db_path, args.schema_name)
            totalRows += len(frame)
            lastNonEmptyFrame = frame
            _logLine(
                f"Batch {idx}/{totalBatches}: ingested {len(frame):,} rows for "
                f"{frame['symbol'].nunique()} symbol(s)."
            )
        return totalRows, lastNonEmptyFrame

    _logLine(
        f"Submitting {totalBatches} batch(es) to a pool of {parallelism} worker(s); "
        "DuckDB writes remain single-threaded as each future completes."
    )
    with ThreadPoolExecutor(max_workers=parallelism, thread_name_prefix="fetch") as executor:
        futureToContext = {
            executor.submit(
                _fetchBatchToFrame,
                batch,
                args.schema_name,
                args.start_ts,
                args.end_ts,
                strictSymbols=args.strict_symbols,
            ): (idx, batch)
            for idx, batch in enumerate(pending, start=1)
        }
        completedCount = 0
        try:
            for fut in as_completed(futureToContext):
                idx, batch = futureToContext[fut]
                completedCount += 1
                try:
                    frame = fut.result()
                except Exception as exc:
                    _logLine(
                        f"Batch {idx}/{totalBatches} ({len(batch)} symbol(s)) "
                        f"failed: {exc!r}",
                        file=sys.stderr,
                    )
                    # Stop scheduling new fetches; let in-flight ones drain via the with-block.
                    for stillPending in futureToContext:
                        if not stillPending.done():
                            stillPending.cancel()
                    raise
                if frame is None:
                    _logLine(
                        f"[{completedCount}/{totalBatches}] Batch #{idx} "
                        f"({len(batch)} symbol(s)): no rows."
                    )
                    continue
                saveToDuckdb(frame, args.db_path, args.schema_name)
                roundtripCheck(frame, args.db_path, args.schema_name)
                totalRows += len(frame)
                lastNonEmptyFrame = frame
                _logLine(
                    f"[{completedCount}/{totalBatches}] Batch #{idx} "
                    f"({len(batch)} symbol(s)): ingested {len(frame):,} rows for "
                    f"{frame['symbol'].nunique()} symbol(s)."
                )
        except KeyboardInterrupt:
            _logLine(
                "Interrupted by user; cancelling pending fetches and waiting for in-flight ones to finish.",
                file=sys.stderr,
            )
            for stillPending in futureToContext:
                if not stillPending.done():
                    stillPending.cancel()
            raise
    return totalRows, lastNonEmptyFrame


def main() -> None:
    args = parseArgs()
    symbols = loadSymbols(args.symbol_file)
    _logLine(f"Symbols ({len(symbols)}): {_formatSymbolPreview(symbols)}")
    _logLine(f"Schema: {args.schema_name}  Range: {args.start_ts} -> {args.end_ts}")

    batches = _chunkSymbols(symbols, args.symbol_batch_size)
    if len(batches) > 1:
        _logLine(
            f"Chunked into {len(batches)} batch(es) of up to {args.symbol_batch_size} symbol(s) each "
            "(per-request budget tuning; total bytes add up across batches)."
        )

    windowStart, windowEnd = parseWindowBounds(args.start_ts, args.end_ts)
    startSlack, endSlack = coverageSlackFor(args.schema_name)

    pending = _resolvePendingBatches(
        args, batches, windowStart, windowEnd, startSlack, endSlack, symbols,
    )
    if not pending:
        _logLine(
            "Nothing to fetch — every symbol already covers the requested window. "
            "Use --force-full to re-fetch from Databento."
        )
        return

    totalRows, lastNonEmptyFrame = _runBatchesParallel(args, pending)

    if totalRows == 0:
        _logLine(
            "No new bars written in this run. Either every symbol already had data for this "
            "schema and date range, every batch produced no rows from the API, or only empty "
            "frames were returned. Use --force-full to re-download from Databento, or widen "
            "--start-ts/--end-ts, check symbols, dataset, or entitlements."
        )
        return

    if lastNonEmptyFrame is not None:
        with _PRINT_LOCK:
            print(lastNonEmptyFrame.head(10))
    _logLine(f"Rows fetched (last batch sample above; all batches): {totalRows:,}")
    _logLine(f"DuckDB written: {args.db_path}")


if __name__ == "__main__":
    main()
