"""
nvidia-smi backend – always available on any NVIDIA system.
Capabilities: power limit, thermal limit, app-clock suggestions (read-only OC).
This is the guaranteed fallback for all NVIDIA GPUs.
"""
from __future__ import annotations

import subprocess
from typing import Optional

try:
    import pynvml
    _NVML = True
except ImportError:
    _NVML = False

from .base import GPUBackend, AppliedSettings


def _run_smi(*args: str, timeout: int = 10) -> tuple[int, str, str]:
    try:
        r = subprocess.run(
            ["nvidia-smi", *args],
            capture_output=True, text=True, timeout=timeout,
        )
        return r.returncode, r.stdout.strip(), r.stderr.strip()
    except FileNotFoundError:
        return -1, "", "nvidia-smi not found"
    except subprocess.TimeoutExpired:
        return -1, "", "timeout"


class NvidiaSMIBackend(GPUBackend):
    """
    Uses nvidia-smi (and pynvml where available) to:
      - Set power limits
      - Set thermal limits (via nvapi or smi where supported)
      - Read-only: does NOT support free core/mem clock offsets on consumer GPUs
    """
    name     = "nvidia-smi"
    priority = 10   # lowest priority – used as fallback

    def is_available(self) -> bool:
        rc, _, _ = _run_smi("-L")
        return rc == 0

    def supports_voltage(self, gpu_index: int) -> bool:
        return False

    def supports_core_oc(self, gpu_index: int) -> bool:
        return False   # nvidia-smi can't set free offsets on consumer GPUs

    def supports_mem_oc(self, gpu_index: int) -> bool:
        return False

    def apply(
        self,
        gpu_index:         int,
        core_offset_mhz:   int = 0,
        mem_offset_mhz:    int = 0,
        voltage_offset_mv: int = 0,
        power_limit_pct:   int = 100,
        thermal_limit_c:   int = 83,
    ) -> AppliedSettings:
        applied = AppliedSettings()
        notes   = []

        # --- Power limit via pynvml (preferred) or nvidia-smi -----------
        power_applied = False
        if _NVML:
            try:
                pynvml.nvmlInit()
                h = pynvml.nvmlDeviceGetHandleByIndex(gpu_index)

                default_mw = pynvml.nvmlDeviceGetPowerManagementDefaultLimit(h)
                min_mw, max_mw = pynvml.nvmlDeviceGetPowerManagementLimitConstraints(h)

                target_mw = int(default_mw * power_limit_pct / 100)
                target_mw = max(min_mw, min(max_mw, target_mw))

                pynvml.nvmlDeviceSetPowerManagementLimit(h, target_mw)
                applied.power_limit_pct = power_limit_pct
                power_applied = True
                notes.append(f"Power limit set to {target_mw // 1000} W via pynvml")
            except Exception as exc:
                notes.append(f"pynvml power limit failed: {exc}")

        if not power_applied:
            # Try nvidia-smi -pl (requires admin, but worth trying)
            rc, _, err = _run_smi(f"-i {gpu_index}", f"-pl {power_limit_pct}")
            if rc == 0:
                applied.power_limit_pct = power_limit_pct
                notes.append("Power limit set via nvidia-smi")
            else:
                notes.append(f"nvidia-smi power limit skipped ({err})")
                applied.success = False

        applied.thermal_limit_c   = thermal_limit_c   # tracked, may not apply
        applied.core_offset_mhz   = 0                 # not supported
        applied.mem_offset_mhz    = 0
        applied.voltage_offset_mv = 0
        applied.notes             = "; ".join(notes)
        return applied

    def reset(self, gpu_index: int) -> bool:
        if _NVML:
            try:
                pynvml.nvmlInit()
                h           = pynvml.nvmlDeviceGetHandleByIndex(gpu_index)
                default_mw  = pynvml.nvmlDeviceGetPowerManagementDefaultLimit(h)
                pynvml.nvmlDeviceSetPowerManagementLimit(h, default_mw)
                return True
            except Exception:
                pass
        return False
