"""The managed Stock Back Test Python strategy API."""

from __future__ import annotations

from types import MappingProxyType


STOCKBT_API_VERSION = "1"
__version__ = STOCKBT_API_VERSION
MAXIMUM_HISTORY_BARS = 10_000


def SMA(name: str, *, period: int) -> object:
    """Declare an engine-owned simple moving average."""
    if not isinstance(name, str) or not name.isascii() or not name:
        raise ValueError("SMA name must be non-empty ASCII text")
    if isinstance(period, bool) or not isinstance(period, int):
        raise TypeError("SMA period must be an integer")
    if period < 1 or period > MAXIMUM_HISTORY_BARS:
        raise ValueError("SMA period is outside the supported range")
    return MappingProxyType({"kind": "SMA", "name": name, "period": period})
