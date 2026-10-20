from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

DATA_FETCHER_ROOT = Path(__file__).resolve().parents[3] / "DataFetcher"
sys.path.insert(0, str(DATA_FETCHER_ROOT))

from SourceManifest import (  # noqa: E402
    SOURCE_MANIFEST_NAME,
    SourceFileDeclaration,
    loadSourceManifest,
    sourceFileDeclaration,
    writeSourceManifest,
)
import GetFromDb  # noqa: E402


class SourceManifestTest(unittest.TestCase):
    def test_manifest_is_sorted_and_hashes_exact_csv_bytes(self) -> None:
        with tempfile.TemporaryDirectory(prefix="bte-source-manifest-") as temporary:
            directory = Path(temporary)
            first = directory / "A.csv"
            second = directory / "B_B.csv"
            first.write_bytes(b"symbol,ts\r\nSYN,2024-01-01\r\n")
            second.write_bytes(b"symbol,ts\nSYN,2024-01-02\n")

            writeSourceManifest(
                directory,
                [
                    sourceFileDeclaration(second, "ohlcv-1d"),
                    sourceFileDeclaration(first, "ohlcv-1h"),
                ],
            )

            expected = (
                "BTE-SOURCE-MANIFEST-V1\n"
                f"file=A.csv|ohlcv-1h|{hashlib.sha256(first.read_bytes()).hexdigest()}\n"
                f"file=B_B.csv|ohlcv-1d|{hashlib.sha256(second.read_bytes()).hexdigest()}\n"
            )
            self.assertEqual(
                (directory / SOURCE_MANIFEST_NAME).read_text(encoding="utf-8"),
                expected,
            )
            self.assertEqual(
                loadSourceManifest(directory / SOURCE_MANIFEST_NAME),
                (
                    SourceFileDeclaration(
                        "A.csv",
                        "ohlcv-1h",
                        hashlib.sha256(first.read_bytes()).hexdigest(),
                    ),
                    SourceFileDeclaration(
                        "B_B.csv",
                        "ohlcv-1d",
                        hashlib.sha256(second.read_bytes()).hexdigest(),
                    ),
                ),
            )
            self.assertEqual(
                list(directory.glob(f".{SOURCE_MANIFEST_NAME}.*.tmp")), []
            )

    def test_manifest_rejects_duplicate_and_unsafe_entries(self) -> None:
        with tempfile.TemporaryDirectory(prefix="bte-source-manifest-invalid-") as temporary:
            directory = Path(temporary)
            digest = "a" * 64
            duplicate = [
                SourceFileDeclaration("SYN.csv", "ohlcv-1h", digest),
                SourceFileDeclaration("SYN.csv", "ohlcv-1h", digest),
            ]
            with self.assertRaises(ValueError):
                writeSourceManifest(directory, duplicate)

            (directory / SOURCE_MANIFEST_NAME).write_text(
                "BTE-SOURCE-MANIFEST-V1\n"
                f"file=../SYN.csv|ohlcv-1h|{digest}\n",
                encoding="utf-8",
            )
            with self.assertRaises(ValueError):
                loadSourceManifest(directory / SOURCE_MANIFEST_NAME)

    def test_manifest_missing_file_is_distinct_from_empty_manifest(self) -> None:
        with tempfile.TemporaryDirectory(prefix="bte-source-manifest-missing-") as temporary:
            manifest = Path(temporary) / SOURCE_MANIFEST_NAME
            self.assertIsNone(loadSourceManifest(manifest))
            writeSourceManifest(manifest.parent, [])
            self.assertEqual(loadSourceManifest(manifest), ())

    def test_get_from_db_writes_export_timeframe_and_hash_manifest(self) -> None:
        with tempfile.TemporaryDirectory(prefix="bte-get-from-db-manifest-") as temporary:
            root = Path(temporary)
            symbolFile = root / "Symbols.txt"
            database = root / "MarketData.duckdb"
            outputDirectory = root / "Extracted"
            symbolFile.write_text("SYN\n", encoding="utf-8")
            database.touch()

            def exportCsv(**arguments: object) -> tuple[int, str]:
                csvPath = Path(str(arguments["outputCsvPath"]))
                csvPath.parent.mkdir(parents=True, exist_ok=True)
                csvPath.write_bytes(b"symbol,ts,open,high,low,close,volume\n")
                return 1, "ohlcv-1h"

            arguments = argparse.Namespace(
                symbol_file=str(symbolFile),
                db_path=str(database),
                out_dir=str(outputDirectory),
                schema_name=None,
                include_provenance=False,
            )
            with (
                patch.object(GetFromDb, "parseArgs", return_value=arguments),
                patch.object(GetFromDb, "loadSymbols", return_value=["SYN"]),
                patch.object(GetFromDb, "exportDuckdbToCsv", side_effect=exportCsv),
            ):
                GetFromDb.main()

            csvPath = outputDirectory / "SYN.csv"
            self.assertEqual(
                loadSourceManifest(outputDirectory / SOURCE_MANIFEST_NAME),
                (sourceFileDeclaration(csvPath, "ohlcv-1h"),),
            )


if __name__ == "__main__":
    unittest.main()
