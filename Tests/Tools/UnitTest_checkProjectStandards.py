import os
import subprocess
import sys
import unittest
from pathlib import Path


REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, os.fspath(REPOSITORY_ROOT / "tools"))

from checkProjectStandards import (  # noqa: E402
    AddedLine,
    audit_added_line,
    audit_git_tree,
    audit_new_modules,
    audit_text_sources,
    audit_test_registration,
    parse_unified_diff,
)


class ParseUnifiedDiffTest(unittest.TestCase):
    def test_returns_added_lines_with_new_file_line_numbers(self) -> None:
        diff = """diff --git a/Foo.cpp b/Foo.cpp
--- a/Foo.cpp
+++ b/Foo.cpp
@@ -2,0 +3,2 @@
+first
+second
"""

        self.assertEqual(
            parse_unified_diff(diff),
            [AddedLine(Path("Foo.cpp"), 3, "first"), AddedLine(Path("Foo.cpp"), 4, "second")],
        )


class AuditAddedLineTest(unittest.TestCase):
    def test_rejects_raw_allocation_in_cpp(self) -> None:
        violations = audit_added_line(AddedLine(Path("Src/Foo.cpp"), 7, "auto value = new Foo;"))

        self.assertEqual([violation.rule for violation in violations], ["CPP002"])

    def test_rejects_tautological_gtest_assertion(self) -> None:
        violations = audit_added_line(
            AddedLine(Path("Tests/Unit/UnitTest_Foo.cpp"), 9, "EXPECT_EQ(foo.value(), foo.value());")
        )

        self.assertEqual([violation.rule for violation in violations], ["TEST002"])

    def test_accepts_compliant_cpp(self) -> None:
        violations = audit_added_line(
            AddedLine(Path("Src/Foo.cpp"), 11, "auto value = std::make_unique<Foo>();")
        )

        self.assertEqual(violations, [])

    def test_accepts_deleted_copy_constructor(self) -> None:
        violations = audit_added_line(AddedLine(Path("Src/Foo.h"), 12, "Foo(const Foo&) = delete;"))

        self.assertEqual(violations, [])

    def test_checker_rule_definition_does_not_match_itself(self) -> None:
        source = 'UNJUSTIFIED_TASK = re.compile("TO" + "DO")'

        self.assertEqual(audit_added_line(AddedLine(Path("tools/checkProjectStandards.py"), 1, source)), [])

    def test_rejects_untracked_task_in_cpp_comment(self) -> None:
        violations = audit_added_line(
            AddedLine(Path("Src/Foo.cpp"), 14, "return value; // TODO: validate")
        )

        self.assertEqual([violation.rule for violation in violations], ["DOC001"])

    def test_accepts_task_comment_linked_to_issue(self) -> None:
        violations = audit_added_line(
            AddedLine(Path("Src/Foo.cpp"), 15, "// TODO ISSUE-123: validate")
        )

        self.assertEqual(violations, [])

    def test_accepts_policy_document_that_mentions_task_markers(self) -> None:
        violations = audit_added_line(
            AddedLine(Path("Docs/Policy.md"), 4, "Every TODO or FIXME needs an issue.")
        )

        self.assertEqual(violations, [])


class AuditTextSourcesTest(unittest.TestCase):
    def test_audits_every_line_in_every_text_source(self) -> None:
        violations, line_count = audit_text_sources(
            {
                Path("Docs/Clean.md"): "first\nsecond\n",
                Path("Src/Foo.cpp"): "auto value = std::make_unique<Foo>();\nauto bad = new Foo;\n",
            }
        )

        self.assertEqual(line_count, 4)
        self.assertEqual([violation.rule for violation in violations], ["CPP002"])

    def test_git_tree_audit_skips_non_utf8_files_and_counts_text_files(self) -> None:
        original_run = subprocess.run

        def fake_run(command: list[str], **_: object) -> subprocess.CompletedProcess[bytes]:
            path = command[-1].split(":", maxsplit=1)[1]
            content = b"clean\n" if path == "clean.txt" else b"\xff\xfe"
            return subprocess.CompletedProcess(command, 0, content, b"")

        subprocess.run = fake_run
        try:
            violations, line_count, text_file_count = audit_git_tree(
                "HEAD", {Path("clean.txt"), Path("binary.dat")}
            )
        finally:
            subprocess.run = original_run

        self.assertEqual(violations, [])
        self.assertEqual(line_count, 1)
        self.assertEqual(text_file_count, 1)


class AuditNewModulesTest(unittest.TestCase):
    def test_rejects_new_backend_module_without_unit_tests(self) -> None:
        violations = audit_new_modules(
            base_paths={Path("Src/Backend/Core/Private/Bar.cpp")},
            head_paths={
                Path("Src/Backend/Core/Private/Bar.cpp"),
                Path("Src/Backend/Indicators/Private/Sma.cpp"),
            },
            cmake_sources={},
        )

        self.assertEqual([violation.rule for violation in violations], ["MOD001"])

    def test_rejects_new_module_test_that_cmake_does_not_register(self) -> None:
        violations = audit_new_modules(
            base_paths=set(),
            head_paths={
                Path("Src/Launcher/Main.cpp"),
                Path("Tests/Launcher/UnitTest_Launcher.cpp"),
                Path("Tests/CMakeLists.txt"),
            },
            cmake_sources={Path("Tests/CMakeLists.txt"): "add_test(NAME unrelated)"},
        )

        self.assertEqual([violation.rule for violation in violations], ["MOD002"])

    def test_accepts_new_module_with_registered_unit_tests(self) -> None:
        violations = audit_new_modules(
            base_paths=set(),
            head_paths={
                Path("Src/Launcher/Main.cpp"),
                Path("Tests/Launcher/UnitTest_Launcher.cpp"),
                Path("Tests/CMakeLists.txt"),
            },
            cmake_sources={Path("Tests/CMakeLists.txt"): "UnitTest_Launcher.cpp"},
        )

        self.assertEqual(violations, [])


class AuditTestRegistrationTest(unittest.TestCase):
    def test_rejects_unregistered_cpp_test(self) -> None:
        violations = audit_test_registration(
            {Path("Tests/Integration/UnitTest_Database.cpp")},
            {Path("Tests/CMakeLists.txt"): "add_test(NAME unrelated)"},
        )

        self.assertEqual([violation.rule for violation in violations], ["TEST004"])

    def test_accepts_cpp_test_registered_by_cmake(self) -> None:
        violations = audit_test_registration(
            {Path("Tests/Integration/UnitTest_Database.cpp")},
            {Path("Tests/CMakeLists.txt"): "UnitTest_Database.cpp"},
        )

        self.assertEqual(violations, [])

    def test_accepts_python_test_registered_by_discovery_pattern(self) -> None:
        violations = audit_test_registration(
            {Path("Tests/Tools/UnitTest_Checker.py")},
            {Path("Tests/CMakeLists.txt"): "-p UnitTest_*.py"},
        )

        self.assertEqual(violations, [])


if __name__ == "__main__":
    unittest.main()
