"""
Direct NVIDIA API (NVAPI) backend via ctypes — raw buffer approach.

Uses raw byte buffers instead of ctypes structures because the PState20
structure layout varies across driver versions.  The correct layout was
discovered empirically on driver 595.79 (2060 Super, Turing):

  Buffer size:  7416 bytes
  Version tag:  V2  (7416 | 2 << 16 = 0x00021CF8)
  SET func ID:  0x0F4DAE6B

  Header (20 bytes):
    +0   version          uint32
    +4   bIsEditable      uint32
    +8   numPstates       uint32
    +12  numClocks        uint32
    +16  numBaseVoltages  uint32

  P-state entry (448 bytes each, 16 slots):
    +0   pstateId         uint32
    +4   bIsEditable      uint32
    Clock entries (44 bytes each, starts at pstate+8):
      +0   domainId       uint32  (0=graphics, 4=memory)
      +4   field1         uint32
      +8   field2         uint32
      +12  freqDelta_kHz  int32   ** THE DELTA **
      +16  freqDelta_min  int32
      +20  freqDelta_max  int32
      +24  freq_base_kHz  uint32
      +28  freq_max_kHz   uint32
      +32  flags          uint32
      +36  reserved1      uint32
      +40  reserved2      uint32

  For SET: use numPstates=1 (minimal), copy first pstate from GET.

Requires: Windows + NVIDIA drivers (nvapi64.dll) + administrator rights.
"""
from __future__ import annotations

import ctypes
import platform
import struct
import sys
from typing import Any, Callable, Dict, List, Optional

from .base import GPUBackend, AppliedSettings

# ---------------------------------------------------------------------------
# Only load on Windows
# ---------------------------------------------------------------------------

_IS_WINDOWS = sys.platform == "win32"

# ---------------------------------------------------------------------------
# NVAPI function IDs
# ---------------------------------------------------------------------------

_NVAPI_INITIALIZE              = 0x0150E828
_NVAPI_UNLOAD                  = 0x0D22BDD7
_NVAPI_ENUM_PHYSICAL_GPUS      = 0xE5AC921F
_NVAPI_GPU_GET_PSTATE20         = 0x6FF81213
_NVAPI_GPU_SET_PSTATE20         = 0x0F4DAE6B  # corrected ID

# ---------------------------------------------------------------------------
# PState20 V2 buffer constants (discovered empirically)
# ---------------------------------------------------------------------------

_PSTATE20_BUF_SIZE    = 7416
_PSTATE20_VERSION_V2  = _PSTATE20_BUF_SIZE | (2 << 16)  # 0x00021CF8

# Header field offsets
_OFF_VERSION          = 0
_OFF_EDITABLE         = 4
_OFF_NUM_PSTATES      = 8
_OFF_NUM_CLOCKS       = 12
_OFF_NUM_BASE_VOLTS   = 16
_OFF_PSTATE0          = 20   # first pstate entry starts here

# Within a pstate entry
_PSTATE_HEADER_SIZE   = 8    # pstateId + bIsEditable
_CLOCK_ENTRY_SIZE     = 44   # 11 × uint32
_CLOCK_DELTA_OFFSET   = 12   # freqDelta_kHz within a clock entry

# Absolute offsets for P0 deltas (header=20, pstate_hdr=8, clock_entry=44)
_P0_CORE_DELTA = _OFF_PSTATE0 + _PSTATE_HEADER_SIZE + 0 * _CLOCK_ENTRY_SIZE + _CLOCK_DELTA_OFFSET  # 40
_P0_MEM_DELTA  = _OFF_PSTATE0 + _PSTATE_HEADER_SIZE + 1 * _CLOCK_ENTRY_SIZE + _CLOCK_DELTA_OFFSET  # 84

NVAPI_OK = 0


# ---------------------------------------------------------------------------
# NVAPI runtime loader
# ---------------------------------------------------------------------------

class _NVAPILoader:
    _instance: Optional["_NVAPILoader"] = None

    def __init__(self) -> None:
        self._dll:    Optional[ctypes.CDLL]   = None
        self._cache:  Dict[int, ctypes.c_void_p] = {}
        self._handles: List[ctypes.c_void_p]   = []
        self._loaded  = False
        self._last_error = 0

    @classmethod
    def get(cls) -> "_NVAPILoader":
        if cls._instance is None:
            cls._instance = _NVAPILoader()
        return cls._instance

    def load(self) -> bool:
        if self._loaded:
            return True
        if not _IS_WINDOWS:
            return False
        try:
            dll_name  = "nvapi64.dll" if platform.machine().endswith("64") else "nvapi.dll"
            self._dll = ctypes.WinDLL(dll_name)

            qi = getattr(self._dll, "nvapi_QueryInterface", None)
            if qi is None:
                return False
            qi.restype  = ctypes.c_void_p
            qi.argtypes = [ctypes.c_uint32]
            self._qi    = qi

            # Initialize NVAPI
            init_fn = self._get_func(_NVAPI_INITIALIZE, ctypes.c_int32, [])
            if init_fn is None:
                return False
            rc = init_fn()
            if rc != NVAPI_OK:
                return False

            # Enumerate GPUs
            GpuArray   = ctypes.c_void_p * 64
            gpu_arr    = GpuArray()
            gpu_count  = ctypes.c_uint32(0)
            enum_fn    = self._get_func(
                _NVAPI_ENUM_PHYSICAL_GPUS, ctypes.c_int32,
                [ctypes.POINTER(ctypes.c_void_p * 64), ctypes.POINTER(ctypes.c_uint32)],
            )
            if enum_fn is None:
                return False
            rc = enum_fn(ctypes.byref(gpu_arr), ctypes.byref(gpu_count))
            if rc != NVAPI_OK:
                return False

            self._handles = [gpu_arr[i] for i in range(gpu_count.value)]
            self._loaded  = True
            return True

        except (OSError, AttributeError):
            return False

    def _get_func(
        self,
        func_id:    int,
        restype:    Any,
        argtypes:   list,
    ) -> Optional[Callable]:
        if func_id in self._cache:
            ptr = self._cache[func_id]
        else:
            ptr = self._qi(func_id)
            self._cache[func_id] = ptr

        if not ptr:
            return None

        func_type = ctypes.WINFUNCTYPE(restype, *argtypes)
        return func_type(ptr)

    def gpu_handle(self, gpu_index: int) -> Optional[ctypes.c_void_p]:
        if gpu_index < len(self._handles):
            return self._handles[gpu_index]
        return None

    # ------------------------------------------------------------------
    # Raw-buffer PState20 operations
    # ------------------------------------------------------------------

    def get_pstate20_raw(self, gpu_index: int) -> Optional[bytes]:
        """Read PState20 as raw bytes. Returns None on failure."""
        fn = self._get_func(
            _NVAPI_GPU_GET_PSTATE20, ctypes.c_int32,
            [ctypes.c_void_p, ctypes.c_void_p],
        )
        if fn is None:
            return None
        handle = self.gpu_handle(gpu_index)
        if handle is None:
            return None

        buf = (ctypes.c_ubyte * _PSTATE20_BUF_SIZE)()
        struct.pack_into('<I', buf, 0, _PSTATE20_VERSION_V2)
        rc = fn(handle, ctypes.byref(buf))
        if rc != NVAPI_OK:
            self._last_error = rc
            return None
        return bytes(buf)

    def set_pstate20_raw(
        self,
        gpu_index:       int,
        core_offset_khz: int = 0,
        mem_offset_khz:  int = 0,
    ) -> bool:
        """Apply clock offsets via PState20 V2 raw buffer.

        Strategy: GET the full state, modify only the delta fields in P0,
        then SET back with numPstates=1 (minimal write).
        """
        fn = self._get_func(
            _NVAPI_GPU_SET_PSTATE20, ctypes.c_int32,
            [ctypes.c_void_p, ctypes.c_void_p],
        )
        if fn is None:
            self._last_error = -3  # NO_IMPLEMENTATION
            return False
        handle = self.gpu_handle(gpu_index)
        if handle is None:
            self._last_error = -104  # EXPECTED_PHYSICAL_GPU_HANDLE
            return False

        # Read current state to get a valid buffer
        current = self.get_pstate20_raw(gpu_index)
        if current is None:
            return False

        # Copy into mutable buffer
        buf = (ctypes.c_ubyte * _PSTATE20_BUF_SIZE)()
        for i, b in enumerate(current):
            buf[i] = b

        # Override header for minimal SET
        struct.pack_into('<I', buf, _OFF_VERSION, _PSTATE20_VERSION_V2)
        struct.pack_into('<I', buf, _OFF_EDITABLE, 1)
        struct.pack_into('<I', buf, _OFF_NUM_PSTATES, 1)  # only P0

        # Set the delta values
        struct.pack_into('<i', buf, _P0_CORE_DELTA, core_offset_khz)
        struct.pack_into('<i', buf, _P0_MEM_DELTA, mem_offset_khz)

        rc = fn(handle, ctypes.byref(buf))
        if rc != NVAPI_OK:
            self._last_error = rc
        return rc == NVAPI_OK


# ---------------------------------------------------------------------------
# Backend class
# ---------------------------------------------------------------------------

class NVAPIBackend(GPUBackend):
    """
    Full NVIDIA OC control via direct NVAPI calls (raw buffer approach).
    Priority 30 — preferred over nvidia-smi.
    Requires: Windows + NVIDIA drivers (nvapi64.dll) + administrator rights.
    """

    name     = "nvapi-direct"
    priority = 30

    def __init__(self) -> None:
        self._loader = _NVAPILoader.get()
        self._ready  = False

    def is_available(self) -> bool:
        if not _IS_WINDOWS:
            return False
        if not self._ready:
            self._ready = self._loader.load()
        return self._ready

    def supports_voltage(self, gpu_index: int) -> bool:
        # Voltage via PState20 is not supported on driver 595.79+
        # (numBaseVoltages=0 in GET response). Might revisit later.
        return False

    def supports_core_oc(self, gpu_index: int) -> bool:
        return self.is_available()

    def supports_mem_oc(self, gpu_index: int) -> bool:
        return self.is_available()

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
            return AppliedSettings(success=False, notes="NVAPI not available")

        notes: list[str] = []

        # --- Apply power limit via pynvml ---
        try:
            import pynvml
            pynvml.nvmlInit()
            h         = pynvml.nvmlDeviceGetHandleByIndex(gpu_index)
            default_mw = pynvml.nvmlDeviceGetPowerManagementDefaultLimit(h)
            min_mw, max_mw = pynvml.nvmlDeviceGetPowerManagementLimitConstraints(h)
            target_mw = int(default_mw * power_limit_pct / 100)
            target_mw = max(min_mw, min(max_mw, target_mw))
            pynvml.nvmlDeviceSetPowerManagementLimit(h, target_mw)
            notes.append(f"Power limit → {target_mw // 1000} W")
        except Exception as e:
            notes.append(f"Power limit skipped ({e})")

        # --- Apply clock offsets via PState20 V2 raw buffer ---
        core_khz = core_offset_mhz * 1000
        mem_khz  = mem_offset_mhz  * 1000

        ok = self._loader.set_pstate20_raw(gpu_index, core_khz, mem_khz)
        if ok:
            notes.append(
                f"Core+{core_offset_mhz} MHz  Mem+{mem_offset_mhz} MHz via NVAPI V2"
            )
        else:
            rc = self._loader._last_error
            notes.append(f"NVAPI PState20 set failed (rc={rc})")

        # --- Read-back verification ---
        verified = False
        if ok:
            readback = self.verify(gpu_index)
            if readback is not None:
                core_ok = abs(readback["core_offset_khz"] - core_khz) <= 1000
                mem_ok  = abs(readback["mem_offset_khz"]  - mem_khz)  <= 1000
                if core_ok and mem_ok:
                    verified = True
                else:
                    notes.append(
                        f"WARNING: Read-back mismatch "
                        f"(core: {readback['core_offset_khz']} vs {core_khz}, "
                        f"mem: {readback['mem_offset_khz']} vs {mem_khz})"
                    )

        return AppliedSettings(
            core_offset_mhz   = core_offset_mhz   if ok else 0,
            mem_offset_mhz    = mem_offset_mhz    if ok else 0,
            voltage_offset_mv = 0,  # voltage not supported via PState20 on modern drivers
            power_limit_pct   = power_limit_pct,
            thermal_limit_c   = thermal_limit_c,
            success           = ok,
            notes             = "; ".join(notes),
            verified          = verified,
        )

    def reset(self, gpu_index: int) -> bool:
        """Reset all clock offsets to zero."""
        return self._loader.set_pstate20_raw(gpu_index, 0, 0)

    def verify(self, gpu_index: int) -> dict | None:
        """Read back PState20 and return actual P0 clock offsets."""
        if not self.is_available():
            return None
        raw = self._loader.get_pstate20_raw(gpu_index)
        if raw is None:
            return None
        core_delta = struct.unpack_from('<i', raw, _P0_CORE_DELTA)[0]
        mem_delta  = struct.unpack_from('<i', raw, _P0_MEM_DELTA)[0]
        return {
            "core_offset_khz": core_delta,
            "mem_offset_khz":  mem_delta,
            "volt_offset_uv":  0,  # not available via PState20 on modern drivers
        }
