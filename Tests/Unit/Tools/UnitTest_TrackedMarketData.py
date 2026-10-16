"""Structural validation for every tracked CSV data group."""

from __future__ import annotations

import csv
import math
import re
import subprocess
import unittest
from datetime import datetime, timedelta
from pathlib import Path


REPOSITORY_ROOT = Path(__file__).resolve().parents[3]
EXTRACT_DIRECTORY = REPOSITORY_ROOT / "StockData" / "Extracted"
SYMBOL_MANIFEST = REPOSITORY_ROOT / "StockData" / "Symbols.txt"
EXTRACT_HEADER = ("symbol", "ts", "open", "high", "low", "close", "volume")
SAFE_SYMBOL = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_.-]*$")
INTENTIONAL_EXTRA_EXTRACTS = {"AAPL.csv", "TSLA.csv"}


def tracked_csv_paths() -> set[Path]:
    result = subprocess.run(
        ["git", "ls-files", "--", "*.csv"],
        cwd=REPOSITORY_ROOT,
        check=True,
        capture_output=True,
        text=True,
    )
    return {
        REPOSITORY_ROOT / relative_path
        for relative_path in result.stdout.splitlines()
        if relative_path
    }


def tracked_extract_paths() -> set[Path]:
    return {path for path in tracked_csv_paths() if path.parent == EXTRACT_DIRECTORY}


def manifest_symbols() -> list[str]:
    return [
        line.strip()
        for line in SYMBOL_MANIFEST.read_text(encoding="utf-8").splitlines()
        if line.strip() and not line.lstrip().startswith("#")
    ]


def normalized_extract_symbols(symbols: list[str]) -> dict[str, str]:
    symbols_by_name: dict[str, str] = {}
    case_insensitive_names: dict[str, str] = {}
    for symbol in symbols:
        name = f"{symbol.replace('.', '_')}.csv"
        normalized_name = name.casefold()
        if normalized_name in case_insensitive_names:
            previous = case_insensitive_names[normalized_name]
            raise ValueError(
                f"symbols {previous!r} and {symbol!r} map to the same extract {name!r}"
            )
        case_insensitive_names[normalized_name] = symbol
        symbols_by_name[name] = symbol
    return symbols_by_name


class TrackedMarketDataTest(unittest.TestCase):
    def test_symbol_manifest_has_unique_safe_entries_and_complete_extracts(self) -> None:
        symbols = manifest_symbols()

        self.assertGreater(len(symbols), 0)
        self.assertEqual(len(symbols), len(set(symbols)), "duplicate manifest symbol")
        for symbol in symbols:
            self.assertRegex(symbol, SAFE_SYMBOL)

        expected = set(normalized_extract_symbols(symbols))
        actual = {path.name for path in tracked_extract_paths()}
        self.assertEqual(len(actual), len({name.casefold() for name in actual}))
        self.assertEqual(actual - expected, INTENTIONAL_EXTRA_EXTRACTS)
        self.assertEqual(expected - actual, set(), "manifest symbols missing extracts")

    def test_extract_filename_normalization_rejects_ambiguous_symbols(self) -> None:
        for symbols in (["BRK.B", "BRK_B"], ["a", "A"]):
            with self.subTest(symbols=symbols):
                with self.assertRaisesRegex(ValueError, "map to the same extract"):
                    normalized_extract_symbols(symbols)

    def test_every_tracked_extract_has_valid_ordered_utc_ohlcv_rows(self) -> None:
        extract_paths = sorted(tracked_extract_paths())
        self.assertGreater(len(extract_paths), 0)
        symbols_by_name = normalized_extract_symbols(manifest_symbols())
        symbols_by_name.update(
            {name: Path(name).stem for name in INTENTIONAL_EXTRA_EXTRACTS}
        )
        self.assertEqual({path.name for path in extract_paths}, set(symbols_by_name))

        for path in extract_paths:
            self._validate_extract(path, symbols_by_name[path.name])

    def test_every_other_tracked_csv_has_a_consistent_nonempty_shape(self) -> None:
        extract_paths = tracked_extract_paths()
        auxiliary_paths = sorted(tracked_csv_paths() - extract_paths)
        self.assertGreater(len(auxiliary_paths), 0)

        for path in auxiliary_paths:
            with self.subTest(path=path.relative_to(REPOSITORY_ROOT)):
                with path.open(encoding="utf-8", newline="") as source:
                    rows = csv.reader(source)
                    header = next(rows, None)
                    self.assertIsNotNone(header)
                    self.assertGreater(len(header), 0)
                    self.assertTrue(all(header))
                    self.assertEqual(len(header), len(set(header)))
                    row_count = 0
                    for line_number, row in enumerate(rows, start=2):
                        self.assertEqual(
                            len(row),
                            len(header),
                            f"{path.relative_to(REPOSITORY_ROOT)}:{line_number}",
                        )
                        row_count += 1
                    self.assertGreater(row_count, 0)

    def _validate_extract(self, path: Path, expected_symbol: str) -> None:
        relative_path = path.relative_to(REPOSITORY_ROOT)
        row_count = 0
        previous_timestamp: datetime | None = None

        self.assertEqual(
            path.name,
            f"{expected_symbol.replace('.', '_')}.csv",
            f"{relative_path}: canonical symbol does not match extract filename",
        )

        with path.open(encoding="utf-8", newline="") as source:
            header = source.readline().rstrip("\r\n").split(",")
            self.assertEqual(tuple(header), EXTRACT_HEADER, str(relative_path))

            for line_number, line in enumerate(source, start=2):
                fields = line.rstrip("\r\n").split(",")
                location = f"{relative_path}:{line_number}"
                if len(fields) != len(EXTRACT_HEADER):
                    self.fail(f"{location}: expected {len(EXTRACT_HEADER)} columns")
                symbol, timestamp_text, *numeric_text = fields
                if symbol != expected_symbol:
                    self.fail(f"{location}: row symbol does not match canonical symbol")

                try:
                    timestamp = datetime.fromisoformat(timestamp_text)
                    open_price, high, low, close, volume = map(float, numeric_text)
                except ValueError as error:
                    self.fail(f"{location}: {error}")

                if timestamp.tzinfo is None:
                    self.fail(f"{location}: timezone required")
                if timestamp.utcoffset() != timedelta(0):
                    self.fail(f"{location}: timestamp must be UTC")
                if previous_timestamp is not None and timestamp <= previous_timestamp:
                    self.fail(f"{location}: timestamps must be unique and ascending")
                previous_timestamp = timestamp

                values = (open_price, high, low, close, volume)
                if not all(math.isfinite(value) for value in values):
                    self.fail(f"{location}: values must be finite")
                if min(open_price, high, low, close) <= 0.0 or volume < 0.0:
                    self.fail(f"{location}: prices must be positive and volume nonnegative")
                if high < max(open_price, low, close):
                    self.fail(f"{location}: high violates OHLC invariant")
                if low > min(open_price, high, close):
                    self.fail(f"{location}: low violates OHLC invariant")
                row_count += 1

        self.assertGreater(row_count, 0, f"{relative_path}: extract has no rows")


if __name__ == "__main__":
    unittest.main()
