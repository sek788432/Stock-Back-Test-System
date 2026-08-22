#!/usr/bin/env python3
"""Reject repository content that violates the project's hard coding rules."""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path


CPP_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx"}
CPP_HEADER_SUFFIXES = {".h", ".hh", ".hpp", ".hxx"}
TEST_PATH_PARTS = {"Test", "Tests", "test", "tests"}
HASH_COMMENT_SUFFIXES = {
    ".cmake",
    ".py",
    ".sh",
    ".toml",
    ".yaml",
    ".yml",
}
CONVENTIONAL_FILE_NAMES = {
    "AGENTS.md",
    "BUILD.md",
    "CHANGELOG.md",
    "CLAUDE.md",
    "CMakeLists.txt",
    "CMakePresets.json",
    "CONTRIBUTING.md",
    "LICENSE",
    "README.md",
    "SKILL.md",
    "main.cpp",
    "requirements.txt",
}
PASCAL_CASE_NAME = re.compile(r"[A-Z][A-Za-z0-9]*")
NUMBERED_PASCAL_CASE_NAME = re.compile(r"\d{2}[A-Z][A-Za-z0-9]*")
ADR_FILE_NAME = re.compile(r"\d{4}-[a-z0-9]+(?:-[a-z0-9]+)*\.md")
UNIT_TEST_FILE_NAME = re.compile(r"UnitTest_[A-Z][A-Za-z0-9]*\.(?:cpp|py)")


@dataclass(frozen=True)
class AddedLine:
    path: Path
    number: int
    text: str


@dataclass(frozen=True)
class Violation:
    path: Path
    line: int
    rule: str
    message: str


@dataclass(frozen=True)
class CppMaskState:
    in_block_comment: bool = False
    raw_delimiter: str | None = None
    continued_quote: str | None = None


CPP_RULES = (
    ("CPP001", re.compile(r"\busing\s+namespace\s+std\s*;"), "using namespace std is forbidden"),
    (
        "CPP002",
        re.compile(r"(?:\bnew\s+[A-Za-z_:]|\bdelete(?:\s*\[\s*\])?\s+[A-Za-z_(])"),
        "raw new/delete is forbidden; use RAII",
    ),
    ("CPP003", re.compile(r"\b(?:malloc|free)\s*\("), "malloc/free is forbidden; use RAII"),
    ("CPP004", re.compile(r"\bNULL\b"), "NULL is forbidden; use nullptr"),
    ("CPP005", re.compile(r"\btypedef\b"), "typedef is forbidden; use a using alias"),
    (
        "CPP006",
        re.compile(r"\b(?:printf|fprintf|sprintf|snprintf|strcpy|strcat|strlen)\s*\("),
        "C-style string or output functions are forbidden",
    ),
    ("CPP007", re.compile(r"\.\s*(?:lock|unlock)\s*\("), "manual lock/unlock is forbidden; use RAII locks"),
    ("CPP008", re.compile(r"\.\s*detach\s*\("), "detached threads are forbidden; use std::jthread"),
    (
        "CPP009",
        re.compile(r"\benum\s+(?!class\b|struct\b)"),
        "unscoped enums are forbidden; use enum class",
    ),
    (
        "CPP010",
        re.compile(
            r"\(\s*(?:bool|char|short|int|long|float|double|signed|unsigned|size_t|"
            r"std::(?:size_t|u?int(?:8|16|32|64)_t))\s*(?:\*|&)?\s*\)\s*[A-Za-z_(]"
        ),
        "C-style casts are forbidden; use a named C++ cast",
    ),
    ("CPP011", re.compile(r"\bgoto\b"), "goto is forbidden; use structured control flow"),
    (
        "CPP012",
        re.compile(r"^\s*#\s*define\s+[A-Za-z_][A-Za-z0-9_]*\s*\("),
        "function-like macros are forbidden; use a constexpr function",
    ),
    (
        "CPP013",
        re.compile(
            r"(?:"
            r"\busing\s+[A-Za-z_][A-Za-z0-9_]*\s*=\s*[^;=\[\]\n]+"
            r"(?:\[(?!\[)[^\]]*\])+\s*;"
            r"|\b(?:[A-Za-z_][A-Za-z0-9_:]*(?:\s*<[^;\[\]{}()]*>)?"
            r"|decltype\s*\([^)]*\))\s*\(\s*[*&]\s*"
            r"[A-Za-z_][A-Za-z0-9_]*\s*\[(?!\[)[^\]]*\]\s*\)"
            r"|\b(?:[A-Za-z_][A-Za-z0-9_:]*(?:\s*<[^;\[\]{}()]*>)?"
            r"|decltype\s*\([^)]*\))\s*\(\s*[*&]\s*"
            r"[A-Za-z_][A-Za-z0-9_]*\s*\)\s*\[(?!\[)[^\]]*\]"
            r"|\b(?!(?:return|co_return|throw|case|delete|sizeof|alignof)\b)"
            r"(?:(?:const|volatile|signed|unsigned|short|long)\s+)*"
            r"(?:[A-Za-z_][A-Za-z0-9_:]*(?:\s*<[^;\[\]{}()]*>)?"
            r"|decltype\s*\([^)]*\))"
            r"(?:\s+(?:const\s+)?(?:[*&]\s*)?|\s*[*&]\s*)"
            r"[A-Za-z_][A-Za-z0-9_]*\s*\[(?!\[)[^\]]*\])"
        ),
        "C arrays are forbidden; use std::array, std::vector, or std::span",
    ),
    (
        "CPP014",
        re.compile(r"\b(?:std::)?auto_ptr\b"),
        "auto_ptr is forbidden; use std::unique_ptr",
    ),
    ("CPP015", re.compile(r"\bstd::thread\b"), "std::thread is forbidden; use std::jthread"),
    (
        "CPP016",
        re.compile(r"\bpthread_create\s*\("),
        "pthread_create is forbidden; use std::jthread",
    ),
    (
        "CPP017",
        re.compile(r"\bvolatile\b"),
        "volatile is forbidden for project synchronization; use std::atomic",
    ),
    (
        "CPP018",
        re.compile(r"\bstd::vector\s*<\s*bool\b"),
        "std::vector<bool> is forbidden; use an explicit value container",
    ),
)

TRIVIAL_BOOL_ASSERTION = re.compile(r"\b(?:EXPECT|ASSERT)_(?:TRUE|FALSE)\s*\(\s*(?:true|false)\s*\)")
EQUALITY_ASSERTION = re.compile(r"\b(?:EXPECT|ASSERT)_(?:EQ|NE)\s*\(")
PYTHON_TRIVIAL_ASSERTION = re.compile(r"^\s*assert\s+(?:True|False)\s*(?:#.*)?$")
UNJUSTIFIED_TASK = re.compile(
    r"\b(?:" + "|".join(("TO" + "DO", "FIX" + "ME")) + r")\b(?![^\n]*\bISSUE-\d+\b)"
)
NOLINT_SUPPRESSION = re.compile(
    r"\bNOLINT(?:NEXTLINE|BEGIN)?(?P<checks>\([^)]*\))?(?P<reason>\s*:\s*\S.*)?$"
)
BTE_INCLUDE = re.compile(r'^\s*#\s*include\s*[<"]Bte/([^/">]+)')
MODULE_DEPENDENCIES = {
    "Core": {"Core"},
    "Data": {"Core", "Data"},
    "Indicators": {"Core", "Indicators"},
    "Strategy": {"Core", "Indicators", "Strategy"},
    "Engine": {"Core", "Data", "Engine", "Indicators", "Metrics", "Results", "Strategy"},
    "Bindings": {"Bindings", "Core", "Data", "Engine", "Indicators", "Results", "Strategy"},
    # Existing Qt-facing value types still expose Core, Indicators, and Strategy.
    # Engine and Data remain behind Bindings until that public surface is narrowed.
    "Frontend": {"Bindings", "Core", "Frontend", "Indicators", "Strategy"},
    "App": {"App", "Frontend"},
}
MAIN_ABI_SIGNATURE = re.compile(
    r"\bmain\s*\(\s*int\s+[A-Za-z_][A-Za-z0-9_]*\s*,\s*"
    r"char\s*\*\s*[A-Za-z_][A-Za-z0-9_]*\s*\[\s*\]\s*\)"
)
MAIN_ARGV_DECLARATOR = re.compile(
    r"\bchar\s*\*\s*[A-Za-z_][A-Za-z0-9_]*\s*\[\s*\]"
)
RAW_STRING_OPENER = re.compile(
    r'(?:u8|u|U|L)?R"(?P<delimiter>[^ ()\\\t\r\n]{0,16})\('
)


def mask_cpp_line(
    text: str, state: CppMaskState = CppMaskState()
) -> tuple[str, CppMaskState]:
    """Mask C++ comments and literals while preserving token positions."""
    masked: list[str] = []
    index = 0
    quote = state.continued_quote
    in_block_comment = state.in_block_comment
    raw_delimiter = state.raw_delimiter

    while index < len(text):
        if raw_delimiter is not None:
            terminator = f'){raw_delimiter}"'
            terminator_index = text.find(terminator, index)
            if terminator_index < 0:
                masked.extend(" " * (len(text) - index))
                index = len(text)
                continue
            terminator_end = terminator_index + len(terminator)
            masked.extend(" " * (terminator_end - index))
            index = terminator_end
            raw_delimiter = None
            continue

        if in_block_comment:
            if text.startswith("*/", index):
                masked.extend("  ")
                index += 2
                in_block_comment = False
            else:
                masked.append(" ")
                index += 1
            continue

        if quote is not None:
            character = text[index]
            masked.append(" ")
            if character == "\\" and index + 1 < len(text):
                masked.append(" ")
                index += 2
                continue
            if character == quote:
                quote = None
            index += 1
            continue

        if text.startswith("//", index):
            masked.extend(" " * (len(text) - index))
            break
        if text.startswith("/*", index):
            masked.extend("  ")
            index += 2
            in_block_comment = True
            continue
        raw_match = RAW_STRING_OPENER.match(text, index)
        if raw_match is not None:
            raw_delimiter = raw_match.group("delimiter")
            masked.extend(" " * (raw_match.end() - index))
            index = raw_match.end()
            continue
        if (
            text[index] == "'"
            and index > 0
            and index + 1 < len(text)
            and text[index - 1].isalnum()
            and text[index + 1].isalnum()
        ):
            masked.append(text[index])
            index += 1
            continue
        if text[index] in {'"', "'"}:
            quote = text[index]
            masked.append(" ")
            index += 1
            continue

        masked.append(text[index])
        index += 1

    trailing_backslashes = len(text) - len(text.rstrip("\\"))
    continued_quote = quote if quote is not None and trailing_backslashes % 2 else None
    return "".join(masked), CppMaskState(
        in_block_comment, raw_delimiter, continued_quote
    )


def has_pragma_once(source: str) -> bool:
    """Return whether source contains an active #pragma once directive."""
    state = CppMaskState()
    for line in source.splitlines():
        code, state = mask_cpp_line(line, state)
        if re.fullmatch(r"\s*#\s*pragma\s+once\s*", code):
            return True
    return False


def cpp_comment_text(text: str, state: CppMaskState = CppMaskState()) -> str:
    """Return the first actual C++ comment, ignoring comment-like literals."""
    comments: list[str] = []
    quote = state.continued_quote
    raw_delimiter = state.raw_delimiter
    in_block_comment = state.in_block_comment
    index = 0
    while index < len(text):
        if in_block_comment:
            terminator_index = text.find("*/", index)
            if terminator_index < 0:
                comments.append(text[index:])
                break
            comments.append(text[index : terminator_index + 2])
            index = terminator_index + 2
            in_block_comment = False
            continue
        if raw_delimiter is not None:
            terminator = f'){raw_delimiter}"'
            terminator_index = text.find(terminator, index)
            if terminator_index < 0:
                return ""
            index = terminator_index + len(terminator)
            raw_delimiter = None
            continue
        character = text[index]
        if quote is not None:
            if character == "\\" and index + 1 < len(text):
                index += 2
                continue
            if character == quote:
                quote = None
            index += 1
            continue
        if text.startswith("//", index) or text.startswith("/*", index):
            if text.startswith("//", index):
                comments.append(text[index:])
                break
            in_block_comment = True
            continue
        raw_match = RAW_STRING_OPENER.match(text, index)
        if raw_match is not None:
            raw_delimiter = raw_match.group("delimiter")
            index = raw_match.end()
            continue
        if character in {'"', "'"}:
            quote = character
        index += 1
    return "\n".join(comments)


def comment_text(path: Path, text: str) -> str:
    """Return the comment portion for file types governed by DOC001."""
    suffix = path.suffix.lower()
    if suffix in CPP_SUFFIXES:
        return cpp_comment_text(text)

    if suffix in HASH_COMMENT_SUFFIXES or path.name == "CMakeLists.txt":
        marker = text.find("#")
        return text[marker:] if marker >= 0 else ""

    return ""


def source_module(path: Path) -> str | None:
    """Return the project module that owns a source path."""
    parts = path.parts
    if len(parts) >= 3 and parts[:2] == ("Src", "Backend"):
        return parts[2]
    if len(parts) >= 2 and parts[0] == "Src":
        return parts[1]
    return None


def audit_suppression(
    added: AddedLine, cpp_comment: str | None = None
) -> list[Violation]:
    """Require narrow, named, and reasoned clang-tidy suppressions."""
    comment = (
        cpp_comment
        if cpp_comment is not None
        else comment_text(added.path, added.text)
    )
    if "NOLINT" not in comment:
        return []

    if "NOLINTBEGIN" in comment or "NOLINTEND" in comment:
        return [
            Violation(
                added.path,
                added.number,
                "SUP002",
                "range NOLINT suppressions are forbidden; suppress one line",
            )
        ]

    match = NOLINT_SUPPRESSION.search(comment.strip())
    if match is None:
        return [
            Violation(
                added.path,
                added.number,
                "SUP001",
                "NOLINT suppression must name exact checks and end with ': reason'",
            )
        ]

    checks = match.group("checks")
    reason = match.group("reason")
    valid_checks = bool(
        checks
        and re.fullmatch(
            r"\([A-Za-z0-9.-]+(?:\s*,\s*[A-Za-z0-9.-]+)*\)", checks
        )
    )
    if not valid_checks or reason is None:
        return [
            Violation(
                added.path,
                added.number,
                "SUP001",
                "NOLINT suppression must name exact checks and end with ': reason'",
            )
        ]
    return []


def audit_module_include(added: AddedLine) -> list[Violation]:
    """Reject includes that point against the implemented module graph."""
    owner = source_module(added.path)
    match = BTE_INCLUDE.match(added.text)
    if owner is None or match is None:
        return []

    dependency = match.group(1)
    allowed = MODULE_DEPENDENCIES.get(owner)
    if allowed is None or dependency in allowed:
        return []
    return [
        Violation(
            added.path,
            added.number,
            "MOD003",
            f"{owner} may not include the higher-level {dependency} module",
        )
    ]


def cpp_rule_text(added: AddedLine, rule: str, code: str) -> str:
    """Remove only the explicitly governed ABI declarator from a rule input."""
    if (
        rule == "CPP013"
        and added.path.name == "main.cpp"
        and MAIN_ABI_SIGNATURE.search(code) is not None
    ):
        return MAIN_ARGV_DECLARATOR.sub("char **argv", code, count=1)
    return code


def parse_unified_diff(diff_text: str) -> list[AddedLine]:
    added_lines: list[AddedLine] = []
    current_path: Path | None = None
    new_line_number = 0

    for raw_line in diff_text.splitlines():
        if raw_line.startswith("+++ b/"):
            current_path = Path(raw_line[6:])
            continue

        if raw_line.startswith("@@"):
            match = re.search(r"\+(\d+)", raw_line)
            if match is None:
                raise ValueError(f"Malformed unified-diff hunk: {raw_line}")
            new_line_number = int(match.group(1))
            continue

        if current_path is None or raw_line.startswith("---"):
            continue

        if raw_line.startswith("+"):
            added_lines.append(AddedLine(current_path, new_line_number, raw_line[1:]))
            new_line_number += 1
        elif raw_line.startswith(" "):
            new_line_number += 1

    return added_lines


def equality_assertion_arguments(code: str) -> tuple[str, str] | None:
    match = EQUALITY_ASSERTION.search(code)
    if match is None:
        return None

    argument_start = match.end()
    nesting = 0
    comma: int | None = None
    for index in range(argument_start, len(code)):
        character = code[index]
        if character in "([{":
            nesting += 1
        elif character in ")]}":
            if nesting == 0:
                if comma is None:
                    return None
                return code[argument_start:comma].strip(), code[comma + 1 : index].strip()
            nesting -= 1
        elif character == "," and nesting == 0 and comma is None:
            comma = index

    return None


def audit_added_line(
    added: AddedLine,
    cpp_code: str | None = None,
    cpp_comment: str | None = None,
) -> list[Violation]:
    violations: list[Violation] = []
    text = added.text
    suffix = added.path.suffix.lower()

    if text.rstrip() != text:
        violations.append(Violation(added.path, added.number, "FMT001", "trailing whitespace is forbidden"))

    task_comment = (
        cpp_comment
        if suffix in CPP_SUFFIXES and cpp_comment is not None
        else comment_text(added.path, text)
    )
    if UNJUSTIFIED_TASK.search(task_comment):
        violations.append(
            Violation(
                added.path,
                added.number,
                "DOC001",
                "TO" "DO/FIX" "ME comments must reference ISSUE-NNN",
            )
        )

    if suffix in CPP_SUFFIXES:
        violations.extend(audit_suppression(added, cpp_comment))
        violations.extend(audit_module_include(added))
        code = cpp_code if cpp_code is not None else mask_cpp_line(text)[0]
        for rule, pattern, message in CPP_RULES:
            if pattern.search(cpp_rule_text(added, rule, code)):
                violations.append(Violation(added.path, added.number, rule, message))

        if TEST_PATH_PARTS.intersection(added.path.parts):
            if TRIVIAL_BOOL_ASSERTION.search(code):
                violations.append(
                    Violation(added.path, added.number, "TEST001", "trivially true/false assertions are forbidden")
                )
            equality = equality_assertion_arguments(code)
            if equality is not None and equality[0] == equality[1]:
                violations.append(
                    Violation(added.path, added.number, "TEST002", "tautological assertions are forbidden")
                )

    if suffix == ".py" and TEST_PATH_PARTS.intersection(added.path.parts):
        if PYTHON_TRIVIAL_ASSERTION.search(text):
            violations.append(
                Violation(added.path, added.number, "TEST003", "literal Python assertions are forbidden")
            )

    return violations


def audit_added_lines(lines: list[AddedLine]) -> list[Violation]:
    return [violation for added in lines for violation in audit_added_line(added)]


def audit_text_sources(sources: dict[Path, str]) -> tuple[list[Violation], int]:
    violations: list[Violation] = []
    line_count = 0
    for path, source in sources.items():
        suffix = path.suffix.lower()
        if (
            suffix in CPP_HEADER_SUFFIXES
            and path.is_relative_to(Path("Src"))
            and not has_pragma_once(source)
        ):
            violations.append(
                Violation(path, 1, "CPP019", "project-owned C++ headers require #pragma once")
            )
        needs_content_rules = (
            suffix in CPP_SUFFIXES
            or suffix in HASH_COMMENT_SUFFIXES
            or path.name == "CMakeLists.txt"
            or (suffix == ".py" and TEST_PATH_PARTS.intersection(path.parts))
        )
        cpp_mask_state = CppMaskState()
        for number, line in enumerate(source.splitlines(), start=1):
            line_count += 1
            if needs_content_rules:
                cpp_code: str | None = None
                cpp_comment: str | None = None
                if suffix in CPP_SUFFIXES:
                    cpp_comment = cpp_comment_text(line, cpp_mask_state)
                    cpp_code, cpp_mask_state = mask_cpp_line(
                        line, cpp_mask_state
                    )
                violations.extend(
                    audit_added_line(
                        AddedLine(path, number, line), cpp_code, cpp_comment
                    )
                )
            elif line.rstrip() != line:
                violations.append(
                    Violation(path, number, "FMT001", "trailing whitespace is forbidden")
                )
    return violations, line_count


def is_domain_data_file(path: Path) -> bool:
    data_roots = (
        Path("StockData", "Extracted"),
        Path("Tests", "Fixtures"),
    )
    return any(path.is_relative_to(root) for root in data_roots)


def is_conventional_file(path: Path) -> bool:
    if path.name.startswith(".") or path.name in CONVENTIONAL_FILE_NAMES:
        return True
    if path.parts[0] in {".agents", ".github"}:
        return True
    if path.parent == Path("Docs", "Decisions") and ADR_FILE_NAME.fullmatch(path.name):
        return True
    if UNIT_TEST_FILE_NAME.fullmatch(path.name):
        return True
    if is_domain_data_file(path):
        return True
    return False


def audit_path_conventions(paths: set[Path]) -> list[Violation]:
    violations: list[Violation] = []
    reported_directories: set[Path] = set()

    for path in sorted(paths):
        if path.parts[0] in {".agents", ".github"}:
            continue

        for parent in path.parents:
            if parent == Path("."):
                continue
            if parent in reported_directories:
                continue
            if not PASCAL_CASE_NAME.fullmatch(parent.name):
                violations.append(
                    Violation(parent, 1, "PATH001", "project directory names must use PascalCase")
                )
                reported_directories.add(parent)

        if path.parts[0] == "Tests" and path.stem.startswith("UnitTest_"):
            if len(path.parts) < 3 or path.parts[1] != "Unit":
                violations.append(
                    Violation(path, 1, "TEST005", "unit tests must be under Tests/Unit/<Module>")
                )
                continue

        if is_conventional_file(path):
            continue

        suffix_is_lowercase = path.suffix == path.suffix.lower()
        stem_is_pascal_case = bool(
            PASCAL_CASE_NAME.fullmatch(path.stem)
            or NUMBERED_PASCAL_CASE_NAME.fullmatch(path.stem)
        )
        if not suffix_is_lowercase or not stem_is_pascal_case:
            violations.append(
                Violation(path, 1, "PATH001", "project file stems must use PascalCase")
            )

    return violations


def module_and_test_roots(path: Path) -> tuple[Path, Path] | None:
    parts = path.parts
    if len(parts) >= 3 and parts[0:2] == ("Src", "Backend"):
        return Path(*parts[:3]), Path("Tests", "Unit", parts[2])
    if len(parts) >= 2 and parts[0] == "Src":
        return Path(*parts[:2]), Path("Tests", "Unit", parts[1])
    return None


def audit_new_modules(
    base_paths: set[Path], head_paths: set[Path], cmake_sources: dict[Path, str]
) -> list[Violation]:
    head_modules = {
        roots
        for path in head_paths
        if (roots := module_and_test_roots(path)) is not None
    }
    base_module_roots = {
        roots[0]
        for path in base_paths
        if (roots := module_and_test_roots(path)) is not None
    }

    violations: list[Violation] = []
    cmake_text = "\n".join(cmake_sources.values())
    for module_root, test_root in sorted(head_modules):
        if module_root in base_module_roots:
            continue

        unit_tests = sorted(
            path
            for path in head_paths
            if path.is_relative_to(test_root)
            and path.stem.startswith("UnitTest_")
            and path.suffix.lower() in {".cpp", ".py"}
        )
        if not unit_tests:
            violations.append(
                Violation(
                    module_root,
                    1,
                    "MOD001",
                    f"new module requires unit tests under {test_root}",
                )
            )
            continue

        unregistered = [test for test in unit_tests if test.name not in cmake_text]
        if unregistered:
            names = ", ".join(test.name for test in unregistered)
            violations.append(
                Violation(
                    module_root,
                    1,
                    "MOD002",
                    f"new module unit tests are not registered by CMake: {names}",
                )
            )

    return violations


def audit_test_registration(head_paths: set[Path], cmake_sources: dict[Path, str]) -> list[Violation]:
    cmake_text = "\n".join(cmake_sources.values())
    violations: list[Violation] = []
    for path in sorted(head_paths):
        if not path.is_relative_to(Path("Tests")) or not path.stem.startswith("UnitTest_"):
            continue
        if path.suffix.lower() == ".cpp":
            registered = path.name in cmake_text
        elif path.suffix.lower() == ".py":
            registered = path.name in cmake_text or "UnitTest_*.py" in cmake_text
        else:
            continue
        if not registered:
            violations.append(
                Violation(path, 1, "TEST004", "test file is not registered by CMake/CTest")
            )
    return violations


def git_diff(base: str, head: str | None) -> str:
    command = ["git", "diff", "--unified=0", "--no-ext-diff", base]
    if head is not None:
        command.append(head)
    command.append("--")
    result = subprocess.run(command, check=True, capture_output=True, text=True)
    return result.stdout


def git_tree_paths(revision: str) -> set[Path]:
    result = subprocess.run(
        ["git", "ls-tree", "-r", "--name-only", revision],
        check=True,
        capture_output=True,
        text=True,
    )
    return {Path(line) for line in result.stdout.splitlines() if line}


def git_cmake_sources(revision: str, paths: set[Path]) -> dict[Path, str]:
    sources: dict[Path, str] = {}
    for path in sorted(path for path in paths if path.name == "CMakeLists.txt"):
        result = subprocess.run(
            ["git", "show", f"{revision}:{path.as_posix()}"],
            check=True,
            capture_output=True,
            text=True,
        )
        sources[path] = result.stdout
    return sources


def audit_clang_format_source(
    path: Path,
    source: str,
    formatter: str,
    style_file: Path | None = None,
) -> list[Violation]:
    """Return a violation when clang-format would change a C++ source."""
    result = subprocess.run(
        [
            formatter,
            "--dry-run",
            "--Werror",
            f"--style=file:{style_file}" if style_file is not None else "--style=file",
            f"--assume-filename={path.as_posix()}",
        ],
        input=source,
        capture_output=True,
        text=True,
        check=False,
    )
    if result.returncode == 0:
        return []
    line_match = re.search(r":(\d+):\d+: (?:error|warning):", result.stderr)
    line = int(line_match.group(1)) if line_match is not None else 1
    return [
        Violation(path, line, "FMT002", "file does not match the repository clang-format style")
    ]


def find_clang_format() -> str:
    """Resolve the repository's pinned clang-format major version."""
    for candidate in ("clang-format-18", "clang-format"):
        resolved = shutil.which(candidate)
        if resolved is None:
            continue
        version = subprocess.run(
            [resolved, "--version"],
            capture_output=True,
            text=True,
            check=False,
        )
        if version.returncode == 0 and re.search(
            r"\bversion\s+18(?:[.\s]|$)", version.stdout
        ):
            return resolved
    raise ValueError("clang-format 18 is required for --clang-format")


def audit_git_tree(
    revision: str, paths: set[Path], formatter: str | None = None
) -> tuple[list[Violation], int, int]:
    violations: list[Violation] = []
    if formatter is not None and Path(".clang-format") not in paths:
        violations.append(
            Violation(
                Path(".clang-format"),
                1,
                "FMT003",
                "clang-format enforcement requires a tracked .clang-format",
            )
        )
    line_count = 0
    text_file_count = 0
    with tempfile.TemporaryDirectory(prefix="bte-clang-format-") as directory:
        style_file: Path | None = None
        if formatter is not None and Path(".clang-format") in paths:
            style_result = subprocess.run(
                ["git", "show", f"{revision}:.clang-format"],
                check=True,
                capture_output=True,
            )
            style_file = Path(directory, ".clang-format")
            style_file.write_bytes(style_result.stdout)

        for path in sorted(paths):
            result = subprocess.run(
                ["git", "show", f"{revision}:{path.as_posix()}"],
                check=True,
                capture_output=True,
            )
            try:
                source = result.stdout.decode("utf-8")
            except UnicodeDecodeError:
                continue
            source_violations, source_line_count = audit_text_sources({path: source})
            violations.extend(source_violations)
            if formatter is not None and path.suffix.lower() in CPP_SUFFIXES:
                violations.extend(
                    audit_clang_format_source(
                        path, source, formatter, style_file
                    )
                )
            line_count += source_line_count
            text_file_count += 1
    return violations, line_count, text_file_count


def format_violation(violation: Violation) -> str:
    message = f"[{violation.rule}] {violation.message}"
    if os.environ.get("GITHUB_ACTIONS") == "true":
        return f"::error file={violation.path},line={violation.line}::{message}"
    return f"{violation.path}:{violation.line}: {message}"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base", required=True, help="Git base revision")
    parser.add_argument("--head", help="Git head revision; omit to include working-tree changes")
    parser.add_argument(
        "--full-tree",
        action="store_true",
        help="audit every UTF-8 text file at --head, not only added lines",
    )
    parser.add_argument(
        "--clang-format",
        action="store_true",
        help="also require every C++ source at --head to match clang-format 18",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        added_lines = parse_unified_diff(git_diff(args.base, args.head))
        audited_line_count = len(added_lines)
        if args.full_tree and args.head is None:
            raise ValueError("--full-tree requires --head")
        if args.clang_format and not args.full_tree:
            raise ValueError("--clang-format requires --full-tree")
        if args.full_tree:
            head_paths = git_tree_paths(args.head)
            formatter = find_clang_format() if args.clang_format else None
            violations, audited_line_count, audited_file_count = audit_git_tree(
                args.head, head_paths, formatter
            )
        else:
            violations = audit_added_lines(added_lines)
        if args.head is not None:
            base_paths = git_tree_paths(args.base)
            head_paths = git_tree_paths(args.head)
            cmake_sources = git_cmake_sources(args.head, head_paths)
            violations.extend(audit_new_modules(base_paths, head_paths, cmake_sources))
            violations.extend(audit_test_registration(head_paths, cmake_sources))
            violations.extend(audit_path_conventions(head_paths))
    except (subprocess.CalledProcessError, ValueError) as error:
        print(f"Standards audit could not inspect the diff: {error}", file=sys.stderr)
        return 2

    for violation in violations:
        print(format_violation(violation))

    if violations:
        print(f"Project standards audit failed with {len(violations)} violation(s).", file=sys.stderr)
        return 1

    if args.full_tree:
        print(
            "Project standards audit passed for "
            f"{audited_line_count} line(s) in {audited_file_count} tracked UTF-8 file(s)."
        )
    else:
        print(f"Project standards audit passed for {audited_line_count} added line(s).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
