"""Tests for the locked, offline Python runtime assembly tools."""

from __future__ import annotations

import hashlib
import importlib.util
import io
import json
import subprocess
import sys
import tarfile
import tempfile
import unittest
import zipfile
from pathlib import Path


REPOSITORY_ROOT = Path(__file__).resolve().parents[3]
TOOLS_ROOT = REPOSITORY_ROOT / "Tools"
BUILDER_PATH = TOOLS_ROOT / "BuildPythonRuntime.py"
LOCK_PATH = TOOLS_ROOT / "PythonRuntimeArtifacts.json"
STOCKBT_PATH = REPOSITORY_ROOT / "Src" / "Bindings" / "Python" / "stockbt" / "__init__.py"
WORKER_BOOTSTRAP_PATH = REPOSITORY_ROOT / "Src" / "Bindings" / "Python" / "worker_bootstrap.py"
PROFILE_ID = "macos-arm64-cpython-3.14.9-stockbt-v1"
ARTIFACTS = REPOSITORY_ROOT / "Output" / "PythonRuntimeArtifacts"


def load_module(path: Path, name: str):
    if not path.is_file():
        return None
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        return None
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


BUILDER = load_module(BUILDER_PATH, "BuildPythonRuntime")
STOCKBT = load_module(STOCKBT_PATH, "stockbt_runtime_test")
WORKER_BOOTSTRAP = load_module(WORKER_BOOTSTRAP_PATH, "worker_bootstrap_runtime_test")


class PythonRuntimeBuilderTest(unittest.TestCase):
    def run_builder(self, artifacts: Path, output: Path) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [
                sys.executable,
                str(BUILDER_PATH),
                "--artifacts-dir",
                str(artifacts),
                "--output-dir",
                str(output),
            ],
            check=False,
            capture_output=True,
            text=True,
        )

    def test_lock_contains_all_exactly_approved_components(self) -> None:
        self.assertTrue(LOCK_PATH.is_file(), "runtime artifact lock is required")
        lock = json.loads(LOCK_PATH.read_text(encoding="utf-8"))
        self.assertEqual(lock["profileId"], PROFILE_ID)
        artifacts = lock["artifacts"]
        self.assertEqual(
            {artifact["name"] for artifact in artifacts},
            {
                "CPython",
                "numpy",
                "pandas",
                "python-dateutil",
                "six",
                "tzdata",
            },
        )
        self.assertTrue(
            all(len(artifact["sha256"]) == 64 for artifact in artifacts)
        )

    def test_lock_rejects_unknown_or_changed_license_declarations(self) -> None:
        self.assertIsNotNone(BUILDER, "runtime artifact lock validation is required")
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            lock = json.loads(LOCK_PATH.read_text(encoding="utf-8"))
            lock["artifacts"][0]["licenses"] = ["Unreviewed-Unknown-License"]
            candidate = root / "runtime-lock.json"
            candidate.write_text(json.dumps(lock), encoding="utf-8")

            with self.assertRaisesRegex(ValueError, "unapproved license declaration"):
                BUILDER.load_lock(candidate)

    def test_builder_reports_missing_locked_artifact_without_output(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            artifacts = root / "artifacts"
            artifacts.mkdir()
            output = root / PROFILE_ID

            result = self.run_builder(artifacts, output)

            self.assertEqual(result.returncode, 1, result.stderr)
            self.assertIn("Missing locked runtime artifact", result.stderr)
            self.assertFalse(output.exists())

    def test_builder_rejects_hash_mismatch_before_output(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            artifacts = root / "artifacts"
            artifacts.mkdir()
            cpython_name = (
                "cpython-3.14.9+20261021-aarch64-apple-darwin-"
                "install_only.tar.gz"
            )
            (artifacts / cpython_name).write_bytes(b"not the pinned archive")
            output = root / PROFILE_ID

            result = self.run_builder(artifacts, output)

            self.assertEqual(result.returncode, 1, result.stderr)
            self.assertIn("SHA-256 mismatch", result.stderr)
            self.assertFalse(output.exists())

    def test_archive_extractor_rejects_path_traversal(self) -> None:
        self.assertIsNotNone(BUILDER, "runtime archive extractor is required")
        extractor = getattr(BUILDER, "safe_extract_tarball", None)
        self.assertTrue(callable(extractor), "safe tar extraction is required")
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            archive = root / "unsafe.tar.gz"
            destination = root / "extract"
            with tarfile.open(archive, "w:gz") as bundle:
                member = tarfile.TarInfo("python/../../outside.txt")
                payload = b"escape"
                member.size = len(payload)
                bundle.addfile(member, io.BytesIO(payload))

            with self.assertRaisesRegex(ValueError, "unsafe archive path"):
                extractor(archive, destination)

            self.assertFalse((root / "outside.txt").exists())

    def test_wheel_installer_rejects_path_traversal(self) -> None:
        self.assertIsNotNone(BUILDER, "runtime wheel installer is required")
        installer = getattr(BUILDER, "install_wheel", None)
        self.assertTrue(callable(installer), "safe wheel installation is required")
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            wheel = root / "unsafe.whl"
            destination = root / "site-packages"
            with zipfile.ZipFile(wheel, "w") as archive:
                archive.writestr("../../outside.py", "malicious = True\n")

            with self.assertRaisesRegex(ValueError, "unsafe wheel path"):
                installer(wheel, destination, expected_name="demo", expected_version="1")

            self.assertFalse((root / "outside.py").exists())

    def test_locked_artifacts_build_a_working_runtime_when_cache_is_available(self) -> None:
        self.assertTrue(LOCK_PATH.is_file(), "runtime artifact lock is required")
        lock = json.loads(LOCK_PATH.read_text(encoding="utf-8"))
        missing = [
            item["filename"]
            for item in lock["artifacts"]
            if not (ARTIFACTS / item["filename"]).is_file()
        ]
        if missing:
            self.skipTest(f"offline runtime artifacts are not cached: {missing}")

        with tempfile.TemporaryDirectory() as temporary_directory:
            output = Path(temporary_directory) / PROFILE_ID
            result = self.run_builder(ARTIFACTS, output)

            self.assertEqual(result.returncode, 0, result.stderr)
            manifest = json.loads((output / "manifest.json").read_text(encoding="utf-8"))
            self.assertEqual(manifest["profileId"], PROFILE_ID)
            self.assertEqual(
                {component["name"] for component in manifest["components"]},
                {
                    "CPython",
                    "numpy",
                    "pandas",
                    "python-dateutil",
                    "six",
                    "tzdata",
                    "stockbt",
                },
            )
            self.assertTrue((output / "sbom.spdx.json").is_file())
            self.assertFalse((output / "python" / "bin" / "pip").exists())
            self.assertFalse(
                (output / "python" / "lib" / "python3.14" / "ensurepip").exists()
            )

    def test_bundle_verifier_rejects_missing_profile_or_worker_bootstrap(self) -> None:
        self.assertIsNotNone(BUILDER, "runtime bundle verifier is required")
        verifier = getattr(BUILDER, "verify_app_bundle", None)
        self.assertTrue(callable(verifier), "application bundle verification is required")
        with tempfile.TemporaryDirectory() as temporary_directory:
            app_bundle = Path(temporary_directory) / "StockBacktester.app"
            resources = app_bundle / "Contents" / "Resources"
            resources.mkdir(parents=True)

            with self.assertRaisesRegex(ValueError, "worker bootstrap is missing"):
                verifier(app_bundle, BUILDER.load_lock())


class StockbtPackageTest(unittest.TestCase):
    def test_sma_descriptor_is_read_only_and_accepts_supported_boundaries(self) -> None:
        self.assertIsNotNone(STOCKBT, "managed stockbt package is required")
        self.assertEqual(STOCKBT.STOCKBT_API_VERSION, "1")
        self.assertEqual(STOCKBT.__version__, "1")
        self.assertEqual(dict(STOCKBT.SMA("fast", period=1)), {
            "kind": "SMA",
            "name": "fast",
            "period": 1,
        })
        self.assertEqual(dict(STOCKBT.SMA("slow", period=10_000)), {
            "kind": "SMA",
            "name": "slow",
            "period": 10_000,
        })
        with self.assertRaises(TypeError):
            STOCKBT.SMA("fast", period=True)
        with self.assertRaises(ValueError):
            STOCKBT.SMA("fast", period=0)
        with self.assertRaises(ValueError):
            STOCKBT.SMA("fast", period=10_001)
        with self.assertRaises(TypeError):
            STOCKBT.SMA("fast", period=1)["period"] = 2

    def test_sma_rejects_invalid_names(self) -> None:
        self.assertIsNotNone(STOCKBT, "managed stockbt package is required")
        for name in ("", "平均"):
            with self.subTest(name=name), self.assertRaises(ValueError):
                STOCKBT.SMA(name, period=5)


class PythonWorkerDiagnosticBoundsTest(unittest.TestCase):
    def test_traceback_truncation_preserves_utf8_and_respects_byte_limit(self) -> None:
        self.assertIsNotNone(WORKER_BOOTSTRAP, "Python worker bootstrap is required")
        truncate = getattr(WORKER_BOOTSTRAP, "_bounded_traceback", None)
        self.assertTrue(callable(truncate), "bounded traceback helper is required")
        for message in ("😀" * 20_000, "中" * 20_000):
            with self.subTest(character=message[0]):
                text, omitted = truncate([message], 48 * 1024)
                encoded = text.encode("utf-8")
                self.assertLessEqual(len(encoded), 48 * 1024)
                self.assertEqual(encoded.decode("utf-8"), text)
                self.assertGreater(omitted, 0)
                self.assertNotIn("�", text)

class PythonWorkerManagedPackageTest(unittest.TestCase):
    def test_managed_worker_loads_the_project_owned_stockbt_package(self) -> None:
        self.assertIsNotNone(WORKER_BOOTSTRAP, "worker bootstrap is required")
        sys.path.insert(0, str(STOCKBT_PATH.parent.parent))
        try:
            WORKER_BOOTSTRAP._install_stockbt_module(require_managed=True)
        finally:
            sys.path.pop(0)
        import stockbt

        self.assertEqual(Path(stockbt.__file__).resolve(), STOCKBT_PATH.resolve())
        self.assertEqual(stockbt.STOCKBT_API_VERSION, "1")

    def test_managed_worker_fails_closed_when_stockbt_is_missing(self) -> None:
        script = "\n".join(
            [
                "import importlib.util",
                "import sys",
                f"spec = importlib.util.spec_from_file_location('worker_bootstrap', {str(WORKER_BOOTSTRAP_PATH)!r})",
                "worker = importlib.util.module_from_spec(spec)",
                "sys.modules[spec.name] = worker",
                "spec.loader.exec_module(worker)",
                "try:",
                "    worker._install_stockbt_module(require_managed=True)",
                "except worker.WorkerFailure as error:",
                "    assert error.code == 'RuntimeUnavailable'",
                "else:",
                "    raise AssertionError('worker accepted missing managed stockbt')",
            ]
        )
        result = subprocess.run(
            [sys.executable, "-I", "-B", "-c", script],
            check=False,
            capture_output=True,
            text=True,
        )

        self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
