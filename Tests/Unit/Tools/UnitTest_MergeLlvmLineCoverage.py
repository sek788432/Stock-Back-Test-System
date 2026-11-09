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
    def test_preserves_compiler_line_numbers_across_non_newline_whitespace(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            source = root / "Src" / "Example.cpp"
            source.parent.mkdir(parents=True)
            for whitespace in ("\f", "\v"):
                with self.subTest(whitespace=repr(whitespace)):
                    source.write_text(
                        "int covered() { return 1; }\n"
                        f"{whitespace}// harmless comment\n"
                        "void uncovered() { performWork(); }\n",
                        encoding="utf-8",
                    )
                    original = {
                        "files": [{
                            "file": "Src/Example.cpp",
                            "lines": [{"line_number": 3, "count": 0, "branches": []}],
                        }]
                    }
                    report = copy.deepcopy(original)
                    coverage_merge.merge_line_hits(
                        report, f"SF:{source}\nDA:1,1\nend_of_record\n", root
                    )
                    self.assertEqual(report, original)

    def test_preserves_files_with_ambiguous_multiline_lexical_context(self) -> None:
        sources = (
            'void uncovered() { const char *text = R"(\n//)"; performWork(); }\n',
            'void uncovered() { const char *text = R"(\n#)"; performWork(); }\n',
            'const char *text = "continued\\\n// string"; performWork();\n',
            '#define BODY \\\n// macro continuation\n',
            '/* block comment\n// */ performWork();\n',
        )
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            source = root / "Src" / "Example.cpp"
            source.parent.mkdir(parents=True)
            for text in sources:
                with self.subTest(source=text):
                    source.write_text(text, encoding="utf-8")
                    original = {
                        "files": [{
                            "file": "Src/Example.cpp",
                            "lines": [{"line_number": 2, "count": 0, "branches": []}],
                        }]
                    }
                    report = copy.deepcopy(original)
                    coverage_merge.merge_line_hits(
                        report, f"SF:{source}\nDA:1,1\nend_of_record\n", root
                    )
                    self.assertEqual(report, original)

    def test_removes_only_unmapped_comment_and_directive_records(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            source = root / "Src" / "Example.cpp"
            source.parent.mkdir(parents=True)
            source.write_text(
                "// compiler-unmapped comment\n"
                "#else\n"
                "return untested();\n"
                "// compiler-mapped comment\n"
                "#endif\n"
                "// comment with decision evidence\n"
                "#define BODY performWork()\n",
                encoding="utf-8",
            )
            branch = {"branchno": 0, "count": 0}
            decision = {"type": "conditional", "count_true": 0, "count_false": 2}
            report = {
                "files": [
                    {
                        "file": "Src/Example.cpp",
                        "lines": [
                            {"line_number": 1, "count": 0, "branches": []},
                            {"line_number": 2, "count": 0, "branches": []},
                            {"line_number": 3, "count": 0, "branches": []},
                            {"line_number": 4, "count": 0, "branches": []},
                            {"line_number": 5, "count": 0, "branches": [branch]},
                            {
                                "line_number": 6,
                                "count": 0,
                                "branches": [],
                                "gcovr/decision": decision,
                            },
                            {"line_number": 7, "count": 0, "branches": []},
                        ],
                    }
                ]
            }
            lcov = f"SF:{source}\nDA:3,0\nDA:4,0\nend_of_record\n"

            coverage_merge.merge_line_hits(report, lcov, root)

            lines = report["files"][0]["lines"]
            self.assertEqual([line["line_number"] for line in lines], [3, 4, 5, 6, 7])
            self.assertEqual([line["count"] for line in lines], [0, 0, 0, 0, 0])
            self.assertEqual(lines[2]["branches"], [branch])
            self.assertEqual(lines[3]["gcovr/decision"], decision)

    def test_preserves_noncode_without_compiler_and_source_evidence(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            source = root / "Src" / "Example.cpp"
            source.parent.mkdir(parents=True)
            source.write_text("// comment\n", encoding="utf-8")
            original = {
                "files": [{
                    "file": "Src/Example.cpp",
                    "lines": [
                        {"line_number": 1, "count": 0, "branches": []},
                        {"line_number": 2, "count": 0, "branches": []},
                    ],
                }]
            }
            for lcov in ("", f"SF:{source}\nend_of_record\n"):
                with self.subTest(lcov=lcov):
                    report = copy.deepcopy(original)
                    coverage_merge.merge_line_hits(report, lcov, root)
                    self.assertEqual(report, original)

            source.unlink()
            report = copy.deepcopy(original)
            coverage_merge.merge_line_hits(
                report, f"SF:{source}\nDA:2,0\nend_of_record\n", root
            )
            self.assertEqual(report, original)

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
