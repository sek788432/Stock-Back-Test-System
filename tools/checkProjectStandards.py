#!/usr/bin/env python3
"""Reject newly added lines that violate the repository's hard coding rules."""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path


CPP_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx"}
TEST_PATH_PARTS = {"Test", "Tests", "test", "tests"}


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
        re.compile(r"\b(?:printf|fprintf|sprintf|strcpy|strcat)\s*\("),
        "C-style string or output functions are forbidden",
    ),
    ("CPP007", re.compile(r"\.\s*(?:lock|unlock)\s*\("), "manual lock/unlock is forbidden; use RAII locks"),
    ("CPP008", re.compile(r"\.\s*detach\s*\("), "detached threads are forbidden; use std::jthread"),
)

TRIVIAL_BOOL_ASSERTION = re.compile(r"\b(?:EXPECT|ASSERT)_(?:TRUE|FALSE)\s*\(\s*(?:true|false)\s*\)")
EQUALITY_ASSERTION = re.compile(r"\b(?:EXPECT|ASSERT)_(?:EQ|NE)\s*\(")
PYTHON_TRIVIAL_ASSERTION = re.compile(r"^\s*assert\s+(?:True|False)\s*(?:#.*)?$")
UNJUSTIFIED_TASK = re.compile(
    r"\b(?:" + "|".join(("TO" + "DO", "FIX" + "ME")) + r")\b(?![^\n]*\bISSUE-\d+\b)"
)


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


def audit_added_line(added: AddedLine) -> list[Violation]:
    violations: list[Violation] = []
    text = added.text
    suffix = added.path.suffix.lower()

    if text.rstrip() != text:
        violations.append(Violation(added.path, added.number, "FMT001", "trailing whitespace is forbidden"))

    if UNJUSTIFIED_TASK.search(text):
        violations.append(
            Violation(
                added.path,
                added.number,
                "DOC001",
                "TO" "DO/FIX" "ME comments must reference ISSUE-NNN",
            )
        )

    if suffix in CPP_SUFFIXES:
        code = text.split("//", maxsplit=1)[0]
        for rule, pattern, message in CPP_RULES:
            if pattern.search(code):
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


def module_and_test_roots(path: Path) -> tuple[Path, Path] | None:
    parts = path.parts
    if len(parts) >= 3 and parts[0:2] == ("Src", "Backend"):
        return Path(*parts[:3]), Path("Tests", "Unit", parts[2])
    if len(parts) >= 2 and parts[0] == "Src":
        return Path(*parts[:2]), Path("Tests", parts[1])
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


def format_violation(violation: Violation) -> str:
    message = f"[{violation.rule}] {violation.message}"
    if os.environ.get("GITHUB_ACTIONS") == "true":
        return f"::error file={violation.path},line={violation.line}::{message}"
    return f"{violation.path}:{violation.line}: {message}"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base", required=True, help="Git base revision")
    parser.add_argument("--head", help="Git head revision; omit to include working-tree changes")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        added_lines = parse_unified_diff(git_diff(args.base, args.head))
        violations = audit_added_lines(added_lines)
        if args.head is not None:
            base_paths = git_tree_paths(args.base)
            head_paths = git_tree_paths(args.head)
            violations.extend(audit_new_modules(base_paths, head_paths, git_cmake_sources(args.head, head_paths)))
    except (subprocess.CalledProcessError, ValueError) as error:
        print(f"Standards audit could not inspect the diff: {error}", file=sys.stderr)
        return 2

    for violation in violations:
        print(format_violation(violation))

    if violations:
        print(f"Project standards audit failed with {len(violations)} violation(s).", file=sys.stderr)
        return 1

    print(f"Project standards audit passed for {len(added_lines)} added line(s).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
