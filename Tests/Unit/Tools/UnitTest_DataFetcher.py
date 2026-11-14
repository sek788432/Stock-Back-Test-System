from __future__ import annotations

import importlib.util
import argparse
import gc
import os
from pathlib import Path
import sys
import tempfile
import time
import unittest
import weakref
from unittest.mock import patch

HAS_DEPENDENCIES = all(importlib.util.find_spec(name) for name in ("databento", "duckdb", "pandas"))
REQUIRE_DEPENDENCIES = os.environ.get("BTE_REQUIRE_DATAFETCHER_TEST_DEPENDENCIES") == "1"
if REQUIRE_DEPENDENCIES and not HAS_DEPENDENCIES:
    raise RuntimeError(
        "DataFetcher test dependencies are required; install DataFetcher/requirements.txt"
    )
if HAS_DEPENDENCIES:
    import duckdb
    import pandas as pd

    sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "DataFetcher"))
    import FetchDatabento
    import GetFromDb


@unittest.skipUnless(HAS_DEPENDENCIES, "DataFetcher dependencies are not installed")
class DataFetcherTest(unittest.TestCase):
    def frame(self, value: object) -> object:
        return pd.DataFrame({"ts_event": ["2024-01-01T00:00:00Z"], "symbol": ["SYN"],
                             "open": [1.0], "high": [2.0], "low": [1.0],
                             "close": [1.0], "volume": [value]})

    def test_invalid_numeric_values_are_rejected_before_database_write(self) -> None:
        for value in ("bad", float("nan"), float("inf"), float("-inf")):
            with self.subTest(value=value):
                with self.assertRaises(ValueError):
                    FetchDatabento.normalizeFrame(self.frame(value))
        mixed = pd.concat([self.frame(float("inf")), self.frame(1.0)], ignore_index=True)
        with self.assertRaises(ValueError):
            FetchDatabento.normalizeFrame(mixed)

    def test_roundtrip_failure_rolls_back_batch(self) -> None:
        with tempfile.TemporaryDirectory(prefix="bte-ingest-rollback-") as temporary:
            database = str(Path(temporary) / "market.duckdb")
            valid = FetchDatabento.normalizeFrame(self.frame(1.0))
            FetchDatabento.saveToDuckdb(valid, database, "ohlcv-1h")
            changed = FetchDatabento.normalizeFrame(self.frame(2.0))
            with patch.object(FetchDatabento, "roundtripCheck", side_effect=RuntimeError("forced validation failure")):
                with self.assertRaises(RuntimeError):
                    FetchDatabento.saveToDuckdb(changed, database, "ohlcv-1h")
            with duckdb.connect(database) as connection:
                self.assertEqual(connection.execute("SELECT volume FROM hourlyBars").fetchone()[0], 1.0)

    def test_invalid_direct_write_does_not_modify_existing_database(self) -> None:
        with tempfile.TemporaryDirectory(prefix="bte-ingest-invalid-") as temporary:
            database = str(Path(temporary) / "market.duckdb")
            valid = FetchDatabento.normalizeFrame(self.frame(1.0))
            FetchDatabento.saveToDuckdb(valid, database, "ohlcv-1h")
            invalid = valid.copy()
            invalid.loc[0, "volume"] = float("inf")
            with self.assertRaises(ValueError):
                FetchDatabento.saveToDuckdb(invalid, database, "ohlcv-1h")
            with duckdb.connect(database) as connection:
                self.assertEqual(connection.execute("SELECT volume FROM hourlyBars").fetchone()[0], 1.0)

    def test_legacy_endpoint_bars_do_not_prove_request_completion(self) -> None:
        with tempfile.TemporaryDirectory(prefix="bte-legacy-coverage-") as temporary:
            database = str(Path(temporary) / "market.duckdb")
            first = self.frame(1.0)
            last = self.frame(1.0)
            last.loc[0, "ts_event"] = "2024-01-31T23:00:00Z"
            FetchDatabento.saveToDuckdb(
                FetchDatabento.normalizeFrame(pd.concat([first, last], ignore_index=True)),
                database, "ohlcv-1h")
            coverage = FetchDatabento.loadCoverageForRange(
                database, "ohlcv-1h", ["SYN"], "2024-01-01T00:00:00Z", "2024-02-01T00:00:00Z")
            start, end = FetchDatabento.parseWindowBounds("2024-01-01T00:00:00Z", "2024-02-01T00:00:00Z")
            slackStart, slackEnd = FetchDatabento.coverageSlackFor("ohlcv-1h")
            self.assertFalse(FetchDatabento.symbolAlreadySpansWindow(
                coverage.get("SYN"), start, end, slackStart, slackEnd))

    def test_committed_sparse_intervals_cover_only_contiguous_requests(self) -> None:
        with tempfile.TemporaryDirectory(prefix="bte-fetch-intervals-") as temporary:
            database = str(Path(temporary) / "market.duckdb")
            valid = FetchDatabento.normalizeFrame(self.frame(1.0))
            FetchDatabento.saveToDuckdb(valid, database, "ohlcv-1h", [
                ("SYN", "2024-01-01T00:00:00Z", "2024-01-15T00:00:00Z"),
                ("SYN", "2024-01-15T00:00:00Z", "2024-02-01T00:00:00Z")])
            coverage = FetchDatabento.loadCoverageForRange(
                database, "ohlcv-1h", ["SYN"], "2024-01-01T00:00:00Z", "2024-02-01T00:00:00Z")
            start, end = FetchDatabento.parseWindowBounds("2024-01-01T00:00:00Z", "2024-02-01T00:00:00Z")
            slackStart, slackEnd = FetchDatabento.coverageSlackFor("ohlcv-1h")
            self.assertTrue(FetchDatabento.symbolAlreadySpansWindow(
                coverage.get("SYN"), start, end, slackStart, slackEnd))
            self.assertFalse(FetchDatabento.symbolAlreadySpansWindow(
                coverage.get("SYN"), start, end + pd.Timedelta(hours=1), slackStart, slackEnd))
            with patch.object(FetchDatabento, "roundtripCheck", side_effect=RuntimeError("invalid")):
                with self.assertRaises(RuntimeError):
                    FetchDatabento.saveToDuckdb(valid, database, "ohlcv-1h", [
                        ("SYN", "2024-02-01T00:00:00Z", "2024-02-02T00:00:00Z")])
            afterFailure = FetchDatabento.loadCoverageForRange(
                database, "ohlcv-1h", ["SYN"], "2024-01-01T00:00:00Z", "2024-02-02T00:00:00Z")
            self.assertEqual(afterFailure, coverage)

    def test_empty_provider_response_records_completed_interval(self) -> None:
        with tempfile.TemporaryDirectory(prefix="bte-empty-interval-") as temporary:
            database = str(Path(temporary) / "market.duckdb")
            with (patch.dict(os.environ, {"DATABENTO_API_KEY": "test"}),
                  patch.object(FetchDatabento.db, "Historical", return_value=object()),
                  patch.object(FetchDatabento, "_getRangeDataFrame", return_value=pd.DataFrame())):
                frame = FetchDatabento._fetchBatchToFrame(
                    ["SYN"], "ohlcv-1h", "2024-01-01T00:00:00Z",
                    "2024-02-01T00:00:00Z", strictSymbols=False)
            self.assertIsNotNone(frame)
            self.assertTrue(frame.empty)
            FetchDatabento.saveToDuckdb(frame, database, "ohlcv-1h", frame.attrs["completedFetchIntervals"])
            coverage = FetchDatabento.loadCoverageForRange(database, "ohlcv-1h", ["SYN"],
                                                           "2024-01-01T00:00:00Z", "2024-02-01T00:00:00Z")
            start, end = FetchDatabento.parseWindowBounds("2024-01-01T00:00:00Z", "2024-02-01T00:00:00Z")
            self.assertTrue(FetchDatabento.symbolAlreadySpansWindow(
                coverage.get("SYN"), start, end, pd.Timedelta(0), pd.Timedelta(0)))

    def test_batch_runner_commits_bars_and_completed_interval_together(self) -> None:
        with tempfile.TemporaryDirectory(prefix="bte-batch-interval-") as temporary:
            database = str(Path(temporary) / "market.duckdb")
            arguments = argparse.Namespace(parallel_batches=1, schema_name="ohlcv-1h",
                                           start_ts="2024-01-01T00:00:00Z",
                                           end_ts="2024-02-01T00:00:00Z",
                                           strict_symbols=False, db_path=database)
            with (patch.dict(os.environ, {"DATABENTO_API_KEY": "test"}),
                  patch.object(FetchDatabento.db, "Historical", return_value=object()),
                  patch.object(FetchDatabento, "_getRangeDataFrame", return_value=self.frame(1.0))):
                count, _ = FetchDatabento._runBatchesParallel(arguments, [["SYN"]])
            self.assertEqual(count, 1)
            coverage = FetchDatabento.loadCoverageForRange(database, "ohlcv-1h", ["SYN"],
                                                           arguments.start_ts, arguments.end_ts)
            start, end = FetchDatabento.parseWindowBounds(arguments.start_ts, arguments.end_ts)
            self.assertTrue(FetchDatabento.symbolAlreadySpansWindow(
                coverage.get("SYN"), start, end, pd.Timedelta(0), pd.Timedelta(0)))

    def test_incompatible_existing_database_fails_before_fetch(self) -> None:
        with tempfile.TemporaryDirectory(prefix="bte-incompatible-coverage-") as temporary:
            database = str(Path(temporary) / "market.duckdb")
            with duckdb.connect(database) as connection:
                connection.execute("CREATE TABLE hourlyBars (symbol TEXT)")
            with self.assertRaisesRegex(ValueError, "incompatible"):
                FetchDatabento.loadCoverageForRange(database, "ohlcv-1h", ["SYN"],
                                                   "2024-01-01", "2024-02-01")

    def test_gap_in_committed_intervals_requires_refetch(self) -> None:
        start, end = FetchDatabento.parseWindowBounds("2024-01-01T00:00:00Z", "2024-02-01T00:00:00Z")
        left, gapStart = FetchDatabento.parseWindowBounds("2024-01-01T00:00:00Z", "2024-01-15T00:00:00Z")
        gapEnd, right = FetchDatabento.parseWindowBounds("2024-01-15T01:00:00Z", "2024-02-01T00:00:00Z")
        self.assertFalse(FetchDatabento.symbolAlreadySpansWindow(
            [(left, gapStart), (gapEnd, right)], start, end, pd.Timedelta(days=2), pd.Timedelta(hours=2)))

    def test_parallel_write_failure_does_not_schedule_remaining_batches(self) -> None:
        started = []
        def fetch(batch: object, *args: object, **kwargs: object) -> object:
            started.append(batch)
            return FetchDatabento.normalizeFrame(self.frame(1.0))
        arguments = argparse.Namespace(parallel_batches=2, schema_name="ohlcv-1h",
                                       start_ts="2024-01-01", end_ts="2024-01-02",
                                       strict_symbols=False, db_path="unused")
        batches = [[f"S{index}"] for index in range(8)]
        with (patch.object(FetchDatabento, "_fetchBatchToFrame", side_effect=fetch),
              patch.object(FetchDatabento, "saveToDuckdb", side_effect=RuntimeError("write failed"))):
            with self.assertRaisesRegex(RuntimeError, "write failed"):
                FetchDatabento._runBatchesParallel(arguments, batches)
        self.assertLessEqual(len(started), 2)

    def test_parallel_slow_writer_bounds_frames_and_writes_each_batch(self) -> None:
        references = []
        written = []
        aliveCounts = []
        def fetch(batch: object, *args: object, **kwargs: object) -> object:
            frame = FetchDatabento.normalizeFrame(self.frame(1.0))
            frame["symbol"] = batch[0]
            references.append(weakref.ref(frame))
            return frame
        def write(frame: object, *args: object) -> None:
            time.sleep(0.01)
            gc.collect()
            aliveCounts.append(sum(reference() is not None for reference in references))
            written.append(frame["symbol"].iloc[0])
        arguments = argparse.Namespace(parallel_batches=2, schema_name="ohlcv-1h",
                                       start_ts="2024-01-01", end_ts="2024-01-02",
                                       strict_symbols=False, db_path="unused")
        batches = [[f"S{index}"] for index in range(8)]
        with (patch.object(FetchDatabento, "_fetchBatchToFrame", new=fetch),
              patch.object(FetchDatabento, "saveToDuckdb", new=write)):
            count, last = FetchDatabento._runBatchesParallel(arguments, batches)
        self.assertEqual(count, 8)
        self.assertCountEqual(written, [batch[0] for batch in batches])
        self.assertLessEqual(max(aliveCounts), 3)
        self.assertIsNotNone(last)

    def test_dotted_export_preserves_symbol_and_rejects_later_alias(self) -> None:
        with tempfile.TemporaryDirectory(prefix="bte-dotted-export-") as temporary:
            root = Path(temporary)
            database = str(root / "market.duckdb")
            output = root / "Extracted"
            symbols = root / "Symbols.txt"
            with duckdb.connect(database) as connection:
                connection.execute("""CREATE TABLE hourlyBars (
                    symbol TEXT, ts TIMESTAMPTZ, open DOUBLE, high DOUBLE,
                    low DOUBLE, close DOUBLE, volume DOUBLE, source TEXT,
                    schemaName TEXT, ingestedAt TIMESTAMPTZ)""")
                connection.execute("""INSERT INTO hourlyBars VALUES
                    ('BRK.B', '2024-01-01 00:00:00+00', 1, 2, 1, 1, 10,
                     'databento', 'ohlcv-1h', '2024-01-01 00:00:00+00')""")
            symbols.write_text("BRK.B\n", encoding="utf-8")
            arguments = argparse.Namespace(symbol_file=str(symbols), db_path=database,
                                           out_dir=str(output), schema_name="ohlcv-1h",
                                           include_provenance=False)
            with patch.object(GetFromDb, "parseArgs", return_value=arguments):
                GetFromDb.main()
            exported = output / "BRK_B.csv"
            original = exported.read_bytes()
            self.assertEqual(pd.read_csv(exported)["symbol"].tolist(), ["BRK.B"])
            manifest = (output / "SourceManifest.btesource").read_bytes()
            symbols.write_text("BRK_B\n", encoding="utf-8")
            with patch.object(GetFromDb, "parseArgs", return_value=arguments):
                with self.assertRaises(ValueError):
                    GetFromDb.main()
            self.assertEqual(exported.read_bytes(), original)
            self.assertEqual((output / "SourceManifest.btesource").read_bytes(), manifest)

    def test_real_duckdb_late_schema_failure_preserves_published_generation(self) -> None:
        with tempfile.TemporaryDirectory(prefix="bte-export-real-rollback-") as temporary:
            root = Path(temporary)
            database = str(root / "market.duckdb")
            symbols = root / "Symbols.txt"
            output = root / "Extracted"
            with duckdb.connect(database) as connection:
                connection.execute("""CREATE TABLE hourlyBars (
                    symbol TEXT, ts TIMESTAMPTZ, open DOUBLE, high DOUBLE,
                    low DOUBLE, close DOUBLE, volume DOUBLE, source TEXT,
                    schemaName TEXT, ingestedAt TIMESTAMPTZ)""")
                connection.execute("""INSERT INTO hourlyBars VALUES
                    ('AAA', '2024-01-01 00:00:00+00', 1, 2, 1, 1, 10, 'databento', 'ohlcv-1h', '2024-01-01 00:00:00+00'),
                    ('BBB', '2024-01-01 00:00:00+00', 1, 2, 1, 1, 10, 'databento', 'ohlcv-1h', '2024-01-01 00:00:00+00'),
                    ('BBB', '2024-01-01 00:00:00+00', 1, 2, 1, 1, 10, 'databento', 'ohlcv-1d', '2024-01-01 00:00:00+00')""")
            symbols.write_text("AAA\n", encoding="utf-8")
            arguments = argparse.Namespace(symbol_file=str(symbols), db_path=database,
                                           out_dir=str(output), schema_name=None,
                                           include_provenance=False)
            with patch.object(GetFromDb, "parseArgs", return_value=arguments):
                GetFromDb.main()
            csvPath = output / "AAA.csv"
            original = csvPath.read_bytes()
            manifest = (output / "SourceManifest.btesource").read_bytes()
            with duckdb.connect(database) as connection:
                connection.execute("UPDATE hourlyBars SET close = 1.5 WHERE symbol = 'AAA'")
            symbols.write_text("AAA\nBBB\n", encoding="utf-8")
            with patch.object(GetFromDb, "parseArgs", return_value=arguments):
                with self.assertRaisesRegex(ValueError, "multiple schemas"):
                    GetFromDb.main()
            self.assertEqual(csvPath.read_bytes(), original)
            self.assertEqual((output / "SourceManifest.btesource").read_bytes(), manifest)
