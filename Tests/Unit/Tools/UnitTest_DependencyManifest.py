"""Regression coverage for the accepted Results-storage dependency pin."""

from __future__ import annotations

import json
import unittest
from pathlib import Path


REPOSITORY_ROOT = Path(__file__).resolve().parents[3]
SQLITE_BASELINE = "9e593bb18ea69cc5095e012465dcd675a822ed0d"


class VcpkgManifestTest(unittest.TestCase):
    def test_sqlite_dependency_is_exactly_pinned_for_results_storage(self) -> None:
        manifest_path = REPOSITORY_ROOT / "vcpkg.json"
        self.assertTrue(manifest_path.is_file(), "Results requires a pinned manifest")

        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))

        self.assertEqual(manifest["builtin-baseline"], SQLITE_BASELINE)
        self.assertEqual(
            manifest["dependencies"],
            [{"name": "sqlite3", "version>=": "3.53.4"}],
        )
        self.assertEqual(
            manifest["overrides"],
            [{"name": "sqlite3", "version": "3.53.4", "port-version": 0}],
        )


if __name__ == "__main__":
    unittest.main()
