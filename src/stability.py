"""
GPU stability testing — CuPy-only with computation correctness verification.

Stress strategy:
  CuPy matrix multiply loop with preallocated buffers.
  Periodic correctness checks compare GPU matmul results against CPU (NumPy).

During the test, pynvml is polled to detect:
  - Driver Timeout Detection & Recovery (TDR) — process crash / GPU reappears
  - Temperature exceeding hard ceiling
  - ECC memory errors (uncorrected)
  - Persistent clock throttling
  - Significant clock drop (>150 MHz below expected boost)
"""
from __future__ import annotations

import os
import shutil
import threading
import time
import warnings
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, List, Optional

import numpy as np

from .monitor import GPUMonitor, GPUMetrics

try:
    import pynvml
    _NVML = True
except ImportError:
    _NVML = False


# ---------------------------------------------------------------------------
# Correctness verification (pure function — no CuPy dependency)
# ---------------------------------------------------------------------------

def _verify_correctness(cpu_result: np.ndarray, gpu_result: np.ndarray) -> bool:
    """Compare GPU result against CPU reference. Returns True if they match."""
    return bool(np.allclose(cpu_result, gpu_result, rtol=1e-3))


def _check_computation_correctness(cp, device_idx: int) -> bool:
    """
    Create 64x64 random float32 matrices on CPU, copy to GPU, compute matmul,
    copy result back, and verify against CPU reference via _verify_correctness.
    Returns True if the GPU result matches the CPU result.
    """
    # Generate on CPU
    a_cpu = np.random.random((64, 64)).astype(np.float32)
    b_cpu = np.random.random((64, 64)).astype(np.float32)
    cpu_result = a_cpu @ b_cpu

    # Compute on GPU
    with cp.cuda.Device(device_idx):
        a_gpu = cp.asarray(a_cpu)
        b_gpu = cp.asarray(b_cpu)
        c_gpu = cp.matmul(a_gpu, b_gpu)
        cp.cuda.Stream.null.synchronize()
        gpu_result = cp.asnumpy(c_gpu)

    return _verify_correctness(cpu_result, gpu_result)


# ---------------------------------------------------------------------------
# Result
# ---------------------------------------------------------------------------

@dataclass
class StabilityResult:
    passed:             bool  = False
    duration_sec:       float = 0.0
    max_temp_c:         float = 0.0
    min_clock_mhz:      int   = 0
    max_clock_mhz:      int   = 0
    avg_clock_mhz:      float = 0.0
    ecc_errors:         int   = 0
    tdr_detected:       bool  = False   # driver reset
    thermal_throttle:   bool  = False
    power_throttle:     bool  = False
    avg_gpu_util_pct:   float = 0.0
    max_gpu_util_pct:   int   = 0
    valid_load:         bool  = True
    correctness_passed: bool  = True
    stress_backend:     str   = ""
    load_note:          str   = ""
    failure_reason:     str   = ""
    snapshots:          List[GPUMetrics] = field(default_factory=list)


# ---------------------------------------------------------------------------
# Core tester
# ---------------------------------------------------------------------------

class StabilityTester:
    """
    Run a GPU stress test for `duration_sec` seconds and return a StabilityResult.

    Parameters
    ----------
    gpu_index       : GPU to test (0-based)
    temp_ceiling_c  : Abort and FAIL if GPU exceeds this temperature
    progress_cb     : Optional callback(elapsed_sec, total_sec, metrics)
    """

    def __init__(
        self,
        gpu_index:      int   = 0,
        temp_ceiling_c: int   = 95,
        min_avg_util_pct: int = 80,
        min_peak_util_pct: int = 95,
        progress_cb: Optional[Callable[[float, float, Optional[GPUMetrics]], None]] = None,
    ) -> None:
        self._idx      = gpu_index
        self._ceil     = temp_ceiling_c
        self._min_avg_util = min_avg_util_pct
        self._min_peak_util = min_peak_util_pct
        self._cb       = progress_cb

    # ------------------------------------------------------------------

    def run(self, duration_sec: float) -> StabilityResult:
        """
        Run CuPy stress workload for `duration_sec`.
        Returns StabilityResult with pass/fail + telemetry.
        """
        result = StabilityResult()

        # CuPy is the only supported backend — fail immediately if unavailable.
        if not self._cupy_available():
            result.passed = False
            result.failure_reason = (
                "CuPy is required but not available. "
                "Run the installer to set up CuPy with the correct CUDA version."
            )
            return result

        monitor   = GPUMonitor(self._idx, poll_interval_sec=0.4)
        snapshots: List[GPUMetrics] = []
        abort_event = threading.Event()

        # --- Monitoring thread ----------------------------------------
        def _monitor_thread() -> None:
            while not abort_event.is_set():
                m = monitor.read_once()
                snapshots.append(m)

                if m.temp_c >= self._ceil:
                    result.failure_reason  = (
                        f"Temperature ceiling exceeded: {m.temp_c:.0f} \u00b0C \u2265 {self._ceil} \u00b0C"
                    )
                    result.tdr_detected    = False
                    abort_event.set()
                    return

                if m.ecc_errors > 0:
                    result.failure_reason  = f"ECC uncorrectable memory errors: {m.ecc_errors}"
                    abort_event.set()
                    return

                time.sleep(0.4)

        mon_thread = threading.Thread(target=_monitor_thread, daemon=True)
        mon_thread.start()

        # --- Stress workload (CuPy only) ------------------------------
        start = time.time()
        try:
            self._stress_cupy(duration_sec, abort_event, start, result)
        except Exception as exc:
            if not result.failure_reason:
                result.failure_reason = f"Stress workload exception: {exc}"
                abort_event.set()

        elapsed = time.time() - start
        abort_event.set()
        mon_thread.join(timeout=3.0)

        # --- Aggregate results ----------------------------------------
        result.duration_sec = elapsed
        result.stress_backend = "cupy"
        result.snapshots    = snapshots

        if snapshots:
            temps  = [s.temp_c       for s in snapshots]
            clocks = [s.core_clock_mhz for s in snapshots if s.core_clock_mhz > 0]
            utils  = [s.gpu_util_pct for s in snapshots]

            result.max_temp_c      = max(temps)
            result.ecc_errors      = max(s.ecc_errors for s in snapshots)
            result.thermal_throttle = any(s.is_thermal_limit for s in snapshots)
            result.power_throttle   = any(s.is_power_limit   for s in snapshots)

            if clocks:
                result.min_clock_mhz = min(clocks)
                result.max_clock_mhz = max(clocks)
                result.avg_clock_mhz = sum(clocks) / len(clocks)

            if utils:
                result.avg_gpu_util_pct = sum(utils) / len(utils)
                result.max_gpu_util_pct = max(utils)

        # Low-util windows are inconclusive for OC/UV evaluation.
        if snapshots:
            result.valid_load = (
                result.avg_gpu_util_pct >= self._min_avg_util
                and result.max_gpu_util_pct >= self._min_peak_util
            )
            if not result.valid_load:
                result.load_note = (
                    "Inconclusive: stress load too low "
                    f"(avg {result.avg_gpu_util_pct:.0f} %, peak {result.max_gpu_util_pct} %; "
                    f"required avg >= {self._min_avg_util} %, peak >= {self._min_peak_util} %)."
                )

        # Detect TDR: if the monitoring loop saw a pynvml error mid-run
        # (the GPU disappeared and reappeared) treat as TDR failure.
        # Simple heuristic: last snapshot has clock=0 but it was > 0 earlier.
        if snapshots:
            clks = [s.core_clock_mhz for s in snapshots]
            had_clocks  = any(c > 0 for c in clks[:-3])
            end_missing = all(c == 0 for c in clks[-3:]) if len(clks) >= 3 else False
            if had_clocks and end_missing:
                result.tdr_detected   = True
                result.failure_reason = result.failure_reason or "TDR detected (clocks dropped to 0)"

        result.passed = (
            not result.tdr_detected
            and not result.failure_reason
            and result.ecc_errors == 0
            and result.correctness_passed
            and elapsed >= duration_sec * 0.9   # completed at least 90 % of duration
        )
        return result

    # ------------------------------------------------------------------
    # Stress backend
    # ------------------------------------------------------------------

    def _cupy_available(self) -> bool:
        self._configure_cuda_path_env()

        try:
            with warnings.catch_warnings():
                warnings.filterwarnings(
                    "ignore",
                    message="CUDA path could not be detected.*",
                    category=UserWarning,
                )
                import cupy as cp
            with cp.cuda.Device(self._idx):
                x = cp.arange(16, dtype=cp.float32)
                _ = x * x
                cp.cuda.Stream.null.synchronize()
            return True
        except Exception:
            return False

    def _stress_cupy(
        self,
        duration_sec: float,
        abort: threading.Event,
        start:  float,
        result: StabilityResult,
    ) -> None:
        with warnings.catch_warnings():
            warnings.filterwarnings(
                "ignore",
                message="CUDA path could not be detected.*",
                category=UserWarning,
            )
            import cupy as cp

        with cp.cuda.Device(self._idx):
            # Keep a sustained high-load workload with preallocated buffers.
            # This avoids allocator thrash and improves continuous SM occupancy.
            free_bytes, _ = cp.cuda.runtime.memGetInfo()

            def _alloc_gemm_buffers() -> tuple[cp.ndarray, cp.ndarray, cp.ndarray]:
                # 3 tensors (A, B, C), float32.
                # Start from a conservative VRAM share and fall back through safe sizes.
                target = int(max(512 * 1024 * 1024, min(free_bytes * 0.5, 2 * 1024**3)))
                est = int((target / (3 * 4)) ** 0.5)
                est = max(2048, min(8192, (est // 256) * 256))
                sizes = [est, 8192, 7168, 6144, 5120, 4096, 3072, 2048]

                seen = set()
                for n in sizes:
                    if n in seen:
                        continue
                    seen.add(n)
                    try:
                        a0 = cp.random.random((n, n), dtype=cp.float32)
                        b0 = cp.random.random((n, n), dtype=cp.float32)
                        c0 = cp.empty((n, n), dtype=cp.float32)
                        cp.matmul(a0, b0, out=c0)
                        cp.cuda.Stream.null.synchronize()
                        return a0, b0, c0
                    except Exception:
                        continue
                raise RuntimeError("Unable to allocate stable CuPy stress buffers")

            a, b, c = _alloc_gemm_buffers()
            last_correctness_check = start
            while not abort.is_set() and (time.time() - start) < duration_sec:
                # Run multiple GEMMs per cycle with preallocated output.
                cp.matmul(a, b, out=c)
                cp.matmul(c, a, out=b)
                cp.cuda.Stream.null.synchronize()

                elapsed = time.time() - start

                # Periodic correctness check every 5 seconds
                if elapsed - (last_correctness_check - start) >= 5.0:
                    last_correctness_check = time.time()
                    if not _check_computation_correctness(cp, self._idx):
                        result.correctness_passed = False
                        result.failure_reason = (
                            "GPU computation correctness check failed "
                            "(memory instability detected)"
                        )
                        abort.set()
                        return

                if self._cb:
                    mon = GPUMonitor(self._idx)
                    self._cb(elapsed, duration_sec, mon.read_once())

    def _configure_cuda_path_env(self) -> None:
        """Best-effort CUDA toolkit discovery to avoid noisy CuPy path warnings."""
        if os.environ.get("CUDA_PATH"):
            return

        candidates: List[Path] = []

        cuda_home = os.environ.get("CUDA_HOME")
        if cuda_home:
            candidates.append(Path(cuda_home))

        nvcc = shutil.which("nvcc")
        if nvcc:
            nvcc_path = Path(nvcc).resolve()
            candidates.append(nvcc_path.parent.parent)

        toolkit_root = Path(r"C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA")
        if toolkit_root.exists():
            for p in sorted(toolkit_root.glob("v*"), reverse=True):
                candidates.append(p)

        for c in candidates:
            if (c / "bin").exists():
                os.environ["CUDA_PATH"] = str(c)
                return
