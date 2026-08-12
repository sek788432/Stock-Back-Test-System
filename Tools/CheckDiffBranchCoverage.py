#!/usr/bin/env python3
"""Enforce changed-branch coverage from a gcovr JSON report."""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path


TRANSLATION_UNIT_SUFFIXES = {".cc", ".cpp", ".cxx"}


def changed_lines(diff_text: str) -> dict[str, set[int]]:
    changed: dict[str, set[int]] = {}
    current_file: str | None = None
    for line in diff_text.splitlines():
        if line.startswith("+++ "):
            current_file = line[6:] if line.startswith("+++ b/") else None
            if current_file is not None:
                changed.setdefault(current_file, set())
            continue
        if not line.startswith("@@") or current_file is None:
            continue
        new_range = line.split("+")[1].split(" ")[0]
        start_text, separator, count_text = new_range.partition(",")
        start = int(start_text)
        count = int(count_text) if separator else 1
        changed[current_file].update(range(start, start + count))
    return changed


def branch_counts(report: dict[str, object], changed: dict[str, set[int]]) -> tuple[int, int]:
    covered = 0
    total = 0
    for file_entry in report.get("files", []):
        filename = str(file_entry["file"]).replace("\\", "/")
        relevant_lines = changed.get(filename, set())
        for line_entry in file_entry.get("lines", []):
            if int(line_entry["line_number"]) not in relevant_lines:
                continue
            for branch in line_entry.get("branches", []):
                total += 1
                covered += int(branch.get("count", 0)) > 0
    return covered, total


def missing_changed_sources(
    report: dict[str, object], changed: dict[str, set[int]]
) -> list[str]:
    reported_files = {
        str(file_entry["file"]).replace("\\", "/")
        for file_entry in report.get("files", [])
    }
    return sorted(
        filename
        for filename, lines in changed.items()
        if lines
        and Path(filename).suffix in TRANSLATION_UNIT_SUFFIXES
        and filename not in reported_files
    )


def coverage_percentage(covered: int, total: int) -> float:
    return 100.0 if total == 0 else covered * 100.0 / total


def meets_threshold(covered: int, total: int, threshold: float) -> bool:
    return coverage_percentage(covered, total) >= threshold


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("report", type=Path)
    parser.add_argument("--base", required=True)
    parser.add_argument("--head", required=True)
    parser.add_argument("--fail-under", type=float, default=80.0)
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    diff = subprocess.run(
        ["git", "diff", "--unified=0", arguments.base, arguments.head, "--", "Src"],
        check=True,
        capture_output=True,
        text=True,
    ).stdout
    report = json.loads(arguments.report.read_text(encoding="utf-8"))
    changed = changed_lines(diff)
    missing_sources = missing_changed_sources(report, changed)
    if missing_sources:
        print(
            "changed source files missing from coverage report: "
            + ", ".join(missing_sources),
            file=sys.stderr,
        )
        return 2

    covered, total = branch_counts(report, changed)
    percentage = coverage_percentage(covered, total)
    print(f"Changed-branch coverage: {percentage:.1f}% ({covered}/{total})")
    return 0 if meets_threshold(covered, total, arguments.fail_under) else 1


if __name__ == "__main__":
    raise SystemExit(main())
