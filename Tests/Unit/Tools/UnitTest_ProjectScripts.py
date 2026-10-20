"""Behavior tests for the repository's root developer scripts."""

from __future__ import annotations

import hashlib
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

    def install_fake_snapshot_builder(
        self, snapshot_id: str = "a" * 64, exit_code: int = 0
    ) -> None:
        builder = (
            self.root / "Output/qt-dev/Src/Backend/Data/bte_snapshot_builder"
        )
        builder.parent.mkdir(parents=True, exist_ok=True)
        builder.write_text(
            "#!/usr/bin/env bash\n"
            'printf "snapshot-builder %s\\n" "$*" >> "$BTE_SCRIPT_TEST_LOG"\n'
            f"if [[ {exit_code} -ne 0 ]]; then exit {exit_code}; fi\n"
            'mkdir -p "$2/Snapshots/'
            + snapshot_id
            + '"\n'
            + 'touch "$2/Snapshots/'
            + snapshot_id
            + '/Manifest.btesnapshot"\n'
            + f'printf "%s\\n" "{snapshot_id}"\n',
            encoding="utf-8",
        )
        builder.chmod(builder.stat().st_mode | stat.S_IXUSR)

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
        self.install_fake_command(
            "cmake", 'printf "cmake %s\\n" "$*" >> "$BTE_SCRIPT_TEST_LOG"\n'
        )
        self.install_fake_snapshot_builder()
        app = self.root / "Output/qt-dev/Src/App/stockBacktester"
        app.parent.mkdir(parents=True)
        app.write_text(
            "#!/usr/bin/env bash\n"
            'printf "app %s data=%s snapshot=%s\\n" "$PWD" '
            '"$BTE_DATA_STORE" "$BTE_DATA_SNAPSHOT_ID" '
            '>> "$BTE_SCRIPT_TEST_LOG"\n',
            encoding="utf-8",
        )
        app.chmod(app.stat().st_mode | stat.S_IXUSR)

        result = self.run_script(script)

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(f"Checkout: {self.root}", result.stdout)
        self.assertIn("Commit: unavailable", result.stdout)
        self.assertIn("Dirty: unknown", result.stdout)
        self.assertEqual(
            self.logged_commands(),
            [
                "cmake --preset qt-dev -DBTE_BUILD_TESTS=ON -DCMAKE_COMPILE_WARNING_AS_ERROR=ON",
                "cmake --build --preset qt-dev --parallel",
                "snapshot-builder "
                f"{self.root / 'StockData/Extracted'} "
                f"{self.root / 'Output/DevData'} "
                f"{'e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855'} "
                f"{'e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855'} "
                "AAPL MSFT NVDA",
                f"app {self.root} data={self.root / 'Output/DevData'} "
                f"snapshot={'a' * 64}",
            ],
        )

    def test_launch_reports_checkout_build_and_snapshot_identity(self) -> None:
        script = self.copy_script("Launch.sh")
        self.install_fake_command("cmake", 'printf "cmake %s\\n" "$*" >> "$BTE_SCRIPT_TEST_LOG"\n')
        self.install_fake_command(
            "git",
            '''printf "git %s\\n" "$*" >> "$BTE_SCRIPT_TEST_LOG"
if [[ "$*" == *"rev-parse --verify HEAD" ]]; then
  printf '%s\\n' '1234567890abcdef1234567890abcdef12345678'
elif [[ "$*" == *"symbolic-ref --short -q HEAD" ]]; then
  printf '%s\\n' 'codex/provenance-test'
elif [[ "$*" == *"status --porcelain"* ]]; then
  printf '%s\\n' ' M Launch.sh'
fi
''',
        )
        self.install_fake_snapshot_builder(snapshot_id="a" * 64)
        app = self.root / "Output/qt-dev/Src/App/stockBacktester"
        app.parent.mkdir(parents=True)
        app.write_text(
            '#!/usr/bin/env bash\nprintf "identity %s %s %s %s %s %s\\n" '
            '"$BTE_LAUNCH_CHECKOUT" "$BTE_LAUNCH_BRANCH" '
            '"$BTE_LAUNCH_COMMIT" "$BTE_LAUNCH_DIRTY" '
            '"$BTE_LAUNCH_APP_SHA256" "$BTE_DATA_SNAPSHOT_ID" '
            '>> "$BTE_SCRIPT_TEST_LOG"\n',
            encoding="utf-8",
        )
        app.chmod(app.stat().st_mode | stat.S_IXUSR)

        result = self.run_script(script)

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(f"Checkout: {self.root}", result.stdout)
        self.assertIn("Branch: codex/provenance-test", result.stdout)
        self.assertIn("Commit: 1234567890abcdef1234567890abcdef12345678", result.stdout)
        self.assertIn("Dirty: yes", result.stdout)
        self.assertIn(
            f"App SHA-256: {hashlib.sha256(app.read_bytes()).hexdigest()}",
            result.stdout,
        )
        self.assertIn(f"Snapshot ID: {'a' * 64}", result.stdout)
        self.assertIn(
            "identity "
            f"{self.root} codex/provenance-test "
            "1234567890abcdef1234567890abcdef12345678 yes "
            f"{hashlib.sha256(app.read_bytes()).hexdigest()} {'a' * 64}",
            self.logged_commands(),
        )
        self.assertFalse(
            any(
                command.startswith(("git fetch", "git checkout", "git switch"))
                for command in self.logged_commands()
            )
        )

    def test_launch_reports_clean_detached_checkout_without_inventing_branch(self) -> None:
        script = self.copy_script("Launch.sh")
        self.install_fake_command("cmake", "exit 0\n")
        self.install_fake_command(
            "git",
            '''if [[ "$*" == *"rev-parse --verify HEAD" ]]; then
  printf '%s\\n' 'abcdef1234567890abcdef1234567890abcdef12'
elif [[ "$*" == *"symbolic-ref --short -q HEAD" ]]; then
  exit 1
fi
''',
        )
        self.install_fake_snapshot_builder()
        app = self.root / "Output/qt-dev/Src/App/stockBacktester"
        app.parent.mkdir(parents=True)
        app.write_text("#!/usr/bin/env bash\nexit 0\n", encoding="utf-8")
        app.chmod(app.stat().st_mode | stat.S_IXUSR)

        result = self.run_script(script)

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Branch: detached", result.stdout)
        self.assertIn("Commit: abcdef1234567890abcdef1234567890abcdef12", result.stdout)
        self.assertIn("Dirty: no", result.stdout)

    def test_launch_preserves_complete_explicit_snapshot_configuration(self) -> None:
        script = self.copy_script("Launch.sh")
        self.install_fake_command(
            "cmake", 'printf "cmake %s\\n" "$*" >> "$BTE_SCRIPT_TEST_LOG"\n'
        )
        app = self.root / "Output/qt-dev/Src/App/stockBacktester"
        app.parent.mkdir(parents=True)
        app.write_text(
            "#!/usr/bin/env bash\n"
            'printf "app data=%s snapshot=%s\\n" "$BTE_DATA_STORE" '
            '"$BTE_DATA_SNAPSHOT_ID" >> "$BTE_SCRIPT_TEST_LOG"\n',
            encoding="utf-8",
        )
        app.chmod(app.stat().st_mode | stat.S_IXUSR)
        environment = self.environment()
        environment["BTE_DATA_STORE"] = str(self.root / "Explicit Data")
        environment["BTE_DATA_SNAPSHOT_ID"] = "b" * 64
        manifest = (
            self.root
            / "Explicit Data/Snapshots"
            / ("b" * 64)
            / "Manifest.btesnapshot"
        )
        manifest.parent.mkdir(parents=True)
        manifest.touch()

        result = subprocess.run(
            ["bash", str(script)],
            cwd=Path(self.temporary_directory.name),
            env=environment,
            capture_output=True,
            text=True,
            check=False,
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(
            self.logged_commands()[-1],
            f"app data={self.root / 'Explicit Data'} snapshot={'b' * 64}",
        )
        self.assertFalse(
            any(
                command.startswith("snapshot-builder ")
                for command in self.logged_commands()
            )
        )

    def test_launch_rejects_invalid_explicit_snapshot_identity(self) -> None:
        script = self.copy_script("Launch.sh")
        environment = self.environment()
        environment["BTE_DATA_STORE"] = str(self.root / "Explicit Data")
        environment["BTE_DATA_SNAPSHOT_ID"] = "invalid"

        result = subprocess.run(
            ["bash", str(script)],
            cwd=Path(self.temporary_directory.name),
            env=environment,
            capture_output=True,
            text=True,
            check=False,
        )

        self.assertEqual(result.returncode, 2)
        self.assertIn(
            "BTE_DATA_SNAPSHOT_ID must be 64 lowercase hex", result.stderr
        )
        self.assertEqual(self.logged_commands(), [])

    def test_launch_rejects_missing_explicit_snapshot_manifest(self) -> None:
        script = self.copy_script("Launch.sh")
        environment = self.environment()
        environment["BTE_DATA_STORE"] = str(self.root / "Explicit Data")
        environment["BTE_DATA_SNAPSHOT_ID"] = "b" * 64

        result = subprocess.run(
            ["bash", str(script)],
            cwd=Path(self.temporary_directory.name),
            env=environment,
            capture_output=True,
            text=True,
            check=False,
        )

        self.assertEqual(result.returncode, 2)
        self.assertIn("snapshot manifest does not exist", result.stderr)
        self.assertEqual(self.logged_commands(), [])

    def test_launch_rejects_partial_snapshot_configuration(self) -> None:
        script = self.copy_script("Launch.sh")
        environment = self.environment()
        environment["BTE_DATA_STORE"] = str(self.root / "Explicit Data")
        environment.pop("BTE_DATA_SNAPSHOT_ID", None)

        result = subprocess.run(
            ["bash", str(script)],
            cwd=Path(self.temporary_directory.name),
            env=environment,
            capture_output=True,
            text=True,
            check=False,
        )

        self.assertEqual(result.returncode, 2)
        self.assertIn(
            "BTE_DATA_STORE and BTE_DATA_SNAPSHOT_ID must be set together",
            result.stderr,
        )
        self.assertEqual(self.logged_commands(), [])

    def test_launch_does_not_start_app_when_snapshot_builder_fails(self) -> None:
        script = self.copy_script("Launch.sh")
        self.install_fake_command(
            "cmake", 'printf "cmake %s\\n" "$*" >> "$BTE_SCRIPT_TEST_LOG"\n'
        )
        self.install_fake_snapshot_builder(exit_code=29)
        app = self.root / "Output/qt-dev/Src/App/stockBacktester"
        app.parent.mkdir(parents=True)
        app.write_text(
            '#!/usr/bin/env bash\nprintf "started\\n" >> "$BTE_SCRIPT_TEST_LOG"\n',
            encoding="utf-8",
        )
        app.chmod(app.stat().st_mode | stat.S_IXUSR)

        result = self.run_script(script)

        self.assertEqual(result.returncode, 29)
        self.assertNotIn("started", self.logged_commands())

    def test_launch_rejects_invalid_snapshot_builder_identity(self) -> None:
        script = self.copy_script("Launch.sh")
        self.install_fake_command(
            "cmake", 'printf "cmake %s\\n" "$*" >> "$BTE_SCRIPT_TEST_LOG"\n'
        )
        self.install_fake_snapshot_builder(snapshot_id="not-a-snapshot-id")
        app = self.root / "Output/qt-dev/Src/App/stockBacktester"
        app.parent.mkdir(parents=True)
        app.write_text(
            '#!/usr/bin/env bash\nprintf "started\\n" >> "$BTE_SCRIPT_TEST_LOG"\n',
            encoding="utf-8",
        )
        app.chmod(app.stat().st_mode | stat.S_IXUSR)

        result = self.run_script(script)

        self.assertEqual(result.returncode, 2)
        self.assertIn("snapshot builder returned an invalid ID", result.stderr)
        self.assertNotIn("started", self.logged_commands())

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

    def test_launch_does_not_start_app_or_claim_build_when_build_fails(self) -> None:
        script = self.copy_script("Launch.sh")
        self.install_fake_command(
            "cmake",
            'if [[ "$1" == "--build" ]]; then exit 31; fi\n',
        )
        app = self.root / "Output/qt-dev/Src/App/stockBacktester"
        app.parent.mkdir(parents=True)
        app.write_text(
            '#!/usr/bin/env bash\nprintf "started\\n" >> "$BTE_SCRIPT_TEST_LOG"\n',
            encoding="utf-8",
        )
        app.chmod(app.stat().st_mode | stat.S_IXUSR)

        result = self.run_script(script)

        self.assertEqual(result.returncode, 31)
        self.assertNotIn("started", self.logged_commands())
        self.assertNotIn("App SHA-256:", result.stdout)

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

    def install_quality_commands(self) -> None:
        for name in (
            "clang",
            "clang-18",
            "clang++",
            "clang++-18",
            "clang-format",
            "clang-format-18",
            "clang-tidy",
            "clang-tidy-18",
            "cmake",
            "cppcheck",
            "ctest",
            "diff-cover",
            "iwyu_tool.py",
            "python3",
            "scan-build",
            "scan-build-18",
        ):
            version_output = "echo 'clang version 18.1.8'\n" if name in ("clang", "clang-18") else ""
            self.install_fake_command(
                name,
                f"""{version_output}printf "{name} %s\\n" "$*" >> "$BTE_SCRIPT_TEST_LOG"
if [[ -n "${{BTE_FAIL_MATCH:-}}" && "{name} $*" == *"$BTE_FAIL_MATCH"* ]]; then
  exit 23
fi
""",
            )
        self.install_fake_command(
            "git",
            """
printf "git %s\\n" "$*" >> "$BTE_SCRIPT_TEST_LOG"
if [[ "$1" == "rev-parse" && "$*" == *"base-sha"* ]]; then
  echo base-sha
elif [[ "$1" == "rev-parse" ]]; then
  echo head-sha
fi
""",
        )

    def test_run_quality_fast_runs_full_analysis_and_coverage_gates(self) -> None:
        script = self.copy_script("RunQuality.sh")
        self.install_quality_commands()
        environment = self.environment()
        environment["BTE_GCOV_EXECUTABLE"] = "test-gcov"
        environment["BTE_QUALITY_PYTHON"] = "python3"
        environment["BTE_QUALITY_DIFF_COVER"] = "diff-cover"

        result = subprocess.run(
            [
                "bash",
                str(script),
                "--fast",
                "--base",
                "base-sha",
                "--head",
                "head-sha",
            ],
            cwd=Path(self.temporary_directory.name),
            env=environment,
            capture_output=True,
            text=True,
            check=False,
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        commands = self.logged_commands()
        analysis_build = commands.index("cmake --build --preset analysis --parallel")
        clang_tidy = commands.index("python3 Tools/RunStaticAnalysis.py clang-tidy")
        self.assertLess(analysis_build, clang_tidy)
        self.assertIn("python3 Tools/RunStaticAnalysis.py clang-tidy", commands)
        self.assertIn("python3 Tools/RunStaticAnalysis.py cppcheck", commands)
        self.assertIn("python3 Tools/RunStaticAnalysis.py iwyu", commands)
        self.assertIn(
            "python3 Tools/CheckProjectStandards.py --full-tree --clang-format --base base-sha --head head-sha",
            commands,
        )
        self.assertIn("cmake -E remove_directory Output/analysis", commands)
        self.assertIn("ctest --preset coverage --no-tests=error", commands)
        self.assertIn(
            "diff-cover Output/CoverageReport/coverage.xml --compare-branch=base-sha --fail-under=98",
            commands,
        )
        self.assertIn(
            "python3 Tools/CheckDiffBranchCoverage.py Output/CoverageReport/coverage.json --base base-sha --head head-sha --fail-under 90",
            commands,
        )
        self.assertFalse(any(command.startswith("scan-build ") for command in commands))

    def test_run_quality_default_includes_whole_tree_scan_build(self) -> None:
        script = self.copy_script("RunQuality.sh")
        self.install_quality_commands()
        environment = self.environment()
        environment["BTE_GCOV_EXECUTABLE"] = "test-gcov"
        environment["BTE_QUALITY_PYTHON"] = "python3"
        environment["BTE_QUALITY_DIFF_COVER"] = "diff-cover"

        result = subprocess.run(
            ["bash", str(script)],
            cwd=Path(self.temporary_directory.name),
            env=environment,
            capture_output=True,
            text=True,
            check=False,
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(
            any(
                command.startswith(("scan-build --use-analyzer=", "scan-build-18 --use-analyzer="))
                and "--status-bugs --keep-empty" in command
                for command in self.logged_commands()
            )
        )

    def test_run_quality_rejects_dirty_worktree_before_running_tools(self) -> None:
        script = self.copy_script("RunQuality.sh")
        self.install_fake_command(
            "git",
            """
if [[ "$1" == "rev-parse" ]]; then
  echo head-sha
elif [[ "$1" == "status" ]]; then
  echo " M Src/Dirty.cpp"
fi
""",
        )

        result = self.run_script(script)

        self.assertEqual(result.returncode, 2)
        self.assertIn("requires a clean working tree", result.stderr)

    def test_run_quality_propagates_tool_failures_without_success_message(self) -> None:
        script = self.copy_script("RunQuality.sh")
        for failure in (
            "CheckProjectStandards.py",
            "clang-tidy",
            "cppcheck",
            "iwyu",
            "ctest --preset coverage",
            "python3 -m gcovr",
            "diff-cover",
            "CheckDiffBranchCoverage.py",
        ):
            with self.subTest(failure=failure):
                self.bin_directory = self.root / f"FakeBin-{failure.split()[0]}"
                self.bin_directory.mkdir(exist_ok=True)
                self.command_log = self.root / f"commands-{failure.split()[0]}.log"
                self.install_quality_commands()
                environment = self.environment()
                environment["BTE_GCOV_EXECUTABLE"] = "test-gcov"
                environment["BTE_QUALITY_PYTHON"] = "python3"
                environment["BTE_QUALITY_DIFF_COVER"] = "diff-cover"
                environment["BTE_FAIL_MATCH"] = failure

                result = subprocess.run(
                    ["bash", str(script), "--fast"],
                    cwd=self.root,
                    env=environment,
                    capture_output=True,
                    text=True,
                    check=False,
                )

                self.assertEqual(result.returncode, 23, result.stderr)
                self.assertNotIn("Quality checks passed", result.stdout)

    def test_run_quality_rejects_head_other_than_checked_out_commit(self) -> None:
        script = self.copy_script("RunQuality.sh")
        self.install_fake_command(
            "git",
            """
if [[ "$*" == *"other-head"* ]]; then
  echo other-sha
elif [[ "$1" == "rev-parse" ]]; then
  echo head-sha
fi
""",
        )

        result = self.run_script(script, "--head", "other-head")

        self.assertEqual(result.returncode, 2)
        self.assertIn("must resolve to the checked-out HEAD", result.stderr)

    def test_run_quality_rejects_missing_revision_argument(self) -> None:
        script = self.copy_script("RunQuality.sh")

        result = self.run_script(script, "--base")

        self.assertEqual(result.returncode, 2)
        self.assertIn("--base requires a revision", result.stderr)
        self.assertEqual(self.logged_commands(), [])

    def test_run_quality_first_run_creates_private_coverage_environment(self) -> None:
        script = self.copy_script("RunQuality.sh")
        for name in (
            "brew",
            "clang",
            "clang++",
            "clang-format",
            "clang-tidy",
            "cmake",
            "cppcheck",
            "ctest",
            "iwyu_tool.py",
        ):
            body = "exit 1\n" if name == "brew" else (
                "echo 'clang version 18.1.8'\n"
                if name == "clang"
                else
                f'printf "{name} %s\\n" "$*" >> "$BTE_SCRIPT_TEST_LOG"\n'
            )
            self.install_fake_command(name, body)
        self.install_fake_command("xcrun", 'echo "/fake/macOS.sdk"\n')
        self.install_fake_command(
            "git",
            """
if [[ "$1" == "rev-parse" && "$*" == *"origin/main"* ]]; then
  echo base-sha
elif [[ "$1" == "rev-parse" ]]; then
  echo head-sha
fi
""",
        )
        self.install_fake_command(
            "python3.12",
            """
if [[ "${1:-}" == "-m" && "${2:-}" == "venv" ]]; then
  mkdir -p "$3/bin"
  cp "$0" "$3/bin/python"
  printf '#!/usr/bin/env bash\nprintf "diff-cover %%s\\n" "$*" >> "$BTE_SCRIPT_TEST_LOG"\n' > "$3/bin/diff-cover"
  chmod +x "$3/bin/diff-cover"
  exit 0
fi
printf "python3.12 %s\\n" "$*" >> "$BTE_SCRIPT_TEST_LOG"
""",
        )
        tools_directory = self.root / "Tools"
        tools_directory.mkdir()
        (tools_directory / "CoverageRequirements.txt").write_text(
            "example==1.0 --hash=sha256:abc\n", encoding="utf-8"
        )
        environment = self.environment()
        environment["BTE_QUALITY_OS"] = "Darwin"
        environment["BTE_GCOV_EXECUTABLE"] = "test-gcov"

        result = subprocess.run(
            ["bash", str(script), "--fast"],
            cwd=Path(self.temporary_directory.name),
            env=environment,
            capture_output=True,
            text=True,
            check=False,
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue((self.root / "Output/QualityVenv/bin/python").is_file())
        self.assertIn("First-run setup: creating the locked coverage environment", result.stdout)

    def test_run_quality_first_run_installs_missing_macos_analyzers(self) -> None:
        script = self.copy_script("RunQuality.sh")
        self.install_fake_command("clang", "echo 'clang version 17.0.0'\n")
        self.install_fake_command("clang++", "exit 0\n")
        self.install_fake_command("clang-tidy", "exit 0\n")
        self.install_fake_command(
            "git",
            """
if [[ "$1" == "rev-parse" && "$*" == *"origin/main"* ]]; then
  echo base-sha
elif [[ "$1" == "rev-parse" ]]; then
  echo head-sha
fi
""",
        )
        self.install_fake_command(
            "brew",
            """
printf "brew %s\\n" "$*" >> "$BTE_SCRIPT_TEST_LOG"
if [[ "$1" == "--prefix" ]]; then
  exit 1
fi
exit 29
""",
        )
        environment = self.environment()
        environment["BTE_QUALITY_OS"] = "Darwin"

        result = subprocess.run(
            ["bash", str(script), "--fast"],
            cwd=Path(self.temporary_directory.name),
            env=environment,
            capture_output=True,
            text=True,
            check=False,
        )

        self.assertEqual(result.returncode, 29)
        self.assertTrue(
            any(
                command.startswith("brew install ") and "llvm@18" in command
                for command in self.logged_commands()
            )
        )

    def test_coverage_gates_publish_summary_and_preserve_failures(self) -> None:
        tools_directory = self.root / "Tools"
        tools_directory.mkdir()
        script = tools_directory / "RunCoverageGates.sh"
        shutil.copy2(REPOSITORY_ROOT / "Tools/RunCoverageGates.sh", script)
        (self.root / "coverage.xml").touch()
        (self.root / "coverage.json").write_text("{}", encoding="utf-8")
        (self.root / "summary.md").write_text("# Coverage\n", encoding="utf-8")
        self.install_fake_command(
            "diff-cover",
            'printf "diff-cover %s\\n" "$*" >> "$BTE_SCRIPT_TEST_LOG"\necho "line gate"\nexit "${BTE_LINE_STATUS:-0}"\n',
        )
        self.install_fake_command(
            "python3",
            'printf "python3 %s\\n" "$*" >> "$BTE_SCRIPT_TEST_LOG"\necho "branch gate"\nexit "${BTE_BRANCH_STATUS:-0}"\n',
        )

        for line_status, branch_status, expected in (
            (0, 0, 0),
            (1, 0, 1),
            (0, 1, 1),
            (1, 1, 1),
        ):
            with self.subTest(line_status=line_status, branch_status=branch_status):
                summary_output = self.root / f"job-{line_status}-{branch_status}.md"
                environment = self.environment()
                environment["BTE_LINE_STATUS"] = str(line_status)
                environment["BTE_BRANCH_STATUS"] = str(branch_status)
                environment["GITHUB_STEP_SUMMARY"] = str(summary_output)
                result = subprocess.run(
                    [
                        "bash",
                        str(script),
                        "coverage.xml",
                        "coverage.json",
                        "base-sha",
                        "head-sha",
                        "commit-sha",
                        "summary.md",
                    ],
                    cwd=self.root,
                    env=environment,
                    capture_output=True,
                    text=True,
                    check=False,
                )

                self.assertEqual(result.returncode, expected, result.stderr)
                rendered = summary_output.read_text(encoding="utf-8")
                self.assertLess(rendered.index("# Coverage"), rendered.index("### Changed-code gates"))
                self.assertIn("line gate", rendered)
                self.assertIn("branch gate", rendered)
                self.assertIn("Report commit: `commit-sha`", rendered)
                self.assertIn(
                    "diff-cover coverage.xml --compare-branch=base-sha --fail-under=98",
                    self.logged_commands(),
                )
                self.assertIn(
                    "python3 Tools/CheckDiffBranchCoverage.py coverage.json --base base-sha --head head-sha --fail-under 90",
                    self.logged_commands(),
                )


if __name__ == "__main__":
    unittest.main()
