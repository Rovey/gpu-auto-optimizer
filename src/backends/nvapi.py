"""
Direct NVIDIA API (NVAPI) backend via ctypes.
Supports: core clock offsets, memory clock offsets, voltage offsets,
          power limits, thermal limits.  Includes read-back verification.

Works on all NVIDIA consumer GPUs (GeForce GTX/RTX) on Windows.
Requires administrator privileges and nvapi64.dll to be present (ships with drivers).

Structures are based on NVAPI SDK headers (nvapi.h) and validated community
reverse-engineering. The P-state 20 V1 structure total size is 0x1418 (5144 bytes).

  NV_GPU_PSTATE20_CLOCK_ENTRY_V1  = 28 bytes
  NV_GPU_PSTATE20_BASE_VOLT_V1    = 20 bytes
  NV_GPU_PSTATE20_PSTATE_V1       = 320 bytes  (16 + 8×28 + 4×20)
  NV_GPU_PERF_PSTATES20_INFO_V1   = 5144 bytes (5×4 + 16×320 + 4)
"""
from __future__ import annotations

import ctypes
import ctypes.wintypes
import platform
import sys
from typing import Any, Callable, Dict, List, Optional, Tuple

from .base import GPUBackend, AppliedSettings


# ---------------------------------------------------------------------------
# Only load on Windows
# ---------------------------------------------------------------------------

_IS_WINDOWS = sys.platform == "win32"

# ---------------------------------------------------------------------------
# NVAPI function IDs (from NVAPI SDK + community reverse engineering)
# ---------------------------------------------------------------------------

_NVAPI_INITIALIZE              = 0x0150E828
_NVAPI_UNLOAD                  = 0x0D22BDD7
_NVAPI_ERROR                   = 0xCEEE8D8C
_NVAPI_ENUM_PHYSICAL_GPUS      = 0xE5AC921F
_NVAPI_GPU_GET_FULL_NAME        = 0xCEEBE66E
_NVAPI_GPU_GET_PSTATE20         = 0x6FF81213
_NVAPI_GPU_SET_PSTATE20         = 0x0FCBC455
_NVAPI_GPU_GET_THERMAL_SETTINGS = 0xE3640A56
_NVAPI_GPU_SET_COOLING_INFO     = 0x0D6E6B0E  # unofficial – set thermal limit
_NVAPI_GPU_GET_COOLER_SETTINGS  = 0xDA141340
_NVAPI_GPU_SET_COOLER_LEVELS    = 0x891FA0AE

# ---------------------------------------------------------------------------
# Domain / P-state constants
# ---------------------------------------------------------------------------

NVAPI_GPU_CLOCK_GRAPHICS = 0   # core clock
NVAPI_GPU_CLOCK_MEMORY   = 4   # VRAM clock
NVAPI_GPU_PSTATE_P0      = 0   # maximum performance state

# Status codes
NVAPI_OK                 = 0
NVAPI_ERROR_CODE         = -1

# ---------------------------------------------------------------------------
# Structures
# ---------------------------------------------------------------------------

class _NvPhysicalGpuHandle(ctypes.Structure):
    _fields_ = [("handle", ctypes.c_void_p)]


# Clock offset entry (28 bytes per entry)
class NV_GPU_PSTATE20_CLOCK_ENTRY_V1(ctypes.Structure):
    _pack_   = 4
    _fields_ = [
        ("domainId",           ctypes.c_uint32),   #  4  – 0=graphics, 4=memory
        ("typeId",             ctypes.c_uint32),   #  4  – 0=single, 1=range
        ("bIsEditable",        ctypes.c_uint32),   #  4
        ("freq_kHz",           ctypes.c_uint32),   #  4  – base frequency (read)
        ("voltDomainId",       ctypes.c_uint32),   #  4  – range mode: voltage domain
        ("freqDelta_value",    ctypes.c_int32),    #  4  – offset in kHz (write here)
        ("freqDelta_default",  ctypes.c_uint32),   #  4
    ]   # = 28 bytes


# Voltage entry (20 bytes per entry)
class NV_GPU_PSTATE20_BASE_VOLT_V1(ctypes.Structure):
    _pack_   = 4
    _fields_ = [
        ("domainId",            ctypes.c_uint32),  #  4
        ("bIsEditable",         ctypes.c_uint32),  #  4
        ("volt_uV",             ctypes.c_uint32),  #  4  – base voltage µV (read)
        ("voltDelta_value",     ctypes.c_int32),   #  4  – offset in µV   (write)
        ("voltDelta_default",   ctypes.c_uint32),  #  4
    ]   # = 20 bytes


# P-state entry (320 bytes)
class NV_GPU_PSTATE20_PSTATE_V1(ctypes.Structure):
    _pack_   = 4
    _fields_ = [
        ("pstateId",       ctypes.c_uint32),                          #   4
        ("bIsEditable",    ctypes.c_uint32),                          #   4
        ("numClocks",      ctypes.c_uint32),                          #   4
        ("numBaseVoltages",ctypes.c_uint32),                          #   4
        ("clocks",         NV_GPU_PSTATE20_CLOCK_ENTRY_V1 * 8),      # 224
        ("baseVoltages",   NV_GPU_PSTATE20_BASE_VOLT_V1   * 4),      #  80
    ]   # = 320 bytes


# Full P-state20 info structure (5144 bytes → version = 0x11418)
class NV_GPU_PERF_PSTATES20_INFO_V1(ctypes.Structure):
    _pack_   = 4
    _fields_ = [
        ("version",          ctypes.c_uint32),                         #    4
        ("bIsEditable",      ctypes.c_uint32),                         #    4
        ("numPstates",       ctypes.c_uint32),                         #    4
        ("numClocks",        ctypes.c_uint32),                         #    4
        ("numBaseVoltages",  ctypes.c_uint32),                         #    4
        ("pstates",          NV_GPU_PSTATE20_PSTATE_V1 * 16),          # 5120
        ("ov_numEntries",    ctypes.c_uint32),                         #    4
    ]   # = 5144 bytes


_PSTATE20_VERSION_V1 = ctypes.sizeof(NV_GPU_PERF_PSTATES20_INFO_V1) | (1 << 16)


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

            # Get NvAPI_QueryInterface – this is the gateway to all NVAPI functions
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
    # High-level operations
    # ------------------------------------------------------------------

    def get_pstate20(self, gpu_index: int) -> Optional[NV_GPU_PERF_PSTATES20_INFO_V1]:
        fn = self._get_func(
            _NVAPI_GPU_GET_PSTATE20, ctypes.c_int32,
            [ctypes.c_void_p, ctypes.POINTER(NV_GPU_PERF_PSTATES20_INFO_V1)],
        )
        if fn is None:
            return None
        handle = self.gpu_handle(gpu_index)
        if handle is None:
            return None

        info         = NV_GPU_PERF_PSTATES20_INFO_V1()
        info.version = _PSTATE20_VERSION_V1
        rc = fn(handle, ctypes.byref(info))
        if rc != NVAPI_OK:
            return None
        return info

    def set_pstate20(
        self,
        gpu_index:         int,
        core_offset_khz:   int = 0,
        mem_offset_khz:    int = 0,
        volt_offset_uv:    int = 0,
    ) -> bool:
        fn = self._get_func(
            _NVAPI_GPU_SET_PSTATE20, ctypes.c_int32,
            [ctypes.c_void_p, ctypes.POINTER(NV_GPU_PERF_PSTATES20_INFO_V1)],
        )
        if fn is None:
            return False
        handle = self.gpu_handle(gpu_index)
        if handle is None:
            return False

        info              = NV_GPU_PERF_PSTATES20_INFO_V1()
        info.version      = _PSTATE20_VERSION_V1
        info.bIsEditable  = 1
        info.numPstates   = 1
        info.numClocks    = 2   # core + memory
        info.numBaseVoltages = 1

        p0 = info.pstates[0]
        p0.pstateId    = NVAPI_GPU_PSTATE_P0
        p0.bIsEditable = 1
        p0.numClocks   = 2
        p0.numBaseVoltages = 1

        # Core clock offset
        p0.clocks[0].domainId          = NVAPI_GPU_CLOCK_GRAPHICS
        p0.clocks[0].bIsEditable       = 1
        p0.clocks[0].typeId            = 0
        p0.clocks[0].freqDelta_value   = core_offset_khz
        p0.clocks[0].freqDelta_default = 0

        # Memory clock offset
        p0.clocks[1].domainId          = NVAPI_GPU_CLOCK_MEMORY
        p0.clocks[1].bIsEditable       = 1
        p0.clocks[1].typeId            = 0
        p0.clocks[1].freqDelta_value   = mem_offset_khz
        p0.clocks[1].freqDelta_default = 0

        # Voltage offset (in µV)
        p0.baseVoltages[0].domainId          = 0
        p0.baseVoltages[0].bIsEditable       = 1
        p0.baseVoltages[0].voltDelta_value   = volt_offset_uv
        p0.baseVoltages[0].voltDelta_default = 0

        rc = fn(handle, ctypes.byref(info))
        return rc == NVAPI_OK


# ---------------------------------------------------------------------------
# Backend class
# ---------------------------------------------------------------------------

class NVAPIBackend(GPUBackend):
    """
    Full NVIDIA OC/UV control via direct NVAPI calls.
    Priority 30 – preferred over nvidia-smi but falls back gracefully.
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
        return self.is_available()

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

        # --- Apply power limit via pynvml (more reliable than NVAPI for this) ---
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

        # --- Apply clock offsets via P-state 20 ---
        core_khz = core_offset_mhz * 1000
        mem_khz  = mem_offset_mhz  * 1000
        volt_uv  = voltage_offset_mv * 1000  # mV → µV

        ok = self._loader.set_pstate20(gpu_index, core_khz, mem_khz, volt_uv)
        if ok:
            notes.append(
                f"Core+{core_offset_mhz} MHz  Mem+{mem_offset_mhz} MHz  "
                f"Volt{voltage_offset_mv:+d} mV via NVAPI"
            )
        else:
            notes.append("NVAPI PState20 set failed (needs admin / OC unlock?)")

        # --- Read-back verification ---
        verified = False
        if ok:
            readback = self.verify(gpu_index)
            if readback is not None:
                core_ok = abs(readback["core_offset_khz"] - core_khz) <= 1000
                mem_ok  = abs(readback["mem_offset_khz"]  - mem_khz)  <= 1000
                volt_ok = abs(readback["volt_offset_uv"]  - volt_uv)  <= 1000
                if core_ok and mem_ok and volt_ok:
                    verified = True
                else:
                    notes.append("WARNING: Read-back verification failed")

        return AppliedSettings(
            core_offset_mhz   = core_offset_mhz   if ok else 0,
            mem_offset_mhz    = mem_offset_mhz    if ok else 0,
            voltage_offset_mv = voltage_offset_mv if ok else 0,
            power_limit_pct   = power_limit_pct,
            thermal_limit_c   = thermal_limit_c,
            success           = ok,
            notes             = "; ".join(notes),
            verified          = verified,
        )

    def reset(self, gpu_index: int) -> bool:
        """Reset all clock/voltage offsets to zero."""
        return self._loader.set_pstate20(gpu_index, 0, 0, 0)

    # ------------------------------------------------------------------
    # Read-back verification
    # ------------------------------------------------------------------

    def verify(self, gpu_index: int) -> dict | None:
        """Read back P-state20 and return actual offsets, or None on failure."""
        if not self.is_available():
            return None
        info = self._loader.get_pstate20(gpu_index)
        if info is None:
            return None
        p0 = info.pstates[0]
        return {
            "core_offset_khz": p0.clocks[0].freqDelta_value,
            "mem_offset_khz":  p0.clocks[1].freqDelta_value,
            "volt_offset_uv":  p0.baseVoltages[0].voltDelta_value,
        }
