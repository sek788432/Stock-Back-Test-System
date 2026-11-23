from __future__ import annotations

import argparse
import csv
import ctypes
import os
import re
import shutil
import sys
import tempfile
from contextlib import contextmanager
from pathlib import Path
from typing import TYPE_CHECKING, Optional

if TYPE_CHECKING:
    import duckdb

if __package__:
    from .SourceManifest import (
        SOURCE_MANIFEST_NAME,
        SourceFileDeclaration,
        loadSourceManifest,
        sourceFileDeclaration,
        writeSourceManifest,
    )
else:
    from SourceManifest import (
        SOURCE_MANIFEST_NAME,
        SourceFileDeclaration,
        loadSourceManifest,
        sourceFileDeclaration,
        writeSourceManifest,
    )

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


def validateExportFilenames(symbols: list[str], outputDirectory: Path) -> None:
    requested: dict[str, str] = {}
    for symbol in symbols:
        filename = f"{symbol.replace('.', '_')}.csv".casefold()
        if filename in requested and requested[filename] != symbol:
            raise ValueError(f"Symbols {requested[filename]!r} and {symbol!r} map to the same CSV filename")
        requested[filename] = symbol

    if not outputDirectory.exists():
        return
    for path in outputDirectory.iterdir():
        symbol = requested.get(path.name.casefold())
        if symbol is None:
            continue
        if path.name != f"{symbol.replace('.', '_')}.csv":
            raise ValueError(f"Existing CSV filename differs only by case: {path}")
        if not path.is_file() or path.is_symlink():
            raise ValueError(f"Existing CSV path is not a regular file: {path}")
        with path.open(encoding="utf-8", newline="") as source:
            rows = csv.DictReader(source)
            if rows.fieldnames is None or "symbol" not in rows.fieldnames:
                raise ValueError(f"Existing CSV lacks symbol identity: {path}")
            identities = {row["symbol"] for row in rows}
        if identities != {symbol}:
            raise ValueError(f"Existing CSV identity conflicts with {symbol!r}: {path}")


@contextmanager
def _exportLock(parent: Path):
    if os.name != "posix":
        raise RuntimeError("Atomic extract publication requires a supported POSIX host")
    import fcntl

    descriptor = os.open(parent, os.O_RDONLY)
    try:
        fcntl.flock(descriptor, fcntl.LOCK_EX)
        yield
    finally:
        fcntl.flock(descriptor, fcntl.LOCK_UN)
        os.close(descriptor)


def _publishGeneration(staged: Path, destination: Path) -> None:
    if not destination.exists():
        os.replace(staged, destination)
        return
    library = ctypes.CDLL(None, use_errno=True)
    sourceBytes = os.fsencode(staged)
    destinationBytes = os.fsencode(destination)
    if sys.platform == "darwin" and hasattr(library, "renamex_np"):
        operation = library.renamex_np
        operation.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint]
        result = operation(sourceBytes, destinationBytes, 2)
    elif sys.platform.startswith("linux") and hasattr(library, "renameat2"):
        operation = library.renameat2
        operation.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_uint]
        result = operation(-100, sourceBytes, -100, destinationBytes, 2)
    else:
        raise RuntimeError("Atomic directory exchange is unavailable on this host")
    if result != 0:
        error = ctypes.get_errno()
        raise OSError(error, os.strerror(error), str(destination))


def exportDuckdbToCsv(
    dbPath: str,
    outputCsvPath: str,
    symbol: Optional[str] = None,
    schemaName: Optional[str] = None,
    startTs: Optional[str] = None,
    endTs: Optional[str] = None,
    includeProvenance: bool = False,
) -> tuple[int, str]:
    """Export rows for a single symbol to CSV.

    Returns the number of rows written. Timestamps are normalized to UTC so the
    output is identical regardless of the host machine's local timezone.

    If ``schemaName`` is ``None`` and the database stores rows under multiple
    schemas for this symbol, a ``ValueError`` is raised so the caller cannot
    silently produce a CSV that mixes intervals.

    When zero rows match, nothing is written. If ``outputCsvPath`` already exists
    (e.g. a stale extract), it is removed so directories do not keep empty CSVs.
    """
    import duckdb
    import pandas as pd

    connection = duckdb.connect(dbPath)
    try:
        schemas: list[str] = []
        if symbol is not None and schemaName is None:
            schemas = _distinctSchemasForSymbol(connection, symbol)
            if len(schemas) > 1:
                raise ValueError(
                    f"Symbol {symbol!r} has rows for multiple schemas in DuckDB: {schemas}. "
                    "Pass --schema-name to pick one (e.g. --schema-name ohlcv-1h)."
                )
        resolvedSchemaName = schemaName or (schemas[0] if schemas else "")

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
        return 0, resolvedSchemaName

    parent = out_path.parent
    if str(parent):
        parent.mkdir(parents=True, exist_ok=True)
    frame.to_csv(outputCsvPath, index=False)
    print(f"Exported {row_count} rows from DuckDB to CSV: {outputCsvPath}")
    if not resolvedSchemaName:
        raise ValueError(
            f"Cannot determine source timeframe for exported CSV: {outputCsvPath}"
        )
    return row_count, resolvedSchemaName


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

    outputDirectory = Path(args.out_dir).absolute()
    outputDirectory.parent.mkdir(parents=True, exist_ok=True)
    with _exportLock(outputDirectory.parent):
        if outputDirectory.is_symlink() or (outputDirectory.exists() and not outputDirectory.is_dir()):
            raise ValueError(f"Extract destination is not a regular directory: {outputDirectory}")
        validateExportFilenames(symbols, outputDirectory)
        staged = Path(tempfile.mkdtemp(prefix=f".{outputDirectory.name}.generation-", dir=outputDirectory.parent))
        try:
            if outputDirectory.exists():
                shutil.copytree(outputDirectory, staged, dirs_exist_ok=True, symlinks=True)
            _writeGeneration(args, symbols, staged)
            _publishGeneration(staged, outputDirectory)
        finally:
            shutil.rmtree(staged, ignore_errors=True)
    print(f"Published source generation: {outputDirectory}")
    print(f"DuckDB source: {args.db_path}")


def _writeGeneration(args: argparse.Namespace, symbols: list[str], outputDirectory: Path) -> None:
    print(f"Extracting {len(symbols)} symbol(s) to a staged generation")
    emptySymbols: list[str] = []
    existingManifest = loadSourceManifest(outputDirectory / SOURCE_MANIFEST_NAME)
    previousDeclarations = {
        declaration.filename: declaration for declaration in existingManifest or ()
    }
    exportedDeclarations: dict[str, SourceFileDeclaration] = {}
    for symbol in symbols:
        safeName = symbol.replace(".", "_")
        outPath = outputDirectory / f"{safeName}.csv"
        rowCount, timeframe = exportDuckdbToCsv(
            dbPath=args.db_path,
            outputCsvPath=str(outPath),
            symbol=symbol,
            schemaName=args.schema_name,
            includeProvenance=args.include_provenance,
        )
        if rowCount == 0:
            emptySymbols.append(symbol)
            continue
        declaration = sourceFileDeclaration(outPath, timeframe)
        exportedDeclarations[declaration.filename] = declaration

    if emptySymbols:
        print("WARNING: no rows for requested symbol(s); previous generation retained: " +
              ", ".join(emptySymbols), file=sys.stderr)
        raise SystemExit(2)

    declarations: list[SourceFileDeclaration] = []
    for csvPath in sorted(outputDirectory.glob("*.csv")):
        declaration = exportedDeclarations.get(csvPath.name)
        if declaration is not None:
            declarations.append(declaration)
            continue
        previousDeclaration = previousDeclarations.get(csvPath.name)
        if previousDeclaration is None:
            raise ValueError(
                "Cannot write a complete source manifest because an existing "
                f"CSV was not exported and has no prior timeframe declaration: {csvPath}"
            )
        currentDeclaration = sourceFileDeclaration(csvPath, previousDeclaration.timeframe)
        if currentDeclaration.sha256 != previousDeclaration.sha256:
            raise ValueError(
                "Cannot retain a stale source declaration because its CSV "
                f"changed without re-export: {csvPath}"
            )
        declarations.append(previousDeclaration)
    writeSourceManifest(outputDirectory, declarations)



if __name__ == "__main__":
    main()
