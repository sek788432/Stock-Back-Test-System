from __future__ import annotations

import argparse
import hashlib
import multiprocessing
import os
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

            case_collision = [
                SourceFileDeclaration("A.csv", "ohlcv-1h", digest),
                SourceFileDeclaration("a.csv", "ohlcv-1h", digest),
            ]
            with self.assertRaises(ValueError):
                writeSourceManifest(directory, case_collision)

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

    def test_export_rejects_symbol_filename_collisions_before_writing(self) -> None:
        with tempfile.TemporaryDirectory(prefix="bte-export-collision-") as temporary:
            root = Path(temporary)
            database = root / "MarketData.duckdb"
            database.touch()
            output = root / "Extracted"
            arguments = argparse.Namespace(symbol_file="unused", db_path=str(database),
                                           out_dir=str(output), schema_name=None,
                                           include_provenance=False)
            for symbols in (["BRK.B", "BRK_B"], ["BRK_B", "BRK.B"], ["AAA", "aaa"]):
                with (patch.object(GetFromDb, "parseArgs", return_value=arguments),
                      patch.object(GetFromDb, "loadSymbols", return_value=symbols),
                      patch.object(GetFromDb, "exportDuckdbToCsv") as export):
                    with self.assertRaises(ValueError):
                        GetFromDb.main()
                    export.assert_not_called()

            output.mkdir(exist_ok=True)
            existing = output / "BRK_B.csv"
            existing.write_text("symbol,ts\nBRK.B,2024-01-01\n", encoding="utf-8")
            with (patch.object(GetFromDb, "parseArgs", return_value=arguments),
                  patch.object(GetFromDb, "loadSymbols", return_value=["BRK_B"]),
                  patch.object(GetFromDb, "exportDuckdbToCsv") as export):
                with self.assertRaises(ValueError):
                    GetFromDb.main()
                export.assert_not_called()
            self.assertEqual(existing.read_text(encoding="utf-8"),
                             "symbol,ts\nBRK.B,2024-01-01\n")

    def test_late_failure_or_empty_symbol_preserves_previous_generation(self) -> None:
        for outcome in ("error", "empty"):
            with self.subTest(outcome=outcome), tempfile.TemporaryDirectory(prefix="bte-export-atomic-") as temporary:
                root = Path(temporary)
                output = root / "Extracted"
                output.mkdir()
                old = output / "AAA.csv"
                old.write_text("symbol,ts\nAAA,old\n", encoding="utf-8")
                writeSourceManifest(output, [sourceFileDeclaration(old, "ohlcv-1h")])
                original = old.read_bytes()
                manifest = (output / SOURCE_MANIFEST_NAME).read_bytes()
                database = root / "market.duckdb"
                database.touch()
                arguments = argparse.Namespace(symbol_file="unused", db_path=str(database),
                                               out_dir=str(output), schema_name="ohlcv-1h",
                                               include_provenance=False)
                def exportCsv(**arguments: object) -> tuple[int, str]:
                    symbol = str(arguments["symbol"])
                    if symbol == "BBB":
                        if outcome == "error":
                            raise ValueError("late failure")
                        return 0, "ohlcv-1h"
                    Path(str(arguments["outputCsvPath"])).write_text("symbol,ts\nAAA,new\n", encoding="utf-8")
                    return 1, "ohlcv-1h"
                with (patch.object(GetFromDb, "parseArgs", return_value=arguments),
                      patch.object(GetFromDb, "loadSymbols", return_value=["AAA", "BBB"]),
                      patch.object(GetFromDb, "exportDuckdbToCsv", side_effect=exportCsv)):
                    with self.assertRaises((ValueError, SystemExit)):
                        GetFromDb.main()
                self.assertEqual(old.read_bytes(), original)
                self.assertEqual((output / SOURCE_MANIFEST_NAME).read_bytes(), manifest)

    @unittest.skipUnless(os.name == "posix", "Atomic directory exchange requires POSIX")
    def test_generation_is_consistent_across_interrupted_exchange(self) -> None:
        for interruption in ("before", "after"):
            with self.subTest(interruption=interruption), tempfile.TemporaryDirectory(prefix="bte-export-interrupt-") as temporary:
                root = Path(temporary)
                output = root / "Extracted"
                output.mkdir()
                csvPath = output / "AAA.csv"
                csvPath.write_text("symbol,ts\nAAA,old\n", encoding="utf-8")
                writeSourceManifest(output, [sourceFileDeclaration(csvPath, "ohlcv-1h")])
                database = root / "market.duckdb"
                database.touch()
                arguments = argparse.Namespace(symbol_file="unused", db_path=str(database),
                                               out_dir=str(output), schema_name="ohlcv-1h",
                                               include_provenance=False)
                def exportCsv(**arguments: object) -> tuple[int, str]:
                    Path(str(arguments["outputCsvPath"])).write_text("symbol,ts\nAAA,new\n", encoding="utf-8")
                    return 1, "ohlcv-1h"
                publish = GetFromDb._publishGeneration
                def interrupt(staged: Path, destination: Path) -> None:
                    if interruption == "after":
                        publish(staged, destination)
                    os._exit(73)
                def child() -> None:
                    with (patch.object(GetFromDb, "parseArgs", return_value=arguments),
                          patch.object(GetFromDb, "loadSymbols", return_value=["AAA"]),
                          patch.object(GetFromDb, "exportDuckdbToCsv", side_effect=exportCsv),
                          patch.object(GetFromDb, "_publishGeneration", side_effect=interrupt)):
                        GetFromDb.main()
                process = multiprocessing.get_context("fork").Process(target=child)
                process.start()
                process.join(10)
                self.assertEqual(process.exitcode, 73)
                expected = "new" if interruption == "after" else "old"
                self.assertEqual(csvPath.read_text(encoding="utf-8"), f"symbol,ts\nAAA,{expected}\n")
                self.assertEqual(loadSourceManifest(output / SOURCE_MANIFEST_NAME),
                                 (sourceFileDeclaration(csvPath, "ohlcv-1h"),))


if __name__ == "__main__":
    unittest.main()
