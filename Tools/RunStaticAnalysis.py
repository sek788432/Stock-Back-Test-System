#!/usr/bin/env python3
"""Run repository static analyzers against CMake's compilation database."""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
from pathlib import Path


REPOSITORY_ROOT = Path(__file__).resolve().parents[1]


def find_command(candidates: tuple[str, ...]) -> str:
    for candidate in candidates:
        resolved = shutil.which(candidate)
        if resolved is not None:
            return resolved
    raise RuntimeError(f"required command not found: {' or '.join(candidates)}")


def compilation_units(database_path: Path, repository_root: Path) -> list[Path]:
    entries = json.loads(database_path.read_text(encoding="utf-8"))
    source_root = (repository_root / "Src").resolve()
    units: set[Path] = set()
    for entry in entries:
        source = Path(entry["file"])
        if not source.is_absolute():
            source = Path(entry["directory"]) / source
        source = source.resolve()
        if source.suffix not in {".cc", ".cpp", ".cxx"}:
            continue
        try:
            source.relative_to(source_root)
        except ValueError:
            continue
        units.add(source)
    return sorted(units)


def analyzer_command(tool: str, build_directory: Path, units: list[Path]) -> list[str]:
    if tool == "clang-tidy":
        return [
            find_command(("clang-tidy-18", "clang-tidy")),
            "-p",
            str(build_directory),
            *map(str, units),
        ]
    if tool == "cppcheck":
        return [
            find_command(("cppcheck",)),
            f"--project={build_directory / 'compile_commands.json'}",
            "--enable=warning,performance,portability",
            "--error-exitcode=1",
            "--inline-suppr",
            "--file-filter=*/Src/*",
            f"--suppressions-list={REPOSITORY_ROOT / 'Cppcheck.suppressions'}",
        ]
    if tool == "iwyu":
        return [
            find_command(("iwyu_tool.py", "iwyu_tool")),
            "-p",
            str(build_directory),
            *map(str, units),
            "--",
            "-Xiwyu",
            f"--mapping_file={REPOSITORY_ROOT / 'Tools/Iwyu.imp'}",
            "-Xiwyu",
            "--error=1",
        ]
    raise ValueError(f"unsupported analyzer: {tool}")


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("tool", choices=("clang-tidy", "cppcheck", "iwyu"))
    parser.add_argument("--build-dir", type=Path, default=Path("Output/analysis"))
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    build_directory = arguments.build_dir.resolve()
    database_path = build_directory / "compile_commands.json"
    if not database_path.is_file():
        print(f"compilation database not found: {database_path}", file=sys.stderr)
        return 2

    units = compilation_units(database_path, REPOSITORY_ROOT)
    if not units:
        print("no project C++ compilation units found", file=sys.stderr)
        return 2

    try:
        command = analyzer_command(arguments.tool, build_directory, units)
    except RuntimeError as error:
        print(error, file=sys.stderr)
        return 2
    return subprocess.run(command, cwd=REPOSITORY_ROOT, check=False).returncode


if __name__ == "__main__":
    raise SystemExit(main())
