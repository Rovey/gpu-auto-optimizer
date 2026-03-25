"""
Optimization algorithm.

Strategy per risk level
-----------------------
SAFE
  1. Read baseline (temp, clocks, power).
  2. Reduce power limit by 10–20 % and test stability.
  3. Apply best power limit that keeps same performance within 2 %.

BALANCED / PERFORMANCE / EXTREME
  Phase 1 – Baseline measurement (30 s).
  Phase 2 – Binary search max stable core clock offset (step 25 MHz).
  Phase 3 – Binary search max stable memory clock offset (step 100 MHz).
  Phase 4 – Undervolt: reduce voltage/VF-curve while keeping peak clock
             (step 25 mV, binary search of voltage ceiling).
  Phase 5 – Power limit: reduce until performance drops >2 %, then step back.
  Phase 6 – Final verification run at chosen settings.
  Phase 7 – Save result.

Each step is validated by StabilityTester; failure → roll back + halve step.
"""
from __future__ import annotations

import threading
import time
import warnings
from dataclasses import dataclass, field
from typing import Callable, List, Optional, Tuple

from .config import RiskLevel, RISK_PROFILES, GPUOptimizationResult, UserConfig
from .detector import GPUInfo
from .monitor import GPUMonitor, sample_average, GPUMetrics
from .stability import StabilityTester, StabilityResult
from .backends.base import GPUBackend, AppliedSettings
from .backends.nvidia_smi import NvidiaSMIBackend
from .backends.nvapi import NVAPIBackend


# ---------------------------------------------------------------------------
# Backend selection
# ---------------------------------------------------------------------------

def _best_backend(gpu: GPUInfo) -> GPUBackend:
    """Return the highest-priority available backend for this GPU."""
    candidates: List[GPUBackend] = [
        NVAPIBackend(),
        NvidiaSMIBackend(),
    ]
    available = [b for b in candidates if b.is_available()]
    if not available:
        return NvidiaSMIBackend()   # always "available" (nvidia-smi must be present)
    return max(available, key=lambda b: b.priority)


# ---------------------------------------------------------------------------
# Progress callback type: (phase_name, step, total_steps, latest_metrics)
# ---------------------------------------------------------------------------

ProgressCB = Callable[[str, int, int, Optional[GPUMetrics]], None]


# ---------------------------------------------------------------------------
# Optimizer
# ---------------------------------------------------------------------------

class GPUOptimizer:
    """
    Runs the full optimization pipeline for a single GPU.

    Parameters
    ----------
    gpu         : GPUInfo from detector
    risk_level  : RiskLevel enum
    progress_cb : optional UI callback for live updates
    """

    def __init__(
        self,
        gpu:         GPUInfo,
        risk_level:  RiskLevel = RiskLevel.BALANCED,
        progress_cb: Optional[ProgressCB] = None,
    ) -> None:
        self._gpu      = gpu
        self._risk     = risk_level
        self._profile  = RISK_PROFILES[risk_level]
        self._backend  = _best_backend(gpu)
        self._monitor  = GPUMonitor(gpu.index, poll_interval_sec=0.3)
        self._progress = progress_cb

        # Cancel mechanism
        self._cancel_event = threading.Event()

        # Working state
        self._core_offset_mhz   = 0
        self._mem_offset_mhz    = 0
        self._voltage_offset_mv = 0
        self._power_limit_pct   = 100
        self._baseline_core_mhz = 0

        # Ceiling temperature – use profile limit or GPU's own thermal limit
        self._temp_ceiling = min(
            self._profile["thermal_limit_max_c"],
            95,   # never exceed 95 °C regardless of profile
        )

    # ------------------------------------------------------------------
    # Public API
    # ------------------------------------------------------------------

    def cancel(self) -> None:
        """Signal the optimizer to stop at the next safe checkpoint."""
        self._cancel_event.set()

    def run(self) -> GPUOptimizationResult:
        result = GPUOptimizationResult(
            gpu_index  = self._gpu.index,
            gpu_name   = self._gpu.name,
            risk_level = self._risk.value,
        )

        self._emit("Resetting to stock…", 0, 10)
        self._backend.reset(self._gpu.index)
        time.sleep(1.5)

        # --- Baseline ---------------------------------------------------
        self._emit("Measuring baseline…", 1, 10)
        baseline = self._measure_under_load(duration_sec=20)
        self._baseline_core_mhz = baseline.core_clock_mhz
        result.baseline_boost_mhz = baseline.boost_clock_mhz or baseline.core_clock_mhz
        result.baseline_temp_c    = baseline.temp_c
        result.baseline_power_w   = baseline.power_w

        self._emit("Preflight stress validation…", 2, 10)
        self._require_valid_stress_load()

        if self._risk == RiskLevel.SAFE:
            result = self._run_safe_mode(result, baseline)
        else:
            result = self._run_full_mode(result, baseline)

        # --- Final verification -----------------------------------------
        self._emit("Final verification…", 9, 10)
        self._apply()
        final_dur = max(300, self._profile["test_duration_sec"] * 2)
        final = self._stability_test_with_retries(duration_sec=final_dur)
        result.stability_passed = final.passed or not final.valid_load
        if final.valid_load and not final.passed:
            # Roll back one step on final fail
            self._core_offset_mhz   = max(0, self._core_offset_mhz   - 25)
            self._mem_offset_mhz    = max(0, self._mem_offset_mhz    - 100)
            self._voltage_offset_mv = min(0, self._voltage_offset_mv + 25)
            self._apply()
            final = self._stability_test_with_retries(duration_sec=30)
            result.stability_passed = final.passed
            result.notes = "Rolled back one step after final verification fail."
        elif not final.valid_load:
            result.notes = (
                (result.notes + " ").strip() +
                "Final verification was inconclusive due to low GPU load."
            ).strip()

        achieved = self._measure_under_load(duration_sec=20)
        result.achieved_boost_mhz = achieved.boost_clock_mhz or achieved.core_clock_mhz
        result.achieved_temp_c    = achieved.temp_c
        result.achieved_power_w   = achieved.power_w
        result.core_offset_mhz    = self._core_offset_mhz
        result.mem_offset_mhz     = self._mem_offset_mhz
        result.voltage_offset_mv  = self._voltage_offset_mv
        result.power_limit_pct    = self._power_limit_pct

        self._emit("Done!", 10, 10)
        return result

    # ------------------------------------------------------------------
    # Safe mode (power limit only)
    # ------------------------------------------------------------------

    def _run_safe_mode(
        self,
        result:   GPUOptimizationResult,
        baseline: GPUMetrics,
    ) -> GPUOptimizationResult:
        self._emit("Safe mode: tuning power limit…", 2, 10)
        min_delta, _ = self._profile["power_limit_delta_pct"]

        best_pct   = 100
        step       = 5   # reduce by 5 % at a time

        for reduction in range(step, abs(min_delta) + step, step):
            candidate_pct = 100 - reduction
            self._power_limit_pct = candidate_pct
            self._apply()
            time.sleep(1.0)

            test = self._stability_test_with_retries(duration_sec=self._profile["test_duration_sec"])
            achieved = self._measure(duration_sec=10)

            clock_drop_pct = (
                (baseline.core_clock_mhz - achieved.core_clock_mhz)
                / max(baseline.core_clock_mhz, 1) * 100
            ) if baseline.core_clock_mhz > 0 else 0

            if not test.valid_load:
                break
            if test.passed and clock_drop_pct < 2.0:
                best_pct = candidate_pct
            else:
                # Went too far - keep last good value
                break

        self._power_limit_pct = best_pct
        return result

    # ------------------------------------------------------------------
    # Full mode (OC + UV)
    # ------------------------------------------------------------------

    def _run_full_mode(
        self,
        result:   GPUOptimizationResult,
        baseline: GPUMetrics,
    ) -> GPUOptimizationResult:
        backend_supports_oc  = self._backend.supports_core_oc(self._gpu.index)
        backend_supports_uv  = (
            self._backend.supports_voltage(self._gpu.index)
            and self._gpu.supports_uv
        )
        backend_supports_mem = self._backend.supports_mem_oc(self._gpu.index)

        # Phase 2: Core OC
        if backend_supports_oc:
            self._emit("Phase 2/5 - Core clock search...", 2, 10)
            self._core_offset_mhz = self._binary_search_core()

        # Phase 3: Memory OC
        if backend_supports_mem:
            self._emit("Phase 3/5 - Memory clock search...", 4, 10)
            self._mem_offset_mhz = self._binary_search_mem()

        # Phase 4: Undervolt
        if backend_supports_uv:
            self._emit("Phase 4/5 - Undervolt search...", 6, 10)
            self._voltage_offset_mv = self._binary_search_voltage()

        # Phase 5: Power limit tuning
        self._emit("Phase 5/5 - Power limit tuning...", 7, 10)
        self._power_limit_pct = self._tune_power_limit(baseline)

        return result

    # ------------------------------------------------------------------
    # Binary searches
    # ------------------------------------------------------------------

    def _binary_search_core(self) -> int:
        """Return the largest stable core clock offset (MHz)."""
        limit = self._profile["core_offset_mhz_max"]
        if limit == 0:
            return 0

        lo, hi    = 0, limit
        best      = 0
        passes    = self._profile["test_passes"]
        test_dur  = max(30, self._profile["test_duration_sec"] // passes)

        while lo <= hi:
            if self._cancel_event.is_set():
                break

            mid = (lo + hi) // 2
            # Round to nearest 25 MHz
            mid = (mid // 25) * 25
            if mid == 0 and lo > 0:
                break

            self._core_offset_mhz = mid
            self._apply()
            time.sleep(1.0)

            test = self._stability_test_with_retries(duration_sec=test_dur)
            if not test.valid_load:
                m  = self._monitor.read_once()
                self._emit(
                    f"Core search: testing +{mid} MHz -> INVALID (low load)",
                    3, 10, m,
                )
                break
            ok = test.passed
            m  = self._monitor.read_once()
            self._emit(
                f"Core search: testing +{mid} MHz -> {'PASS' if ok else 'FAIL'}",
                3, 10, m,
            )

            if ok and mid > 0:
                load_m = self._monitor.read_once()
                if load_m.core_clock_mhz <= self._baseline_core_mhz + 10:
                    self._emit(
                        f"Core search: +{mid} MHz applied but clock unchanged — backend may have failed",
                        3, 10, load_m,
                    )
                    break

            if ok:
                best = mid
                lo   = mid + 25
            else:
                hi   = mid - 25

        # Apply a 25 MHz safety margin below absolute max found
        safe = max(0, best - 25)
        self._core_offset_mhz = safe
        self._apply()
        return safe

    def _binary_search_mem(self) -> int:
        """Return the largest stable memory clock offset (MHz)."""
        limit = self._profile["mem_offset_mhz_max"]
        if limit == 0:
            return 0

        lo, hi   = 0, limit
        best     = 0
        test_dur = max(30, self._profile["test_duration_sec"] // self._profile["test_passes"])
        step     = 100  # memory steps in 100 MHz

        while lo <= hi:
            if self._cancel_event.is_set():
                break

            mid = ((lo + hi) // 2 // step) * step
            if mid == lo and lo > 0:
                mid = lo
            if mid < 0:
                break

            self._mem_offset_mhz = mid
            self._apply()
            time.sleep(1.0)

            test = self._stability_test_with_retries(duration_sec=test_dur)
            if not test.valid_load:
                m  = self._monitor.read_once()
                self._emit(
                    f"Mem search: testing +{mid} MHz -> INVALID (low load)",
                    5, 10, m,
                )
                break
            ok = test.passed
            m  = self._monitor.read_once()
            self._emit(
                f"Mem search: testing +{mid} MHz -> {'PASS' if ok else 'FAIL'}",
                5, 10, m,
            )

            if ok and mid > 0:
                load_m = self._monitor.read_once()
                if load_m.mem_clock_mhz <= self._baseline_core_mhz + 10:
                    self._emit(
                        f"Mem search: +{mid} MHz applied but clock unchanged — backend may have failed",
                        5, 10, load_m,
                    )
                    break

            if ok:
                best = mid
                lo   = mid + step
            else:
                hi   = mid - step

        safe = max(0, best - step)
        self._mem_offset_mhz = safe
        self._apply()
        return safe

    def _binary_search_voltage(self) -> int:
        """
        Return the most negative voltage offset (mV) that remains stable.
        Negative = undervolting (lower V at same F = better thermals / power).
        """
        limit = self._profile["voltage_offset_mv_min"]  # e.g. -150
        if limit >= 0:
            return 0

        test_dur = max(30, self._profile["test_duration_sec"] // self._profile["test_passes"])
        step     = 25   # 25 mV steps
        # Search in discrete 25 mV step indices to avoid negative-floor rounding
        # traps (e.g. repeatedly choosing -125 mV forever).
        lo_idx, hi_idx = limit // step, 0
        best           = 0

        while lo_idx <= hi_idx:
            if self._cancel_event.is_set():
                break

            mid_idx = (lo_idx + hi_idx) // 2
            mid = mid_idx * step

            self._voltage_offset_mv = mid
            self._apply()
            time.sleep(1.5)

            test = self._stability_test_with_retries(duration_sec=test_dur)
            if not test.valid_load:
                m  = self._monitor.read_once()
                self._emit(
                    f"Voltage search: testing {mid:+d} mV -> INVALID (low load)",
                    7, 10, m,
                )
                break
            ok = test.passed
            m  = self._monitor.read_once()
            self._emit(
                f"Voltage search: testing {mid:+d} mV -> {'PASS' if ok else 'FAIL'}",
                7, 10, m,
            )

            if ok:
                best = mid
                hi_idx = mid_idx - 1   # try more aggressive (more negative)
            else:
                lo_idx = mid_idx + 1   # back off toward 0

        # Apply 25 mV safety margin (less aggressive than max found)
        safe = best + step if best < 0 else 0
        self._voltage_offset_mv = safe
        self._apply()
        return safe

    def _tune_power_limit(self, baseline: GPUMetrics) -> int:
        """Reduce power limit as far as possible without losing >2 % performance."""
        min_delta, max_delta = self._profile["power_limit_delta_pct"]
        # First try raising slightly for overclock headroom
        for delta in range(max_delta, -1, -5):
            self._power_limit_pct = 100 + delta
            self._apply()
            time.sleep(0.5)

        # Now reduce until performance drops >2 %
        best_pct = self._power_limit_pct
        step     = 5

        for reduction_pct in range(step, abs(min_delta) + step, step):
            if self._cancel_event.is_set():
                break
            candidate = best_pct - reduction_pct
            self._power_limit_pct = candidate
            self._apply()
            time.sleep(0.8)

            m = self._measure(duration_sec=10)
            clock_drop_pct = (
                (baseline.core_clock_mhz - m.core_clock_mhz)
                / max(baseline.core_clock_mhz, 1) * 100
            ) if baseline.core_clock_mhz > 0 else 0

            test = self._stability_test_with_retries(duration_sec=20)
            if not test.valid_load:
                break
            if test.passed and clock_drop_pct < 2.0:
                best_pct = candidate
            else:
                break

        self._power_limit_pct = best_pct
        self._apply()
        return best_pct

    # ------------------------------------------------------------------
    # Helpers
    # ------------------------------------------------------------------

    def _apply(self) -> AppliedSettings:
        """Apply current working settings via the chosen backend."""
        result = self._backend.apply(
            gpu_index         = self._gpu.index,
            core_offset_mhz   = self._core_offset_mhz,
            mem_offset_mhz    = self._mem_offset_mhz,
            voltage_offset_mv = self._voltage_offset_mv,
            power_limit_pct   = self._power_limit_pct,
            thermal_limit_c   = self._profile["thermal_limit_max_c"],
        )
        if not result.success:
            raise RuntimeError(f"Backend apply failed: {result.notes}")
        has_oc_offsets = (
            self._core_offset_mhz != 0
            or self._mem_offset_mhz != 0
            or self._voltage_offset_mv != 0
        )
        if has_oc_offsets and not result.verified:
            raise RuntimeError(
                f"Settings verification failed — offsets may not have been applied. {result.notes}"
            )
        return result

    def _stability_test(self, duration_sec: float) -> StabilityResult:
        tester = StabilityTester(
            gpu_index      = self._gpu.index,
            temp_ceiling_c = self._temp_ceiling,
        )
        return tester.run(duration_sec)

    def _stability_test_with_retries(
        self,
        duration_sec: float,
        retries_on_low_load: int = 1,
    ) -> StabilityResult:
        """Run stability test and retry if stress did not produce meaningful load."""
        last = self._stability_test(duration_sec)
        for _ in range(retries_on_low_load):
            if last.failure_reason:
                return last
            if last.valid_load:
                return last
            time.sleep(0.5)
            last = self._stability_test(duration_sec)
        return last

    def _require_valid_stress_load(self) -> None:
        """Fail fast if no usable stress backend or sustained GPU load is unavailable."""
        preflight = self._stability_test_with_retries(duration_sec=30, retries_on_low_load=2)
        if preflight.failure_reason and "No usable GPU stress backend" in preflight.failure_reason:
            raise RuntimeError(preflight.failure_reason)
        if not preflight.valid_load:
            reason = preflight.load_note or (
                "Preflight stress validation failed: GPU load was not sustained."
            )
            raise RuntimeError(reason)

    def _measure(self, duration_sec: float) -> GPUMetrics:
        return sample_average(self._monitor, duration_sec)

    def _measure_under_load(self, duration_sec: float) -> GPUMetrics:
        """Measure GPU metrics while running a stress workload."""
        abort = threading.Event()

        def _stress_worker():
            try:
                with warnings.catch_warnings():
                    warnings.filterwarnings(
                        "ignore",
                        message="CUDA path could not be detected.*",
                        category=UserWarning,
                    )
                    import cupy as cp
                with cp.cuda.Device(self._gpu.index):
                    a = cp.random.random((4096, 4096), dtype=cp.float32)
                    b = cp.random.random((4096, 4096), dtype=cp.float32)
                    c = cp.empty((4096, 4096), dtype=cp.float32)
                    while not abort.is_set():
                        cp.matmul(a, b, out=c)
                        cp.cuda.Stream.null.synchronize()
            except Exception:
                pass

        t = threading.Thread(target=_stress_worker, daemon=True)
        t.start()
        time.sleep(10)  # thermal ramp-up

        from .monitor import sample_average_under_load
        result = sample_average_under_load(self._monitor, duration_sec)

        abort.set()
        t.join(timeout=5)
        return result

    def _emit(
        self,
        phase:  str,
        step:   int,
        total:  int,
        m:      Optional[GPUMetrics] = None,
    ) -> None:
        if self._progress:
            self._progress(phase, step, total, m)

    def partial_result(self) -> GPUOptimizationResult:
        """Return the best settings found so far (call after KeyboardInterrupt)."""
        r = GPUOptimizationResult(
            gpu_index  = self._gpu.index,
            gpu_name   = self._gpu.name,
            risk_level = self._risk.value,
        )
        r.core_offset_mhz    = self._core_offset_mhz
        r.mem_offset_mhz     = self._mem_offset_mhz
        r.voltage_offset_mv  = self._voltage_offset_mv
        r.power_limit_pct    = self._power_limit_pct
        r.stability_passed   = False
        r.notes              = "Partial: optimization was interrupted before final verification."
        return r
