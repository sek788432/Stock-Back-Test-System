"""Assemble and verify the locked, application-managed macOS Python profile."""

from __future__ import annotations

import argparse
import email.parser
import email.policy
import hashlib
import json
import os
import platform
import posixpath
import re
import shutil
import stat
import subprocess
import sys
import tarfile
import tempfile
import urllib.parse
import urllib.request
import zipfile
from datetime import datetime, timezone
from pathlib import Path, PurePosixPath
from typing import Any


PROJECT_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_LOCK_PATH = Path(__file__).with_name("PythonRuntimeArtifacts.json")
PROFILE_MANIFEST_LIMIT = 1024 * 1024
MAXIMUM_ARTIFACT_BYTES = 512 * 1024 * 1024
MAXIMUM_EXPANDED_BYTES = 1024 * 1024 * 1024
MAXIMUM_WHEEL_ENTRIES = 100_000
ALLOWED_ARTIFACT_NAMES = {
    "CPython",
    "numpy",
    "pandas",
    "python-dateutil",
    "six",
    "tzdata",
}
APPROVED_ARTIFACT_LICENSES = {
    "CPython": ["PSF-2.0"],
    "numpy": ["BSD-3-Clause"],
    "pandas": ["BSD-3-Clause"],
    "python-dateutil": ["Apache-2.0", "BSD-3-Clause"],
    "six": ["MIT"],
    "tzdata": ["Apache-2.0"],
}


class RuntimeBuildError(ValueError):
    """A locked runtime artifact or generated profile failed validation."""


def canonical_json(value: Any) -> bytes:
    """Serialize the manifest's restricted JSON values in canonical form."""
    return json.dumps(
        value,
        ensure_ascii=False,
        separators=(",", ":"),
        sort_keys=True,
    ).encode("utf-8")


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        while chunk := source.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def _normalized_package_name(value: str) -> str:
    return re.sub(r"[-_.]+", "-", value).lower()


def load_lock(lock_path: Path = DEFAULT_LOCK_PATH) -> dict[str, Any]:
    try:
        lock = json.loads(lock_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise RuntimeBuildError(f"Could not read the runtime artifact lock: {error}") from error
    if not isinstance(lock, dict) or lock.get("schemaVersion") != 1:
        raise RuntimeBuildError("Runtime artifact lock has an unsupported schema.")
    if (
        lock.get("profileId") != "macos-arm64-cpython-3.14.9-stockbt-v1"
        or lock.get("platform") != "macos"
        or lock.get("architecture") != "arm64"
        or lock.get("pythonVersion") != "3.14.9"
        or lock.get("pythonAbi") != "cp314"
        or lock.get("pythonExecutable") != "python/bin/python3"
        or lock.get("stockbtApiVersion") != "1"
        or not isinstance(lock.get("profileBuilderVersion"), str)
    ):
        raise RuntimeBuildError("Runtime artifact lock differs from the accepted profile.")
    artifacts = lock.get("artifacts")
    if not isinstance(artifacts, list) or {
        artifact.get("name") for artifact in artifacts if isinstance(artifact, dict)
    } != ALLOWED_ARTIFACT_NAMES:
        raise RuntimeBuildError("Runtime artifact lock contains missing or unapproved components.")
    filenames: set[str] = set()
    for artifact in artifacts:
        if (
            not isinstance(artifact.get("filename"), str)
            or Path(artifact["filename"]).name != artifact["filename"]
            or not isinstance(artifact.get("version"), str)
            or not isinstance(artifact.get("sha256"), str)
            or re.fullmatch(r"[0-9a-f]{64}", artifact["sha256"]) is None
            or not isinstance(artifact.get("licenses"), list)
            or not artifact["licenses"]
            or not isinstance(artifact.get("source"), dict)
            or not isinstance(artifact.get("upstreamUrl"), str)
        ):
            raise RuntimeBuildError("Runtime artifact lock contains an invalid component entry.")
        if artifact["licenses"] != APPROVED_ARTIFACT_LICENSES[artifact["name"]]:
            raise RuntimeBuildError(
                f"Runtime artifact lock contains an unapproved license declaration "
                f"for {artifact['name']}."
            )
        if artifact["filename"] in filenames:
            raise RuntimeBuildError("Runtime artifact lock contains a duplicate filename.")
        filenames.add(artifact["filename"])
    return lock


def verify_artifact_cache(artifacts_directory: Path, lock: dict[str, Any]) -> None:
    for artifact in lock["artifacts"]:
        path = artifacts_directory / artifact["filename"]
        if not path.is_file() or path.is_symlink():
            raise RuntimeBuildError(
                f"Missing locked runtime artifact: {artifact['filename']}"
            )
        actual = sha256_file(path)
        if actual != artifact["sha256"]:
            raise RuntimeBuildError(
                f"SHA-256 mismatch for {artifact['filename']}: expected "
                f"{artifact['sha256']}, received {actual}"
            )


def _download_url(artifact: dict[str, Any]) -> str:
    source = artifact["source"]
    if source.get("kind") == "url":
        url = source.get("url")
        parsed = urllib.parse.urlparse(url or "")
        if parsed.scheme != "https" or parsed.hostname != "github.com":
            raise RuntimeBuildError("The CPython source is not an approved HTTPS URL.")
        return url
    if source.get("kind") != "pypi":
        raise RuntimeBuildError("Runtime artifact uses an unsupported download source.")

    project = urllib.parse.quote(source["project"], safe="")
    version = urllib.parse.quote(source["version"], safe="")
    metadata_url = f"https://pypi.org/pypi/{project}/{version}/json"
    try:
        with urllib.request.urlopen(metadata_url, timeout=60) as response:
            metadata = json.load(response)
    except (OSError, ValueError) as error:
        raise RuntimeBuildError(
            f"Could not read pinned PyPI metadata for {artifact['filename']}: {error}"
        ) from error
    matches = [
        item
        for item in metadata.get("urls", [])
        if item.get("filename") == artifact["filename"]
    ]
    if (
        len(matches) != 1
        or matches[0].get("digests", {}).get("sha256") != artifact["sha256"]
    ):
        raise RuntimeBuildError(
            f"PyPI metadata does not match the lock for {artifact['filename']}"
        )
    url = matches[0].get("url", "")
    parsed = urllib.parse.urlparse(url)
    if parsed.scheme != "https" or parsed.hostname != "files.pythonhosted.org":
        raise RuntimeBuildError("PyPI returned an unapproved artifact host.")
    return url


def download_artifacts(
    artifacts_directory: Path,
    lock: dict[str, Any],
) -> None:
    artifacts_directory.mkdir(parents=True, exist_ok=True)
    for artifact in lock["artifacts"]:
        destination = artifacts_directory / artifact["filename"]
        if destination.exists():
            if destination.is_symlink() or not destination.is_file():
                raise RuntimeBuildError(
                    f"Artifact cache entry is not a regular file: {artifact['filename']}"
                )
            actual = sha256_file(destination)
            if actual != artifact["sha256"]:
                raise RuntimeBuildError(
                    f"SHA-256 mismatch for cached artifact {artifact['filename']}: {actual}"
                )
            continue

        url = _download_url(artifact)
        temporary_path: Path | None = None
        digest = hashlib.sha256()
        byte_count = 0
        try:
            with urllib.request.urlopen(url, timeout=120) as response:
                with tempfile.NamedTemporaryFile(
                    mode="wb",
                    dir=artifacts_directory,
                    prefix=f".{artifact['filename']}.",
                    suffix=".download",
                    delete=False,
                ) as output:
                    temporary_path = Path(output.name)
                    while chunk := response.read(1024 * 1024):
                        byte_count += len(chunk)
                        if byte_count > MAXIMUM_ARTIFACT_BYTES:
                            raise RuntimeBuildError(
                                f"Runtime artifact exceeds the download limit: {artifact['filename']}"
                            )
                        digest.update(chunk)
                        output.write(chunk)
            if digest.hexdigest() != artifact["sha256"]:
                raise RuntimeBuildError(
                    f"SHA-256 mismatch for downloaded artifact {artifact['filename']}"
                )
            os.replace(temporary_path, destination)
            temporary_path = None
        except OSError as error:
            raise RuntimeBuildError(
                f"Could not download {artifact['filename']}: {error}"
            ) from error
        finally:
            if temporary_path is not None:
                temporary_path.unlink(missing_ok=True)
        print(f"verified {artifact['filename']} ({byte_count} bytes)")


def _safe_member_parts(value: str, description: str) -> tuple[str, ...]:
    if not value or "\x00" in value or "\\" in value or value.startswith("/"):
        raise RuntimeBuildError(f"unsafe {description} path: {value!r}")
    parts = value.split("/")
    if parts[-1] == "":
        parts.pop()
    if (
        not parts
        or any(part in {"", ".", ".."} for part in parts)
        or ":" in parts[0]
    ):
        raise RuntimeBuildError(f"unsafe {description} path: {value!r}")
    return tuple(parts)


def _safe_link_target(member_parts: tuple[str, ...], target: str) -> str:
    if not target or "\x00" in target or "\\" in target or target.startswith("/"):
        raise RuntimeBuildError(f"unsafe archive link target: {target!r}")
    resolved_parts = list(member_parts[:-1])
    for part in target.split("/"):
        if part in {"", "."}:
            continue
        if part == "..":
            if not resolved_parts:
                raise RuntimeBuildError(f"unsafe archive link target: {target!r}")
            resolved_parts.pop()
        else:
            resolved_parts.append(part)
    if not resolved_parts or resolved_parts[0] != "python":
        raise RuntimeBuildError(f"unsafe archive link target: {target!r}")
    return "/".join(resolved_parts)


def safe_extract_tarball(archive_path: Path, destination: Path) -> None:
    """Extract the locked standalone archive without trusting member paths."""
    with tarfile.open(archive_path, "r:gz") as archive:
        members = archive.getmembers()
        if not members:
            raise RuntimeBuildError("Python runtime archive is empty.")
        normalized: list[tuple[tarfile.TarInfo, tuple[str, ...]]] = []
        seen: set[str] = set()
        symlink_names: set[str] = set()
        expanded_size = 0
        for member in members:
            raw_name = member.name[:-1] if member.isdir() and member.name.endswith("/") else member.name
            parts = _safe_member_parts(raw_name, "archive")
            if parts[0] != "python":
                raise RuntimeBuildError(f"unsupported archive root: {member.name!r}")
            normalized_name = "/".join(parts)
            if normalized_name in seen:
                raise RuntimeBuildError(f"duplicate archive path: {normalized_name}")
            seen.add(normalized_name)
            if not (member.isdir() or member.isfile() or member.issym()):
                raise RuntimeBuildError(f"unsupported archive entry type: {member.name!r}")
            if member.isfile():
                expanded_size += member.size
            if member.issym():
                _safe_link_target(parts, member.linkname)
                symlink_names.add(normalized_name)
            normalized.append((member, parts))
        if expanded_size > MAXIMUM_EXPANDED_BYTES:
            raise RuntimeBuildError("Python runtime archive exceeds the extraction limit.")
        for _, parts in normalized:
            for index in range(1, len(parts)):
                if "/".join(parts[:index]) in symlink_names:
                    raise RuntimeBuildError("archive path descends through a symbolic link")

        destination.mkdir(parents=True, exist_ok=False)
        for member, parts in normalized:
            if member.isdir():
                (destination.joinpath(*parts)).mkdir(parents=True, exist_ok=True)
        for member, parts in normalized:
            if not member.isfile():
                continue
            target = destination.joinpath(*parts)
            target.parent.mkdir(parents=True, exist_ok=True)
            source = archive.extractfile(member)
            if source is None:
                raise RuntimeBuildError(f"could not read archive file: {member.name}")
            with source, target.open("xb") as output:
                shutil.copyfileobj(source, output)
            target.chmod(member.mode & 0o777)
        for member, parts in normalized:
            if member.issym():
                target = destination.joinpath(*parts)
                target.parent.mkdir(parents=True, exist_ok=True)
                os.symlink(member.linkname, target)
        canonical_destination = destination.resolve(strict=True)
        for member, parts in normalized:
            if not member.issym():
                continue
            link_path = destination.joinpath(*parts)
            try:
                resolved = link_path.resolve(strict=True)
            except OSError as error:
                raise RuntimeBuildError(f"broken archive link: {member.name}") from error
            if not resolved.is_relative_to(canonical_destination):
                raise RuntimeBuildError(f"archive link escapes profile: {member.name}")


def _wheel_path_parts(name: str) -> tuple[str, ...]:
    return _safe_member_parts(name, "wheel")


def install_wheel(
    wheel_path: Path,
    site_packages: Path,
    *,
    expected_name: str,
    expected_version: str,
) -> None:
    """Install a pinned wheel by copying its approved package files offline."""
    with zipfile.ZipFile(wheel_path) as wheel:
        infos = wheel.infolist()
        if len(infos) > MAXIMUM_WHEEL_ENTRIES:
            raise RuntimeBuildError(f"wheel contains too many entries: {wheel_path.name}")
        expanded_size = sum(info.file_size for info in infos)
        if expanded_size > MAXIMUM_EXPANDED_BYTES:
            raise RuntimeBuildError(f"wheel exceeds the extraction limit: {wheel_path.name}")
        metadata_entries: list[zipfile.ZipInfo] = []
        installed: list[tuple[zipfile.ZipInfo, tuple[str, ...]]] = []
        output_names: set[str] = set()
        for info in infos:
            if info.is_dir():
                continue
            parts = _wheel_path_parts(info.filename)
            mode = info.external_attr >> 16
            kind = stat.S_IFMT(mode)
            if kind not in {0, stat.S_IFREG}:
                raise RuntimeBuildError(f"unsupported wheel entry type: {info.filename!r}")
            if len(parts) >= 2 and parts[0].endswith(".data"):
                if len(parts) < 3 or parts[1] not in {"purelib", "platlib"}:
                    raise RuntimeBuildError(f"unsupported wheel data scheme: {info.filename!r}")
                installed_parts = parts[2:]
            else:
                installed_parts = parts
            installed_name = "/".join(installed_parts)
            if installed_name in output_names:
                raise RuntimeBuildError(f"duplicate wheel installation path: {installed_name}")
            output_names.add(installed_name)
            if len(parts) >= 2 and parts[-2].endswith(".dist-info") and parts[-1] == "METADATA":
                metadata_entries.append(info)
            installed.append((info, installed_parts))
        if len(metadata_entries) != 1:
            raise RuntimeBuildError(f"wheel metadata is missing or duplicated: {wheel_path.name}")
        try:
            metadata_bytes = wheel.read(metadata_entries[0])
            metadata = email.parser.BytesParser(policy=email.policy.default).parsebytes(
                metadata_bytes
            )
        except (OSError, ValueError, zipfile.BadZipFile) as error:
            raise RuntimeBuildError(f"wheel metadata is invalid: {wheel_path.name}") from error
        if (
            _normalized_package_name(metadata.get("Name", ""))
            != _normalized_package_name(expected_name)
            or metadata.get("Version") != expected_version
        ):
            raise RuntimeBuildError(f"wheel identity does not match its lock: {wheel_path.name}")
        site_packages.mkdir(parents=True, exist_ok=True)
        try:
            if wheel.testzip() is not None:
                raise RuntimeBuildError(f"wheel CRC check failed: {wheel_path.name}")
            for info, parts in installed:
                target = site_packages.joinpath(*parts)
                target.parent.mkdir(parents=True, exist_ok=True)
                if target.exists() or target.is_symlink():
                    raise RuntimeBuildError(f"wheel would overwrite a runtime file: {target}")
                with wheel.open(info) as source, target.open("xb") as output:
                    shutil.copyfileobj(source, output)
                target.chmod(0o755 if (info.external_attr >> 16) & stat.S_IXUSR else 0o644)
        except (OSError, zipfile.BadZipFile) as error:
            raise RuntimeBuildError(f"could not install wheel {wheel_path.name}: {error}") from error


def _strip_install_tools(python_root: Path) -> None:
    version_root = python_root / "lib" / "python3.14"
    site_packages = version_root / "site-packages"
    shutil.rmtree(site_packages, ignore_errors=True)
    shutil.rmtree(version_root / "ensurepip", ignore_errors=True)
    shutil.rmtree(version_root / "venv", ignore_errors=True)
    shutil.rmtree(version_root / "idlelib", ignore_errors=True)
    shutil.rmtree(version_root / "pydoc_data", ignore_errors=True)
    shutil.rmtree(version_root / "test", ignore_errors=True)
    shutil.rmtree(version_root / "tkinter", ignore_errors=True)
    shutil.rmtree(version_root / "turtledemo", ignore_errors=True)
    shutil.rmtree(python_root / "include", ignore_errors=True)
    shutil.rmtree(python_root / "lib" / "pkgconfig", ignore_errors=True)
    shutil.rmtree(python_root / "share" / "man", ignore_errors=True)
    binary_directory = python_root / "bin"
    for name in ("pip", "pip3", "pip3.14", "idle3", "idle3.14", "pydoc3", "pydoc3.14"):
        (binary_directory / name).unlink(missing_ok=True)
    for path in python_root.rglob("*"):
        if path.name == "__pycache__":
            shutil.rmtree(path, ignore_errors=True)
        elif path.suffix in {".pyc", ".pyo"}:
            path.unlink(missing_ok=True)


def _copy_notices(
    profile_root: Path,
    lock: dict[str, Any],
    stockbt_license: Path,
) -> None:
    licenses_root = profile_root / "Licenses"
    licenses_root.mkdir()
    cpython_license = profile_root / "python" / "lib" / "python3.14" / "LICENSE.txt"
    if not cpython_license.is_file():
        raise RuntimeBuildError("CPython license notice is missing from its archive.")
    shutil.copy2(cpython_license, licenses_root / "CPython-LICENSE.txt")
    if not stockbt_license.is_file():
        raise RuntimeBuildError("Project license is missing for stockbt.")
    shutil.copy2(stockbt_license, licenses_root / "stockbt-LICENSE.txt")
    for artifact in lock["artifacts"]:
        if artifact["name"] == "CPython":
            continue
        normalized_name = _normalized_package_name(artifact["name"])
        package_root = (
            profile_root / "python" / "lib" / "python3.14" / "site-packages"
        )
        distributions = [
            path
            for path in package_root.glob("*.dist-info")
            if path.is_dir()
            and path.joinpath("METADATA").is_file()
            and _normalized_package_name(
                email.parser.BytesParser(policy=email.policy.default)
                .parsebytes(path.joinpath("METADATA").read_bytes())
                .get("Name", "")
            )
            == normalized_name
        ]
        if len(distributions) != 1:
            raise RuntimeBuildError(f"installed metadata is missing for {artifact['name']}")
        notice_files = [
            path
            for path in distributions[0].rglob("*")
            if path.is_file()
            and re.search(r"(license|copying|notice|copyright)", path.name, re.IGNORECASE)
        ]
        if not notice_files:
            raise RuntimeBuildError(f"license notice is missing for {artifact['name']}")
        destination = licenses_root / artifact["name"]
        for source in notice_files:
            relative = source.relative_to(distributions[0])
            notice_target = destination / relative
            notice_target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, notice_target)

    notice_lines = [
        "# Third-party runtime notices",
        "",
        "This managed runtime is assembled from the exact artifacts in the project lock.",
        "Retained full license texts are listed below.",
        "",
        "| Component | Version | SPDX license | Notice files |",
        "|---|---|---|---|",
        "| CPython | 3.14.9 | PSF-2.0 | `Licenses/CPython-LICENSE.txt` |",
        "| stockbt | 1 | Apache-2.0 | `Licenses/stockbt-LICENSE.txt` |",
    ]
    for artifact in lock["artifacts"]:
        if artifact["name"] == "CPython":
            continue
        files = sorted(
            path.relative_to(profile_root).as_posix()
            for path in (licenses_root / artifact["name"]).rglob("*")
            if path.is_file()
        )
        notice_lines.append(
            f"| {artifact['name']} | {artifact['version']} | "
            f"{' OR '.join(artifact['licenses'])} | "
            f"{', '.join(f'`{path}`' for path in files)} |"
        )
    (licenses_root / "THIRD_PARTY_NOTICES.md").write_text(
        "\n".join(notice_lines) + "\n", encoding="utf-8", newline="\n"
    )


def _spdx_document(lock: dict[str, Any], stockbt_hash: str) -> dict[str, Any]:
    components = []
    for artifact in lock["artifacts"]:
        source = artifact["source"]
        download_location = (
            source["url"]
            if source["kind"] == "url"
            else f"https://pypi.org/project/{source['project']}/{source['version']}/"
        )
        components.append(
            {
                "name": artifact["name"],
                "versionInfo": artifact["version"],
                "SPDXID": "SPDXRef-Package-" + re.sub(r"[^A-Za-z0-9.-]", "-", artifact["name"]),
                "downloadLocation": download_location,
                "filesAnalyzed": False,
                "licenseConcluded": "NOASSERTION",
                "licenseDeclared": " OR ".join(artifact["licenses"]),
                "checksums": [
                    {"algorithm": "SHA256", "checksumValue": artifact["sha256"]}
                ],
                "externalRefs": (
                    [
                        {
                            "referenceCategory": "PACKAGE-MANAGER",
                            "referenceType": "purl",
                            "referenceLocator": (
                                f"pkg:pypi/{source['project']}@{source['version']}"
                            ),
                        }
                    ]
                    if source["kind"] == "pypi"
                    else []
                ),
            }
        )
    components.append(
        {
            "name": "stockbt",
            "versionInfo": lock["stockbtApiVersion"],
            "SPDXID": "SPDXRef-Package-stockbt",
            "downloadLocation": "NOASSERTION",
            "filesAnalyzed": False,
            "licenseConcluded": "NOASSERTION",
            "licenseDeclared": "Apache-2.0",
            "checksums": [{"algorithm": "SHA256", "checksumValue": stockbt_hash}],
        }
    )
    component_digest = hashlib.sha256(canonical_json(components)).hexdigest()
    epoch = int(os.environ.get("SOURCE_DATE_EPOCH", "0"))
    if epoch < 0:
        raise RuntimeBuildError("SOURCE_DATE_EPOCH must not be negative.")
    created = datetime.fromtimestamp(epoch, timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    return {
        "spdxVersion": "SPDX-2.3",
        "dataLicense": "CC0-1.0",
        "SPDXID": "SPDXRef-DOCUMENT",
        "name": f"Stock Back Test Python Runtime {lock['profileId']}",
        "documentNamespace": (
            "https://spdx.org/spdxdocs/stock-back-test-python-runtime-"
            f"{lock['profileId']}-{component_digest}"
        ),
        "creationInfo": {
            "creators": ["Tool: Stock Back Test System Python Runtime Builder 1"],
            "created": created,
        },
        "packages": components,
        "relationships": [
            {
                "spdxElementId": "SPDXRef-DOCUMENT",
                "relatedSpdxElement": component["SPDXID"],
                "relationshipType": "DESCRIBES",
            }
            for component in components
        ],
    }


def _runtime_file_entries(profile_root: Path) -> list[dict[str, str]]:
    entries: list[dict[str, str]] = []
    root = profile_root.resolve(strict=True)
    for path in sorted(profile_root.rglob("*")):
        relative = path.relative_to(profile_root).as_posix()
        if relative == "manifest.json" or path.is_dir() and not path.is_symlink():
            continue
        if path.is_symlink():
            try:
                resolved = path.resolve(strict=True)
            except OSError as error:
                raise RuntimeBuildError(f"runtime contains a broken symlink: {relative}") from error
            if not resolved.is_relative_to(root) or not resolved.is_file():
                raise RuntimeBuildError(f"runtime symlink is unsafe: {relative}")
            file_path = resolved
        elif path.is_file():
            file_path = path
        else:
            raise RuntimeBuildError(f"runtime contains an unsupported filesystem entry: {relative}")
        entries.append({"path": relative, "sha256": sha256_file(file_path)})
    return entries


def _verify_interpreter(profile_root: Path, lock: dict[str, Any]) -> None:
    python_path = profile_root / lock["pythonExecutable"]
    probe = r"""
import importlib.metadata
import importlib.util
import sys
import zoneinfo
import numpy
import pandas
import stockbt
import dateutil
import six
import tzdata
from pathlib import Path

expected = {
    "numpy": "2.5.4",
    "pandas": "3.0.6",
    "python-dateutil": "2.9.0.post0",
    "six": "1.17.0",
    "tzdata": "2026.5",
}
assert sys.version_info[:3] == (3, 14, 9), sys.version
assert stockbt.STOCKBT_API_VERSION == "1"
for name, version in expected.items():
    assert importlib.metadata.version(name) == version, (name, importlib.metadata.version(name))
assert importlib.util.find_spec("pip") is None
assert not hasattr(__import__("sys"), "real_prefix")
expected_tzdata = (
    Path(sys.prefix)
    / "lib"
    / "python3.14"
    / "site-packages"
    / "tzdata"
    / "zoneinfo"
).resolve(strict=True)
zoneinfo.reset_tzpath([str(expected_tzdata)])
assert zoneinfo.TZPATH == (str(expected_tzdata),), zoneinfo.TZPATH
assert (expected_tzdata / "America" / "New_York").is_file()
zone = zoneinfo.ZoneInfo("America/New_York")
assert zone.key == "America/New_York"
print("managed Python runtime imports and pinned timezone database verified")
"""
    environment = {
        "PATH": "/usr/bin:/bin",
        "LANG": "C",
        "TZ": "UTC",
        "PYTHONNOUSERSITE": "1",
        "PYTHONDONTWRITEBYTECODE": "1",
        "PYTHONSAFEPATH": "1",
    }
    try:
        result = subprocess.run(
            [str(python_path), "-I", "-B", "-c", probe],
            check=False,
            capture_output=True,
            text=True,
            timeout=45,
            env=environment,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        raise RuntimeBuildError(f"Could not run the assembled CPython profile: {error}") from error
    if result.returncode != 0:
        detail = (result.stderr or result.stdout).strip()
        raise RuntimeBuildError(f"Assembled runtime verification failed: {detail}")
    if "verified" not in result.stdout:
        raise RuntimeBuildError("Assembled runtime did not report successful verification.")


def _expected_components(lock: dict[str, Any], stockbt_hash: str) -> list[dict[str, Any]]:
    components = [
        {
            "artifactSha256": artifact["sha256"],
            "licenses": artifact["licenses"],
            "name": artifact["name"],
            "upstreamUrl": artifact["upstreamUrl"],
            "version": artifact["version"],
        }
        for artifact in lock["artifacts"]
    ]
    components.append(
        {
            "artifactSha256": stockbt_hash,
            "licenses": ["Apache-2.0"],
            "name": "stockbt",
            "upstreamUrl": "project-owned",
            "version": lock["stockbtApiVersion"],
        }
    )
    return sorted(components, key=lambda item: item["name"])


def verify_profile(
    profile_root: Path,
    lock: dict[str, Any],
    *,
    run_imports: bool = True,
) -> dict[str, Any]:
    root = profile_root.resolve(strict=True)
    manifest_path = root / "manifest.json"
    try:
        manifest_bytes = manifest_path.read_bytes()
        manifest = json.loads(manifest_bytes)
    except (OSError, json.JSONDecodeError) as error:
        raise RuntimeBuildError(f"Runtime profile manifest is invalid: {error}") from error
    if len(manifest_bytes) > PROFILE_MANIFEST_LIMIT or canonical_json(manifest) != manifest_bytes:
        raise RuntimeBuildError("Runtime profile manifest is non-canonical or exceeds 1 MiB.")
    stockbt_hash = sha256_file(
        root / "python" / "lib" / "python3.14" / "site-packages" / "stockbt" / "__init__.py"
    )
    expected_manifest_fields = {
        "architecture": lock["architecture"],
        "components": _expected_components(lock, stockbt_hash),
        "installedFiles": _runtime_file_entries(root),
        "platform": lock["platform"],
        "profileBuilderVersion": lock["profileBuilderVersion"],
        "profileId": lock["profileId"],
        "pythonAbi": lock["pythonAbi"],
        "pythonExecutable": lock["pythonExecutable"],
        "pythonVersion": lock["pythonVersion"],
        "schemaVersion": lock["schemaVersion"],
        "stockbtApiVersion": lock["stockbtApiVersion"],
        "stockbtSourceSha256": stockbt_hash,
    }
    if manifest != expected_manifest_fields:
        raise RuntimeBuildError("Runtime profile manifest does not match its installed files and lock.")
    forbidden_names = {"pip", "ensurepip", "__pycache__", ".cache"}
    for path in root.rglob("*"):
        if path.name in forbidden_names or path.suffix in {".pyc", ".pyo"}:
            raise RuntimeBuildError(f"runtime contains a build/install cache: {path.relative_to(root)}")
    if run_imports:
        _verify_interpreter(root, lock)
    return manifest


def _copy_profile_atomically(staged_profile: Path, output_directory: Path) -> None:
    parent = output_directory.parent
    backup: Path | None = None
    if output_directory.exists() or output_directory.is_symlink():
        if output_directory.is_symlink() or not output_directory.is_dir():
            raise RuntimeBuildError("Runtime output path exists and is not a directory.")
        backup = parent / f".{output_directory.name}.previous-{os.getpid()}"
        if backup.exists():
            raise RuntimeBuildError("A previous runtime build backup already exists.")
        os.replace(output_directory, backup)
    try:
        os.replace(staged_profile, output_directory)
    except OSError:
        if backup is not None and backup.exists():
            os.replace(backup, output_directory)
        raise
    if backup is not None:
        shutil.rmtree(backup)


def assemble_profile(
    artifacts_directory: Path,
    output_directory: Path,
    lock: dict[str, Any],
    *,
    stockbt_source: Path,
    stockbt_license: Path,
    verify_imports: bool = True,
) -> None:
    verify_artifact_cache(artifacts_directory, lock)
    if output_directory.name != lock["profileId"]:
        raise RuntimeBuildError("Runtime output directory must use the locked profile ID.")
    if output_directory.is_symlink():
        raise RuntimeBuildError("Runtime output directory must not be a symbolic link.")
    if not stockbt_source.is_file():
        raise RuntimeBuildError("The project-owned stockbt source file is missing.")
    if sys.platform != "darwin" or platform.machine().lower() not in {"arm64", "aarch64"}:
        raise RuntimeBuildError("The locked runtime can only be assembled on macOS Apple Silicon.")

    output_directory.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(
        prefix=f".{lock['profileId']}.stage-", dir=output_directory.parent
    ) as temporary_directory:
        staging_parent = Path(temporary_directory)
        profile_root = staging_parent / lock["profileId"]
        archive_root = staging_parent / "archive"
        cpython = next(item for item in lock["artifacts"] if item["name"] == "CPython")
        safe_extract_tarball(artifacts_directory / cpython["filename"], archive_root)
        shutil.move(str(archive_root / "python"), str(profile_root / "python"))
        _strip_install_tools(profile_root / "python")
        site_packages = (
            profile_root / "python" / "lib" / "python3.14" / "site-packages"
        )
        site_packages.mkdir(parents=True, exist_ok=True)
        for artifact in lock["artifacts"]:
            if artifact["name"] == "CPython":
                continue
            install_wheel(
                artifacts_directory / artifact["filename"],
                site_packages,
                expected_name=artifact["name"],
                expected_version=artifact["version"],
            )
        stockbt_directory = site_packages / "stockbt"
        stockbt_directory.mkdir()
        shutil.copy2(stockbt_source, stockbt_directory / "__init__.py")
        _copy_notices(profile_root, lock, stockbt_license)
        stockbt_hash = sha256_file(stockbt_source)
        spdx = _spdx_document(lock, stockbt_hash)
        (profile_root / "sbom.spdx.json").write_text(
            json.dumps(spdx, ensure_ascii=False, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
            newline="\n",
        )
        if verify_imports:
            _verify_interpreter(profile_root, lock)
        manifest = {
            "architecture": lock["architecture"],
            "components": _expected_components(lock, stockbt_hash),
            "installedFiles": _runtime_file_entries(profile_root),
            "platform": lock["platform"],
            "profileBuilderVersion": lock["profileBuilderVersion"],
            "profileId": lock["profileId"],
            "pythonAbi": lock["pythonAbi"],
            "pythonExecutable": lock["pythonExecutable"],
            "pythonVersion": lock["pythonVersion"],
            "schemaVersion": lock["schemaVersion"],
            "stockbtApiVersion": lock["stockbtApiVersion"],
            "stockbtSourceSha256": stockbt_hash,
        }
        manifest_bytes = canonical_json(manifest)
        if len(manifest_bytes) > PROFILE_MANIFEST_LIMIT:
            raise RuntimeBuildError("Generated runtime manifest exceeds 1 MiB.")
        (profile_root / "manifest.json").write_bytes(manifest_bytes)
        verify_profile(profile_root, lock, run_imports=verify_imports)
        _copy_profile_atomically(profile_root, output_directory)


def verify_app_bundle(app_bundle: Path, lock: dict[str, Any]) -> None:
    bundle = app_bundle.resolve(strict=True)
    resources = bundle / "Contents" / "Resources"
    if not resources.is_dir():
        raise RuntimeBuildError("macOS application bundle has no Contents/Resources directory.")
    bundled_bootstrap = resources / "Python" / "worker_bootstrap.py"
    source_bootstrap = PROJECT_ROOT / "Src" / "Bindings" / "Python" / "worker_bootstrap.py"
    if not bundled_bootstrap.is_file():
        raise RuntimeBuildError("worker bootstrap is missing from the application bundle.")
    if bundled_bootstrap.read_bytes() != source_bootstrap.read_bytes():
        raise RuntimeBuildError("bundled worker bootstrap differs from the build source.")
    profile_root = resources / "RuntimeProfiles" / lock["profileId"]
    verify_profile(profile_root, lock)


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group()
    group.add_argument("--download-artifacts", action="store_true")
    group.add_argument("--verify-artifacts", action="store_true")
    group.add_argument("--verify-profile", type=Path)
    group.add_argument("--verify-app-bundle", type=Path)
    parser.add_argument("--artifacts-dir", type=Path)
    parser.add_argument("--output-dir", type=Path)
    return parser


def main(arguments: list[str] | None = None) -> int:
    parser = _parser()
    options = parser.parse_args(arguments)
    try:
        lock = load_lock()
        if options.download_artifacts:
            if options.artifacts_dir is None:
                parser.error("--download-artifacts requires --artifacts-dir")
            download_artifacts(options.artifacts_dir, lock)
            verify_artifact_cache(options.artifacts_dir, lock)
        elif options.verify_artifacts:
            if options.artifacts_dir is None:
                parser.error("--verify-artifacts requires --artifacts-dir")
            verify_artifact_cache(options.artifacts_dir, lock)
        elif options.verify_profile is not None:
            verify_profile(options.verify_profile, lock)
            print(f"verified runtime profile {lock['profileId']}")
        elif options.verify_app_bundle is not None:
            verify_app_bundle(options.verify_app_bundle, lock)
            print(f"verified bundled runtime {lock['profileId']}")
        else:
            if options.artifacts_dir is None or options.output_dir is None:
                parser.error("assembly requires --artifacts-dir and --output-dir")
            assemble_profile(
                options.artifacts_dir,
                options.output_dir,
                lock,
                stockbt_source=PROJECT_ROOT / "Src" / "Bindings" / "Python" / "stockbt" / "__init__.py",
                stockbt_license=PROJECT_ROOT / "LICENSE",
            )
            print(f"assembled managed runtime profile {lock['profileId']}")
    except (OSError, RuntimeBuildError, tarfile.TarError, zipfile.BadZipFile) as error:
        print(str(error), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
