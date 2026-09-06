from __future__ import annotations

import os
from pathlib import Path
import subprocess
import tempfile
import unittest


class SnapshotBuilderCommandTest(unittest.TestCase):
    def setUp(self) -> None:
        executable = os.environ.get("BTE_SNAPSHOT_BUILDER", "")
        self.assertTrue(executable, "BTE_SNAPSHOT_BUILDER must name the built target")
        self.executable = Path(executable)
        self.assertTrue(self.executable.is_file())

    def write_symbol(self, directory: Path, rows: str) -> None:
        directory.mkdir(parents=True)
        (directory / "SYN.csv").write_text(
            "symbol,ts,open,high,low,close,volume,schemaName\n" + rows,
            encoding="utf-8",
        )

    def run_builder(self, source: Path, store: Path) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [
                str(self.executable),
                str(source),
                str(store),
                "a" * 64,
                "b" * 64,
                "SYN",
            ],
            check=False,
            capture_output=True,
            text=True,
        )

    def test_one_row_build_is_deterministic_and_published_outside_source(self) -> None:
        with tempfile.TemporaryDirectory(prefix="bte-snapshot-command-") as temporary:
            root = Path(temporary)
            source = root / "Source"
            store = root / "Store"
            self.write_symbol(
                source,
                "SYN,2024-01-01 23:00:00+00:00,100,102,99,101,1200,ohlcv-1h\n",
            )

            first = self.run_builder(source, store)
            second = self.run_builder(source, store)

            self.assertEqual(first.returncode, 0, first.stderr)
            self.assertEqual(second.returncode, 0, second.stderr)
            snapshot_id = first.stdout.strip()
            self.assertEqual(snapshot_id, second.stdout.strip())
            self.assertRegex(snapshot_id, r"^[0-9a-f]{64}$")
            self.assertTrue(
                (store / "Snapshots" / snapshot_id / "Manifest.btesnapshot").is_file()
            )
            self.assertFalse((source / "Snapshots").exists())

    def test_invalid_arguments_and_out_of_order_rows_fail_without_snapshot(self) -> None:
        invalid = subprocess.run(
            [str(self.executable)], check=False, capture_output=True, text=True
        )
        self.assertEqual(invalid.returncode, 2)
        self.assertIn("Usage:", invalid.stderr)

        with tempfile.TemporaryDirectory(prefix="bte-snapshot-command-") as temporary:
            root = Path(temporary)
            source = root / "Source"
            store = root / "Store"
            self.write_symbol(
                source,
                "SYN,2024-01-02 00:00:00+00:00,101,103,100,102,100,ohlcv-1h\n"
                "SYN,2024-01-01 23:00:00+00:00,100,102,99,101,100,ohlcv-1h\n",
            )

            rejected = self.run_builder(source, store)

            self.assertEqual(rejected.returncode, 1)
            self.assertIn("increasing", rejected.stderr)
            self.assertFalse((store / "Snapshots").exists())


if __name__ == "__main__":
    unittest.main()
