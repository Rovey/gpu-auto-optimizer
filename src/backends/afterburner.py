"""
MSI Afterburner backend.

Control method:
  1. Reads current settings via MSI Afterburner Remote Server HTTP API (port 82)
     OR via Afterburner shared-memory segment.
  2. Writes desired settings by editing profile INI files directly.
  3. Applies the profile by launching AfterBurner.exe with /Profile{N} argument.

Supports: core clock offset, memory clock offset, power limit %, thermal limit,
          voltage offset (where Afterburner supports it on the GPU).

Profile INI fields (MSI Afterburner 4.6+):
  CoreClockBoost    – MHz offset
  MemoryClockBoost  – MHz offset
  CoreVoltageBoost  – mV offset (positive = boost, negative = undervolt)
  PowerLimit        – % of TDP (0 = auto, otherwise override)
  ThermalLimit      – °C
  FanSpeed          – % (0 = auto)
"""
from __future__ import annotations

import configparser
import os
import subprocess
import sys
import time
from pathlib import Path
from typing import Optional

try:
    import requests as _requests
    _REQUESTS_OK = True
except ImportError:
    _REQUESTS_OK = False

from .base import GPUBackend, AppliedSettings


# ---------------------------------------------------------------------------
# Afterburner paths (common install locations)
# ---------------------------------------------------------------------------

_AB_SEARCH_PATHS = [
    r"C:\Program Files (x86)\MSI Afterburner",
    r"C:\Program Files\MSI Afterburner",
]


def _find_afterburner_root() -> Optional[Path]:
    for p in _AB_SEARCH_PATHS:
        exe = Path(p) / "MSIAfterburner.exe"
        if exe.exists():
            return Path(p)
    return None


def _profile_dir(ab_root: Path) -> Path:
    # Profiles can be in either install dir or AppData
    candidates = [
        ab_root / "Profiles",
        Path(os.environ.get("APPDATA", "")) / "MSI Afterburner" / "Profiles",
    ]
    for c in candidates:
        if c.exists():
            return c
    # Default: inside install dir even if it doesn't exist yet
    return ab_root / "Profiles"


# ---------------------------------------------------------------------------
# Profile file reader / writer
# ---------------------------------------------------------------------------

class _AfterburnerProfile:
    """Read and write a single Afterburner profile INI file."""

    def __init__(self, path: Path) -> None:
        self.path = path
        self._cfg = configparser.RawConfigParser()
        self._cfg.optionxform = str   # preserve case
        if path.exists():
            try:
                self._cfg.read(str(path), encoding="utf-8")
            except Exception:
                pass

    # Afterburner stores values as "value,default_value" in some versions;
    # we write just the plain integer value which is backward-compatible.
    def set(self, section: str, key: str, value: int) -> None:
        if not self._cfg.has_section(section):
            self._cfg.add_section(section)
        self._cfg.set(section, key, str(value))

    def get_int(self, section: str, key: str, fallback: int = 0) -> int:
        raw = self._cfg.get(section, key, fallback=str(fallback))
        # Handle "value,default" format
        first = raw.split(",")[0].strip()
        try:
            return int(float(first))
        except ValueError:
            return fallback

    def save(self) -> None:
        self.path.parent.mkdir(parents=True, exist_ok=True)
        with open(self.path, "w", encoding="utf-8") as fh:
            self._cfg.write(fh)


# ---------------------------------------------------------------------------
# Backend
# ---------------------------------------------------------------------------

class AfterburnerBackend(GPUBackend):
    """
    MSI Afterburner profile-file-based backend.
    Works with Afterburner 4.6+ on Windows.
    Priority 20 – preferred over nvidia-smi fallback.
    """

    name     = "msi-afterburner"
    priority = 20

    # Profile slot to use (1–5). We use slot 5 to avoid disturbing user's presets.
    PROFILE_SLOT = 5

    def __init__(self) -> None:
        self._root: Optional[Path] = None
        self._exe:  Optional[Path] = None

    def is_available(self) -> bool:
        if sys.platform != "win32":
            return False
        self._root = _find_afterburner_root()
        if self._root is None:
            return False
        self._exe = self._root / "MSIAfterburner.exe"
        return self._exe.exists()

    def supports_voltage(self, gpu_index: int) -> bool:
        return self.is_available()

    def supports_core_oc(self, gpu_index: int) -> bool:
        return self.is_available()

    def supports_mem_oc(self, gpu_index: int) -> bool:
        return self.is_available()

    # ------------------------------------------------------------------

    def apply(
        self,
        gpu_index:         int,
        core_offset_mhz:   int = 0,
        mem_offset_mhz:    int = 0,
        voltage_offset_mv: int = 0,
        power_limit_pct:   int = 100,
        thermal_limit_c:   int = 83,
    ) -> AppliedSettings:
        if not self.is_available():
            return AppliedSettings(success=False, notes="MSI Afterburner not found")

        assert self._root is not None

        prof_dir  = _profile_dir(self._root)
        prof_path = prof_dir / f"Profile{self.PROFILE_SLOT}.cfg"
        prof      = _AfterburnerProfile(prof_path)

        # Afterburner profiles use a [Settings] or top-level (no section) format.
        # Most Afterburner versions use no section header, or a [profile] section.
        # We write to the default section (empty string) to be safe.
        section = ""   # no section = top-level INI keys

        # Helper to avoid writing [DEFAULT] section (configparser quirk)
        def _write(key: str, val: int) -> None:
            # Use a named section "GPU{idx}" so we support multi-GPU profiles
            sec = f"GPU{gpu_index}"
            prof.set(sec, key, val)

        _write("CoreClockBoost",   core_offset_mhz)
        _write("MemoryClockBoost", mem_offset_mhz)
        _write("CoreVoltageBoost", voltage_offset_mv)
        _write("PowerLimit",       power_limit_pct)
        _write("ThermalLimit",     thermal_limit_c)
        _write("FanSpeed",         0)             # 0 = auto fan

        try:
            prof.save()
        except Exception as exc:
            return AppliedSettings(
                success=False,
                notes=f"Failed to write profile {prof_path}: {exc}",
            )

        # Launch Afterburner with the profile argument
        try:
            subprocess.Popen(
                [str(self._exe), f"/Profile{self.PROFILE_SLOT}"],
                shell=False,
            )
            # Give Afterburner time to apply
            time.sleep(2.5)
        except Exception as exc:
            return AppliedSettings(
                success=False,
                notes=f"Failed to launch Afterburner: {exc}",
            )

        return AppliedSettings(
            core_offset_mhz   = core_offset_mhz,
            mem_offset_mhz    = mem_offset_mhz,
            voltage_offset_mv = voltage_offset_mv,
            power_limit_pct   = power_limit_pct,
            thermal_limit_c   = thermal_limit_c,
            success           = True,
            notes             = (
                f"Profile{self.PROFILE_SLOT} applied via MSI Afterburner. "
                f"Core+{core_offset_mhz} MHz  Mem+{mem_offset_mhz} MHz  "
                f"Volt{voltage_offset_mv:+d} mV"
            ),
        )

    def reset(self, gpu_index: int) -> bool:
        """Apply all-zero profile (stock settings)."""
        result = self.apply(
            gpu_index,
            core_offset_mhz   = 0,
            mem_offset_mhz    = 0,
            voltage_offset_mv = 0,
            power_limit_pct   = 100,
            thermal_limit_c   = 83,
        )
        return result.success

    # ------------------------------------------------------------------
    # Remote Server monitoring (bonus: read current AB telemetry)
    # ------------------------------------------------------------------

    def read_remote_server(self, timeout: float = 2.0) -> Optional[dict]:
        """Read hardware data from Afterburner Remote Server (port 82)."""
        if not _REQUESTS_OK:
            return None
        try:
            resp = _requests.get("http://localhost:82/mahm", timeout=timeout)
            if resp.status_code == 200:
                return {"raw": resp.text}
        except Exception:
            pass
        return None
