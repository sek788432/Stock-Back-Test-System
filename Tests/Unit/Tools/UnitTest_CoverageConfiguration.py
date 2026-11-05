"""Integration tests for compiler-specific coverage instrumentation."""

from __future__ import annotations

import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


REPOSITORY_ROOT = Path(__file__).resolve().parents[3]


class CoverageConfigurationTest(unittest.TestCase):
    def run_coverage_probe(self, compiler: str, expected_format: str) -> None:
        with tempfile.TemporaryDirectory(prefix="coverage-configuration-") as temporary_directory:
            root = Path(temporary_directory)
            source_directory = root / "source"
            build_directory = root / "build"
            source_directory.mkdir()
            (source_directory / "CMakeLists.txt").write_text(
                "\n".join(
                    [
                        "cmake_minimum_required(VERSION 3.24)",
                        "project(CoverageProbe LANGUAGES CXX)",
                        f'include("{REPOSITORY_ROOT / "CMake" / "Coverage.cmake"}")',
                        "set(BTE_COVERAGE ON)",
                        "bte_enable_coverage()",
                        "add_executable(coverage_probe main.cpp)",
                        "",
                    ]
                ),
                encoding="utf-8",
            )
            (source_directory / "main.cpp").write_text(
                "int main(int argc, char**) {\n"
                "  if (argc == 3) return 1;\n"
                "  return 0;\n"
                "}\n",
                encoding="utf-8",
            )

            subprocess.run(
                [
                    "cmake",
                    "-S",
                    str(source_directory),
                    "-B",
                    str(build_directory),
                    f"-DCMAKE_CXX_COMPILER={compiler}",
                ],
                check=True,
                capture_output=True,
                text=True,
            )
            cache = (build_directory / "CMakeCache.txt").read_text(encoding="utf-8")
            self.assertIn(
                f"BTE_COVERAGE_FORMAT:INTERNAL={expected_format}",
                cache,
            )
            subprocess.run(
                ["cmake", "--build", str(build_directory)],
                check=True,
                capture_output=True,
                text=True,
            )

            executable = build_directory / "coverage_probe"
            if expected_format == "llvm-source-based":
                profile_directory = build_directory / "profiles"
                profile_directory.mkdir()
                environment = os.environ | {
                    "LLVM_PROFILE_FILE": str(profile_directory / "%p.profraw")
                }
                subprocess.run([str(executable)], env=environment, check=True)
                self.assertTrue(list(profile_directory.glob("*.profraw")))
                self.assertFalse(list(build_directory.rglob("*.gcda")))
            else:
                subprocess.run([str(executable)], check=True)
                self.assertTrue(list(build_directory.rglob("*.gcda")))
                self.assertFalse(list(build_directory.rglob("*.profraw")))

    def test_clang_coverage_writes_llvm_source_profiles(self) -> None:
        compiler = shutil.which("clang++-18") or shutil.which("clang++")
        if compiler is None:
            self.skipTest("Clang is not installed")
        version = subprocess.run(
            [compiler, "--version"],
            check=True,
            capture_output=True,
            text=True,
        ).stdout
        if "clang" not in version.lower():
            self.skipTest("clang++ resolves to a non-Clang compiler")

        self.run_coverage_probe(compiler, "llvm-source-based")

    def test_gnu_coverage_writes_gcov_profiles(self) -> None:
        compiler = next(
            (
                candidate
                for name in ("g++-15", "g++-14", "g++-13", "g++")
                if (candidate := shutil.which(name)) is not None
                and "gcc" in subprocess.run(
                    [candidate, "--version"],
                    check=True,
                    capture_output=True,
                    text=True,
                ).stdout.lower()
            ),
            None,
        )
        if compiler is None:
            self.skipTest("GNU C++ is not installed")

        self.run_coverage_probe(compiler, "gcov")


if __name__ == "__main__":
    unittest.main()
