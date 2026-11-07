"""List CTest executables needed to export LLVM source-based coverage."""

from __future__ import annotations

import argparse
import json
import os
import subprocess
from pathlib import Path
from typing import Any, Callable


def coverage_binary_paths(
    manifest: dict[str, Any], build_directory: Path
) -> list[str]:
    """Return executable command arguments located inside the build tree."""
    tests = manifest.get("tests")
    if not isinstance(tests, list):
        raise ValueError("CTest JSON manifest does not contain a tests list")

    build_directory = build_directory.resolve()
    binaries: set[str] = set()
    for test in tests:
        if not isinstance(test, dict):
            raise ValueError("CTest JSON manifest contains a malformed test entry")
        command = test.get("command")
        if not isinstance(command, list):
            raise ValueError("CTest test command is malformed")
        for argument in command:
            if not isinstance(argument, str):
                continue
            candidate_value = (
                argument.partition("=")[2] if argument.startswith("-") else argument
            )
            if not candidate_value:
                continue
            candidate = Path(candidate_value)
            if not candidate.is_absolute():
                candidate = build_directory / candidate
            try:
                resolved = candidate.resolve(strict=True)
                resolved.relative_to(build_directory)
            except (OSError, ValueError):
                continue
            if resolved.is_file() and os.access(resolved, os.X_OK):
                binaries.add(str(resolved))
    return sorted(binaries)


def collect_coverage_binaries(
    build_directory: Path,
    *,
    ctest_executable: str = "ctest",
    runner: Callable[..., Any] = subprocess.run,
) -> list[str]:
    """Read the CTest manifest and return its build-local executable paths."""
    build_directory = build_directory.resolve()
    manifest_result = runner(
        [
            ctest_executable,
            "--test-dir",
            str(build_directory),
            "--show-only=json-v1",
        ],
        check=True,
        capture_output=True,
        text=True,
    )
    manifest = json.loads(manifest_result.stdout)
    binaries = coverage_binary_paths(manifest, build_directory)
    if not binaries:
        raise RuntimeError(
            f"CTest found no executable binaries under {build_directory}"
        )
    return binaries


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--ctest", default="ctest")
    arguments = parser.parse_args()

    binaries = collect_coverage_binaries(
        arguments.build_dir,
        ctest_executable=arguments.ctest,
    )
    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    arguments.output.write_text("\n".join(binaries) + "\n", encoding="utf-8")
    print(f"Found {len(binaries)} instrumented coverage binaries.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
