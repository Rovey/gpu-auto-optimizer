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


def _compute_reshape_deltas(
    points: List[VFPoint],
    target_voltage_uv: int,
    target_freq_khz: int,
) -> List[int]:
    """Compute delta_khz per point for a curve-RESHAPE undervolt (no voltage lock).

    Unlike ``_compute_undervolt_deltas`` (which flattens the *entire* curve to a single
    frequency and is paired with a hard voltage lock — the combination that froze the
    RTX 4070), this leaves points below the target voltage untouched:

      - voltage < target  → delta 0 (stock; preserves dynamic boost / idle down-clock)
      - voltage >= target → delta = target_freq - base_freq (flat top at target_freq)

    The flat top caps frequency at/above the target voltage, which caps the voltage the
    GPU will request (no frequency gain → no reason to raise voltage), achieving the
    undervolt by curve *shape* rather than a hard lock. Returns one delta per point,
    in the same order as ``points``.
    """
    deltas: List[int] = []
    for p in points:
        if p.voltage_uv < target_voltage_uv:
            deltas.append(0)
        else:
            deltas.append(target_freq_khz - p.base_freq_khz)
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

    def apply_vf_reshape(
        self,
        gpu_index: int,
        target_voltage_uv: int,
        target_freq_khz: int,
    ) -> bool:
        """Apply a curve-RESHAPE undervolt: flat-top the V/F curve at target_freq for
        voltages >= target, keep below-cap points at stock, and set NO hard voltage lock
        (it clears any stale lock instead). Caps voltage via curve shape while preserving
        dynamic boost below the cap — the freeze-safe alternative to ``apply_vf_lock``.
        Returns True on success."""
        mask = self._read_raw_mask(gpu_index)
        vfp = self._read_raw_vfp(gpu_index, mask)
        ct_bytes = self._read_raw_clock_table(gpu_index, mask)
        if mask is None or vfp is None or ct_bytes is None:
            return False

        points = _parse_vf_points(mask, vfp, ct_bytes)
        if not points:
            return False

        deltas = _compute_reshape_deltas(points, target_voltage_uv, target_freq_khz)

        # Build modified clock table
        ct_buf = bytearray(ct_bytes)
        ct_buf[4:68] = mask[4:68]

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

        ok = self._write_raw_clock_table(gpu_index, bytes(ct_buf))
        if not ok:
            return False

        # Reshape caps voltage by curve shape — never set a hard lock (the freeze cause).
        # Clear any stale lock so none remains active.
        self._clear_voltage_lock(gpu_index)
        return True

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
        # Phase B: use the freeze-safe curve RESHAPE (flat-top, no hard voltage lock)
        # instead of the single-point lock that froze the RTX 4070.
        vf_ok = False
        if target_voltage_mv > 0 and target_freq_mhz > 0:
            vf_ok = self.apply_vf_reshape(
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

        self._write_raw_clock_table(gpu_index, bytes(ct_buf))

        # Unlock voltage
        self._clear_voltage_lock(gpu_index)

        # Reset memory via PState20
        from .nvapi import _NVAPILoader
        _NVAPILoader.get().set_pstate20_raw(gpu_index, 0, 0)

        # Verify by read-back, not the write's return code: some drivers report a
        # non-OK rc from SetClockBoostTable even when the write applied (observed live —
        # reset reported False while the curve was actually stock). Trust the curve.
        points = self.read_vf_curve(gpu_index)
        if not points:
            return False
        return all(p.delta_khz == 0 for p in points)

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
