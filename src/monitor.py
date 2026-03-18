"""
Real-time GPU monitoring via pynvml (+ nvidia-smi fallback).
"""
from __future__ import annotations

import subprocess
import threading
import time
from dataclasses import dataclass, field
from typing import Callable, List, Optional

try:
    import pynvml
    _NVML_AVAILABLE = True
except ImportError:
    _NVML_AVAILABLE = False


# ---------------------------------------------------------------------------
# Snapshot dataclass
# ---------------------------------------------------------------------------

@dataclass
class GPUMetrics:
    gpu_index:         int
    timestamp:         float = field(default_factory=time.time)

    # Clocks
    core_clock_mhz:    int   = 0
    mem_clock_mhz:     int   = 0
    boost_clock_mhz:   int   = 0   # max possible at current P-state

    # Thermals
    temp_c:            float = 0.0
    hotspot_c:         float = 0.0  # available on Turing+

    # Power
    power_w:           float = 0.0
    power_limit_w:     float = 0.0

    # Fan
    fan_speed_pct:     int   = 0

    # Utilisation
    gpu_util_pct:      int   = 0
    mem_util_pct:      int   = 0
    mem_used_mb:       int   = 0
    mem_total_mb:      int   = 0

    # Reliability
    ecc_errors:        int   = 0    # volatile uncorrected errors
    throttle_reasons:  int   = 0    # bitmask

    # Derived
    is_throttling:     bool  = False
    is_thermal_limit:  bool  = False
    is_power_limit:    bool  = False


# Throttle-reason bit flags (pynvml constants)
_THROTTLE_THERMAL = 0x0000000000000008
_THROTTLE_POWER   = 0x0000000000000004


# ---------------------------------------------------------------------------
# Monitor class
# ---------------------------------------------------------------------------

class GPUMonitor:
    """Thread-safe, polling-based monitor for a single GPU."""

    def __init__(self, gpu_index: int, poll_interval_sec: float = 0.5):
        self._index          = gpu_index
        self._interval       = poll_interval_sec
        self._handle         = None
        self._latest: Optional[GPUMetrics] = None
        self._lock           = threading.Lock()
        self._running        = False
        self._thread: Optional[threading.Thread] = None
        self._callbacks: List[Callable[[GPUMetrics], None]] = []

        if _NVML_AVAILABLE:
            try:
                pynvml.nvmlInit()
                self._handle = pynvml.nvmlDeviceGetHandleByIndex(gpu_index)
            except Exception:
                self._handle = None

    # ------------------------------------------------------------------
    # Public API
    # ------------------------------------------------------------------

    def start(self) -> None:
        """Start background polling thread."""
        if self._running:
            return
        self._running = True
        self._thread  = threading.Thread(target=self._poll_loop, daemon=True)
        self._thread.start()

    def stop(self) -> None:
        self._running = False
        if self._thread:
            self._thread.join(timeout=3.0)

    def snapshot(self) -> Optional[GPUMetrics]:
        """Return the most recent reading (thread-safe)."""
        with self._lock:
            return self._latest

    def read_once(self) -> GPUMetrics:
        """Blocking single read."""
        return self._read()

    def on_update(self, callback: Callable[[GPUMetrics], None]) -> None:
        self._callbacks.append(callback)

    # ------------------------------------------------------------------
    # Internal
    # ------------------------------------------------------------------

    def _poll_loop(self) -> None:
        while self._running:
            try:
                metrics = self._read()
                with self._lock:
                    self._latest = metrics
                for cb in self._callbacks:
                    try:
                        cb(metrics)
                    except Exception:
                        pass
            except Exception:
                pass
            time.sleep(self._interval)

    def _read(self) -> GPUMetrics:
        if self._handle is not None and _NVML_AVAILABLE:
            return self._read_nvml()
        return self._read_smi()

    def _read_nvml(self) -> GPUMetrics:
        h = self._handle
        m = GPUMetrics(gpu_index=self._index)

        try:
            m.core_clock_mhz = pynvml.nvmlDeviceGetClockInfo(h, pynvml.NVML_CLOCK_GRAPHICS)
            m.mem_clock_mhz  = pynvml.nvmlDeviceGetClockInfo(h, pynvml.NVML_CLOCK_MEM)
            m.boost_clock_mhz = pynvml.nvmlDeviceGetMaxClockInfo(h, pynvml.NVML_CLOCK_GRAPHICS)
        except pynvml.NVMLError:
            pass

        try:
            m.temp_c = float(pynvml.nvmlDeviceGetTemperature(h, pynvml.NVML_TEMPERATURE_GPU))
        except pynvml.NVMLError:
            pass

        try:
            m.hotspot_c = float(
                pynvml.nvmlDeviceGetTemperature(h, 1)  # NVML_TEMPERATURE_COUNT = hotspot on newer cards
            )
        except (pynvml.NVMLError, Exception):
            m.hotspot_c = m.temp_c

        try:
            m.power_w       = pynvml.nvmlDeviceGetPowerUsage(h) / 1000.0
            m.power_limit_w = pynvml.nvmlDeviceGetPowerManagementLimit(h) / 1000.0
        except pynvml.NVMLError:
            pass

        try:
            m.fan_speed_pct = pynvml.nvmlDeviceGetFanSpeed(h)
        except pynvml.NVMLError:
            pass

        try:
            util = pynvml.nvmlDeviceGetUtilizationRates(h)
            m.gpu_util_pct = util.gpu
            m.mem_util_pct = util.memory
        except pynvml.NVMLError:
            pass

        try:
            mem_info      = pynvml.nvmlDeviceGetMemoryInfo(h)
            m.mem_used_mb  = mem_info.used  // (1024 * 1024)
            m.mem_total_mb = mem_info.total // (1024 * 1024)
        except pynvml.NVMLError:
            pass

        try:
            m.ecc_errors = pynvml.nvmlDeviceGetTotalEccErrors(
                h,
                pynvml.NVML_MEMORY_ERROR_TYPE_UNCORRECTED,
                pynvml.NVML_VOLATILE_ECC,
            )
        except (pynvml.NVMLError, Exception):
            m.ecc_errors = 0

        try:
            m.throttle_reasons  = pynvml.nvmlDeviceGetCurrentClocksThrottleReasons(h)
            m.is_throttling     = m.throttle_reasons != 0
            m.is_thermal_limit  = bool(m.throttle_reasons & _THROTTLE_THERMAL)
            m.is_power_limit    = bool(m.throttle_reasons & _THROTTLE_POWER)
        except (pynvml.NVMLError, Exception):
            pass

        m.timestamp = time.time()
        return m

    def _read_smi(self) -> GPUMetrics:
        """Fallback: read from nvidia-smi (slower but always available)."""
        m = GPUMetrics(gpu_index=self._index)
        try:
            result = subprocess.run(
                [
                    "nvidia-smi",
                    f"--id={self._index}",
                    "--query-gpu=clocks.current.graphics,clocks.current.memory,"
                    "temperature.gpu,power.draw,power.limit,fan.speed,"
                    "utilization.gpu,utilization.memory,memory.used,memory.total",
                    "--format=csv,noheader,nounits",
                ],
                capture_output=True, text=True, timeout=5,
            )
            parts = [p.strip() for p in result.stdout.strip().split(",")]
            if len(parts) >= 10:
                def _f(v: str) -> float:
                    try: return float(v)
                    except ValueError: return 0.0

                m.core_clock_mhz = int(_f(parts[0]))
                m.mem_clock_mhz  = int(_f(parts[1]))
                m.temp_c         = _f(parts[2])
                m.power_w        = _f(parts[3])
                m.power_limit_w  = _f(parts[4])
                fan_raw          = parts[5]
                m.fan_speed_pct  = int(_f(fan_raw)) if fan_raw not in ("N/A", "[N/A]") else 0
                m.gpu_util_pct   = int(_f(parts[6]))
                m.mem_util_pct   = int(_f(parts[7]))
                m.mem_used_mb    = int(_f(parts[8]))
                m.mem_total_mb   = int(_f(parts[9]))
        except Exception:
            pass

        m.timestamp = time.time()
        return m


# ---------------------------------------------------------------------------
# Convenience: sample metrics over N seconds, return average
# ---------------------------------------------------------------------------

def sample_average(monitor: GPUMonitor, duration_sec: float) -> GPUMetrics:
    """Poll monitor for `duration_sec`, return averaged metrics."""
    samples: List[GPUMetrics] = []
    deadline = time.time() + duration_sec
    while time.time() < deadline:
        m = monitor.read_once()
        samples.append(m)
        time.sleep(0.5)

    if not samples:
        return GPUMetrics(gpu_index=monitor._index)

    def _avg(attr: str) -> float:
        vals = [getattr(s, attr) for s in samples]
        return sum(vals) / len(vals)

    ref = samples[-1]
    ref.core_clock_mhz  = int(_avg("core_clock_mhz"))
    ref.mem_clock_mhz   = int(_avg("mem_clock_mhz"))
    ref.temp_c          = _avg("temp_c")
    ref.power_w         = _avg("power_w")
    ref.fan_speed_pct   = int(_avg("fan_speed_pct"))
    ref.gpu_util_pct    = int(_avg("gpu_util_pct"))
    ref.ecc_errors      = max(s.ecc_errors for s in samples)
    ref.is_throttling   = any(s.is_throttling for s in samples)
    ref.is_thermal_limit = any(s.is_thermal_limit for s in samples)
    ref.is_power_limit   = any(s.is_power_limit  for s in samples)
    return ref
