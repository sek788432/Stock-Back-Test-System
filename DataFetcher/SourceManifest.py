from __future__ import annotations

from dataclasses import dataclass
import hashlib
import os
from pathlib import Path
import tempfile
from collections.abc import Iterable

SOURCE_MANIFEST_NAME = "SourceManifest.btesource"
SOURCE_MANIFEST_MAGIC = "BTE-SOURCE-MANIFEST-V1"
SUPPORTED_TIMEFRAMES = frozenset(
    {"ohlcv-1s", "ohlcv-1m", "ohlcv-1h", "ohlcv-1d", "ohlcv-eod"}
)


@dataclass(frozen=True, order=True)
class SourceFileDeclaration:
    filename: str
    timeframe: str
    sha256: str


def _validateDeclaration(declaration: SourceFileDeclaration) -> None:
    if (
        not declaration.filename.endswith(".csv")
        or len(declaration.filename) <= 4
        or any(
            character not in "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-"
            for character in declaration.filename
        )
    ):
        raise ValueError(f"Unsafe source CSV filename: {declaration.filename!r}")
    if declaration.timeframe not in SUPPORTED_TIMEFRAMES:
        raise ValueError(f"Unsupported source timeframe: {declaration.timeframe!r}")
    if len(declaration.sha256) != 64 or any(
        character not in "0123456789abcdef" for character in declaration.sha256
    ):
        raise ValueError("Source CSV SHA-256 must be 64 lowercase hexadecimal characters")


def sourceFileDeclaration(csvPath: Path, timeframe: str) -> SourceFileDeclaration:
    digest = hashlib.sha256()
    with csvPath.open("rb") as csvFile:
        for chunk in iter(lambda: csvFile.read(1024 * 1024), b""):
            digest.update(chunk)
    declaration = SourceFileDeclaration(csvPath.name, timeframe, digest.hexdigest())
    _validateDeclaration(declaration)
    return declaration


def loadSourceManifest(manifestPath: Path) -> tuple[SourceFileDeclaration, ...] | None:
    if not manifestPath.is_file():
        return None

    lines = manifestPath.read_text(encoding="utf-8").splitlines()
    if not lines or lines[0] != SOURCE_MANIFEST_MAGIC:
        raise ValueError(f"Invalid source manifest header: {manifestPath}")

    declarations: list[SourceFileDeclaration] = []
    names: set[str] = set()
    for line in lines[1:]:
        if not line.startswith("file="):
            raise ValueError("Invalid source manifest entry")
        fields = line[len("file=") :].split("|")
        if len(fields) != 3:
            raise ValueError("Invalid source manifest metadata")
        declaration = SourceFileDeclaration(*fields)
        _validateDeclaration(declaration)
        if declaration.filename in names:
            raise ValueError("Source manifest contains a duplicate filename")
        names.add(declaration.filename)
        declarations.append(declaration)
    return tuple(declarations)


def writeSourceManifest(
    outputDirectory: Path, declarations: Iterable[SourceFileDeclaration]
) -> None:
    entries = sorted(declarations, key=lambda declaration: declaration.filename)
    names: set[str] = set()
    for declaration in entries:
        _validateDeclaration(declaration)
        if declaration.filename in names:
            raise ValueError("Source manifest contains a duplicate filename")
        names.add(declaration.filename)

    manifestText = "\n".join(
        [
            SOURCE_MANIFEST_MAGIC,
            *(
                f"file={entry.filename}|{entry.timeframe}|{entry.sha256}"
                for entry in entries
            ),
        ]
    ) + "\n"
    outputDirectory.mkdir(parents=True, exist_ok=True)
    temporaryPath: Path | None = None
    try:
        with tempfile.NamedTemporaryFile(
            mode="w",
            encoding="utf-8",
            newline="\n",
            prefix=f".{SOURCE_MANIFEST_NAME}.",
            suffix=".tmp",
            dir=outputDirectory,
            delete=False,
        ) as temporaryFile:
            temporaryPath = Path(temporaryFile.name)
            temporaryFile.write(manifestText)
            temporaryFile.flush()
            os.fsync(temporaryFile.fileno())
        os.replace(temporaryPath, outputDirectory / SOURCE_MANIFEST_NAME)
    finally:
        if temporaryPath is not None:
            temporaryPath.unlink(missing_ok=True)
