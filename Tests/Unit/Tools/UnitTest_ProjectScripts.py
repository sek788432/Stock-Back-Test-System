"""Behavior tests for the repository's root developer scripts."""

from __future__ import annotations

import os
import shutil
import stat
import subprocess
import tempfile
import unittest
from pathlib import Path


REPOSITORY_ROOT = Path(__file__).resolve().parents[3]


class ProjectScriptsTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary_directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary_directory.cleanup)
        self.root = Path(self.temporary_directory.name) / "Repository With Spaces"
        self.root.mkdir()
        self.bin_directory = self.root / "FakeBin"
        self.bin_directory.mkdir()
        self.command_log = self.root / "commands.log"

    def install_fake_command(self, name: str, body: str) -> None:
        command = self.bin_directory / name
        command.write_text("#!/usr/bin/env bash\n" + body, encoding="utf-8")
        command.chmod(command.stat().st_mode | stat.S_IXUSR)

    def copy_script(self, name: str) -> Path:
        destination = self.root / name
        shutil.copy2(REPOSITORY_ROOT / name, destination)
        return destination

    def environment(self) -> dict[str, str]:
        environment = os.environ.copy()
        environment["PATH"] = f"{self.bin_directory}{os.pathsep}{environment['PATH']}"
        environment["BTE_SCRIPT_TEST_LOG"] = str(self.command_log)
        return environment

    def run_script(self, script: Path, *arguments: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            ["bash", str(script), *arguments],
            cwd=Path(self.temporary_directory.name),
            env=self.environment(),
            capture_output=True,
            text=True,
            check=False,
        )

    def logged_commands(self) -> list[str]:
        if not self.command_log.exists():
            return []
        return self.command_log.read_text(encoding="utf-8").splitlines()

    def test_launch_configures_builds_and_starts_qt_app_from_any_directory(self) -> None:
        script = self.copy_script("Launch.sh")
        self.install_fake_command("cmake", 'printf "cmake %s\\n" "$*" >> "$BTE_SCRIPT_TEST_LOG"\n')
        app = self.root / "Output/qt-dev/Src/App/stockBacktester"
        app.parent.mkdir(parents=True)
        app.write_text(
            '#!/usr/bin/env bash\nprintf "app %s\\n" "$PWD" >> "$BTE_SCRIPT_TEST_LOG"\n',
            encoding="utf-8",
        )
        app.chmod(app.stat().st_mode | stat.S_IXUSR)

        result = self.run_script(script)

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(
            self.logged_commands(),
            [
                "cmake --preset qt-dev -DBTE_BUILD_TESTS=ON -DCMAKE_COMPILE_WARNING_AS_ERROR=ON",
                "cmake --build --preset qt-dev --parallel",
                f"app {self.root}",
            ],
        )

    def test_launch_does_not_start_app_when_configure_fails(self) -> None:
        script = self.copy_script("Launch.sh")
        self.install_fake_command("cmake", "exit 23\n")
        app = self.root / "Output/qt-dev/Src/App/stockBacktester"
        app.parent.mkdir(parents=True)
        app.write_text(
            '#!/usr/bin/env bash\nprintf "started\\n" >> "$BTE_SCRIPT_TEST_LOG"\n',
            encoding="utf-8",
        )
        app.chmod(app.stat().st_mode | stat.S_IXUSR)

        result = self.run_script(script)

        self.assertEqual(result.returncode, 23)
        self.assertEqual(self.logged_commands(), [])

    def test_run_test_uses_qt_preset_and_runs_every_registered_test(self) -> None:
        script = self.copy_script("RunTest.sh")
        self.install_fake_command("cmake", 'printf "cmake %s\\n" "$*" >> "$BTE_SCRIPT_TEST_LOG"\n')
        self.install_fake_command("ctest", 'printf "ctest %s\\n" "$*" >> "$BTE_SCRIPT_TEST_LOG"\n')

        result = self.run_script(script)

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(
            self.logged_commands(),
            [
                "cmake --preset qt-dev -DBTE_BUILD_TESTS=ON -DCMAKE_COMPILE_WARNING_AS_ERROR=ON",
                "cmake --build --preset qt-dev --parallel",
                "ctest --preset qt-dev --no-tests=error",
            ],
        )

    def test_run_test_verbose_keeps_all_tests_and_enables_verbose_output(self) -> None:
        script = self.copy_script("RunTest.sh")
        self.install_fake_command("cmake", 'printf "cmake %s\\n" "$*" >> "$BTE_SCRIPT_TEST_LOG"\n')
        self.install_fake_command("ctest", 'printf "ctest %s\\n" "$*" >> "$BTE_SCRIPT_TEST_LOG"\n')

        result = self.run_script(script, "--verbose")

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(
            self.logged_commands()[-1],
            "ctest --preset qt-dev --no-tests=error --verbose",
        )


if __name__ == "__main__":
    unittest.main()
