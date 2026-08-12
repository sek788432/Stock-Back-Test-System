"""Behavior tests for CI analysis and changed-coverage helpers."""

from __future__ import annotations

import importlib.util
import json
import re
import subprocess
import tempfile
import unittest
from argparse import Namespace
from pathlib import Path
from types import SimpleNamespace
from unittest import mock


REPOSITORY_ROOT = Path(__file__).resolve().parents[3]
CI_WORKFLOW = REPOSITORY_ROOT / ".github/workflows/ci.yml"
WORKFLOW_TEXT = CI_WORKFLOW.read_text(encoding="utf-8")
COVERAGE_REQUIREMENTS = (
    REPOSITORY_ROOT / "Tools/CoverageRequirements.txt"
).read_text(encoding="utf-8")


def load_tool(name: str):
    specification = importlib.util.spec_from_file_location(name, REPOSITORY_ROOT / "Tools" / f"{name}.py")
    module = importlib.util.module_from_spec(specification)
    assert specification.loader is not None
    specification.loader.exec_module(module)
    return module


static_analysis = load_tool("RunStaticAnalysis")
branch_coverage = load_tool("CheckDiffBranchCoverage")


class CiWorkflowSecurityTest(unittest.TestCase):
    def setUp(self) -> None:
        self.workflow = CI_WORKFLOW.read_text(encoding="utf-8")

    def test_external_actions_are_pinned_to_full_commit_shas(self) -> None:
        action_references = re.findall(
            r"^\s*uses:\s+([^#\s]+)", self.workflow, flags=re.MULTILINE
        )

        self.assertGreater(len(action_references), 0)
        for reference in action_references:
            with self.subTest(reference=reference):
                self.assertRegex(reference, r"^[^@]+@[0-9a-f]{40}$")

    def test_checkout_steps_do_not_persist_credentials(self) -> None:
        step_blocks = re.findall(
            r"(?ms)^\s{6}- name:.*?(?=^\s{6}- name:|^\s{2}[a-z-]+:|\Z)",
            self.workflow,
        )
        checkout_blocks = [
            block for block in step_blocks if "actions/checkout@" in block
        ]

        self.assertGreater(len(checkout_blocks), 0)
        for block in checkout_blocks:
            with self.subTest(block=block):
                self.assertIn("persist-credentials: false", block)

    def test_coverage_dependencies_require_locked_hashes(self) -> None:
        self.assertIn("--only-binary=:all:", self.workflow)
        self.assertIn("--require-hashes", self.workflow)
        self.assertIn("--requirement Tools/CoverageRequirements.txt", self.workflow)


class StaticAnalysisToolTest(unittest.TestCase):
    def test_workflow_uses_supported_scan_build_probe(self) -> None:
        self.assertIn("scan-build-18 --help | sed -n '1p'", WORKFLOW_TEXT)
        self.assertNotIn("scan-build-18 --version", WORKFLOW_TEXT)

    def test_compilation_units_keep_only_project_sources_and_remove_duplicates(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "Src/Core/Bar.cpp"
            source.parent.mkdir(parents=True)
            source.touch()
            external = root / "Output/_deps/gtest.cpp"
            external.parent.mkdir(parents=True)
            external.touch()
            database = root / "compile_commands.json"
            database.write_text(
                json.dumps(
                    [
                        {"directory": str(root), "file": str(source)},
                        {"directory": str(root), "file": str(source)},
                        {"directory": str(root), "file": str(external)},
                    ]
                ),
                encoding="utf-8",
            )

            self.assertEqual(static_analysis.compilation_units(database, root), [source.resolve()])

    def test_changed_translation_units_keep_only_cpp_sources(self) -> None:
        root = Path("/tmp/repository")
        completed = mock.Mock(
            stdout="Src/Core/Bar.cpp\nSrc/Core/Bar.h\nDocs/BUILD.md\n"
        )
        with mock.patch.object(
            static_analysis.subprocess, "run", return_value=completed
        ) as run:
            self.assertEqual(
                static_analysis.changed_translation_units(root, "base", "head"),
                {(root / "Src/Core/Bar.cpp").resolve()},
            )
        run.assert_called_once_with(
            ["git", "diff", "--name-only", "base", "head", "--", "Src"],
            cwd=root,
            check=True,
            capture_output=True,
            text=True,
        )
    @mock.patch.object(static_analysis.shutil, "which", return_value=None)
    def test_missing_analyzer_is_reported(self, unused_which: mock.Mock) -> None:
        with self.assertRaisesRegex(RuntimeError, "required command not found"):
            static_analysis.find_command(("missing-tool",))

    @mock.patch.object(static_analysis.shutil, "which", return_value="/usr/bin/clang-tidy-18")
    def test_clang_tidy_command_uses_requested_database(self, unused_which: mock.Mock) -> None:
        build_directory = Path("/tmp/build")
        source = Path("/tmp/repository/Src/Bar.cpp")

        command = static_analysis.analyzer_command("clang-tidy", build_directory, [source])

        self.assertEqual(command, ["/usr/bin/clang-tidy-18", "-p", "/tmp/build", str(source)])

    @mock.patch.object(static_analysis.shutil, "which", return_value="/usr/bin/cppcheck")
    def test_cppcheck_command_uses_database_and_repository_suppressions(
        self, unused_which: mock.Mock
    ) -> None:
        command = static_analysis.analyzer_command(
            "cppcheck", Path("/tmp/build"), [Path("/tmp/repository/Src/Bar.cpp")]
        )

        self.assertIn("--project=/tmp/build/compile_commands.json", command)
        self.assertIn("--error-exitcode=1", command)
        self.assertIn("--file-filter=/tmp/repository/Src/Bar.cpp", command)
        self.assertTrue(command[-1].endswith("Cppcheck.suppressions"))

    @mock.patch.object(static_analysis.shutil, "which", return_value="/usr/bin/iwyu_tool.py")
    def test_iwyu_command_uses_mapping_and_finding_exit_code(self, unused_which: mock.Mock) -> None:
        command = static_analysis.analyzer_command(
            "iwyu", Path("/tmp/build"), [Path("/tmp/repository/Src/Bar.cpp")]
        )

        self.assertIn("/tmp/repository/Src/Bar.cpp", command)
        self.assertTrue(any(argument.endswith("Tools/Iwyu.imp") for argument in command))
        self.assertEqual(command[-2:], ["-Xiwyu", "--error=1"])

    def test_unsupported_analyzer_is_rejected(self) -> None:
        with self.assertRaisesRegex(ValueError, "unsupported analyzer"):
            static_analysis.analyzer_command("unknown", Path("/tmp/build"), [])

    def test_main_rejects_missing_compilation_database(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            arguments = SimpleNamespace(tool="clang-tidy", build_dir=Path(directory))
            with mock.patch.object(static_analysis, "parse_arguments", return_value=arguments):
                self.assertEqual(static_analysis.main(), 2)

    def test_main_propagates_analyzer_failure_exit_code(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            build_directory = root / "Output/analysis"
            build_directory.mkdir(parents=True)
            source = root / "Src/Core/Bar.cpp"
            source.parent.mkdir(parents=True)
            source.touch()
            (build_directory / "compile_commands.json").write_text(
                json.dumps([{"directory": str(root), "file": str(source)}]),
                encoding="utf-8",
            )
            arguments = SimpleNamespace(
                tool="clang-tidy", build_dir=build_directory, base=None, head=None
            )
            completed = mock.Mock(returncode=17)

            with (
                mock.patch.object(static_analysis, "REPOSITORY_ROOT", root),
                mock.patch.object(static_analysis, "parse_arguments", return_value=arguments),
                mock.patch.object(static_analysis, "find_command", return_value="clang-tidy"),
                mock.patch.object(static_analysis.subprocess, "run", return_value=completed),
            ):
                self.assertEqual(static_analysis.main(), 17)

    def test_main_skips_analyzer_when_no_translation_unit_changed(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            build_directory = root / "Output/analysis"
            build_directory.mkdir(parents=True)
            source = root / "Src/Core/Bar.cpp"
            source.parent.mkdir(parents=True)
            source.touch()
            (build_directory / "compile_commands.json").write_text(
                json.dumps([{"directory": str(root), "file": str(source)}]),
                encoding="utf-8",
            )
            arguments = SimpleNamespace(
                tool="clang-tidy",
                build_dir=build_directory,
                base="base",
                head="head",
            )
            with (
                mock.patch.object(static_analysis, "REPOSITORY_ROOT", root),
                mock.patch.object(
                    static_analysis, "parse_arguments", return_value=arguments
                ),
                mock.patch.object(
                    static_analysis,
                    "changed_translation_units",
                    return_value=set(),
                ),
                mock.patch.object(static_analysis, "analyzer_command") as command,
            ):
                self.assertEqual(static_analysis.main(), 0)
                command.assert_not_called()


class ChangedBranchCoverageTest(unittest.TestCase):
    def test_workflow_installs_html_report_dependency_at_an_exact_version(self) -> None:
        self.assertIn("jinja2==3.1.6", COVERAGE_REQUIREMENTS)
        self.assertIn("markupsafe==3.0.3", COVERAGE_REQUIREMENTS)
        self.assertIn("--decisions", WORKFLOW_TEXT)

    def test_changed_lines_parse_added_ranges_and_single_lines(self) -> None:
        diff = """+++ b/Src/Foo.cpp
@@ -2,0 +3,2 @@
@@ -9 +11 @@
"""

        self.assertEqual(
            branch_coverage.changed_lines(diff),
            {"Src/Foo.cpp": {3, 4, 11}},
        )

    def test_changed_lines_ignore_deleted_files(self) -> None:
        diff = """+++ /dev/null
@@ -1,2 +0,0 @@
"""

        self.assertEqual(branch_coverage.changed_lines(diff), {})

    def test_branch_counts_include_only_source_decisions_on_changed_lines(self) -> None:
        report = {
            "files": [
                {
                    "file": "Src/Foo.cpp",
                    "lines": [
                        {
                            "line_number": 3,
                            "gcovr/decision": {
                                "type": "conditional",
                                "count_true": 1,
                                "count_false": 0,
                            },
                        },
                        {
                            "line_number": 8,
                            "gcovr/decision": {"type": "switch", "count": 0},
                        },
                    ],
                }
            ]
        }

        self.assertEqual(
            branch_coverage.branch_counts(report, {"Src/Foo.cpp": {3}}),
            (1, 2),
        )

    def test_uncheckable_source_decisions_do_not_inflate_the_denominator(self) -> None:
        report = {
            "files": [
                {
                    "file": "Src/Foo.cpp",
                    "lines": [
                        {
                            "line_number": 3,
                            "gcovr/decision": {"type": "uncheckable"},
                        }
                    ],
                }
            ]
        }

        self.assertEqual(
            branch_coverage.branch_counts(report, {"Src/Foo.cpp": {3}}),
            (0, 0),
        )

    def test_no_changed_branches_has_an_empty_denominator(self) -> None:
        covered, total = branch_coverage.branch_counts({"files": []}, {})

        self.assertEqual((covered, total), (0, 0))
        self.assertEqual(branch_coverage.coverage_percentage(covered, total), 100.0)
        self.assertTrue(branch_coverage.meets_threshold(covered, total, 80.0))

    def test_changed_source_missing_from_report_is_rejected(self) -> None:
        self.assertEqual(
            branch_coverage.missing_changed_sources(
                {"files": []}, {"Src/New.cpp": {1, 2, 3}}
            ),
            ["Src/New.cpp"],
        )

    def test_changed_header_missing_from_report_has_no_branch_denominator(self) -> None:
        self.assertEqual(
            branch_coverage.missing_changed_sources(
                {"files": []}, {"Src/NewHeader.h": {1, 2, 3}}
            ),
            [],
        )

    def test_changed_branch_percentage_exposes_below_threshold_result(self) -> None:
        self.assertEqual(branch_coverage.coverage_percentage(3, 4), 75.0)
        self.assertFalse(branch_coverage.meets_threshold(3, 4, 80.0))

    def test_changed_branch_percentage_accepts_exact_threshold(self) -> None:
        self.assertEqual(branch_coverage.coverage_percentage(4, 5), 80.0)
        self.assertTrue(branch_coverage.meets_threshold(4, 5, 80.0))

    @mock.patch.object(branch_coverage, "parse_arguments")
    def test_main_propagates_git_failure(self, parse_arguments: mock.Mock) -> None:
        parse_arguments.return_value = Namespace(
            report=Path("coverage.json"),
            base="missing-base",
            head="head",
            fail_under=80.0,
        )
        with mock.patch.object(
            branch_coverage.subprocess,
            "run",
            side_effect=subprocess.CalledProcessError(128, ["git", "diff"]),
        ):
            with self.assertRaises(subprocess.CalledProcessError):
                branch_coverage.main()

    def run_main_with_counts(self, counts: list[int], threshold: float = 80.0) -> int:
        with tempfile.TemporaryDirectory() as directory:
            report = Path(directory) / "coverage.json"
            report.write_text(
                json.dumps(
                    {
                        "files": [
                            {
                                "file": "Src/Foo.cpp",
                                "lines": [
                                    {
                                        "line_number": 3 + index,
                                        "gcovr/decision": {
                                            "type": "switch",
                                            "count": count,
                                        },
                                    }
                                    for index, count in enumerate(counts)
                                ],
                            }
                        ]
                    }
                ),
                encoding="utf-8",
            )
            arguments = SimpleNamespace(
                report=report,
                base="base",
                head="head",
                fail_under=threshold,
            )
            diff_result = mock.Mock(
                stdout=f"+++ b/Src/Foo.cpp\n@@ -2,0 +3,{len(counts)} @@\n"
            )
            with (
                mock.patch.object(branch_coverage, "parse_arguments", return_value=arguments),
                mock.patch.object(branch_coverage.subprocess, "run", return_value=diff_result),
            ):
                return branch_coverage.main()

    def test_main_rejects_branch_coverage_below_threshold(self) -> None:
        self.assertEqual(self.run_main_with_counts([1, 1, 1, 0]), 1)

    def test_main_accepts_branch_coverage_at_exact_threshold(self) -> None:
        self.assertEqual(self.run_main_with_counts([1, 1, 1, 1, 0]), 0)

    def test_main_rejects_changed_source_missing_from_report(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            report = Path(directory) / "coverage.json"
            report.write_text('{"files": []}', encoding="utf-8")
            arguments = SimpleNamespace(
                report=report, base="base", head="head", fail_under=80.0
            )
            diff_result = mock.Mock(
                stdout="+++ b/Src/New.cpp\n@@ -0,0 +1,2 @@\n"
            )
            with (
                mock.patch.object(
                    branch_coverage, "parse_arguments", return_value=arguments
                ),
                mock.patch.object(
                    branch_coverage.subprocess, "run", return_value=diff_result
                ),
            ):
                self.assertEqual(branch_coverage.main(), 2)


if __name__ == "__main__":
    unittest.main()
