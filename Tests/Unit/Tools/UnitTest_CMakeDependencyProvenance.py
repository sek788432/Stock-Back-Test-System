import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


REPOSITORY_ROOT = Path(__file__).resolve().parents[3]


class CMakeDependencyProvenanceTest(unittest.TestCase):
    def configure(self, *arguments: str) -> subprocess.CompletedProcess[str]:
        environment = os.environ.copy()
        environment.pop("VCPKG_ROOT", None)
        with tempfile.TemporaryDirectory(prefix="bte-cmake-provenance-") as build_directory:
            return subprocess.run(
                [
                    shutil.which("cmake") or "cmake",
                    "-S",
                    os.fspath(REPOSITORY_ROOT),
                    "-B",
                    build_directory,
                    "-DBTE_BUILD_TESTS=OFF",
                    "-DBTE_BUILD_QT_APP=OFF",
                    *arguments,
                ],
                cwd=REPOSITORY_ROOT,
                env=environment,
                capture_output=True,
                text=True,
                check=False,
            )

    def test_configure_without_vcpkg_toolchain_fails_for_provenance(self) -> None:
        result = self.configure()

        self.assertNotEqual(result.returncode, 0)
        self.assertIn(
            "Pinned vcpkg manifest provenance is required",
            result.stdout + result.stderr,
        )

    def test_configure_with_explicit_vcpkg_toolchain_succeeds(self) -> None:
        toolchain = os.environ.get("BTE_TEST_CMAKE_TOOLCHAIN_FILE", "")
        self.assertTrue(toolchain, "registered test must provide the vcpkg toolchain")

        result = self.configure(f"-DCMAKE_TOOLCHAIN_FILE={toolchain}")

        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('found suitable exact version "3.53.4"', result.stdout)
        self.assertIn("vcpkg_installed", result.stdout)

    def test_configure_rejects_host_sqlite_preseeded_in_cache(self) -> None:
        toolchain = os.environ.get("BTE_TEST_CMAKE_TOOLCHAIN_FILE", "")
        self.assertTrue(toolchain, "registered test must provide the vcpkg toolchain")

        manifest_result = self.configure(f"-DCMAKE_TOOLCHAIN_FILE={toolchain}")
        self.assertEqual(
            manifest_result.returncode,
            0,
            manifest_result.stdout + manifest_result.stderr,
        )

        installed_root = REPOSITORY_ROOT / "vcpkg_installed"
        headers = sorted(installed_root.glob("*/include/sqlite3.h"))
        libraries = sorted(
            path
            for path in installed_root.glob("*/lib/*sqlite3*")
            if path.is_file()
        )
        self.assertTrue(headers, "manifest configure must install sqlite3.h")
        self.assertTrue(libraries, "manifest configure must install the SQLite library")

        with tempfile.TemporaryDirectory(prefix="bte-host-sqlite-") as host_root:
            host_include = Path(host_root) / "include"
            host_library = Path(host_root) / "lib" / libraries[0].name
            host_include.mkdir()
            host_library.parent.mkdir()
            shutil.copy2(headers[0], host_include / "sqlite3.h")
            shutil.copy2(libraries[0], host_library)

            result = self.configure(
                f"-DCMAKE_TOOLCHAIN_FILE={toolchain}",
                f"-DSQLite3_INCLUDE_DIR:PATH={host_include}",
                f"-DSQLite3_LIBRARY:FILEPATH={host_library}",
            )

        self.assertNotEqual(result.returncode, 0)
        self.assertIn(
            "SQLite3 dependency provenance violation",
            result.stdout + result.stderr,
        )


if __name__ == "__main__":
    unittest.main()
