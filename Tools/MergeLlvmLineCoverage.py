"""Overlay LLVM source line hits onto existing gcovr JSON line records."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any


def project_source_path(value: str, root: Path) -> str | None:
    """Return a normalized Src-relative report path for files inside root."""
    candidate = Path(value.replace("\\", "/"))
    if not candidate.is_absolute():
        candidate = root / candidate
    try:
        relative = candidate.resolve().relative_to(root)
    except ValueError:
        return None
    if not relative.parts or relative.parts[0] != "Src":
        return None
    return relative.as_posix()


def parse_llvm_lcov_line_counts(lcov_text: str, root: Path) -> dict[str, dict[int, int]]:
    """Read LLVM LCOV DA records, retaining the greatest count per source line."""
    root = root.resolve()
    line_counts: dict[str, dict[int, int]] = {}
    current_source: str | None = None
    has_current_record = False

    for line_number, record in enumerate(lcov_text.splitlines(), start=1):
        if record.startswith("SF:"):
            source = record[3:]
            if not source:
                raise ValueError(f"LCOV source path is empty at line {line_number}")
            current_source = project_source_path(source, root)
            has_current_record = True
            continue
        if record == "end_of_record":
            current_source = None
            has_current_record = False
            continue
        if not record.startswith("DA:"):
            continue
        if not has_current_record:
            raise ValueError(f"LCOV line data has no source record at line {line_number}")

        fields = record[3:].split(",")
        if len(fields) < 2:
            raise ValueError(f"Malformed LCOV line data at line {line_number}")
        try:
            source_line = int(fields[0])
            hit_count = int(fields[1])
        except ValueError as error:
            raise ValueError(f"Malformed LCOV line data at line {line_number}") from error
        if source_line < 1 or hit_count < 0:
            raise ValueError(f"Invalid LCOV line data at line {line_number}")
        if current_source is None:
            continue

        source_counts = line_counts.setdefault(current_source, {})
        source_counts[source_line] = max(
            hit_count, source_counts.get(source_line, 0)
        )

    return line_counts


def merge_line_hits(
    report: dict[str, Any], lcov_text: str, root: Path
) -> int:
    """Overlay LLVM hits and discard compiler-unmapped noncode pseudo-lines."""
    files = report.get("files")
    if not isinstance(files, list):
        raise ValueError("gcovr JSON report does not contain a files list")

    llvm_counts = parse_llvm_lcov_line_counts(lcov_text, root)
    updated_lines = 0
    resolved_root = root.resolve()

    for file_entry in files:
        if not isinstance(file_entry, dict) or not isinstance(
            file_entry.get("file"), str
        ):
            raise ValueError("gcovr JSON report contains a malformed file entry")
        source = project_source_path(file_entry["file"], resolved_root)
        if source is None:
            continue
        lines = file_entry.get("lines")
        if not isinstance(lines, list):
            raise ValueError(f"gcovr JSON source {source} has no lines list")

        source_counts = llvm_counts.get(source, {})
        source_path = resolved_root / source
        source_lines = (
            source_path.read_text(encoding="utf-8").split("\n")
            if source_counts and source_path.is_file()
            else []
        )
        # Avoid inferring lexical context across multiline constructs. Preserve
        # the whole file unless line-leading comment/directive syntax is safe.
        if any(
            'R"' in line or "/*" in line or line.rstrip().endswith("\\")
            for line in source_lines
        ):
            source_lines = []
        retained_lines = []

        for line_entry in lines:
            if not isinstance(line_entry, dict):
                raise ValueError(f"gcovr JSON source {source} has a malformed line")
            if line_entry.get("gcovr/excluded"):
                retained_lines.append(line_entry)
                continue
            source_line = line_entry.get("line_number")
            count = line_entry.get("count")
            if (
                isinstance(source_line, bool)
                or not isinstance(source_line, int)
                or source_line < 1
                or isinstance(count, bool)
                or not isinstance(count, int)
                or count < 0
            ):
                raise ValueError(f"gcovr JSON source {source} has invalid line data")

            llvm_count = source_counts.get(source_line)
            source_text = (
                source_lines[source_line - 1].lstrip()
                if source_line <= len(source_lines)
                else ""
            )
            directive = source_text.split(maxsplit=1)[0] if source_text else ""
            is_noncode = source_text.startswith("//") or directive in {
                "#if",
                "#ifdef",
                "#ifndef",
                "#elif",
                "#else",
                "#endif",
            }
            if (
                llvm_count is None
                and is_noncode
                and not line_entry.get("branches")
                and not line_entry.get("gcovr/decision")
            ):
                continue
            retained_lines.append(line_entry)
            if llvm_count is not None and llvm_count > count:
                line_entry["count"] = llvm_count
                updated_lines += 1

        file_entry["lines"] = retained_lines

    return updated_lines


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--gcovr-json", type=Path, required=True)
    parser.add_argument("--llvm-lcov", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    arguments = parser.parse_args()

    report = json.loads(arguments.gcovr_json.read_text(encoding="utf-8"))
    if not isinstance(report, dict):
        raise ValueError("gcovr JSON report must be an object")
    updated_lines = merge_line_hits(
        report,
        arguments.llvm_lcov.read_text(encoding="utf-8"),
        arguments.root,
    )
    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    arguments.output.write_text(
        json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    print(f"Updated LLVM line hits on {updated_lines} existing gcovr lines.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
