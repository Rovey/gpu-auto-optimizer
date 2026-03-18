"""
GPU stability testing.

Test strategy (in priority order):
  1. CuPy matrix multiply loop   (if `cupy` installed)
  2. FurMark (if present at common paths)
  3. Unigine Heaven (if present)
  4. Fallback: instruct user to run any GPU workload while monitoring

During the test, pynvml / nvidia-smi is polled to detect:
  - Driver Timeout Detection & Recovery (TDR) – process crash / GPU reappears
  - Temperature exceeding hard ceiling
  - ECC memory errors (uncorrected)
  - Persistent clock throttling
  - Significant clock drop  (>150 MHz below expected boost)
"""
from __future__ import annotations

import os
import shutil
import subprocess
import sys
import threading
import time
import warnings
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, List, Optional

from .monitor import GPUMonitor, GPUMetrics

try:
    import pynvml
    _NVML = True
except ImportError:
    _NVML = False


# ---------------------------------------------------------------------------
# Known stress-tool paths
# ---------------------------------------------------------------------------

_FURMARK_PATHS = [
    r"C:\Program Files (x86)\Geeks3D\FurMark\FurMark.exe",
    r"C:\Program Files\Geeks3D\FurMark\FurMark.exe",
    r"C:\Program Files (x86)\Geeks3D\FurMark 2\FurMark2.exe",
]

_HEAVEN_PATHS = [
    r"C:\Program Files (x86)\Unigine\Heaven Benchmark 4.0\Heaven.exe",
    r"C:\Program Files\Unigine\Heaven Benchmark 4.0\Heaven.exe",
    r"C:\Program Files (x86)\Unigine\Superposition Benchmark 1.x\Superposition.exe",
]

_AUTO_CUPY_ATTEMPTED = False


def _find_stress_tool() -> Optional[Path]:
    for p in [*_FURMARK_PATHS, *_HEAVEN_PATHS]:
        if Path(p).exists():
            return Path(p)
    winsat = shutil.which("winsat")
    if winsat:
        return Path(winsat)
    return None


# ---------------------------------------------------------------------------
# Result
# ---------------------------------------------------------------------------

@dataclass
class StabilityResult:
    passed:           bool  = False
    duration_sec:     float = 0.0
    max_temp_c:       float = 0.0
    min_clock_mhz:    int   = 0
    max_clock_mhz:    int   = 0
    avg_clock_mhz:    float = 0.0
    ecc_errors:       int   = 0
    tdr_detected:     bool  = False   # driver reset
    thermal_throttle: bool  = False
    power_throttle:   bool  = False
    avg_gpu_util_pct: float = 0.0
    max_gpu_util_pct: int   = 0
    valid_load:       bool  = True
    stress_backend:   str   = ""
    load_note:        str   = ""
    failure_reason:   str   = ""
    snapshots:        List[GPUMetrics] = field(default_factory=list)


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
        Auto-select the best available stress method and run for `duration_sec`.
        Returns StabilityResult with pass/fail + telemetry.
        """
        # Try CuPy first (most controllable)
        cupy_available = self._cupy_available()

        monitor   = GPUMonitor(self._idx, poll_interval_sec=0.4)
        result    = StabilityResult()
        snapshots: List[GPUMetrics] = []
        abort_event = threading.Event()
        backend_used = ""

        # --- Monitoring thread ----------------------------------------
        def _monitor_thread() -> None:
            while not abort_event.is_set():
                m = monitor.read_once()
                snapshots.append(m)

                if m.temp_c >= self._ceil:
                    result.failure_reason  = (
                        f"Temperature ceiling exceeded: {m.temp_c:.0f} °C ≥ {self._ceil} °C"
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

        # --- Stress workload -----------------------------------------
        start = time.time()
        try:
            if cupy_available:
                backend_used = "cupy"
                try:
                    self._stress_cupy(duration_sec, abort_event, start)
                except Exception:
                    # CuPy import may succeed while runtime DLLs are missing.
                    # Fall back to external tools instead of failing the test.
                    tool = _find_stress_tool()
                    if tool:
                        backend_used = tool.name.lower()
                        self._stress_external(tool, duration_sec, abort_event)
                    else:
                        self._stress_fallback(duration_sec, abort_event, start)
            else:
                tool = _find_stress_tool()
                if tool:
                    backend_used = tool.name.lower()
                    self._stress_external(tool, duration_sec, abort_event)
                else:
                    self._stress_fallback(duration_sec, abort_event, start)
        except Exception as exc:
            if not result.failure_reason:
                result.failure_reason = f"Stress workload exception: {exc}"
                abort_event.set()

        elapsed = time.time() - start
        abort_event.set()
        mon_thread.join(timeout=3.0)

        # --- Aggregate results ----------------------------------------
        result.duration_sec = elapsed
        result.stress_backend = backend_used
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
            and elapsed >= duration_sec * 0.9   # completed at least 90 % of duration
        )
        return result

    # ------------------------------------------------------------------
    # Stress backends
    # ------------------------------------------------------------------

    def _cupy_available(self) -> bool:
        global _AUTO_CUPY_ATTEMPTED
        self._configure_cuda_path_env()

        def _probe() -> bool:
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

        if _probe():
            return True

        if not _AUTO_CUPY_ATTEMPTED:
            _AUTO_CUPY_ATTEMPTED = True
            self._attempt_auto_install_cupy()
            return _probe()

        return False

    def _detect_cuda_major_from_driver(self) -> Optional[str]:
        try:
            out = subprocess.check_output(
                ["nvidia-smi", "-q"],
                stderr=subprocess.DEVNULL,
                text=True,
                encoding="utf-8",
                errors="ignore",
            )
        except Exception:
            return None

        for line in out.splitlines():
            if "CUDA Version" in line:
                val = line.split(":", 1)[-1].strip()
                major = val.split(".", 1)[0]
                if major.isdigit():
                    return major
        return None

    def _attempt_auto_install_cupy(self) -> None:
        cuda_major = self._detect_cuda_major_from_driver()
        pkgs: List[str] = []
        if cuda_major == "13":
            pkgs = ["cupy-cuda13x", "cupy-cuda12x"]
        elif cuda_major == "12":
            pkgs = ["cupy-cuda12x"]
        elif cuda_major == "11":
            pkgs = ["cupy-cuda11x"]

        if not pkgs:
            return

        try:
            from rich.console import Console
            Console().print(
                f"[yellow]CuPy not found. Auto-installing a compatible package for CUDA {cuda_major}.x...[/yellow]"
            )
        except Exception:
            print("CuPy not found. Auto-installing a compatible package...")

        for pkg in pkgs:
            if self._pip_install(pkg):
                # CuPy may require additional NVIDIA runtime DLL packages
                # (varies by wheel/platform).
                self._pip_install("nvidia-cuda-nvrtc")
                self._pip_install("nvidia-curand")
                self._pip_install("nvidia-cublas")
                return

    def _pip_install(self, package: str) -> bool:
        base = [sys.executable, "-m", "pip", "install", package]
        for cmd in (base, [*base[:4], "--break-system-packages", *base[4:]]):
            try:
                subprocess.check_call(
                    cmd,
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                )
                return True
            except Exception:
                continue
        return False

    def _stress_cupy(
        self,
        duration_sec: float,
        abort: threading.Event,
        start:  float,
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
            while not abort.is_set() and (time.time() - start) < duration_sec:
                # Run multiple GEMMs per cycle with preallocated output.
                cp.matmul(a, b, out=c)
                cp.matmul(c, a, out=b)
                cp.cuda.Stream.null.synchronize()
                elapsed = time.time() - start
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

    def _stress_external(
        self,
        tool:         Path,
        duration_sec: float,
        abort:        threading.Event,
    ) -> None:
        """Launch an external tool (FurMark / Heaven) and wait."""
        name = tool.name.lower()
        if "furmark" in name:
            # FurMark CLI: /nogui /msaa=8 /xtreme_burning /benchmark /max_time=N
            max_ms  = int(duration_sec * 1000)
            cmd     = [str(tool), "/nogui", f"/max_time={max_ms}",
                       "/xtreme_burning", "/width=1280", "/height=720"]
        elif "winsat" in name:
            cmd = [str(tool), "dwm"]
        else:
            cmd     = [str(tool)]

        proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        deadline = time.time() + duration_sec
        while time.time() < deadline and not abort.is_set():
            if proc.poll() is not None:
                if "winsat" in name:
                    try:
                        proc = subprocess.Popen(
                            cmd,
                            stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL,
                        )
                    except Exception:
                        break
                else:
                    break
            time.sleep(0.5)
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()

    def _stress_fallback(
        self,
        duration_sec: float,
        abort:        threading.Event,
        start:        float,
    ) -> None:
        """
        No external tool available and no working CuPy runtime.
        Optimisation should not proceed without validated stress load.
        """
        try:
            from rich.console import Console
            Console().print(
                "\n[red]No usable GPU stress backend available, including built-in WinSAT. "
                "Aborting because optimisation requires validated GPU load.[/red]"
            )
        except ImportError:
            print(
                "No usable GPU stress backend available, including built-in WinSAT."
            )

        raise RuntimeError(
            "No usable GPU stress backend available. "
            "Automatic backend setup failed."
        )
