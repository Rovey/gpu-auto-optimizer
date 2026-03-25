"""
Abstract base class for GPU control backends.
"""
from __future__ import annotations

from abc import ABC, abstractmethod
from dataclasses import dataclass
from typing import Optional


@dataclass
class AppliedSettings:
    """Confirmation of what was actually applied."""
    core_offset_mhz:   int   = 0
    mem_offset_mhz:    int   = 0
    voltage_offset_mv: int   = 0
    power_limit_pct:   int   = 100
    thermal_limit_c:   int   = 83
    success:           bool  = True
    notes:             str   = ""
    verified:          bool  = False


class GPUBackend(ABC):
    """
    Each concrete backend implements how settings are physically applied.
    Backends are tried in priority order by the optimizer.
    """

    name:     str = "base"
    priority: int = 0   # higher = preferred

    @abstractmethod
    def is_available(self) -> bool:
        """Return True if this backend can operate on the current system."""

    @abstractmethod
    def apply(
        self,
        gpu_index:         int,
        core_offset_mhz:   int = 0,
        mem_offset_mhz:    int = 0,
        voltage_offset_mv: int = 0,
        power_limit_pct:   int = 100,
        thermal_limit_c:   int = 83,
    ) -> AppliedSettings:
        """Apply the requested settings. Returns what was actually applied."""

    @abstractmethod
    def reset(self, gpu_index: int) -> bool:
        """Reset GPU to stock / default settings."""

    def supports_voltage(self, gpu_index: int) -> bool:   # noqa: ARG002
        return False

    def supports_core_oc(self, gpu_index: int) -> bool:   # noqa: ARG002
        return False

    def supports_mem_oc(self, gpu_index: int) -> bool:    # noqa: ARG002
        return False

    def verify(self, gpu_index: int) -> dict | None:     # noqa: ARG002
        """Read back applied offsets. Returns dict with core_offset_khz,
        mem_offset_khz, volt_offset_uv or None if unsupported."""
        return None
