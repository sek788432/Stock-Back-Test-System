import os
import subprocess
import sys
import unittest
from pathlib import Path


REPOSITORY_ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, os.fspath(REPOSITORY_ROOT / "Tools"))

from CheckProjectStandards import (  # noqa: E402
    AddedLine,
    audit_added_line,
    audit_clang_format_source,
    audit_git_tree,
    audit_module_include,
    audit_new_modules,
    audit_path_conventions,
    audit_text_sources,
    audit_test_registration,
    audit_suppression,
    find_clang_format,
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

        self.assertEqual(audit_added_line(AddedLine(Path("Tools/CheckProjectStandards.py"), 1, source)), [])

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

    def test_rejects_unambiguous_cpp_skill_bans(self) -> None:
        cases = {
            "enum Side { buy, sell };": "CPP009",
            "auto value = (int)price;": "CPP010",
            "goto cleanup;": "CPP011",
            "#define MAX(a, b) ((a) > (b) ? (a) : (b))": "CPP012",
            "double prices[16] {};": "CPP013",
            "const char* names[] {};": "CPP013",
            "using price_t = double; price_t prices[16] {};": "CPP013",
            "std::byte buffer[16] {};": "CPP013",
            "std::string names[4] {};": "CPP013",
            "std::filesystem::path paths[3] {};": "CPP013",
            "lowercase_type values[3] {};": "CPP013",
            "using Prices = double[16];": "CPP013",
            "decltype(value) values[3] {};": "CPP013",
            "int (*callbacks[3])();": "CPP013",
            "int (&values)[3] = source;": "CPP013",
            "std::auto_ptr<Foo> value;": "CPP014",
            "std::thread worker;": "CPP015",
            "pthread_create(&thread, nullptr, run, nullptr);": "CPP016",
            "volatile bool ready = false;": "CPP017",
            "std::vector<bool> flags;": "CPP018",
            "auto length = strlen(name);": "CPP006",
            "snprintf(buffer, size, \"%d\", value);": "CPP006",
        }

        for source, expected_rule in cases.items():
            with self.subTest(source=source):
                violations = audit_added_line(AddedLine(Path("Src/Foo.cpp"), 21, source))
                self.assertIn(expected_rule, [violation.rule for violation in violations])

    def test_accepts_modern_cpp_counterparts(self) -> None:
        sources = (
            "enum class Side { buy, sell };",
            "auto value = static_cast<int>(price);",
            "auto prices = std::array<double, 16> {};",
            "auto worker = std::jthread(run);",
            "std::atomic<bool> ready {false};",
            "std::vector<char> flags;",
            "return bars[index];",
            "using Element = decltype(values[0]);",
            "using Callback = decltype([] { return 42; });",
            "using Type = typename Foo<values[0]>::type;",
            "int value [[maybe_unused]] = 0;",
            "auto value [[maybe_unused]] = makeValue();",
        )

        for source in sources:
            with self.subTest(source=source):
                self.assertEqual(
                    audit_added_line(AddedLine(Path("Src/Foo.cpp"), 22, source)),
                    [],
                )

    def test_accepts_only_the_required_main_argv_array(self) -> None:
        main_signature = AddedLine(
            Path("Src/App/main.cpp"), 10, "int main(int argc, char *argv[]) {"
        )
        ordinary_array = AddedLine(
            Path("Src/App/main.cpp"), 11, "double prices[16] {};"
        )
        non_entrypoint = AddedLine(
            Path("Src/App/Runner.cpp"), 10, "int main(int argc, char *argv[]) {"
        )
        same_line_array = AddedLine(
            Path("Src/App/main.cpp"),
            10,
            "int main(int argc, char *argv[]) { double prices[16] {}; }",
        )

        self.assertEqual(audit_added_line(main_signature), [])
        self.assertEqual(
            [violation.rule for violation in audit_added_line(ordinary_array)],
            ["CPP013"],
        )
        self.assertEqual(
            [violation.rule for violation in audit_added_line(non_entrypoint)],
            ["CPP013"],
        )
        self.assertEqual(
            [violation.rule for violation in audit_added_line(same_line_array)],
            ["CPP013"],
        )

    def test_ignores_banned_tokens_in_cpp_comments_and_literals(self) -> None:
        sources = (
            '// enum Legacy { value };',
            'auto message = "do not use goto or std::thread";',
            'auto url = "https://example.test/NOLINT";',
            "/* volatile is not synchronization */",
        )

        for source in sources:
            with self.subTest(source=source):
                self.assertEqual(
                    audit_added_line(AddedLine(Path("Src/Foo.cpp"), 23, source)),
                    [],
                )

    def test_ignores_banned_tokens_in_multiline_raw_string(self) -> None:
        violations, _ = audit_text_sources(
            {
                Path("Src/Foo.cpp"): (
                    'auto text = R"style(first "quoted" line\n'
                    'std::thread and // NOLINT are documentation\n'
                    ')style";\n'
                )
            }
        )

        self.assertEqual(violations, [])

    def test_ignores_task_and_suppression_tokens_after_literal_boundaries(self) -> None:
        violations, _ = audit_text_sources(
            {
                Path("Src/Foo.cpp"): (
                    "/* opening\n"
                    '*/ auto url = "https://example.test/NOLINT";\n'
                    'auto text = "documentation\\\n'
                    'std::thread and // TODO are text";\n'
                )
            }
        )

        self.assertEqual(violations, [])

    def test_digit_separator_does_not_mask_later_cpp_tokens(self) -> None:
        violations = audit_added_line(
            AddedLine(
                Path("Src/Foo.cpp"),
                24,
                "auto limit = 1'000; std::thread worker;",
            )
        )

        self.assertEqual([violation.rule for violation in violations], ["CPP015"])


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


class AuditSuppressionTest(unittest.TestCase):
    def test_rejects_blank_broad_and_unreasoned_nolint(self) -> None:
        sources = (
            "// NOLINT",
            "// NOLINTNEXTLINE(*)",
            "// NOLINTNEXTLINE(performance-unnecessary-copy-initialization)",
            "// NOLINTBEGIN(performance-unnecessary-copy-initialization): too broad",
            "// NOLINTEND(performance-unnecessary-copy-initialization)",
        )

        for source in sources:
            with self.subTest(source=source):
                violations = audit_suppression(
                    AddedLine(Path("Src/Foo.cpp"), 7, source)
                )
                self.assertEqual(len(violations), 1)
                self.assertIn(violations[0].rule, {"SUP001", "SUP002"})

    def test_accepts_named_nolint_with_reason(self) -> None:
        violations = audit_suppression(
            AddedLine(
                Path("Src/Foo.cpp"),
                8,
                "// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): Qt callback signature",
            )
        )

        self.assertEqual(violations, [])

    def test_accepts_official_mixed_case_clang_analyzer_check(self) -> None:
        violations = audit_suppression(
            AddedLine(
                Path("Src/Foo.cpp"),
                9,
                "// NOLINTNEXTLINE(clang-analyzer-core.NullDereference): proven non-null",
            )
        )

        self.assertEqual(violations, [])


class AuditModuleIncludeTest(unittest.TestCase):
    def test_rejects_reverse_backend_dependency(self) -> None:
        violations = audit_module_include(
            AddedLine(
                Path("Src/Backend/Core/Private/Foo.cpp"),
                3,
                '#include "Bte/Engine/Backtest.h"',
            )
        )

        self.assertEqual([violation.rule for violation in violations], ["MOD003"])

    def test_rejects_frontend_dependency_on_engine(self) -> None:
        violations = audit_module_include(
            AddedLine(
                Path("Src/Frontend/Foo.cpp"),
                3,
                '#include "Bte/Engine/Backtest.h"',
            )
        )

        self.assertEqual([violation.rule for violation in violations], ["MOD003"])

    def test_accepts_dependency_in_declared_direction(self) -> None:
        violations = audit_module_include(
            AddedLine(
                Path("Src/Backend/Strategy/Private/Foo.cpp"),
                3,
                '#include "Bte/Indicators/StreamingIndicator.h"',
            )
        )

        self.assertEqual(violations, [])


class AuditClangFormatTest(unittest.TestCase):
    def test_git_tree_format_audit_requires_tracked_configuration(self) -> None:
        violations, _, _ = audit_git_tree("HEAD", set(), "clang-format-18")

        self.assertEqual([violation.rule for violation in violations], ["FMT003"])

    def test_resolves_only_clang_format_major_18(self) -> None:
        import CheckProjectStandards

        original_which = CheckProjectStandards.shutil.which
        original_run = subprocess.run
        CheckProjectStandards.shutil.which = lambda candidate: f"/bin/{candidate}"
        subprocess.run = lambda command, **kwargs: subprocess.CompletedProcess(
            command, 0, "clang-format version 18.1.8\n", ""
        )
        try:
            formatter = find_clang_format()
        finally:
            CheckProjectStandards.shutil.which = original_which
            subprocess.run = original_run

        self.assertEqual(formatter, "/bin/clang-format-18")

    def test_rejects_unpinned_clang_format_major(self) -> None:
        import CheckProjectStandards

        original_which = CheckProjectStandards.shutil.which
        original_run = subprocess.run
        CheckProjectStandards.shutil.which = lambda candidate: f"/bin/{candidate}"
        subprocess.run = lambda command, **kwargs: subprocess.CompletedProcess(
            command, 0, "clang-format version 19.1.0\n", ""
        )
        try:
            with self.assertRaisesRegex(ValueError, "clang-format 18"):
                find_clang_format()
        finally:
            CheckProjectStandards.shutil.which = original_which
            subprocess.run = original_run

    def test_accepts_source_when_formatter_succeeds(self) -> None:
        original_run = subprocess.run
        subprocess.run = lambda *args, **kwargs: subprocess.CompletedProcess(
            args[0], 0, "", ""
        )
        try:
            violations = audit_clang_format_source(
                Path("Src/Foo.cpp"), "int value = 0;\n", "clang-format-18"
            )
        finally:
            subprocess.run = original_run

        self.assertEqual(violations, [])

    def test_reports_formatter_line_when_source_needs_changes(self) -> None:
        original_run = subprocess.run
        subprocess.run = lambda *args, **kwargs: subprocess.CompletedProcess(
            args[0], 1, "", "Src/Foo.cpp:14:3: error: needs formatting"
        )
        try:
            violations = audit_clang_format_source(
                Path("Src/Foo.cpp"), "int value=0;\n", "clang-format-18"
            )
        finally:
            subprocess.run = original_run

        self.assertEqual(
            [(violation.rule, violation.line) for violation in violations],
            [("FMT002", 14)],
        )

    def test_rejects_project_header_without_pragma_once(self) -> None:
        violations, _ = audit_text_sources(
            {Path("Src/Backend/Core/Include/Bte/Core/Foo.h"): "class Foo {};\n"}
        )

        self.assertEqual([violation.rule for violation in violations], ["CPP019"])

    def test_accepts_project_header_with_pragma_once(self) -> None:
        violations, _ = audit_text_sources(
            {
                Path("Src/Backend/Core/Include/Bte/Core/Foo.h"): (
                    "#pragma once\n\nclass Foo {};\n"
                )
            }
        )

        self.assertEqual(violations, [])

    def test_commented_pragma_once_does_not_satisfy_header_rule(self) -> None:
        violations, _ = audit_text_sources(
            {
                Path("Src/Backend/Core/Include/Bte/Core/Foo.h"): (
                    "/*\n#pragma once\n*/\nclass Foo {};\n"
                )
            }
        )

        self.assertEqual([violation.rule for violation in violations], ["CPP019"])

    def test_ignores_banned_tokens_across_multiline_cpp_comment(self) -> None:
        violations, _ = audit_text_sources(
            {
                Path("Src/Foo.cpp"): (
                    "/*\nenum Legacy { value };\nstd::thread worker;\n*/\n"
                )
            }
        )

        self.assertEqual(violations, [])


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
                Path("Tests/Unit/Launcher/UnitTest_Launcher.cpp"),
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
                Path("Tests/Unit/Launcher/UnitTest_Launcher.cpp"),
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
            {Path("Tests/Unit/Tools/UnitTest_Checker.py")},
            {Path("Tests/CMakeLists.txt"): "-p UnitTest_*.py"},
        )

        self.assertEqual(violations, [])


class AuditPathConventionsTest(unittest.TestCase):
    def test_rejects_non_pascal_case_project_paths(self) -> None:
        violations = audit_path_conventions(
            {
                Path("Docs/bad folder/GoodFile.md"),
                Path("Docs/GoodFolder/bad_file.md"),
            }
        )

        self.assertEqual([violation.rule for violation in violations], ["PATH001", "PATH001"])

    def test_accepts_pascal_case_and_conventional_paths(self) -> None:
        violations = audit_path_conventions(
            {
                Path("AGENTS.md"),
                Path("Docs/Specs/CiDevFlow.md"),
                Path("StockData/Extracted/BRK_B.csv"),
                Path("Tests/Unit/Core/UnitTest_Bar.cpp"),
            }
        )

        self.assertEqual(violations, [])

    def test_rejects_numbered_kebab_case_decision_archive_name(self) -> None:
        violations = audit_path_conventions(
            {Path("Docs/Decisions/0010-old-decision.md")}
        )

        self.assertEqual([violation.rule for violation in violations], ["PATH001"])

    def test_rejects_unit_test_outside_unit_tree(self) -> None:
        violations = audit_path_conventions(
            {Path("Tests/Frontend/UnitTest_ReplayTab.cpp")}
        )

        self.assertEqual([violation.rule for violation in violations], ["TEST005"])

if __name__ == "__main__":
    unittest.main()
