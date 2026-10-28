"""Project-owned, single-run Python Strategy worker bootstrap."""

from __future__ import annotations

import argparse
import ast
import builtins
import hashlib
import hmac
import json
import math
import mmap
import os
import random
import struct
import sys
import traceback
import types
import zoneinfo
from dataclasses import dataclass
from types import MappingProxyType


PROTOCOL_VERSION = 1
STOCKBT_API_VERSION = "1"
STOCKBT_TEMPLATE_VERSION = "1.0.0"
MAXIMUM_FRAME_BYTES = 1024 * 1024
MAXIMUM_SAFE_INTEGER = (1 << 53) - 1
MAXIMUM_STRATEGY_BYTES = 1024 * 1024
MAXIMUM_PARAMETERS_BYTES = 64 * 1024
MAXIMUM_PARAMETERS_KEYS = 64
MAXIMUM_HISTORY_BARS = 10_000
MAXIMUM_COMMANDS_PER_CALLBACK = 1
MAXIMUM_CALLBACK_LOG_BYTES = 512 * 1024
MAXIMUM_TRACEBACK_BYTES = 64 * 1024
MAXIMUM_STARTUP_DIAGNOSTIC_BYTES = 48 * 1024
FORBIDDEN_IMPORT_POLICY_NAMES = frozenset(
    {
        "__builtins__",
        "__import__",
        "globals",
        "locals",
        "vars",
        "getattr",
        "hasattr",
        "setattr",
        "delattr",
        "dir",
        "get_type_hints",
        "evaluate_forward_ref",
    }
)
FORBIDDEN_IMPORT_POLICY_ATTRIBUTES = frozenset(
    {
        "__builtins__",
        "__import__",
        "__dict__",
        "__class__",
        "__mro__",
        "__bases__",
        "__base__",
        "__subclasses__",
        "__globals__",
        "__getattribute__",
        "attrgetter",
        "methodcaller",
        "ForwardRef",
        "evaluate_forward_ref",
        "get_type_hints",
        "_eval_type",
        "_evaluate",
    }
)
FORBIDDEN_IMPORT_POLICY_CONSTANTS = frozenset(
    {
        "__builtins__",
        "__import__",
        "__dict__",
        "__class__",
        "__mro__",
        "__bases__",
        "__base__",
        "__subclasses__",
        "__globals__",
        "__getattribute__",
    }
)
ALLOWED_IMPORT_ROOTS = frozenset(
    {
        "stockbt",
        "numpy",
        "pandas",
        "math",
        "statistics",
        "decimal",
        "fractions",
        "datetime",
        "zoneinfo",
        "collections",
        "itertools",
        "functools",
        "operator",
        "bisect",
        "heapq",
        "dataclasses",
        "enum",
        "typing",
        "random",
        "json",
    }
)
ALLOWED_HOOKS = frozenset({"on_start", "on_market", "on_fill", "on_finish"})


class WorkerFailure(Exception):
    def __init__(self, code: str, message: str,
                 diagnostics: dict[str, object] | None = None) -> None:
        super().__init__(message)
        self.code = code
        self.diagnostics = diagnostics


class ProtocolViolation(WorkerFailure):
    def __init__(self, message: str) -> None:
        super().__init__("StrategyProtocolViolation", message)


class _NumberRejected(ValueError):
    pass


def _parse_integer(text: str) -> int:
    value = int(text)
    if abs(value) > MAXIMUM_SAFE_INTEGER:
        raise _NumberRejected("JSON integer exceeds the exactly representable range")
    return value


def _parse_float(text: str) -> float:
    value = float(text)
    if not math.isfinite(value):
        raise _NumberRejected("JSON number is not finite")
    return value


def _reject_constant(text: str) -> None:
    raise _NumberRejected(f"JSON constant {text} is not permitted")


def _object_without_duplicates(pairs: list[tuple[str, object]]) -> dict[str, object]:
    result: dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            raise _NumberRejected(f"duplicate JSON property {key!r}")
        result[key] = value
    return result


def _decode_json(payload: bytes) -> object:
    try:
        text = payload.decode("utf-8", errors="strict")
        return json.loads(
            text,
            object_pairs_hook=_object_without_duplicates,
            parse_int=_parse_integer,
            parse_float=_parse_float,
            parse_constant=_reject_constant,
        )
    except (UnicodeDecodeError, json.JSONDecodeError, _NumberRejected, ValueError) as error:
        raise ProtocolViolation("worker received invalid I-JSON payload") from error


def _serialize_number(value: float) -> str:
    if not math.isfinite(value):
        raise ProtocolViolation("JSON numbers must be finite IEEE-754 values")
    if value == 0.0:
        return "0"

    text = repr(value).lower()
    negative = text.startswith("-")
    if negative:
        text = text[1:]
    exponent_offset = text.find("e")
    if exponent_offset >= 0:
        mantissa = text[:exponent_offset]
        exponent = int(text[exponent_offset + 1 :])
    else:
        mantissa = text
        exponent = 0

    decimal_offset = mantissa.find(".")
    integer_digits = len(mantissa) if decimal_offset < 0 else decimal_offset
    digits = mantissa.replace(".", "")
    decimal_position = integer_digits + exponent
    leading_zeroes = len(digits) - len(digits.lstrip("0"))
    digits = digits[leading_zeroes:]
    decimal_position -= leading_zeroes
    digits = digits.rstrip("0") or "0"

    sign = "-" if negative else ""
    if 0 < decimal_position <= 21:
        if len(digits) <= decimal_position:
            return sign + digits + "0" * (decimal_position - len(digits))
        return sign + digits[:decimal_position] + "." + digits[decimal_position:]
    if -5 <= decimal_position <= 0:
        return sign + "0." + "0" * (-decimal_position) + digits

    scientific_exponent = decimal_position - 1
    coefficient = digits[0]
    if len(digits) > 1:
        coefficient += "." + digits[1:]
    exponent_sign = "+" if scientific_exponent >= 0 else ""
    return f"{sign}{coefficient}e{exponent_sign}{scientific_exponent}"


def _serialize_string(value: str) -> str:
    try:
        value.encode("utf-8", errors="strict")
        return json.dumps(value, ensure_ascii=False, separators=(",", ":"))
    except (UnicodeEncodeError, TypeError) as error:
        raise ProtocolViolation("JSON string is not valid Unicode") from error


def _serialize_json(value: object, depth: int = 0) -> str:
    if depth > 128:
        raise ProtocolViolation("JSON nesting exceeds the supported depth")
    if value is None:
        return "null"
    if value is True:
        return "true"
    if value is False:
        return "false"
    if isinstance(value, int):
        if abs(value) > MAXIMUM_SAFE_INTEGER:
            raise ProtocolViolation("JSON integer exceeds the exactly representable range")
        return str(value)
    if isinstance(value, float):
        return _serialize_number(value)
    if isinstance(value, str):
        return _serialize_string(value)
    if isinstance(value, (list, tuple)):
        return "[" + ",".join(_serialize_json(element, depth + 1) for element in value) + "]"
    if isinstance(value, dict):
        entries: list[str] = []
        try:
            keys = sorted(value, key=lambda key: key.encode("utf-16-be", errors="strict"))
        except (AttributeError, UnicodeEncodeError) as error:
            raise ProtocolViolation("JSON object keys must be Unicode strings") from error
        for key in keys:
            if not isinstance(key, str):
                raise ProtocolViolation("JSON object keys must be strings")
            entries.append(
                _serialize_string(key) + ":" + _serialize_json(value[key], depth + 1)
            )
        return "{" + ",".join(entries) + "}"
    raise ProtocolViolation("unsupported JSON value type")


def _canonical_bytes(value: object) -> bytes:
    return _serialize_json(value).encode("utf-8", errors="strict")


def _read_exact(descriptor: int, byte_count: int) -> bytes:
    chunks: list[bytes] = []
    remaining = byte_count
    while remaining:
        chunk = os.read(descriptor, remaining)
        if not chunk:
            raise ProtocolViolation("protocol stream ended before the frame was complete")
        chunks.append(chunk)
        remaining -= len(chunk)
    return b"".join(chunks)


def _read_message(run_id: str, sequence: int) -> tuple[str, object]:
    header = _read_exact(0, 4)
    payload_length = struct.unpack(">I", header)[0]
    if payload_length > MAXIMUM_FRAME_BYTES:
        raise ProtocolViolation("protocol frame exceeds the payload limit")
    digest = _read_exact(0, 32)
    payload_bytes = _read_exact(0, payload_length)
    if not hmac.compare_digest(digest, hashlib.sha256(payload_bytes).digest()):
        raise ProtocolViolation("protocol frame SHA-256 digest does not match")
    payload = _decode_json(payload_bytes)
    if _canonical_bytes(payload) != payload_bytes:
        raise ProtocolViolation("protocol frame is not RFC 8785 canonical JSON")
    if not isinstance(payload, dict) or set(payload) != {
        "messageType",
        "payload",
        "protocolVersion",
        "runId",
        "sequence",
    }:
        raise ProtocolViolation("protocol message envelope has unknown or missing properties")
    if (
        payload["protocolVersion"] != PROTOCOL_VERSION
        or isinstance(payload["protocolVersion"], bool)
        or payload["runId"] != run_id
        or payload["sequence"] != sequence
        or isinstance(payload["sequence"], bool)
        or not isinstance(payload["sequence"], int)
        or not isinstance(payload["messageType"], str)
        or not payload["messageType"]
    ):
        raise ProtocolViolation("protocol message version, identity, sequence, or type is invalid")
    return payload["messageType"], payload["payload"]


def _write_all(descriptor: int, payload: bytes) -> None:
    offset = 0
    while offset < len(payload):
        written = os.write(descriptor, payload[offset:])
        if written <= 0:
            raise ProtocolViolation("protocol stream made no forward progress")
        offset += written


def _send_message(run_id: str, sequence: int, message_type: str, payload: object) -> None:
    envelope = {
        "messageType": message_type,
        "payload": payload,
        "protocolVersion": PROTOCOL_VERSION,
        "runId": run_id,
        "sequence": sequence,
    }
    payload_bytes = _canonical_bytes(envelope)
    if len(payload_bytes) > MAXIMUM_FRAME_BYTES:
        raise ProtocolViolation("protocol payload exceeds the 1 MiB limit")
    frame = struct.pack(">I", len(payload_bytes)) + hashlib.sha256(payload_bytes).digest()
    _write_all(3, frame + payload_bytes)


def _immutable(value: object) -> object:
    if isinstance(value, dict):
        return MappingProxyType({key: _immutable(item) for key, item in value.items()})
    if isinstance(value, list):
        return tuple(_immutable(item) for item in value)
    return value


@dataclass(frozen=True)
class Bar:
    timestamp_utc_ms: int
    open: float
    high: float
    low: float
    close: float
    volume: float


_HISTORY: list[Bar] = []
_MAX_HISTORY_BARS = 0
_SHARED_HISTORY_BYTES = b""


@dataclass(frozen=True)
class Position:
    quantity_shares: int
    mark_price: float | None


@dataclass(frozen=True)
class Portfolio:
    cash: float
    equity: float
    positions: object


class _OrderCommands:
    def __init__(self, symbol: str, enabled: bool) -> None:
        self._symbol = symbol
        self._enabled = enabled
        self._commands: list[dict[str, object]] = []

    def __getattr__(self, name: str) -> object:
        if name.startswith("_"):
            raise AttributeError(name)
        raise WorkerFailure(
            "StrategyCommandRejected", f"unsupported order method {name!r}"
        )

    def _append(self, command: dict[str, object]) -> None:
        if not self._enabled:
            raise WorkerFailure("StrategyCommandRejected", "orders are unavailable in this hook")
        if len(self._commands) >= MAXIMUM_COMMANDS_PER_CALLBACK:
            raise WorkerFailure("StrategyCommandRejected", "only one command is allowed per callback")
        self._commands.append(command)

    def buy_market(self, symbol: str, *, quantity: int) -> None:
        if symbol != self._symbol:
            raise WorkerFailure("StrategyCommandRejected", "command symbol must equal ctx.symbol")
        if isinstance(quantity, bool) or not isinstance(quantity, int):
            raise WorkerFailure("StrategyCommandRejected", "quantity must be a whole-share integer")
        if quantity < 1 or quantity > 1_000_000_000:
            raise WorkerFailure("StrategyCommandRejected", "quantity is outside the supported range")
        self._append({"kind": "buy_market", "quantity_shares": quantity, "symbol": symbol})

    def close_position(self, symbol: str) -> None:
        if symbol != self._symbol:
            raise WorkerFailure("StrategyCommandRejected", "command symbol must equal ctx.symbol")
        self._append({"kind": "close_position", "quantity_shares": 0, "symbol": symbol})


class _Log:
    def __init__(self) -> None:
        self._entries: list[dict[str, object]] = []
        self._byte_count = 0
        self.omitted_entries = 0
        self.omitted_bytes = 0

    def _append(self, level: str, message: str, fields: dict[str, object]) -> None:
        if not isinstance(message, str):
            raise WorkerFailure("StrategyContractViolation", "log message must be text")
        encoded = message.encode("utf-8", errors="strict")
        if len(encoded) > 8192:
            raise WorkerFailure("StrategyContractViolation", "log message exceeds 8 KiB")
        entry = {"fields": fields, "level": level, "message": message}
        serialized_entry = _canonical_bytes(entry)
        size = len(serialized_entry)
        if (len(self._entries) >= 10_000
                or self._byte_count + size > MAXIMUM_CALLBACK_LOG_BYTES):
            self.omitted_entries += 1
            self.omitted_bytes += size
            return
        snapshot = _decode_json(serialized_entry)
        if not isinstance(snapshot, dict):
            raise ProtocolViolation("structured log entry is not a JSON object")
        self._entries.append(snapshot)
        self._byte_count += size

    def info(self, message: str, **fields: object) -> None:
        self._append("info", message, fields)

    def warning(self, message: str, **fields: object) -> None:
        self._append("warning", message, fields)

    def error(self, message: str, **fields: object) -> None:
        self._append("error", message, fields)


@dataclass(frozen=True)
class Context:
    symbol: str
    timestamp_utc_ms: int | None
    bar: Bar | None
    bars: object
    indicators: object
    history: object
    portfolio: Portfolio
    orders: _OrderCommands
    log: _Log


def _context(payload: object, *, allow_orders: bool) -> Context:
    if not isinstance(payload, dict):
        raise ProtocolViolation("strategy context must be an object")
    symbol = payload.get("symbol")
    if not isinstance(symbol, str) or not symbol:
        raise ProtocolViolation("strategy context symbol is invalid")
    timestamp = payload.get("timestamp_utc_ms")
    if timestamp is not None and (isinstance(timestamp, bool) or not isinstance(timestamp, int)):
        raise ProtocolViolation("strategy context timestamp is invalid")
    raw_bar = payload.get("bar")
    bar = None if raw_bar is None else _decode_bar(raw_bar)
    history_bars = tuple(_HISTORY)

    def history(
        requested_symbol: str,
        *,
        count: int,
        end_utc_ms: int | None = None,
    ) -> tuple[Bar, ...]:
        if timestamp is None:
            raise WorkerFailure(
                "HistoryUnavailable", "history is unavailable during on_start"
            )
        if not isinstance(requested_symbol, str) or requested_symbol != symbol:
            raise WorkerFailure(
                "InvalidHistoryRequest", "history symbol must equal ctx.symbol"
            )
        if isinstance(count, bool) or not isinstance(count, int) or count <= 0:
            raise WorkerFailure(
                "InvalidHistoryRequest", "history count must be a positive integer"
            )
        if count > _MAX_HISTORY_BARS or count > MAXIMUM_HISTORY_BARS:
            raise WorkerFailure(
                "HistoryBudgetExceeded", "history count exceeds the declared budget"
            )
        if end_utc_ms is None:
            end_timestamp = timestamp
        elif (
            isinstance(end_utc_ms, bool)
            or not isinstance(end_utc_ms, int)
            or end_utc_ms < -(1 << 63)
            or end_utc_ms > (1 << 63) - 1
        ):
            raise WorkerFailure(
                "InvalidHistoryRequest", "history end must be a signed UTC millisecond integer"
            )
        else:
            end_timestamp = end_utc_ms
        if end_timestamp > timestamp:
            raise WorkerFailure(
                "FutureDataRequest", "history end cannot be later than the current timestamp"
            )
        available = tuple(
            item for item in history_bars if item.timestamp_utc_ms <= end_timestamp
        )
        return available[-count:]
    raw_portfolio = payload.get("portfolio")
    if not isinstance(raw_portfolio, dict):
        raise ProtocolViolation("strategy portfolio must be an object")
    positions = {
        key: Position(quantity_shares=value["quantity_shares"], mark_price=value["mark_price"])
        for key, value in raw_portfolio.get("positions", {}).items()
    }
    return Context(
        symbol=symbol,
        timestamp_utc_ms=timestamp,
        bar=bar,
        bars=_immutable(payload.get("bars", {})),
        indicators=_immutable(payload.get("indicators", {})),
        history=history,
        portfolio=Portfolio(
            cash=raw_portfolio["cash"],
            equity=raw_portfolio["equity"],
            positions=MappingProxyType(positions),
        ),
        orders=_OrderCommands(symbol, allow_orders),
        log=_Log(),
    )


def _decode_bar(raw_bar: object) -> Bar:
    if not isinstance(raw_bar, dict) or set(raw_bar) != {
        "close",
        "high",
        "low",
        "open",
        "timestamp_utc_ms",
        "volume",
    }:
        raise ProtocolViolation("strategy bar has unknown or missing properties")
    timestamp = raw_bar["timestamp_utc_ms"]
    if (
        isinstance(timestamp, bool)
        or not isinstance(timestamp, int)
        or timestamp < -(1 << 63)
        or timestamp > (1 << 63) - 1
    ):
        raise ProtocolViolation("strategy bar timestamp is invalid")
    values: list[float] = []
    for key in ("open", "high", "low", "close", "volume"):
        value = raw_bar[key]
        if isinstance(value, bool) or not isinstance(value, (int, float)):
            raise ProtocolViolation("strategy bar values must be numeric")
        converted = float(value)
        if not math.isfinite(converted):
            raise ProtocolViolation("strategy bar values must be finite")
        values.append(converted)
    open_value, high_value, low_value, close_value, volume_value = values
    if (
        min(open_value, high_value, low_value, close_value) <= 0.0
        or high_value < max(open_value, low_value, close_value)
        or low_value > min(open_value, high_value, close_value)
        or volume_value < 0.0
    ):
        raise ProtocolViolation("strategy bar violates OHLCV invariants")
    return Bar(
        timestamp_utc_ms=timestamp,
        open=open_value,
        high=high_value,
        low=low_value,
        close=close_value,
        volume=volume_value,
    )


def _record_context_bar(payload: object) -> None:
    if not isinstance(payload, dict):
        raise ProtocolViolation("strategy callback payload must be an object")
    raw_context = payload.get("context")
    if not isinstance(raw_context, dict):
        raise ProtocolViolation("strategy callback context must be an object")
    raw_bar = raw_context.get("bar")
    if raw_bar is None:
        return
    bar = _decode_bar(raw_bar)
    if _HISTORY and bar.timestamp_utc_ms <= _HISTORY[-1].timestamp_utc_ms:
        if bar.timestamp_utc_ms == _HISTORY[-1].timestamp_utc_ms and bar == _HISTORY[-1]:
            return
        raise ProtocolViolation("strategy history bars are not strictly chronological")
    _HISTORY.append(bar)
    if len(_HISTORY) > _MAX_HISTORY_BARS:
        del _HISTORY[0]


def _seed_history(payload: object) -> None:
    if not isinstance(payload, dict) or set(payload) != {"barCount", "historySha256"}:
        raise ProtocolViolation("history seed has unknown or missing properties")
    count = payload["barCount"]
    history_digest = payload["historySha256"]
    if (
        isinstance(count, bool)
        or not isinstance(count, int)
        or count < 0
        or count > _MAX_HISTORY_BARS
        or not isinstance(history_digest, str)
        or len(history_digest) != 64
        or count * 48 > len(_SHARED_HISTORY_BYTES)
    ):
        raise ProtocolViolation("history seed metadata is invalid")
    raw = _SHARED_HISTORY_BYTES[-(count * 48) :] if count else b""
    if len(raw) != count * 48:
        raise ProtocolViolation("history seed length does not match its bar count")
    if hashlib.sha256(raw).hexdigest() != history_digest:
        raise ProtocolViolation("history seed digest does not match its bars")
    bars = [
        Bar(timestamp_utc_ms=timestamp, open=open_value, high=high_value,
            low=low_value, close=close_value, volume=volume_value)
        for timestamp, open_value, high_value, low_value, close_value, volume_value
        in struct.iter_unpack(">q5d", raw)
    ]
    previous_timestamp: int | None = None
    for bar in bars:
        _decode_bar(
            {
                "timestamp_utc_ms": bar.timestamp_utc_ms,
                "open": bar.open,
                "high": bar.high,
                "low": bar.low,
                "close": bar.close,
                "volume": bar.volume,
            }
        )
        if previous_timestamp is not None and bar.timestamp_utc_ms <= previous_timestamp:
            raise ProtocolViolation("history seed bars are not strictly chronological")
        previous_timestamp = bar.timestamp_utc_ms
    _HISTORY[:] = bars


def _install_stockbt_module(*, require_managed: bool) -> None:
    try:
        import stockbt as module
    except ImportError as error:
        if require_managed:
            raise WorkerFailure(
                "RuntimeUnavailable", "managed profile is missing the stockbt package"
            ) from error
        module = types.ModuleType("stockbt")
        module.__version__ = STOCKBT_API_VERSION
        module.STOCKBT_API_VERSION = STOCKBT_API_VERSION

        def sma(name: str, *, period: int) -> object:
            if not isinstance(name, str) or not name.isascii() or not name:
                raise ValueError("SMA name must be non-empty ASCII text")
            if isinstance(period, bool) or not isinstance(period, int):
                raise ValueError("SMA period must be an integer")
            if period < 1 or period > MAXIMUM_HISTORY_BARS:
                raise ValueError("SMA period is outside the supported range")
            return MappingProxyType({"kind": "SMA", "name": name, "period": period})

        module.SMA = sma
    if (
        getattr(module, "STOCKBT_API_VERSION", None) != STOCKBT_API_VERSION
        or getattr(module, "__version__", None) != STOCKBT_API_VERSION
        or not callable(getattr(module, "SMA", None))
    ):
        raise WorkerFailure(
            "RuntimeIntegrityFailure", "managed stockbt package has an unsupported API"
        )
    sys.modules["stockbt"] = module


def _bounded_traceback(lines: list[str], maximum_bytes: int) -> tuple[str, int]:
    trace_bytes = "\n".join(lines).encode("utf-8", errors="replace")
    if len(trace_bytes) <= maximum_bytes:
        return trace_bytes.decode("utf-8"), 0
    prefix_limit = maximum_bytes // 2
    suffix_limit = maximum_bytes - prefix_limit
    prefix = trace_bytes[:prefix_limit].decode("utf-8", errors="ignore").encode("utf-8")
    suffix = trace_bytes[-suffix_limit:].decode("utf-8", errors="ignore").encode("utf-8")
    retained = prefix + suffix
    return retained.decode("utf-8"), len(trace_bytes) - len(retained)


def _startup_diagnostics(
    error: BaseException | None = None,
    *,
    source_line: int | None = None,
    source_column: int | None = None,
    message: str | None = None,
) -> dict[str, object]:
    frames = (
        [frame for frame in traceback.extract_tb(error.__traceback__)
         if frame.filename == "strategy.py"]
        if error is not None
        else []
    )
    location = frames[-1] if frames else None
    if source_line is None and isinstance(error, SyntaxError):
        source_line = error.lineno
        source_column = error.offset
    if source_line is None and location is not None:
        source_line = location.lineno
        source_column = (location.colno + 1) if location.colno is not None else None

    lines = ["Traceback (most recent call last):"]
    for frame in frames:
        lines.append(f'  File "strategy.py", line {frame.lineno}, in {frame.name}')
        if frame.line:
            lines.append(f"    {frame.line}")
    if isinstance(error, SyntaxError):
        lines.append(f"{type(error).__name__}: {error.msg}")
        if error.text:
            lines.append(f"    {error.text.rstrip()}")
            if error.offset is not None and error.offset > 0:
                lines.append("    " + " " * (error.offset - 1) + "^")
    elif error is not None:
        lines.append(f"{type(error).__name__}: {error}")
    elif message:
        lines.append(message)

    trace_text, omitted = _bounded_traceback(
        lines, MAXIMUM_STARTUP_DIAGNOSTIC_BYTES
    )
    return {
        "omittedTracebackBytes": omitted,
        "sourceColumn": source_column,
        "sourceLine": source_line,
        "traceback": trace_text,
    }


def _validate_source(source: str) -> object:
    try:
        tree = ast.parse(source, filename="<python-strategy>", mode="exec")
    except SyntaxError as error:
        raise WorkerFailure(
            "StrategySyntaxError",
            "strategy source has invalid Python syntax",
            _startup_diagnostics(error),
        ) from error
    for node in ast.walk(tree):
        if isinstance(node, ast.Import):
            for imported in node.names:
                if imported.name.split(".", 1)[0] not in ALLOWED_IMPORT_ROOTS:
                    message = (
                        f"import of {imported.name!r} is not permitted by the "
                        "worker import allowlist"
                    )
                    raise WorkerFailure(
                        "StrategyImportNotAllowed",
                        message,
                        _startup_diagnostics(
                            source_line=node.lineno,
                            source_column=node.col_offset + 1,
                            message=message,
                        ),
                    )
        elif isinstance(node, ast.ImportFrom):
            if (
                node.level != 0
                or node.module is None
                or node.module.split(".", 1)[0] not in ALLOWED_IMPORT_ROOTS
                or any(
                    alias.name == "*"
                    or alias.name in FORBIDDEN_IMPORT_POLICY_ATTRIBUTES
                    or (
                        alias.name.startswith("__")
                        and alias.name.endswith("__")
                    )
                    for alias in node.names
                )
            ):
                message = (
                    "relative, reflective, star, or non-allowlisted imports "
                    "are not permitted"
                )
                raise WorkerFailure(
                    "StrategyImportNotAllowed",
                    message,
                    _startup_diagnostics(
                        source_line=node.lineno,
                        source_column=node.col_offset + 1,
                        message=message,
                    ),
                )
        elif (
            isinstance(node, ast.Name)
            and node.id in FORBIDDEN_IMPORT_POLICY_NAMES
        ) or (
            isinstance(node, ast.Attribute)
            and (
                node.attr in FORBIDDEN_IMPORT_POLICY_ATTRIBUTES
                or (node.attr.startswith("__") and node.attr.endswith("__"))
            )
        ) or (
            isinstance(node, ast.Constant)
            and node.value in FORBIDDEN_IMPORT_POLICY_CONSTANTS
        ):
            message = "dynamic import and namespace reflection are not permitted"
            raise WorkerFailure(
                "StrategyImportNotAllowed",
                message,
                _startup_diagnostics(
                    source_line=node.lineno,
                    source_column=node.col_offset + 1,
                    message=message,
                ),
            )
        elif isinstance(node, ast.Call) and isinstance(node.func, ast.Name):
            if node.func.id in {"__import__", "eval", "exec", "compile"}:
                message = f"dynamic code operation {node.func.id!r} is not permitted"
                raise WorkerFailure(
                    "StrategyImportNotAllowed",
                    message,
                    _startup_diagnostics(
                        source_line=node.lineno,
                        source_column=node.col_offset + 1,
                        message=message,
                    ),
                )
    return compile(tree, "strategy.py", "exec", dont_inherit=True, optimize=0)


def _strategy_import(
    name: str,
    global_namespace: object = None,
    local_namespace: object = None,
    fromlist: object = (),
    level: int = 0,
) -> object:
    if (
        not isinstance(name, str)
        or not name
        or level != 0
        or name.split(".", 1)[0] not in ALLOWED_IMPORT_ROOTS
    ):
        raise ImportError("import is not permitted by the worker import allowlist")
    return builtins.__import__(
        name, global_namespace, local_namespace, fromlist, level
    )


def _immutable_parameters(value: object, depth: int = 0) -> object:
    if depth > 8:
        raise WorkerFailure("StrategyContractViolation", "parameters exceed nesting depth 8")
    if isinstance(value, dict):
        if len(value) > MAXIMUM_PARAMETERS_KEYS:
            raise WorkerFailure("StrategyContractViolation", "parameters exceed 64 keys")
        result: dict[str, object] = {}
        for key, item in value.items():
            if not isinstance(key, str) or len(key.encode("utf-8")) > 256:
                raise WorkerFailure("StrategyContractViolation", "parameter key is invalid")
            result[key] = _immutable_parameters(item, depth + 1)
        return MappingProxyType(result)
    if isinstance(value, list):
        return tuple(_immutable_parameters(item, depth + 1) for item in value)
    if isinstance(value, str) and len(value.encode("utf-8")) > 4096:
        raise WorkerFailure("StrategyContractViolation", "parameter string exceeds 4 KiB")
    if isinstance(value, float) and not math.isfinite(value):
        raise WorkerFailure("StrategyContractViolation", "parameter number must be finite")
    return value


def _evaluate_module(source_descriptor: int, source_length: int, source_digest: str,
                     *, skip_numpy_for_test: bool,
                     require_managed_stockbt: bool) -> object:
    if source_length < 1 or source_length > MAXIMUM_STRATEGY_BYTES:
        raise WorkerFailure("StrategyLoadFailed", "strategy source length is invalid")
    with mmap.mmap(source_descriptor, source_length, access=mmap.ACCESS_READ) as mapping:
        source_bytes = mapping[:]
    if hashlib.sha256(source_bytes).hexdigest() != source_digest:
        raise WorkerFailure("StrategyLoadFailed", "strategy source digest does not match")
    if b"\x00" in source_bytes:
        raise WorkerFailure("StrategySourceContainsNul", "strategy source contains NUL bytes")
    try:
        source = source_bytes.decode("utf-8", errors="strict")
    except UnicodeDecodeError as error:
        raise WorkerFailure("StrategyLoadFailed", "strategy source is not valid UTF-8") from error
    if source.startswith("\ufeff"):
        raise WorkerFailure("StrategyLoadFailed", "worker source must already be BOM-normalized")

    if not skip_numpy_for_test:
        try:
            import numpy
        except ImportError as error:
            raise WorkerFailure("RuntimeUnavailable", "managed profile is missing NumPy") from error
        numpy.random.seed(0)
    random.seed(0)
    _install_stockbt_module(require_managed=require_managed_stockbt)
    code = _validate_source(source)
    module = types.ModuleType("_bte_python_strategy")
    module.__file__ = "<python-strategy>"
    module.__package__ = ""
    strategy_builtins = builtins.__dict__.copy()
    strategy_builtins["__import__"] = _strategy_import
    module.__dict__["__builtins__"] = strategy_builtins
    try:
        exec(code, module.__dict__, module.__dict__)
    except BaseException as error:
        raise WorkerFailure(
            "StrategyLoadFailed",
            "strategy module evaluation failed",
            _startup_diagnostics(error),
        ) from error

    if module.__dict__.get("STOCKBT_API_VERSION") != STOCKBT_API_VERSION:
        raise WorkerFailure("UnsupportedStrategyApiVersion", "strategy API version is unsupported")
    if module.__dict__.get("STOCKBT_TEMPLATE_VERSION") != STOCKBT_TEMPLATE_VERSION:
        raise WorkerFailure("StrategyContractViolation", "strategy template version is unsupported")
    if not callable(module.__dict__.get("create_strategy")):
        raise WorkerFailure("StrategyContractViolation", "create_strategy(params) is required")
    return module


def _create_strategy(module: object, parameters: object) -> object:
    factory = module.__dict__.get("create_strategy")
    immutable_params = _immutable_parameters(parameters)
    try:
        strategy = factory(immutable_params)
    except BaseException as error:
        raise WorkerFailure(
            "StrategyFactoryFailed",
            "create_strategy(params) failed",
            _startup_diagnostics(error),
        ) from error
    if strategy is None:
        raise WorkerFailure("StrategyContractViolation", "strategy factory returned None")

    max_history = getattr(strategy, "max_history_bars", None)
    if (
        isinstance(max_history, bool)
        or not isinstance(max_history, int)
        or max_history < 1
        or max_history > MAXIMUM_HISTORY_BARS
    ):
        raise WorkerFailure("StrategyContractViolation", "max_history_bars must be from 1 through 10000")
    indicators = getattr(strategy, "indicators", None)
    if not isinstance(indicators, dict):
        raise WorkerFailure("StrategyContractViolation", "indicators must be a mapping")
    registered_indicators: list[dict[str, object]] = []
    for name, declaration in indicators.items():
        if (
            not isinstance(name, str)
            or not name
            or len(name) > 64
            or not name[0].isascii()
            or not name[0].isalpha()
            or not name.isascii()
            or not all(character.isalnum() or character == "_" for character in name)
        ):
            raise WorkerFailure(
                "StrategyContractViolation",
                "SMA names must match [A-Za-z][A-Za-z0-9_]{0,63}",
            )
        if not isinstance(declaration, MappingProxyType) or set(declaration) != {
            "kind",
            "name",
            "period",
        }:
            raise WorkerFailure(
                "StrategyContractViolation",
                "only stockbt.SMA(name, period) declarations are supported",
            )
        period = declaration["period"]
        if (
            declaration["kind"] != "SMA"
            or declaration["name"] != name
            or isinstance(period, bool)
            or not isinstance(period, int)
            or period < 1
            or period > MAXIMUM_HISTORY_BARS
        ):
            raise WorkerFailure(
                "StrategyContractViolation",
                "only valid stockbt.SMA(name, period) declarations are supported",
            )
        registered_indicators.append(
            {"kind": "SMA", "name": name, "period": period}
        )
    if registered_indicators and max(
        item["period"] for item in registered_indicators
    ) > max_history:
        raise WorkerFailure(
            "StrategyContractViolation",
            "declared history budget must cover the largest registered SMA period",
        )
    if not callable(getattr(strategy, "on_market", None)):
        raise WorkerFailure("StrategyContractViolation", "on_market(ctx) is required")
    for name in dir(strategy):
        if name.startswith("on_") and name not in ALLOWED_HOOKS:
            raise WorkerFailure("StrategyContractViolation", f"unsupported lifecycle hook {name!r}")
    for name in ALLOWED_HOOKS - {"on_market"}:
        hook = getattr(strategy, name, None)
        if hook is not None and not callable(hook):
            raise WorkerFailure("StrategyContractViolation", f"{name} must be callable")
    registered_indicators.sort(key=lambda item: item["name"])
    return strategy, registered_indicators


def _run_hook(strategy: object, hook_name: str, payload: object,
              fill: object = None) -> dict[str, object]:
    if hook_name not in ALLOWED_HOOKS:
        raise WorkerFailure("StrategyContractViolation", "unknown Strategy hook")
    hook = getattr(strategy, hook_name, None)
    commands_allowed = hook_name == "on_market"
    context = _context(payload, allow_orders=commands_allowed)
    immutable_fill = _immutable(fill)
    try:
        if hook is not None:
            result = hook(context) if hook_name != "on_fill" else hook(immutable_fill, context)
            if hook_name == "on_market" and result is not None:
                raise WorkerFailure("StrategyContractViolation", "on_market must submit commands through ctx.orders")
    except WorkerFailure as error:
        error.diagnostics = _hook_diagnostics(hook_name, context.log, error)
        raise
    except BaseException as error:
        failure = WorkerFailure(
            "strategyRuntimeError",
            f"{hook_name} raised {type(error).__name__}: {error}",
        )
        failure.diagnostics = _hook_diagnostics(hook_name, context.log, error)
        raise failure from error
    return {
        "commands": context.orders._commands,
        "logs": context.log._entries,
        "omittedLogBytes": context.log.omitted_bytes,
        "omittedLogEntries": context.log.omitted_entries,
    }


def _hook_diagnostics(hook_name: str, logger: _Log,
                      error: BaseException) -> dict[str, object]:
    frames = [
        frame for frame in traceback.extract_tb(error.__traceback__)
        if frame.filename == "strategy.py"
    ]
    location = frames[-1] if frames else None
    traceback_lines = ["Traceback (most recent call last):"]
    for frame in frames:
        traceback_lines.append(
            f'  File "strategy.py", line {frame.lineno}, in {frame.name}'
        )
        if frame.line:
            traceback_lines.append(f"    {frame.line}")
    traceback_lines.append(f"{type(error).__name__}: {error}")
    trace_text, omitted_traceback_bytes = _bounded_traceback(
        traceback_lines, MAXIMUM_TRACEBACK_BYTES
    )
    return {
        "hook": hook_name,
        "logs": logger._entries,
        "omittedLogBytes": logger.omitted_bytes,
        "omittedLogEntries": logger.omitted_entries,
        "sourceColumn": (location.colno + 1) if location and location.colno is not None else None,
        "sourceLine": location.lineno if location else None,
        "traceback": trace_text,
        "omittedTracebackBytes": omitted_traceback_bytes,
    }


def _send_error(run_id: str, sequence: int, error: WorkerFailure) -> None:
    payload: dict[str, object] = {
        "errorCode": error.code,
        "message": str(error)[:8192],
    }
    if error.diagnostics is not None:
        payload["diagnostics"] = error.diagnostics
    _send_message(
        run_id,
        sequence,
        "worker-error",
        payload,
    )


def _read_shared_history(
    descriptor: int, length: int, expected_digest: str
) -> bytes:
    if descriptor < 0:
        if length != 0 or expected_digest:
            raise WorkerFailure(
                "WorkerHandshakeFailed", "shared history metadata has no descriptor"
            )
        return b""
    if (
        length < 48
        or length > MAXIMUM_HISTORY_BARS * 48
        or length % 48 != 0
        or len(expected_digest) != 64
    ):
        raise WorkerFailure("WorkerHandshakeFailed", "shared history metadata is invalid")
    try:
        with mmap.mmap(descriptor, length, access=mmap.ACCESS_READ) as mapping:
            data = mapping[:]
    finally:
        os.close(descriptor)
    if hashlib.sha256(data).hexdigest() != expected_digest:
        raise WorkerFailure("WorkerHandshakeFailed", "shared history digest does not match")
    return data


def main() -> int:
    global _SHARED_HISTORY_BYTES
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--run-id", required=True)
    parser.add_argument("--test-only-skip-numpy-seed", action="store_true")
    parser.add_argument("--require-managed-stockbt", action="store_true")
    parser.add_argument("--shared-data-fd", type=int, default=-1)
    parser.add_argument("--shared-data-length", type=int, default=0)
    parser.add_argument("--shared-data-sha256", default="")
    parser.add_argument("--history-data-fd", type=int, default=-1)
    parser.add_argument("--history-data-length", type=int, default=0)
    parser.add_argument("--history-data-sha256", default="")
    parser.add_argument("--timezone-data-path", default="")
    arguments = parser.parse_args()
    run_id = arguments.run_id
    if not run_id:
        return 2

    output_sequence = 0
    input_sequence = 0
    try:
        _send_message(
            run_id,
            output_sequence,
            "bootstrap-ready",
            {
                "pythonVersion": ".".join(str(part) for part in sys.version_info[:3]),
                "protocolVersion": PROTOCOL_VERSION,
            },
        )
        output_sequence += 1
        message_type, handshake = _read_message(run_id, input_sequence)
        input_sequence += 1
        if message_type != "handshake" or not isinstance(handshake, dict):
            raise WorkerFailure("WorkerHandshakeFailed", "parent handshake was not received")
        if handshake.get("stockbtApiVersion") != STOCKBT_API_VERSION:
            raise WorkerFailure("WorkerHandshakeFailed", "stockbt API version does not match")
        if arguments.shared_data_fd < 0 or arguments.shared_data_length < 1:
            raise WorkerFailure("WorkerHandshakeFailed", "read-only strategy source is missing")
        _SHARED_HISTORY_BYTES = _read_shared_history(
            arguments.history_data_fd,
            arguments.history_data_length,
            arguments.history_data_sha256,
        )
        if arguments.timezone_data_path:
            if (
                not os.path.isabs(arguments.timezone_data_path)
                or not os.path.isdir(arguments.timezone_data_path)
            ):
                raise WorkerFailure(
                    "RuntimeIntegrityFailure", "managed timezone database path is invalid"
                )
            zoneinfo.reset_tzpath([arguments.timezone_data_path])
        _send_message(
            run_id,
            output_sequence,
            "handshake-complete",
            {
                "pythonVersion": ".".join(str(part) for part in sys.version_info[:3]),
                "stockbtApiVersion": STOCKBT_API_VERSION,
            },
        )
        output_sequence += 1

        message_type, load_request = _read_message(run_id, input_sequence)
        input_sequence += 1
        if message_type != "load-module" or not isinstance(load_request, dict):
            raise WorkerFailure("StrategyLoadFailed", "strategy module request was not received")
        if (
            load_request.get("sourceSha256") != arguments.shared_data_sha256
            or load_request.get("stockbtApiVersion") != STOCKBT_API_VERSION
        ):
            raise WorkerFailure("StrategyLoadFailed", "strategy module identity does not match")
        try:
            module = _evaluate_module(
                arguments.shared_data_fd,
                arguments.shared_data_length,
                arguments.shared_data_sha256,
                skip_numpy_for_test=arguments.test_only_skip_numpy_seed,
                require_managed_stockbt=arguments.require_managed_stockbt,
            )
        except WorkerFailure as error:
            _send_error(run_id, output_sequence, error)
            return 3
        finally:
            os.close(arguments.shared_data_fd)
            arguments.shared_data_fd = -1
        _send_message(
            run_id,
            output_sequence,
            "module-loaded",
            {"stockbtApiVersion": STOCKBT_API_VERSION},
        )
        output_sequence += 1

        message_type, factory_request = _read_message(run_id, input_sequence)
        input_sequence += 1
        if message_type != "create-strategy" or not isinstance(factory_request, dict):
            raise WorkerFailure("StrategyFactoryFailed", "strategy factory request was not received")
        try:
            strategy, registered_indicators = _create_strategy(
                module, factory_request.get("parameters")
            )
        except WorkerFailure as error:
            _send_error(run_id, output_sequence, error)
            return 3
        global _MAX_HISTORY_BARS
        _MAX_HISTORY_BARS = strategy.max_history_bars
        _HISTORY.clear()
        _send_message(
            run_id,
            output_sequence,
            "strategy-loaded",
            {
                "indicators": registered_indicators,
                "maxHistoryBars": strategy.max_history_bars,
            },
        )
        output_sequence += 1

        while True:
            message_type, callback = _read_message(run_id, input_sequence)
            input_sequence += 1
            if message_type == "shutdown":
                _send_message(run_id, output_sequence, "shutdown-complete", {})
                return 0
            if message_type == "history-seed":
                try:
                    _seed_history(callback)
                    _send_message(run_id, output_sequence, "history-seeded", {})
                except WorkerFailure as error:
                    _send_error(run_id, output_sequence, error)
                output_sequence += 1
                continue
            if message_type != "callback" or not isinstance(callback, dict):
                raise ProtocolViolation("expected callback or shutdown message")
            hook_name = callback.get("hook")
            try:
                _record_context_bar(callback)
                result = _run_hook(
                    strategy, hook_name, callback.get("context"), callback.get("fill")
                )
                _send_message(run_id, output_sequence, "callback-result", result)
            except WorkerFailure as error:
                _send_error(run_id, output_sequence, error)
            output_sequence += 1
    except WorkerFailure as error:
        try:
            _send_error(run_id, output_sequence, error)
        except (OSError, WorkerFailure):
            return 4
        return 5
    except (OSError, ValueError, TypeError, KeyError) as error:
        try:
            _send_error(run_id, output_sequence, ProtocolViolation(str(error)))
        except (OSError, WorkerFailure):
            return 6
        return 7


if __name__ == "__main__":
    raise SystemExit(main())
