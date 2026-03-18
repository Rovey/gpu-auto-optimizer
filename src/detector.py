"""
GPU detection – enumerates all installed GPUs and returns structured info.
Supports NVIDIA via pynvml; AMD stub for future use.
"""
from __future__ import annotations

import subprocess
from dataclasses import dataclass, field
from typing import List, Optional

try:
    import pynvml
    _NVML_AVAILABLE = True
except ImportError:
    _NVML_AVAILABLE = False


# ---------------------------------------------------------------------------
# Architecture helpers
# ---------------------------------------------------------------------------

# Maps NVIDIA device ID prefix → architecture name + generation
_NVIDIA_ARCH_MAP: dict[tuple[int, int], str] = {
    # (pci_device_id_upper_nibble range check is impractical; use name matching)
}

def _infer_nvidia_arch(name: str) -> str:
    """Best-effort architecture detection from GPU name string."""
    name_up = name.upper()
    if any(x in name_up for x in ("40", "4090", "4080", "4070", "4060", "4050", "RTX 40")):
        return "Ada Lovelace"
    if any(x in name_up for x in ("3090", "3080", "3070", "3060", "3050", "RTX 30")):
        return "Ampere"
    if any(x in name_up for x in ("2080", "2070", "2060", "2050", "RTX 20")):
        return "Turing"
    if any(x in name_up for x in ("1080", "1070", "1060", "1050", "GTX 10")):
        return "Pascal"
    if any(x in name_up for x in ("980", "970", "960", "950", "GTX 9")):
        return "Maxwell"
    if "TITAN" in name_up:
        return "Ampere/Turing/Pascal"
    return "Unknown"


def _supports_voltage_curve(arch: str) -> bool:
    """Turing and newer support the VF (voltage/frequency) curve editor."""
    return arch in ("Turing", "Ampere", "Ada Lovelace")


# ---------------------------------------------------------------------------
# Data class
# ---------------------------------------------------------------------------

@dataclass
class GPUInfo:
    index:             int
    name:              str
    vendor:            str           # "NVIDIA" | "AMD" | "Intel"
    architecture:      str
    vram_mb:           int
    tdp_w:             int           # default TDP watts (0 if unknown)
    min_power_limit_w: int           # 0 if unknown
    max_power_limit_w: int           # 0 if unknown
    default_power_limit_w: int       # 0 if unknown
    driver_version:    str
    uuid:              str
    # Capability flags
    supports_oc:       bool = True
    supports_uv:       bool = False  # voltage curve support
    supports_mem_oc:   bool = True
    # Raw handle (pynvml) stored as any to avoid import issues at type-check time
    _handle:           object = field(default=None, repr=False, compare=False)


# ---------------------------------------------------------------------------
# Detection
# ---------------------------------------------------------------------------

def detect_gpus() -> List[GPUInfo]:
    """Return a list of GPUInfo for every detected GPU."""
    gpus: List[GPUInfo] = []

    if _NVML_AVAILABLE:
        gpus.extend(_detect_nvidia())

    # AMD – placeholder; extend in future
    # gpus.extend(_detect_amd())

    if not gpus:
        # Last-resort: parse nvidia-smi output
        gpus.extend(_detect_via_nvidia_smi())

    return gpus


def _detect_nvidia() -> List[GPUInfo]:
    gpus: List[GPUInfo] = []
    try:
        pynvml.nvmlInit()
        count = pynvml.nvmlDeviceGetCount()
        driver = pynvml.nvmlSystemGetDriverVersion()

        for i in range(count):
            handle = pynvml.nvmlDeviceGetHandleByIndex(i)
            name   = pynvml.nvmlDeviceGetName(handle)
            if isinstance(name, bytes):
                name = name.decode()

            uuid = pynvml.nvmlDeviceGetUUID(handle)
            if isinstance(uuid, bytes):
                uuid = uuid.decode()

            mem_info = pynvml.nvmlDeviceGetMemoryInfo(handle)
            vram_mb  = mem_info.total // (1024 * 1024)

            # Power limits (in milliwatts → watts)
            try:
                default_power_mw = pynvml.nvmlDeviceGetPowerManagementDefaultLimit(handle)
                min_power_mw     = pynvml.nvmlDeviceGetPowerManagementLimitConstraints(handle)[0]
                max_power_mw     = pynvml.nvmlDeviceGetPowerManagementLimitConstraints(handle)[1]
                cur_power_mw     = pynvml.nvmlDeviceGetPowerManagementLimit(handle)
                tdp_w            = default_power_mw  // 1000
                min_power_w      = min_power_mw      // 1000
                max_power_w      = max_power_mw      // 1000
                default_power_w  = cur_power_mw      // 1000
            except pynvml.NVMLError:
                tdp_w = default_power_w = min_power_w = max_power_w = 0

            arch = _infer_nvidia_arch(name)

            if isinstance(driver, bytes):
                driver = driver.decode()

            gpu = GPUInfo(
                index=i,
                name=name,
                vendor="NVIDIA",
                architecture=arch,
                vram_mb=vram_mb,
                tdp_w=tdp_w,
                min_power_limit_w=min_power_w,
                max_power_limit_w=max_power_w,
                default_power_limit_w=default_power_w,
                driver_version=driver,
                uuid=uuid,
                supports_oc=True,
                supports_uv=_supports_voltage_curve(arch),
                supports_mem_oc=True,
                _handle=handle,
            )
            gpus.append(gpu)

    except Exception as exc:
        print(f"[detector] pynvml error: {exc}")

    return gpus


def _detect_via_nvidia_smi() -> List[GPUInfo]:
    """Fallback: parse `nvidia-smi --query-gpu` CSV output."""
    gpus: List[GPUInfo] = []
    try:
        result = subprocess.run(
            [
                "nvidia-smi",
                "--query-gpu=index,name,driver_version,memory.total,power.default_limit,power.min_limit,power.max_limit,uuid",
                "--format=csv,noheader,nounits",
            ],
            capture_output=True,
            text=True,
            timeout=15,
        )
        if result.returncode != 0:
            return gpus

        for line in result.stdout.strip().splitlines():
            parts = [p.strip() for p in line.split(",")]
            if len(parts) < 8:
                continue
            idx, name, driver, vram_mib, tdp, min_pw, max_pw, uuid = parts[:8]

            def _safe_int(v: str, default: int = 0) -> int:
                try:
                    return int(float(v))
                except ValueError:
                    return default

            arch = _infer_nvidia_arch(name)
            gpu  = GPUInfo(
                index=_safe_int(idx),
                name=name,
                vendor="NVIDIA",
                architecture=arch,
                vram_mb=_safe_int(vram_mib),
                tdp_w=_safe_int(tdp),
                min_power_limit_w=_safe_int(min_pw),
                max_power_limit_w=_safe_int(max_pw),
                default_power_limit_w=_safe_int(tdp),
                driver_version=driver,
                uuid=uuid,
                supports_oc=True,
                supports_uv=_supports_voltage_curve(arch),
                supports_mem_oc=True,
            )
            gpus.append(gpu)

    except (FileNotFoundError, subprocess.TimeoutExpired):
        pass

    return gpus
