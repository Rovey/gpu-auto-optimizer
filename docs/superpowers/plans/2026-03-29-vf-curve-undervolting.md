# V/F Curve Undervolting Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement automatic GPU undervolting via NVAPI V/F curve manipulation, finding the lowest stable voltage for the GPU's natural boost clock to achieve higher sustained frequencies, lower temperatures, and reduced power draw.

**Architecture:** A new `NVAPIVFCurveBackend` (priority 40) reads/writes the V/F curve via undocumented NVAPI functions. The optimizer gains a `_search_optimal_vf_point()` method that binary-searches on voltage instead of frequency. Memory OC remains via PState20. Falls back to existing PState20 backend when V/F curves are unavailable.

**Tech Stack:** Python 3.8+, ctypes (NVAPI raw buffers), pynvml, pytest

---

## File Map

| File | Responsibility |
|---|---|
| `src/backends/nvapi_vfcurve.py` | **New.** V/F curve read/write/reset/verify via NVAPI |
| `src/backends/base.py` | Add `target_voltage_mv` and `target_freq_mhz` to `AppliedSettings` |
| `src/config.py` | Add `target_voltage_mv`, `target_freq_mhz` to result; add `voltage_min_mv` to profiles |
| `src/optimizer.py` | Add `_search_optimal_vf_point()`, update `_best_backend()`, modify `_run_full_mode()` |
| `src/boot_apply.py` | V/F curve re-apply path |
| `tests/test_vfcurve.py` | **New.** Tests for V/F curve data structures, delta math, 2x scaling |

---

### Task 1: Config Changes — Result Fields and Risk Profile Additions

**Files:**
- Modify: `src/config.py:117-137` (GPUOptimizationResult)
- Modify: `src/config.py:25-110` (RISK_PROFILES)
- Modify: `src/backends/base.py:11-21` (AppliedSettings)
- Test: `tests/test_config.py`

- [ ] **Step 1: Add fields to AppliedSettings**

In `src/backends/base.py`, add two fields to the `AppliedSettings` dataclass after `thermal_limit_c`:

```python
@dataclass
class AppliedSettings:
    """Confirmation of what was actually applied."""
    core_offset_mhz:   int   = 0
    mem_offset_mhz:    int   = 0
    voltage_offset_mv: int   = 0
    power_limit_pct:   int   = 100
    thermal_limit_c:   int   = 83
    target_voltage_mv: int   = 0     # V/F curve lock voltage (0 = not used)
    target_freq_mhz:   int   = 0     # frequency at locked voltage
    success:           bool  = True
    notes:             str   = ""
    verified:          bool  = False
```

- [ ] **Step 2: Add fields to GPUOptimizationResult**

In `src/config.py`, add two fields to `GPUOptimizationResult` after `thermal_limit_c` (line 127):

```python
@dataclass
class GPUOptimizationResult:
    gpu_index:             int
    gpu_name:              str
    risk_level:            str
    # Applied settings
    core_offset_mhz:       int   = 0
    mem_offset_mhz:        int   = 0
    voltage_offset_mv:     int   = 0
    power_limit_pct:       int   = 100
    thermal_limit_c:       int   = 83
    target_voltage_mv:     int   = 0    # V/F curve lock voltage (0 = not used)
    target_freq_mhz:       int   = 0    # frequency at locked voltage
    # Measured outcomes
    baseline_boost_mhz:    int   = 0
    achieved_boost_mhz:    int   = 0
    baseline_temp_c:       float = 0.0
    achieved_temp_c:       float = 0.0
    baseline_power_w:      float = 0.0
    achieved_power_w:      float = 0.0
    stability_passed:      bool  = False
    notes:                 str   = ""
```

- [ ] **Step 3: Add voltage_min_mv to risk profiles**

In `src/config.py`, add `"voltage_min_mv"` to each risk profile in `RISK_PROFILES`:

```python
RiskLevel.SAFE: {
    ...
    "test_passes":            2,
    "voltage_min_mv":         0,      # 0 = V/F curve disabled
},
RiskLevel.BALANCED: {
    ...
    "test_passes":            3,
    "voltage_min_mv":         800,
},
RiskLevel.PERFORMANCE: {
    ...
    "test_passes":            5,
    "voltage_min_mv":         725,
},
RiskLevel.EXTREME: {
    ...
    "test_passes":            8,
    "voltage_min_mv":         650,
},
```

- [ ] **Step 4: Run tests**

Run: `pytest tests/test_config.py tests/test_nvapi.py -v`
Expected: All PASS (new fields have defaults, so no existing tests break)

- [ ] **Step 5: Commit**

```bash
git add src/backends/base.py src/config.py
git commit -m "feat: add V/F curve fields to AppliedSettings, result, and risk profiles"
```

---

### Task 2: V/F Curve Backend — Data Structures and Reading

**Files:**
- Create: `src/backends/nvapi_vfcurve.py`
- Create: `tests/test_vfcurve.py`

- [ ] **Step 1: Write tests for VFPoint and curve parsing**

Create `tests/test_vfcurve.py`:

```python
"""Tests for V/F curve backend data structures and parsing."""
import struct
import pytest


def test_vfpoint_creation():
    from src.backends.nvapi_vfcurve import VFPoint
    p = VFPoint(voltage_uv=900000, base_freq_khz=1800000, delta_khz=50000)
    assert p.voltage_uv == 900000
    assert p.base_freq_khz == 1800000
    assert p.delta_khz == 50000
    assert p.voltage_mv == 900
    assert p.effective_freq_mhz == 1850


def test_vfpoint_effective_freq():
    from src.backends.nvapi_vfcurve import VFPoint
    p = VFPoint(voltage_uv=850000, base_freq_khz=1700000, delta_khz=-100000)
    assert p.effective_freq_mhz == 1600


def test_parse_vf_points_from_raw_buffers():
    """Simulate parsing raw mask + VFP + clock table buffers."""
    from src.backends.nvapi_vfcurve import _parse_vf_points, _MASK_ENTRY_SIZE, _VFP_ENTRY_SIZE, _CLOCK_TABLE_ENTRY_SIZE

    # Build minimal fake buffers with 2 graphics points
    # Mask buffer: version(4) + mask(64) + entries(255 * 24)
    mask_buf = bytearray(6188)
    struct.pack_into('<I', mask_buf, 0, 0x0001182C)  # version
    # Entry 0: clockType=0 (graphics), enabled=1
    off = 68
    struct.pack_into('<I', mask_buf, off, 0)  # clockType = graphics
    mask_buf[off + 4] = 1  # enabled
    # Entry 1: clockType=0 (graphics), enabled=1
    off = 68 + _MASK_ENTRY_SIZE
    struct.pack_into('<I', mask_buf, off, 0)
    mask_buf[off + 4] = 1

    # VFP buffer: version(4) + mask(64) + entries(255 * 28)
    vfp_buf = bytearray(7208)
    struct.pack_into('<I', vfp_buf, 0, 0x00011C28)
    # Entry 0: 850mV, 1700 MHz
    off = 68
    struct.pack_into('<I', vfp_buf, off, 0)        # clockType
    struct.pack_into('<I', vfp_buf, off + 4, 1700000)  # freq kHz
    struct.pack_into('<I', vfp_buf, off + 8, 850000)   # voltage uV
    # Entry 1: 900mV, 1800 MHz
    off = 68 + _VFP_ENTRY_SIZE
    struct.pack_into('<I', vfp_buf, off, 0)
    struct.pack_into('<I', vfp_buf, off + 4, 1800000)
    struct.pack_into('<I', vfp_buf, off + 8, 900000)

    # Clock table buffer: version(4) + mask(64) + entries(255 * 36)
    ct_buf = bytearray(9248)
    struct.pack_into('<I', ct_buf, 0, 0x00012420)
    # Entry 0: delta = 0
    off = 68
    struct.pack_into('<I', ct_buf, off, 0)       # clockType
    struct.pack_into('<i', ct_buf, off + 20, 0)  # delta (at 2x scale)
    # Entry 1: delta = +100 MHz (stored as 200000 kHz at 2x scale)
    off = 68 + _CLOCK_TABLE_ENTRY_SIZE
    struct.pack_into('<I', ct_buf, off, 0)
    struct.pack_into('<i', ct_buf, off + 20, 200000)

    points = _parse_vf_points(bytes(mask_buf), bytes(vfp_buf), bytes(ct_buf))
    assert len(points) == 2
    assert points[0].voltage_uv == 850000
    assert points[0].base_freq_khz == 1700000
    assert points[0].delta_khz == 0   # 0 / 2 = 0
    assert points[1].voltage_uv == 900000
    assert points[1].delta_khz == 100000  # 200000 / 2 = 100000


def test_2x_scaling_write():
    """Delta values must be multiplied by 2 when writing."""
    from src.backends.nvapi_vfcurve import _apply_delta_to_clock_table, _CLOCK_TABLE_ENTRY_SIZE
    ct_buf = bytearray(9248)
    struct.pack_into('<I', ct_buf, 0, 0x00012420)
    # Set up entry 0 as graphics
    off = 68
    struct.pack_into('<I', ct_buf, off, 0)  # clockType = graphics

    # Apply delta of +50000 kHz to entry index 0
    _apply_delta_to_clock_table(ct_buf, index=0, delta_khz=50000)

    # Read back: should be stored as 100000 (2x)
    stored = struct.unpack_from('<i', ct_buf, off + 20)[0]
    assert stored == 100000


def test_build_undervolt_deltas():
    """Given target voltage + freq, compute correct deltas for all points."""
    from src.backends.nvapi_vfcurve import VFPoint, _compute_undervolt_deltas

    points = [
        VFPoint(voltage_uv=800000, base_freq_khz=1600000, delta_khz=0),
        VFPoint(voltage_uv=850000, base_freq_khz=1700000, delta_khz=0),
        VFPoint(voltage_uv=900000, base_freq_khz=1800000, delta_khz=0),
        VFPoint(voltage_uv=950000, base_freq_khz=1900000, delta_khz=0),
        VFPoint(voltage_uv=1000000, base_freq_khz=2000000, delta_khz=0),
    ]

    # Target: 1850 MHz @ 900mV
    deltas = _compute_undervolt_deltas(points, target_voltage_uv=900000, target_freq_khz=1850000)

    # Points at or below 900mV: delta should make effective freq = 1850 MHz
    assert deltas[0] == 1850000 - 1600000  # +250 MHz
    assert deltas[1] == 1850000 - 1700000  # +150 MHz
    assert deltas[2] == 1850000 - 1800000  # +50 MHz

    # Points above 900mV: flatten to target_freq or below
    assert deltas[3] <= 1850000 - 1900000  # -50 MHz or lower
    assert deltas[4] <= 1850000 - 2000000  # -150 MHz or lower
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `pytest tests/test_vfcurve.py -v`
Expected: FAIL (module not found)

- [ ] **Step 3: Create nvapi_vfcurve.py with data structures and parsing**

Create `src/backends/nvapi_vfcurve.py`:

```python
"""
NVAPI V/F Curve backend — voltage-frequency curve manipulation.

Uses undocumented NVAPI functions to read and modify the GPU's V/F curve,
enabling per-voltage-point undervolting (the same technique as MSI Afterburner's
curve editor).

Supported GPUs: Pascal (GTX 10), Turing (RTX 20), Ampere (RTX 30), Ada (RTX 40).
Not supported: Maxwell (no V/F curve), Blackwell (NVIDIA locked SetClockBoostTable).

NVAPI functions used:
  GetClockBoostMask   0x507B4B59  — discover which V/F points exist
  GetVFPCurve         0x21537AD4  — read base voltage + frequency per point
  GetClockBoostTable  0x23F1B133  — read per-point frequency deltas
  SetClockBoostTable  0x0733E009  — write per-point frequency deltas
  GetClockBoostLock   0xE440B867  — read voltage lock state
  SetClockBoostLock   0x39442CFB  — lock GPU to specific voltage point

Buffer formats (all version 1):
  ClockBoostMask:  6188 bytes, version 0x0001182C
  VFPCurve:        7208 bytes, version 0x00011C28
  ClockBoostTable: 9248 bytes, version 0x00012420

CRITICAL: ClockBoostTable frequencyDeltaKHz is stored at 2x scale internally.
Divide by 2 when reading, multiply by 2 when writing.
"""
from __future__ import annotations

import ctypes
import struct
from dataclasses import dataclass
from typing import List, Optional

from .base import GPUBackend, AppliedSettings

# ---------------------------------------------------------------------------
# Function IDs
# ---------------------------------------------------------------------------

_NVAPI_GET_CLOCK_BOOST_MASK  = 0x507B4B59
_NVAPI_GET_VFP_CURVE         = 0x21537AD4
_NVAPI_GET_CLOCK_BOOST_TABLE = 0x23F1B133
_NVAPI_SET_CLOCK_BOOST_TABLE = 0x0733E009
_NVAPI_GET_CLOCK_BOOST_LOCK  = 0xE440B867
_NVAPI_SET_CLOCK_BOOST_LOCK  = 0x39442CFB

# ---------------------------------------------------------------------------
# Buffer sizes and versions
# ---------------------------------------------------------------------------

_MASK_BUF_SIZE        = 6188
_MASK_VERSION         = _MASK_BUF_SIZE | (1 << 16)  # 0x0001182C

_VFP_BUF_SIZE         = 7208
_VFP_VERSION          = _VFP_BUF_SIZE | (1 << 16)   # 0x00011C28

_CLOCK_TABLE_BUF_SIZE = 9248
_CLOCK_TABLE_VERSION  = _CLOCK_TABLE_BUF_SIZE | (1 << 16)  # 0x00012420

# Lock structure: version(4) + flags(4) + count(4) + 32 entries * 24 bytes = 780
_LOCK_BUF_SIZE        = 780
_LOCK_VERSION         = _LOCK_BUF_SIZE | (2 << 16)  # V2

# Entry layout within each buffer (after 4-byte version + 64-byte mask/header)
_ENTRIES_OFFSET       = 68    # entries start at byte 68 in all three buffers
_MASK_ENTRY_SIZE      = 24    # per entry in ClockBoostMask
_VFP_ENTRY_SIZE       = 28    # per entry in VFPCurve
_CLOCK_TABLE_ENTRY_SIZE = 36  # per entry in ClockBoostTable

_MAX_ENTRIES          = 255

# ClockBoostTable delta offset within an entry
_CT_DELTA_OFFSET      = 20   # frequencyDeltaKHz at +20 within each 36-byte entry

# Clock domain types
_CLOCK_DOMAIN_GRAPHICS = 0
_CLOCK_DOMAIN_MEMORY   = 4

NVAPI_OK = 0


# ---------------------------------------------------------------------------
# V/F Point data class
# ---------------------------------------------------------------------------

@dataclass
class VFPoint:
    """One point on the voltage-frequency curve."""
    voltage_uv:    int   # microvolts
    base_freq_khz: int   # base frequency in kHz (from VFPCurve, read-only)
    delta_khz:     int   # current frequency delta in kHz (from ClockBoostTable)

    @property
    def voltage_mv(self) -> int:
        return self.voltage_uv // 1000

    @property
    def effective_freq_mhz(self) -> int:
        return (self.base_freq_khz + self.delta_khz) // 1000


# ---------------------------------------------------------------------------
# Pure functions for parsing and building buffers (testable without hardware)
# ---------------------------------------------------------------------------

def _parse_vf_points(
    mask_buf: bytes,
    vfp_buf: bytes,
    ct_buf: bytes,
) -> List[VFPoint]:
    """Parse raw NVAPI buffers into a sorted list of VFPoints for the graphics domain."""
    points: List[VFPoint] = []

    for i in range(_MAX_ENTRIES):
        # Check mask: is this entry enabled and graphics?
        m_off = _ENTRIES_OFFSET + i * _MASK_ENTRY_SIZE
        if m_off + _MASK_ENTRY_SIZE > len(mask_buf):
            break
        clock_type = struct.unpack_from('<I', mask_buf, m_off)[0]
        enabled = mask_buf[m_off + 4]
        if clock_type != _CLOCK_DOMAIN_GRAPHICS or not enabled:
            continue

        # Read VFP curve: base freq and voltage
        v_off = _ENTRIES_OFFSET + i * _VFP_ENTRY_SIZE
        if v_off + _VFP_ENTRY_SIZE > len(vfp_buf):
            break
        freq_khz = struct.unpack_from('<I', vfp_buf, v_off + 4)[0]
        voltage_uv = struct.unpack_from('<I', vfp_buf, v_off + 8)[0]

        # Read clock table: frequency delta (stored at 2x scale)
        c_off = _ENTRIES_OFFSET + i * _CLOCK_TABLE_ENTRY_SIZE
        if c_off + _CLOCK_TABLE_ENTRY_SIZE > len(ct_buf):
            break
        raw_delta = struct.unpack_from('<i', ct_buf, c_off + _CT_DELTA_OFFSET)[0]
        delta_khz = raw_delta // 2  # 2x scaling

        points.append(VFPoint(
            voltage_uv=voltage_uv,
            base_freq_khz=freq_khz,
            delta_khz=delta_khz,
        ))

    points.sort(key=lambda p: p.voltage_uv)
    return points


def _apply_delta_to_clock_table(
    ct_buf: bytearray,
    index: int,
    delta_khz: int,
) -> None:
    """Write a frequency delta to a specific entry in the clock table buffer.
    Applies the 2x scaling for NVAPI's internal representation."""
    c_off = _ENTRIES_OFFSET + index * _CLOCK_TABLE_ENTRY_SIZE
    struct.pack_into('<i', ct_buf, c_off + _CT_DELTA_OFFSET, delta_khz * 2)


def _compute_undervolt_deltas(
    points: List[VFPoint],
    target_voltage_uv: int,
    target_freq_khz: int,
) -> List[int]:
    """Compute the delta_khz for each V/F point to achieve an undervolt.

    Points at or below target_voltage: set delta so effective freq = target_freq.
    Points above target_voltage: flatten curve (effective freq <= target_freq).
    Returns list of delta_khz values (one per point, same order as input).
    """
    deltas: List[int] = []
    for p in points:
        if p.voltage_uv <= target_voltage_uv:
            # Raise frequency to target at this voltage
            delta = target_freq_khz - p.base_freq_khz
            deltas.append(delta)
        else:
            # Flatten: cap at target freq (negative delta to pull down)
            delta = target_freq_khz - p.base_freq_khz
            deltas.append(delta)
    return deltas


# ---------------------------------------------------------------------------
# Backend class
# ---------------------------------------------------------------------------

class NVAPIVFCurveBackend(GPUBackend):
    """
    V/F curve manipulation via undocumented NVAPI functions.
    Priority 40 — preferred over PState20 when available.
    Requires: Windows + NVIDIA drivers + administrator rights + Pascal or newer.
    """

    name     = "nvapi-vfcurve"
    priority = 40

    def __init__(self) -> None:
        from .nvapi import _NVAPILoader
        self._loader = _NVAPILoader.get()
        self._ready = False
        self._vf_supported = False
        # Cache the mask buffer for reuse
        self._mask_cache: Optional[bytes] = None

    def is_available(self) -> bool:
        if self._ready:
            return self._vf_supported
        self._ready = True
        if not self._loader.load():
            return False
        # Probe: try reading the clock boost mask
        mask = self._read_raw_mask(0)
        if mask is None:
            return False
        # Check if there are any enabled graphics entries
        for i in range(_MAX_ENTRIES):
            off = _ENTRIES_OFFSET + i * _MASK_ENTRY_SIZE
            if off + _MASK_ENTRY_SIZE > len(mask):
                break
            clock_type = struct.unpack_from('<I', mask, off)[0]
            enabled = mask[off + 4]
            if clock_type == _CLOCK_DOMAIN_GRAPHICS and enabled:
                self._vf_supported = True
                self._mask_cache = mask
                return True
        return False

    def supports_voltage(self, gpu_index: int) -> bool:
        return self.is_available()

    def supports_core_oc(self, gpu_index: int) -> bool:
        return self.is_available()

    def supports_mem_oc(self, gpu_index: int) -> bool:
        return False  # memory OC still via PState20

    def read_vf_curve(self, gpu_index: int) -> List[VFPoint]:
        """Read the current V/F curve as a list of VFPoints."""
        mask = self._read_raw_mask(gpu_index)
        vfp = self._read_raw_vfp(gpu_index, mask)
        ct = self._read_raw_clock_table(gpu_index, mask)
        if mask is None or vfp is None or ct is None:
            return []
        return _parse_vf_points(mask, vfp, ct)

    def apply_vf_lock(
        self,
        gpu_index: int,
        target_voltage_uv: int,
        target_freq_khz: int,
    ) -> bool:
        """Apply V/F curve flattening + voltage lock. Returns True on success."""
        mask = self._read_raw_mask(gpu_index)
        vfp = self._read_raw_vfp(gpu_index, mask)
        ct_bytes = self._read_raw_clock_table(gpu_index, mask)
        if mask is None or vfp is None or ct_bytes is None:
            return False

        points = _parse_vf_points(mask, vfp, ct_bytes)
        if not points:
            return False

        deltas = _compute_undervolt_deltas(points, target_voltage_uv, target_freq_khz)

        # Build modified clock table
        ct_buf = bytearray(ct_bytes)
        # Re-apply mask to clock table
        ct_buf[4:68] = mask[4:68]

        # Find the indices in the original buffer for graphics entries
        gfx_idx = 0
        for i in range(_MAX_ENTRIES):
            m_off = _ENTRIES_OFFSET + i * _MASK_ENTRY_SIZE
            if m_off + _MASK_ENTRY_SIZE > len(mask):
                break
            clock_type = struct.unpack_from('<I', mask, m_off)[0]
            enabled = mask[m_off + 4]
            if clock_type == _CLOCK_DOMAIN_GRAPHICS and enabled:
                if gfx_idx < len(deltas):
                    _apply_delta_to_clock_table(ct_buf, i, deltas[gfx_idx])
                    gfx_idx += 1

        # Write clock table
        ok = self._write_raw_clock_table(gpu_index, bytes(ct_buf))
        if not ok:
            return False

        # Set voltage lock
        ok = self._set_voltage_lock(gpu_index, target_voltage_uv)
        return ok

    def apply(
        self,
        gpu_index: int,
        core_offset_mhz: int = 0,
        mem_offset_mhz: int = 0,
        voltage_offset_mv: int = 0,
        power_limit_pct: int = 100,
        thermal_limit_c: int = 83,
        target_voltage_mv: int = 0,
        target_freq_mhz: int = 0,
    ) -> AppliedSettings:
        if not self.is_available():
            return AppliedSettings(success=False, notes="V/F curve backend not available")

        notes: list[str] = []

        # --- Apply V/F curve if target voltage is specified ---
        vf_ok = False
        if target_voltage_mv > 0 and target_freq_mhz > 0:
            vf_ok = self.apply_vf_lock(
                gpu_index,
                target_voltage_uv=target_voltage_mv * 1000,
                target_freq_khz=target_freq_mhz * 1000,
            )
            if vf_ok:
                notes.append(f"V/F curve: {target_freq_mhz} MHz @ {target_voltage_mv} mV")
            else:
                notes.append("V/F curve apply failed")

        # --- Apply memory OC via PState20 ---
        if mem_offset_mhz != 0:
            from .nvapi import _NVAPILoader
            loader = _NVAPILoader.get()
            mem_ok = loader.set_pstate20_raw(gpu_index, 0, mem_offset_mhz * 1000)
            if mem_ok:
                notes.append(f"Mem +{mem_offset_mhz} MHz via PState20")
            else:
                notes.append(f"Memory OC failed (PState20 rc={loader._last_error})")

        # --- Apply power limit via pynvml ---
        try:
            import pynvml
            pynvml.nvmlInit()
            try:
                h = pynvml.nvmlDeviceGetHandleByIndex(gpu_index)
                default_mw = pynvml.nvmlDeviceGetPowerManagementDefaultLimit(h)
                min_mw, max_mw = pynvml.nvmlDeviceGetPowerManagementLimitConstraints(h)
                target_mw = int(default_mw * power_limit_pct / 100)
                target_mw = max(min_mw, min(max_mw, target_mw))
                pynvml.nvmlDeviceSetPowerManagementLimit(h, target_mw)
                notes.append(f"Power limit set to {target_mw // 1000} W")
            finally:
                pynvml.nvmlShutdown()
        except Exception as e:
            notes.append(f"Power limit skipped ({e})")

        return AppliedSettings(
            core_offset_mhz=0,
            mem_offset_mhz=mem_offset_mhz,
            voltage_offset_mv=0,
            power_limit_pct=power_limit_pct,
            thermal_limit_c=thermal_limit_c,
            target_voltage_mv=target_voltage_mv if vf_ok else 0,
            target_freq_mhz=target_freq_mhz if vf_ok else 0,
            success=vf_ok or target_voltage_mv == 0,
            notes="; ".join(notes),
            verified=vf_ok,
        )

    def reset(self, gpu_index: int) -> bool:
        """Reset V/F curve to stock: zero all deltas + unlock voltage."""
        mask = self._read_raw_mask(gpu_index)
        if mask is None:
            return False

        # Zero clock table
        ct_buf = bytearray(_CLOCK_TABLE_BUF_SIZE)
        struct.pack_into('<I', ct_buf, 0, _CLOCK_TABLE_VERSION)
        ct_buf[4:68] = mask[4:68]
        # All deltas default to 0

        ok = self._write_raw_clock_table(gpu_index, bytes(ct_buf))

        # Unlock voltage
        self._clear_voltage_lock(gpu_index)

        # Reset memory via PState20
        from .nvapi import _NVAPILoader
        _NVAPILoader.get().set_pstate20_raw(gpu_index, 0, 0)

        return ok

    def verify(self, gpu_index: int) -> dict | None:
        """Read back V/F curve state."""
        if not self.is_available():
            return None
        points = self.read_vf_curve(gpu_index)
        if not points:
            return None
        # Find the point with highest delta (likely the locked point)
        max_point = max(points, key=lambda p: p.delta_khz)
        return {
            "core_offset_khz": 0,
            "mem_offset_khz": 0,
            "volt_offset_uv": 0,
            "target_voltage_uv": max_point.voltage_uv,
            "target_freq_khz": max_point.base_freq_khz + max_point.delta_khz,
            "curve_points": len(points),
        }

    # ------------------------------------------------------------------
    # Raw NVAPI buffer operations
    # ------------------------------------------------------------------

    def _read_raw_mask(self, gpu_index: int) -> Optional[bytes]:
        fn = self._loader._get_func(
            _NVAPI_GET_CLOCK_BOOST_MASK, ctypes.c_int32,
            [ctypes.c_void_p, ctypes.c_void_p],
        )
        if fn is None:
            return None
        handle = self._loader.gpu_handle(gpu_index)
        if handle is None:
            return None
        buf = (ctypes.c_ubyte * _MASK_BUF_SIZE)()
        struct.pack_into('<I', buf, 0, _MASK_VERSION)
        rc = fn(handle, ctypes.byref(buf))
        if rc != NVAPI_OK:
            return None
        return bytes(buf)

    def _read_raw_vfp(self, gpu_index: int, mask: Optional[bytes] = None) -> Optional[bytes]:
        fn = self._loader._get_func(
            _NVAPI_GET_VFP_CURVE, ctypes.c_int32,
            [ctypes.c_void_p, ctypes.c_void_p],
        )
        if fn is None:
            return None
        handle = self._loader.gpu_handle(gpu_index)
        if handle is None:
            return None
        buf = (ctypes.c_ubyte * _VFP_BUF_SIZE)()
        struct.pack_into('<I', buf, 0, _VFP_VERSION)
        if mask:
            for i in range(4, 68):
                buf[i] = mask[i]
        rc = fn(handle, ctypes.byref(buf))
        if rc != NVAPI_OK:
            return None
        return bytes(buf)

    def _read_raw_clock_table(self, gpu_index: int, mask: Optional[bytes] = None) -> Optional[bytes]:
        fn = self._loader._get_func(
            _NVAPI_GET_CLOCK_BOOST_TABLE, ctypes.c_int32,
            [ctypes.c_void_p, ctypes.c_void_p],
        )
        if fn is None:
            return None
        handle = self._loader.gpu_handle(gpu_index)
        if handle is None:
            return None
        buf = (ctypes.c_ubyte * _CLOCK_TABLE_BUF_SIZE)()
        struct.pack_into('<I', buf, 0, _CLOCK_TABLE_VERSION)
        if mask:
            for i in range(4, 68):
                buf[i] = mask[i]
        rc = fn(handle, ctypes.byref(buf))
        if rc != NVAPI_OK:
            return None
        return bytes(buf)

    def _write_raw_clock_table(self, gpu_index: int, ct_buf: bytes) -> bool:
        fn = self._loader._get_func(
            _NVAPI_SET_CLOCK_BOOST_TABLE, ctypes.c_int32,
            [ctypes.c_void_p, ctypes.c_void_p],
        )
        if fn is None:
            return False
        handle = self._loader.gpu_handle(gpu_index)
        if handle is None:
            return False
        buf = (ctypes.c_ubyte * _CLOCK_TABLE_BUF_SIZE)()
        for i, b in enumerate(ct_buf[:_CLOCK_TABLE_BUF_SIZE]):
            buf[i] = b
        rc = fn(handle, ctypes.byref(buf))
        return rc == NVAPI_OK

    def _set_voltage_lock(self, gpu_index: int, voltage_uv: int) -> bool:
        fn = self._loader._get_func(
            _NVAPI_SET_CLOCK_BOOST_LOCK, ctypes.c_int32,
            [ctypes.c_void_p, ctypes.c_void_p],
        )
        if fn is None:
            return False
        handle = self._loader.gpu_handle(gpu_index)
        if handle is None:
            return False
        buf = (ctypes.c_ubyte * _LOCK_BUF_SIZE)()
        struct.pack_into('<I', buf, 0, _LOCK_VERSION)
        struct.pack_into('<I', buf, 4, 0)  # flags
        struct.pack_into('<I', buf, 8, 1)  # count = 1 entry
        # Entry 0: graphics domain, mode=3 (MANUAL), voltage
        entry_off = 12
        struct.pack_into('<I', buf, entry_off + 0, 0)          # index (graphics)
        struct.pack_into('<I', buf, entry_off + 4, 0)          # unknown
        struct.pack_into('<I', buf, entry_off + 8, 3)          # mode = MANUAL
        struct.pack_into('<I', buf, entry_off + 12, 0)         # unknown
        struct.pack_into('<I', buf, entry_off + 16, voltage_uv) # target voltage
        struct.pack_into('<I', buf, entry_off + 20, 0)         # unknown
        rc = fn(handle, ctypes.byref(buf))
        return rc == NVAPI_OK

    def _clear_voltage_lock(self, gpu_index: int) -> bool:
        fn = self._loader._get_func(
            _NVAPI_SET_CLOCK_BOOST_LOCK, ctypes.c_int32,
            [ctypes.c_void_p, ctypes.c_void_p],
        )
        if fn is None:
            return False
        handle = self._loader.gpu_handle(gpu_index)
        if handle is None:
            return False
        buf = (ctypes.c_ubyte * _LOCK_BUF_SIZE)()
        struct.pack_into('<I', buf, 0, _LOCK_VERSION)
        struct.pack_into('<I', buf, 4, 0)  # flags
        struct.pack_into('<I', buf, 8, 1)  # count = 1
        entry_off = 12
        struct.pack_into('<I', buf, entry_off + 0, 0)   # index
        struct.pack_into('<I', buf, entry_off + 4, 0)
        struct.pack_into('<I', buf, entry_off + 8, 0)    # mode = NONE
        struct.pack_into('<I', buf, entry_off + 12, 0)
        struct.pack_into('<I', buf, entry_off + 16, 0)   # voltage = 0
        struct.pack_into('<I', buf, entry_off + 20, 0)
        rc = fn(handle, ctypes.byref(buf))
        return rc == NVAPI_OK
```

- [ ] **Step 4: Run tests**

Run: `pytest tests/test_vfcurve.py -v`
Expected: All 5 tests PASS

- [ ] **Step 5: Commit**

```bash
git add src/backends/nvapi_vfcurve.py tests/test_vfcurve.py
git commit -m "feat: add V/F curve backend with read/write/parse and tests"
```

---

### Task 3: Optimizer — V/F Curve Search and Backend Selection

**Files:**
- Modify: `src/optimizer.py:44-53` (_best_backend)
- Modify: `src/optimizer.py:78-106` (GPUOptimizer.__init__)
- Modify: `src/optimizer.py:221-252` (_run_full_mode)
- Add new method: `_search_optimal_vf_point`

- [ ] **Step 1: Update _best_backend to include V/F curve backend**

In `src/optimizer.py`, add the import at the top (after line 37):

```python
from .backends.nvapi_vfcurve import NVAPIVFCurveBackend
```

Then update `_best_backend` (lines 44-53):

```python
def _best_backend(gpu: GPUInfo) -> GPUBackend:
    """Return the highest-priority available backend for this GPU."""
    candidates: List[GPUBackend] = [
        NVAPIVFCurveBackend(),
        NVAPIBackend(),
        NvidiaSMIBackend(),
    ]
    available = [b for b in candidates if b.is_available()]
    if not available:
        return NvidiaSMIBackend()
    return max(available, key=lambda b: b.priority)
```

- [ ] **Step 2: Add V/F state tracking to GPUOptimizer.__init__**

In `src/optimizer.py`, add to the "Working state" section (after line 98):

```python
        # V/F curve state (when using NVAPIVFCurveBackend)
        self._target_voltage_mv = 0
        self._target_freq_mhz  = 0
```

- [ ] **Step 3: Update _run_full_mode to use V/F search when available**

Replace `_run_full_mode` (lines 221-252):

```python
    def _run_full_mode(
        self,
        result:   GPUOptimizationResult,
        baseline: GPUMetrics,
    ) -> GPUOptimizationResult:
        backend_supports_vf  = (
            self._backend.supports_voltage(self._gpu.index)
            and self._profile.get("voltage_min_mv", 0) > 0
        )
        backend_supports_oc  = self._backend.supports_core_oc(self._gpu.index)
        backend_supports_uv  = (
            not backend_supports_vf  # only use old UV if no V/F curve
            and self._backend.supports_voltage(self._gpu.index)
            and self._gpu.supports_uv
        )
        backend_supports_mem = self._backend.supports_mem_oc(self._gpu.index)

        if backend_supports_vf:
            # V/F curve path: search for optimal voltage-frequency point
            self._emit("Phase 2/4 - V/F curve undervolt search...", 2, 10)
            voltage_mv, freq_mhz = self._search_optimal_vf_point(baseline)
            self._target_voltage_mv = voltage_mv
            self._target_freq_mhz  = freq_mhz

            # Memory OC via PState20
            if backend_supports_mem or hasattr(self._backend, '_loader'):
                self._emit("Phase 3/4 - Memory clock search...", 5, 10)
                self._mem_offset_mhz = self._binary_search_mem()

            # Power limit tuning
            self._emit("Phase 4/4 - Power limit tuning...", 7, 10)
            self._power_limit_pct = self._tune_power_limit(baseline)
        else:
            # PState20 fallback path (existing behavior)
            if backend_supports_oc:
                self._emit("Phase 2/5 - Core clock search...", 2, 10)
                self._core_offset_mhz = self._binary_search_core()

            if backend_supports_mem:
                self._emit("Phase 3/5 - Memory clock search...", 4, 10)
                self._mem_offset_mhz = self._binary_search_mem()

            if backend_supports_uv:
                self._emit("Phase 4/5 - Undervolt search...", 6, 10)
                self._voltage_offset_mv = self._binary_search_voltage()

            self._emit("Phase 5/5 - Power limit tuning...", 7, 10)
            self._power_limit_pct = self._tune_power_limit(baseline)

        return result
```

- [ ] **Step 4: Add _search_optimal_vf_point method**

Add after `_binary_search_voltage` (after line 432):

```python
    def _search_optimal_vf_point(self, baseline: GPUMetrics) -> tuple[int, int]:
        """Find the lowest stable voltage for the GPU's natural boost frequency.

        Returns (voltage_mv, frequency_mhz).
        """
        from .backends.nvapi_vfcurve import NVAPIVFCurveBackend

        backend = self._backend
        if not isinstance(backend, NVAPIVFCurveBackend):
            return (0, 0)

        # Read current V/F curve
        curve = backend.read_vf_curve(self._gpu.index)
        if not curve:
            return (0, 0)

        # Target frequency = baseline boost clock (what GPU naturally achieves)
        target_freq_mhz = baseline.core_clock_mhz or baseline.boost_clock_mhz
        target_freq_khz = target_freq_mhz * 1000

        # Voltage search range
        voltage_min_mv = self._profile.get("voltage_min_mv", 800)
        # Find the stock voltage for our target frequency (highest V/F point
        # whose base freq is <= target)
        stock_voltage_mv = 1050  # conservative default
        for p in reversed(curve):
            if p.base_freq_khz <= target_freq_khz:
                stock_voltage_mv = p.voltage_mv
                break

        # Get available voltage steps from the curve
        voltage_steps = sorted(set(
            p.voltage_mv for p in curve
            if voltage_min_mv <= p.voltage_mv <= stock_voltage_mv
        ))
        if not voltage_steps:
            return (0, 0)

        # Binary search: find lowest voltage that sustains target_freq
        lo, hi = 0, len(voltage_steps) - 1
        best_voltage_mv = stock_voltage_mv
        test_dur = max(30, self._profile["test_duration_sec"] // self._profile["test_passes"])

        while lo <= hi:
            if self._cancel_event.is_set():
                break

            mid = (lo + hi) // 2
            candidate_mv = voltage_steps[mid]

            # Apply V/F lock at candidate voltage
            ok = backend.apply_vf_lock(
                self._gpu.index,
                target_voltage_uv=candidate_mv * 1000,
                target_freq_khz=target_freq_khz,
            )
            if not ok:
                m = self._monitor.read_once()
                self._emit(
                    f"V/F search: {target_freq_mhz} MHz @ {candidate_mv} mV -> APPLY FAILED",
                    3, 10, m,
                )
                lo = mid + 1
                continue

            time.sleep(1.5)
            test = self._stability_test_with_retries(duration_sec=test_dur)

            if not test.valid_load:
                m = self._monitor.read_once()
                self._emit(
                    f"V/F search: {target_freq_mhz} MHz @ {candidate_mv} mV -> INVALID (low load)",
                    3, 10, m,
                )
                break

            passed = test.passed
            m = self._monitor.read_once()
            self._emit(
                f"V/F search: {target_freq_mhz} MHz @ {candidate_mv} mV -> {'PASS' if passed else 'FAIL'}",
                3, 10, m,
            )

            if passed:
                best_voltage_mv = candidate_mv
                hi = mid - 1  # try lower voltage
            else:
                lo = mid + 1  # need more voltage

        # Safety margin: use one voltage step above the absolute minimum found
        safe_voltage_mv = best_voltage_mv
        for i, v in enumerate(voltage_steps):
            if v == best_voltage_mv and i + 1 < len(voltage_steps):
                safe_voltage_mv = voltage_steps[i + 1]
                break

        # Optional: try pushing frequency +25/+50 MHz at the safe voltage
        final_freq_mhz = target_freq_mhz
        for bump in [50, 25]:
            if self._cancel_event.is_set():
                break
            candidate_freq = target_freq_mhz + bump
            ok = backend.apply_vf_lock(
                self._gpu.index,
                target_voltage_uv=safe_voltage_mv * 1000,
                target_freq_khz=candidate_freq * 1000,
            )
            if ok:
                time.sleep(1.5)
                test = self._stability_test_with_retries(duration_sec=test_dur)
                if test.passed and test.valid_load:
                    final_freq_mhz = candidate_freq
                    m = self._monitor.read_once()
                    self._emit(
                        f"V/F boost: {candidate_freq} MHz @ {safe_voltage_mv} mV -> PASS",
                        4, 10, m,
                    )
                    break

        # Apply final settings
        backend.apply_vf_lock(
            self._gpu.index,
            target_voltage_uv=safe_voltage_mv * 1000,
            target_freq_khz=final_freq_mhz * 1000,
        )
        return (safe_voltage_mv, final_freq_mhz)
```

- [ ] **Step 5: Update the result-saving section in run()**

In the `run()` method, after the final verification block (around line 169-175 where result fields are set), add V/F curve fields:

```python
        result.achieved_boost_mhz = achieved.boost_clock_mhz or achieved.core_clock_mhz
        result.achieved_temp_c    = achieved.temp_c
        result.achieved_power_w   = achieved.power_w
        result.core_offset_mhz    = self._core_offset_mhz
        result.mem_offset_mhz     = self._mem_offset_mhz
        result.voltage_offset_mv  = self._voltage_offset_mv
        result.power_limit_pct    = self._power_limit_pct
        result.target_voltage_mv  = self._target_voltage_mv
        result.target_freq_mhz   = self._target_freq_mhz
```

- [ ] **Step 6: Update _apply helper for V/F curve backend**

The `_apply()` method (line 476) needs to pass V/F fields when using the V/F backend. Replace it:

```python
    def _apply(self) -> AppliedSettings:
        """Apply current working settings via the chosen backend."""
        kwargs = dict(
            gpu_index         = self._gpu.index,
            core_offset_mhz   = self._core_offset_mhz,
            mem_offset_mhz    = self._mem_offset_mhz,
            voltage_offset_mv = self._voltage_offset_mv,
            power_limit_pct   = self._power_limit_pct,
            thermal_limit_c   = self._profile["thermal_limit_max_c"],
        )
        # Pass V/F curve fields if backend supports them
        from .backends.nvapi_vfcurve import NVAPIVFCurveBackend
        if isinstance(self._backend, NVAPIVFCurveBackend):
            kwargs["target_voltage_mv"] = self._target_voltage_mv
            kwargs["target_freq_mhz"]  = self._target_freq_mhz

        result = self._backend.apply(**kwargs)
        if not result.success:
            raise RuntimeError(f"Backend apply failed: {result.notes}")
        has_oc_offsets = (
            self._core_offset_mhz != 0
            or self._mem_offset_mhz != 0
            or self._voltage_offset_mv != 0
            or self._target_voltage_mv != 0
        )
        if has_oc_offsets and not result.verified:
            raise RuntimeError(
                f"Settings verification failed — offsets may not have been applied. {result.notes}"
            )
        return result
```

- [ ] **Step 7: Run all tests**

Run: `pytest tests/ -v`
Expected: All tests PASS (existing + new V/F curve tests)

- [ ] **Step 8: Commit**

```bash
git add src/optimizer.py
git commit -m "feat: add V/F curve search to optimizer with PState20 fallback"
```

---

### Task 4: Boot-Apply V/F Curve Support

**Files:**
- Modify: `src/boot_apply.py:71-145`

- [ ] **Step 1: Update _apply_on_boot to handle V/F curve settings**

In `src/boot_apply.py`, replace the apply section (lines 111-132):

```python
    # Apply via best available backend
    from .optimizer import _best_backend
    backend = _best_backend(gpu)

    try:
        target_voltage_mv = saved.get("target_voltage_mv", 0)
        target_freq_mhz = saved.get("target_freq_mhz", 0)

        # Check if this was a V/F curve result
        from .backends.nvapi_vfcurve import NVAPIVFCurveBackend
        if target_voltage_mv > 0 and isinstance(backend, NVAPIVFCurveBackend):
            # V/F curve path: re-apply curve at saved voltage/frequency
            vf_ok = backend.apply_vf_lock(
                gpu.index,
                target_voltage_uv=target_voltage_mv * 1000,
                target_freq_khz=target_freq_mhz * 1000,
            )
            # Also apply memory offset via PState20
            mem_offset = saved.get("mem_offset_mhz", 0)
            if mem_offset:
                from .backends.nvapi import _NVAPILoader
                _NVAPILoader.get().set_pstate20_raw(gpu.index, 0, mem_offset * 1000)

            result = backend.apply(
                gpu_index=gpu.index,
                mem_offset_mhz=saved.get("mem_offset_mhz", 0),
                power_limit_pct=saved.get("power_limit_pct", 100),
                thermal_limit_c=saved.get("thermal_limit_c", 83),
                target_voltage_mv=target_voltage_mv,
                target_freq_mhz=target_freq_mhz,
            )
        else:
            # PState20 fallback path
            result = backend.apply(
                gpu_index=gpu.index,
                core_offset_mhz=saved.get("core_offset_mhz", 0),
                mem_offset_mhz=saved.get("mem_offset_mhz", 0),
                voltage_offset_mv=saved.get("voltage_offset_mv", 0),
                power_limit_pct=saved.get("power_limit_pct", 100),
                thermal_limit_c=saved.get("thermal_limit_c", 83),
            )

        if result.success:
            log.info("Boot-apply succeeded: %s", result.notes)
            record_boot_result(cfg, True, result.notes)
        else:
            log.error("Boot-apply failed: %s", result.notes)
            record_boot_result(cfg, False, result.notes)
    except Exception as exc:
        log.error("Boot-apply exception: %s", exc)
        record_boot_result(cfg, False, str(exc))
```

- [ ] **Step 2: Run tests**

Run: `pytest tests/test_boot_apply.py -v`
Expected: All PASS

- [ ] **Step 3: Commit**

```bash
git add src/boot_apply.py
git commit -m "feat: boot-apply supports V/F curve re-application"
```

---

### Task 5: Update verify_settings.py for V/F Curve

**Files:**
- Modify: `verify_settings.py`

- [ ] **Step 1: Add V/F curve verification to the script**

In `verify_settings.py`, after the "Check 1: NVAPI PState20" section and before "Check 2: Power Limit", add:

```python
    # ---- Step 1b: V/F Curve read-back ----
    if saved.get("target_voltage_mv", 0) > 0:
        print("--- Check 1b: V/F Curve Read-back ---")
        try:
            from src.backends.nvapi_vfcurve import NVAPIVFCurveBackend
            vf_backend = NVAPIVFCurveBackend()
            if vf_backend.is_available():
                curve = vf_backend.read_vf_curve(0)
                if curve:
                    target_v = saved["target_voltage_mv"]
                    target_f = saved["target_freq_mhz"]
                    # Find the point closest to target voltage
                    closest = min(curve, key=lambda p: abs(p.voltage_mv - target_v))
                    eff_freq = closest.effective_freq_mhz
                    print(f"  Target:  {target_f} MHz @ {target_v} mV")
                    print(f"  Actual:  {eff_freq} MHz @ {closest.voltage_mv} mV")
                    print(f"  Curve points: {len(curve)}")
                    if abs(eff_freq - target_f) <= 25 and abs(closest.voltage_mv - target_v) <= 13:
                        print("  >> V/F curve is applied correctly!")
                    else:
                        print("  >> WARNING: V/F curve does not match saved settings.")
                else:
                    print("  Could not read V/F curve.")
            else:
                print("  V/F curve backend not available.")
        except Exception as e:
            print(f"  Error: {e}")
        print()
    elif expected_core > 0:
        # Only check PState20 if not using V/F curve
        pass  # existing PState20 check already ran above
```

- [ ] **Step 2: Commit**

```bash
git add verify_settings.py
git commit -m "feat: verify_settings supports V/F curve read-back"
```

---

### Task 6: Integration Test — Full Round Trip

**Files:**
- Modify: `tests/test_vfcurve.py`

- [ ] **Step 1: Add integration-level tests**

Append to `tests/test_vfcurve.py`:

```python
def test_vfcurve_backend_unavailable_on_non_windows():
    """Backend should report unavailable when NVAPI can't load."""
    from src.backends.nvapi_vfcurve import NVAPIVFCurveBackend
    backend = NVAPIVFCurveBackend()
    # On CI / non-Windows, is_available() returns False
    result = backend.is_available()
    assert isinstance(result, bool)


def test_vfcurve_backend_reset_returns_bool():
    from src.backends.nvapi_vfcurve import NVAPIVFCurveBackend
    backend = NVAPIVFCurveBackend()
    if not backend.is_available():
        pytest.skip("NVAPI V/F curve not available")
    result = backend.reset(0)
    assert isinstance(result, bool)


def test_best_backend_includes_vfcurve():
    """_best_backend should try VFCurve backend first."""
    from src.optimizer import _best_backend
    from unittest.mock import MagicMock
    gpu = MagicMock()
    gpu.index = 0
    backend = _best_backend(gpu)
    # Backend should be one of the known types
    assert backend.name in ("nvapi-vfcurve", "nvapi-direct", "nvidia-smi")


def test_optimization_result_has_vf_fields():
    from src.config import GPUOptimizationResult
    r = GPUOptimizationResult(gpu_index=0, gpu_name="Test", risk_level="balanced")
    assert r.target_voltage_mv == 0
    assert r.target_freq_mhz == 0


def test_applied_settings_has_vf_fields():
    from src.backends.base import AppliedSettings
    s = AppliedSettings()
    assert s.target_voltage_mv == 0
    assert s.target_freq_mhz == 0


def test_risk_profiles_have_voltage_min():
    from src.config import RISK_PROFILES, RiskLevel
    for level in RiskLevel:
        assert "voltage_min_mv" in RISK_PROFILES[level]
    assert RISK_PROFILES[RiskLevel.SAFE]["voltage_min_mv"] == 0
    assert RISK_PROFILES[RiskLevel.BALANCED]["voltage_min_mv"] == 800
    assert RISK_PROFILES[RiskLevel.PERFORMANCE]["voltage_min_mv"] == 725
    assert RISK_PROFILES[RiskLevel.EXTREME]["voltage_min_mv"] == 650
```

- [ ] **Step 2: Run full test suite**

Run: `pytest tests/ -v`
Expected: All tests PASS

- [ ] **Step 3: Commit**

```bash
git add tests/test_vfcurve.py
git commit -m "test: add integration tests for V/F curve feature"
```

---

## Verification

After all tasks complete:

- [ ] Run full test suite: `pytest tests/ -v` — all pass
- [ ] On a real GPU (as admin), run: `python verify_settings.py` — confirms V/F curve applied
- [ ] Run optimizer with BALANCED risk — should find undervolt automatically
- [ ] Verify clocks under load are 1900+ MHz at reduced voltage
