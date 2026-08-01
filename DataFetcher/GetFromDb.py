import argparse
import os
import re
import sys
from pathlib import Path
from typing import Optional

import duckdb
import pandas as pd

# ---------------------------------------------------------------------------
# Environment variables honored by this module (see README). Shell wrappers
# may export these; ExtractFromDB.sh does not load `.env`.
# ---------------------------------------------------------------------------
_ENV_STOCK_REPO_ROOT = "STOCK_REPO_ROOT"

# Require a leading alphanumeric so lines like ".", "..", or "---" are rejected.
_SAFE_SYMBOL_PATTERN = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_.-]*$")
CORE_COLUMNS = ["symbol", "ts", "open", "high", "low", "close", "volume"]
PROVENANCE_COLUMNS = ["source", "schemaName", "ingestedAt"]


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
DEFAULT_EXTRACT_DIR = str(_REPO_ROOT / STOCK_DATA_DIR / "Extracted")


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


def _distinctSchemasForSymbol(connection: duckdb.DuckDBPyConnection, symbol: str) -> list[str]:
    rows = connection.execute(
        "SELECT DISTINCT schemaName FROM hourlyBars WHERE symbol = ? ORDER BY schemaName",
        [symbol],
    ).fetchall()
    return [row[0] for row in rows]


def exportDuckdbToCsv(
    dbPath: str,
    outputCsvPath: str,
    symbol: Optional[str] = None,
    schemaName: Optional[str] = None,
    startTs: Optional[str] = None,
    endTs: Optional[str] = None,
    includeProvenance: bool = False,
) -> int:
    """Export rows for a single symbol to CSV.

    Returns the number of rows written. Timestamps are normalized to UTC so the
    output is identical regardless of the host machine's local timezone.

    If ``schemaName`` is ``None`` and the database stores rows under multiple
    schemas for this symbol, a ``ValueError`` is raised so the caller cannot
    silently produce a CSV that mixes intervals.

    When zero rows match, nothing is written. If ``outputCsvPath`` already exists
    (e.g. a stale extract), it is removed so directories do not keep empty CSVs.
    """
    connection = duckdb.connect(dbPath)
    try:
        if symbol is not None and schemaName is None:
            schemas = _distinctSchemasForSymbol(connection, symbol)
            if len(schemas) > 1:
                raise ValueError(
                    f"Symbol {symbol!r} has rows for multiple schemas in DuckDB: {schemas}. "
                    "Pass --schema-name to pick one (e.g. --schema-name ohlcv-1h)."
                )

        conditions: list[str] = []
        params: list = []
        if symbol is not None:
            conditions.append("symbol = ?")
            params.append(symbol)
        if schemaName is not None:
            conditions.append("schemaName = ?")
            params.append(schemaName)
        if startTs:
            conditions.append("ts >= CAST(? AS TIMESTAMPTZ)")
            params.append(startTs)
        if endTs:
            conditions.append("ts <= CAST(? AS TIMESTAMPTZ)")
            params.append(endTs)
        whereClause = f"WHERE {' AND '.join(conditions)}" if conditions else ""

        query = f"""
            SELECT symbol, ts, open, high, low, close, volume, source, schemaName, ingestedAt
            FROM hourlyBars
            {whereClause}
            ORDER BY symbol, ts
        """
        frame = connection.execute(query, params).df()
    finally:
        connection.close()

    # DuckDB returns TIMESTAMPTZ as datetime64 in the host's local timezone; force UTC
    # so CSV output is reproducible across machines.
    for column in ("ts", "ingestedAt"):
        if column in frame.columns and len(frame) > 0:
            frame[column] = pd.to_datetime(frame[column], utc=True)

    columns = list(CORE_COLUMNS)
    if includeProvenance:
        columns += PROVENANCE_COLUMNS
    frame = frame[columns]

    out_path = Path(outputCsvPath)
    row_count = len(frame)
    if row_count == 0:
        if out_path.is_file():
            out_path.unlink()
            print(f"No rows for target filter; removed stale CSV: {outputCsvPath}")
        else:
            print(f"No rows for target filter; skipped writing: {outputCsvPath}")
        return 0

    parent = out_path.parent
    if str(parent):
        parent.mkdir(parents=True, exist_ok=True)
    frame.to_csv(outputCsvPath, index=False)
    print(f"Exported {row_count} rows from DuckDB to CSV: {outputCsvPath}")
    return row_count


def parseArgs() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Export OHLCV bars from DuckDB to one CSV per symbol (interval is stored in schemaName column)."
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
        "--out-dir",
        default=DEFAULT_EXTRACT_DIR,
        help=f"Directory for CSV files (default: {DEFAULT_EXTRACT_DIR})",
    )
    parser.add_argument(
        "--schema-name",
        default=None,
        metavar="NAME",
        help=(
            "Only export rows with this schemaName (e.g. ohlcv-1h). "
            "If omitted and a symbol stores rows under multiple schemas, the export errors out."
        ),
    )
    parser.add_argument(
        "--include-provenance",
        action="store_true",
        help="Include source/schemaName/ingestedAt columns in the CSV (default: OHLCV only).",
    )
    return parser.parse_args()


def main() -> None:
    args = parseArgs()
    symbols = loadSymbols(args.symbol_file)
    if not Path(args.db_path).is_file():
        raise FileNotFoundError(f"DuckDB not found: {args.db_path}")

    os.makedirs(args.out_dir, exist_ok=True)
    print(f"Extracting {len(symbols)} symbol(s) to {args.out_dir}")

    emptySymbols: list[str] = []
    for symbol in symbols:
        safeName = symbol.replace(".", "_")
        outPath = os.path.join(args.out_dir, f"{safeName}.csv")
        rowCount = exportDuckdbToCsv(
            dbPath=args.db_path,
            outputCsvPath=outPath,
            symbol=symbol,
            schemaName=args.schema_name,
            includeProvenance=args.include_provenance,
        )
        if rowCount == 0:
            emptySymbols.append(symbol)

    print(f"DuckDB source: {args.db_path}")

    if emptySymbols:
        print(
            "WARNING: the following symbol(s) had no rows in DuckDB (no CSV written; "
            "any previous files for those names were removed): "
            + ", ".join(emptySymbols),
            file=sys.stderr,
        )
        sys.exit(2)


if __name__ == "__main__":
    main()
