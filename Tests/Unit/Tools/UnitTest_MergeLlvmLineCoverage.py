"""Behavior tests for overlaying LLVM line hits onto gcovr reports."""

from __future__ import annotations

import copy
import importlib.util
import tempfile
import unittest
from pathlib import Path


REPOSITORY_ROOT = Path(__file__).resolve().parents[3]
SPECIFICATION = importlib.util.spec_from_file_location(
    "MergeLlvmLineCoverage", REPOSITORY_ROOT / "Tools/MergeLlvmLineCoverage.py"
)
coverage_merge = importlib.util.module_from_spec(SPECIFICATION)
assert SPECIFICATION.loader is not None
SPECIFICATION.loader.exec_module(coverage_merge)


class LlvmLineCoverageMergeTest(unittest.TestCase):
    def test_updates_existing_report_lines_and_preserves_branch_data(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            source = root / "Src" / "Example.cpp"
            source.parent.mkdir(parents=True)
            source.touch()
            report = {
                "gcovr/format_version": "0.5",
                "files": [
                    {
                        "file": "Src/Example.cpp",
                        "lines": [
                            {
                                "line_number": 10,
                                "count": 0,
                                "branches": [
                                    {"branchno": 0, "count": 0},
                                    {"branchno": 1, "count": 2},
                                ],
                                "gcovr/decision": {
                                    "type": "conditional",
                                    "count_true": 0,
                                    "count_false": 2,
                                },
                            },
                            {"line_number": 11, "count": 3, "branches": []},
                            {
                                "line_number": 12,
                                "count": 0,
                                "branches": [],
                                "gcovr/excluded": True,
                            },
                        ],
                    }
                ],
            }
            original_branches = copy.deepcopy(report["files"][0]["lines"][0]["branches"])
            original_decision = copy.deepcopy(
                report["files"][0]["lines"][0]["gcovr/decision"]
            )
            lcov = "\n".join(
                [
                    f"SF:{source}",
                    "DA:10,7",
                    "DA:11,1",
                    "DA:12,8",
                    "DA:13,9",
                    "end_of_record",
                    "SF:Src/Example.cpp",
                    "DA:10,5",
                    "end_of_record",
                    "SF:/outside/Src/Example.cpp",
                    "DA:10,100",
                ]
            )

            updated = coverage_merge.merge_line_hits(report, lcov, root)

            self.assertEqual(updated, 1)
            self.assertEqual(
                [line["count"] for line in report["files"][0]["lines"]],
                [7, 3, 0],
            )
            self.assertEqual(
                report["files"][0]["lines"][0]["branches"], original_branches
            )
            self.assertEqual(
                report["files"][0]["lines"][0]["gcovr/decision"], original_decision
            )
            self.assertEqual(
                [line["line_number"] for line in report["files"][0]["lines"]],
                [10, 11, 12],
            )

    def test_rejects_malformed_or_unattached_line_records(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            report = {"files": []}

            for lcov in ("DA:1,1", "SF:Src/Example.cpp\nDA:zero,1"):
                with self.subTest(lcov=lcov):
                    with self.assertRaises(ValueError):
                        coverage_merge.merge_line_hits(report, lcov, root)

    def test_rejects_malformed_gcovr_line_records(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            report = {
                "files": [
                    {
                        "file": "Src/Example.cpp",
                        "lines": [{"line_number": 0, "count": 0}],
                    }
                ]
            }

            with self.assertRaises(ValueError):
                coverage_merge.merge_line_hits(report, "", root)


if __name__ == "__main__":
    unittest.main()
