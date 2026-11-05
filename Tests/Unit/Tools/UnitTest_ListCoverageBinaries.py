"""Behavior tests for discovering binaries needed by source-based coverage."""

from __future__ import annotations

import importlib.util
import json
import tempfile
import unittest
from pathlib import Path


REPOSITORY_ROOT = Path(__file__).resolve().parents[3]


def load_tool(name: str):
    specification = importlib.util.spec_from_file_location(
        name, REPOSITORY_ROOT / "Tools" / f"{name}.py"
    )
    module = importlib.util.module_from_spec(specification)
    assert specification.loader is not None
    specification.loader.exec_module(module)
    return module


coverage_binaries = load_tool("ListCoverageBinaries")


class CoverageBinaryDiscoveryTest(unittest.TestCase):
    def test_collects_only_executables_inside_the_build_tree(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            build = Path(temporary_directory) / "build"
            test_binary = build / "Tests" / "bte_core_tests"
            app_binary = build / "StockBackTestSystem.app" / "Contents" / "MacOS" / "StockBackTestSystem"
            test_binary.parent.mkdir(parents=True)
            app_binary.parent.mkdir(parents=True)
            for binary in (test_binary, app_binary):
                binary.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
                binary.chmod(0o755)

            manifest = {
                "tests": [
                    {
                        "name": "BarTest.isValid",
                        "command": [str(test_binary), "--gtest_filter=BarTest.isValid"],
                    },
                    {
                        "name": "app-startup",
                        "command": ["/usr/bin/python3", "run_smoke.py", "--app", str(app_binary)],
                    },
                    {
                        "name": "external-helper",
                        "command": ["/usr/bin/python3", "helper.py"],
                    },
                ]
            }

            self.assertEqual(
                coverage_binaries.coverage_binary_paths(manifest, build),
                sorted([str(test_binary.resolve()), str(app_binary.resolve())]),
            )

    def test_reads_ctest_manifest_and_writes_binary_list(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            build = Path(temporary_directory) / "build"
            build.mkdir()
            executable = build / "bte-test"
            executable.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
            executable.chmod(0o755)
            manifest = {"tests": [{"name": "test", "command": [str(executable)]}]}
            calls: list[list[str]] = []

            def fake_runner(command: list[str], **kwargs):
                calls.append(command)
                return type("Result", (), {"stdout": json.dumps(manifest)})()

            binaries = coverage_binaries.collect_coverage_binaries(
                build,
                ctest_executable="fake-ctest",
                runner=fake_runner,
            )

            self.assertEqual(
                calls,
                [["fake-ctest", "--test-dir", str(build.resolve()), "--show-only=json-v1"]],
            )
            self.assertEqual(binaries, [str(executable.resolve())])


if __name__ == "__main__":
    unittest.main()
