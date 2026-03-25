# GPU Auto Optimizer Rewrite — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Rewrite the GPU auto-optimizer to fix core measurement/verification bugs, strip to CuPy-only stress testing with correctness checks, replace CLI with Tkinter GUI + system tray, and add boot-apply persistence with 3-strike safety.

**Architecture:** Three-layer design — core logic (optimizer, backends, stability, monitor), headless boot-apply service, and Tkinter GUI with system tray. Core logic is UI-agnostic. Config/logs/app live in `%LOCALAPPDATA%\GPUOptimizer\`. NVAPI is the sole OC backend; pynvml handles power limits.

**Tech Stack:** Python 3.8+, pynvml, CuPy (CUDA-version-specific), ctypes/NVAPI, Tkinter + sv-ttk (Sun Valley theme), pystray, Windows Task Scheduler (via `schtasks.exe`)

---

## File Structure

### Files to DELETE

```
src/backends/afterburner.py    — Unreliable, no verification
src/backends/amd.py            — Never implemented stub
src/ui.py                      — Rich terminal UI, replaced by GUI
run_with_log.py                — CLI logging wrapper
run_with_log.bat               — CLI launcher
run_safe.bat                   — CLI shortcut
run_balanced.bat               — CLI shortcut
run_performance.bat            — CLI shortcut
run_monitor.bat                — CLI shortcut
run_reset.bat                  — CLI shortcut
install.bat                    — Replaced by Python installer
```

### Files to MODIFY

```
src/config.py                  — Extend with boot-apply state, strike counter, GPU UUID, driver version, app paths
src/stability.py               — CuPy-only with correctness verification, remove all external tool support
src/backends/nvapi.py          — Add read-back verification, remove VF curve code
src/backends/nvidia_smi.py     — Simplify to power-limit-only backend
src/backends/base.py           — Add verify() method to interface
src/optimizer.py               — Fix idle measurement bug, add clock verification under load, longer test durations
src/detector.py                — Remove AMD placeholder comment
src/monitor.py                 — Add measure-under-load helper
requirements.txt               — Update dependencies (remove rich/click/requests, add sv-ttk/pystray)
gpu_optimizer.py               — Rewrite as GUI launcher (replace Click CLI)
```

### Files to CREATE

```
src/logging_config.py          — Centralized logging with rotation, 50 MB cap, auto-pruning
src/boot_apply.py              — Headless boot-apply script (apply offsets, quick stress, 3-strike logic)
src/scheduler.py               — Windows Task Scheduler registration/removal via schtasks.exe
src/gui/__init__.py            — GUI package
src/gui/app.py                 — Main Tkinter application window, screen navigation
src/gui/theme.py               — Sun Valley theme initialization
src/gui/dashboard.py           — Dashboard screen (live stats, applied settings, boot log)
src/gui/optimization.py        — Optimization screen (risk selection, live progress, per-step results)
src/gui/results.py             — Results screen (before/after under-load comparison)
src/gui/settings_screen.py     — Settings screen (auto-apply toggle, boot log viewer, reset)
src/gui/widgets.py             — Shared widgets (status indicator, GPU metrics card, log viewer)
src/tray.py                    — System tray icon with pystray (status states, context menu, toast)
installer.py                   — Smart installer at project root (CUDA detect, CuPy install, file copy, shortcuts, scheduler). Lives in root to avoid copying itself into install dir.
tests/__init__.py              — Test package (empty file)
tests/conftest.py              — Shared pytest fixtures (mock_gpu, mock_backend, tmp_config)
tests/test_config.py           — Config persistence, boot-apply state, paths
tests/test_monitor.py          — sample_average_under_load filtering logic
tests/test_stability.py        — CuPy correctness verification logic (mocked)
tests/test_optimizer.py        — Optimizer measurement/verification logic (mocked backend)
tests/test_nvapi.py            — NVAPI read-back verification logic (mocked)
tests/test_boot_apply.py       — 3-strike logic, UUID check, driver version check
tests/test_scheduler.py        — Task Scheduler command construction (mocked subprocess)
tests/test_logging_config.py   — Log rotation, pruning, size cap
```

---

## Phase 1: Core Logic Fixes

These tasks fix the fundamental bugs and strip dead code. No UI changes yet.

### Task 1: Extend config.py with boot-apply state and app paths

**Files:**
- Modify: `src/config.py`
- Create: `tests/test_config.py`

- [ ] **Step 1: Write tests for new config fields**

```python
# tests/test_config.py
import json
import os
import tempfile
from src.config import (
    UserConfig, BootApplyState, load_config, save_config,
    get_app_dir, get_config_dir, get_log_dir,
)

def test_user_config_has_boot_apply_state():
    cfg = UserConfig()
    assert cfg.auto_apply_on_boot is False
    assert isinstance(cfg.boot_apply, BootApplyState)
    assert cfg.boot_apply.consecutive_failures == 0
    assert cfg.boot_apply.disabled is False
    assert cfg.boot_apply.gpu_uuid == ""
    assert cfg.boot_apply.driver_version == ""

def test_boot_apply_state_serialization(tmp_path):
    path = tmp_path / "config.json"
    cfg = UserConfig()
    cfg.boot_apply.consecutive_failures = 2
    cfg.boot_apply.gpu_uuid = "GPU-abc-123"
    cfg.boot_apply.driver_version = "560.70"
    save_config(cfg, str(path))

    loaded = load_config(str(path))
    assert loaded.boot_apply.consecutive_failures == 2
    assert loaded.boot_apply.gpu_uuid == "GPU-abc-123"
    assert loaded.boot_apply.driver_version == "560.70"

def test_app_dir_uses_localappdata(monkeypatch):
    monkeypatch.setenv("LOCALAPPDATA", "C:\\Users\\Test\\AppData\\Local")
    assert get_app_dir() == os.path.join("C:\\Users\\Test\\AppData\\Local", "GPUOptimizer")

def test_config_dir():
    app_dir = get_app_dir()
    assert get_config_dir() == os.path.join(app_dir, "config")

def test_log_dir():
    app_dir = get_app_dir()
    assert get_log_dir() == os.path.join(app_dir, "logs")

def test_load_missing_config_returns_defaults(tmp_path):
    path = tmp_path / "nonexistent.json"
    cfg = load_config(str(path))
    assert cfg.risk_level == "balanced"
    assert cfg.auto_apply_on_boot is False

def test_save_and_load_roundtrip(tmp_path):
    path = tmp_path / "config.json"
    cfg = UserConfig()
    cfg.risk_level = "performance"
    cfg.auto_apply_on_boot = True
    cfg.per_gpu_results = {"GPU0": {"core_offset_mhz": 100}}
    save_config(cfg, str(path))

    loaded = load_config(str(path))
    assert loaded.risk_level == "performance"
    assert loaded.auto_apply_on_boot is True
    assert loaded.per_gpu_results["GPU0"]["core_offset_mhz"] == 100
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `python -m pytest tests/test_config.py -v`
Expected: FAIL — `BootApplyState` and new functions don't exist yet

- [ ] **Step 3: Implement config changes**

Rewrite `src/config.py`:
- Add `BootApplyState` dataclass with fields: `consecutive_failures: int`, `disabled: bool`, `gpu_uuid: str`, `driver_version: str`, `last_apply_time: str`, `last_apply_result: str`, `boot_log: list`
- Add `boot_apply: BootApplyState` field to `UserConfig`
- Remove `stability_test_tool` field (CuPy is now the only option)
- Remove `fan_curve_enabled` field (not implemented)
- Add `get_app_dir()` → `%LOCALAPPDATA%\GPUOptimizer`
- Add `get_config_dir()` → `get_app_dir() / config`
- Add `get_log_dir()` → `get_app_dir() / logs`
- Change `load_config()` and `save_config()` to accept optional path parameter (for testability), default to `get_config_dir() / optimizer_config.json`
- Add `migrate_config_if_needed()` function: checks for old `optimizer_config.json` in the project root (next to `gpu_optimizer.py`). If found and new location doesn't exist yet, copies it to the new `%LOCALAPPDATA%` location. This prevents orphaning existing configs during the rewrite.
- Keep `RiskLevel`, `RISK_PROFILES`, `GPUOptimizationResult`, `save_result()` unchanged
- Remove `optimize_all_gpus()` from `optimizer.py` in Task 6 (GUI optimizes one GPU at a time)
- Boot log entries in `BootApplyState.boot_log` should be dicts with: `timestamp`, `action`, `result`, `details` — keep last 20 entries max

- [ ] **Step 4: Run tests to verify they pass**

Run: `python -m pytest tests/test_config.py -v`
Expected: All PASS

- [ ] **Step 5: Create tests/__init__.py and tests/conftest.py**

`tests/__init__.py`: empty file

`tests/conftest.py`:
```python
"""Shared pytest fixtures for GPU optimizer tests."""
import pytest
from unittest.mock import MagicMock, patch

@pytest.fixture
def mock_gpu():
    gpu = MagicMock()
    gpu.index = 0
    gpu.name = "Test GPU"
    gpu.vendor = "NVIDIA"
    gpu.architecture = "Turing"
    gpu.uuid = "GPU-test-uuid-123"
    gpu.driver_version = "560.70"
    gpu.supports_oc = True
    gpu.supports_uv = True
    gpu.supports_mem_oc = True
    return gpu

@pytest.fixture
def mock_backend():
    backend = MagicMock()
    backend.is_available.return_value = True
    backend.supports_core_oc.return_value = True
    backend.supports_mem_oc.return_value = True
    backend.supports_voltage.return_value = True
    backend.name = "mock-backend"
    backend.priority = 99
    return backend

@pytest.fixture
def tmp_config(tmp_path):
    return str(tmp_path / "optimizer_config.json")
```

- [ ] **Step 6: Commit**

```bash
git add src/config.py tests/__init__.py tests/conftest.py tests/test_config.py
git commit -m "feat: extend config with boot-apply state, app paths, and boot log"
```

---

### Task 2: Strip stability.py to CuPy-only with correctness verification

**Files:**
- Modify: `src/stability.py`
- Create: `tests/test_stability.py`

- [ ] **Step 1: Write tests for correctness verification logic**

```python
# tests/test_stability.py
import numpy as np
from unittest.mock import patch, MagicMock

def test_correctness_check_passes_with_correct_result():
    """Verify _verify_correctness returns True when GPU result matches CPU."""
    from src.stability import _verify_correctness
    # _verify_correctness takes (cpu_result, gpu_result_as_numpy) and returns bool
    a = np.random.random((64, 64)).astype(np.float32)
    b = np.random.random((64, 64)).astype(np.float32)
    cpu_result = a @ b
    # Simulate correct GPU result (same as CPU within float32 tolerance)
    assert _verify_correctness(cpu_result, cpu_result.copy()) is True

def test_correctness_check_detects_corruption():
    """If GPU returns wrong values, _verify_correctness should return False."""
    from src.stability import _verify_correctness
    a = np.random.random((64, 64)).astype(np.float32)
    b = np.random.random((64, 64)).astype(np.float32)
    cpu_result = a @ b
    corrupted = cpu_result.copy()
    corrupted[0, 0] += 1000.0  # Simulate bit flip / memory error
    assert _verify_correctness(cpu_result, corrupted) is False

def test_stability_result_fields():
    from src.stability import StabilityResult
    r = StabilityResult()
    assert r.passed is False
    assert r.valid_load is True
    assert r.correctness_passed is True  # new field, default True
    assert r.stress_backend == ""

def test_cupy_required_fails_when_unavailable():
    """If CuPy is not available, StabilityTester.run() should fail with clear message."""
    from src.stability import StabilityTester
    with patch.object(StabilityTester, '_cupy_available', return_value=False):
        tester = StabilityTester(gpu_index=0)
        result = tester.run(duration_sec=10)
        assert not result.passed
        assert "CuPy" in result.failure_reason
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `python -m pytest tests/test_stability.py -v`
Expected: FAIL — `_check_computation_correctness`, `correctness_passed` field don't exist

- [ ] **Step 3: Rewrite stability.py**

Remove from `src/stability.py`:
- `_FURMARK_PATHS`, `_HEAVEN_PATHS` constants
- `_find_stress_tool()` function
- `_stress_external()` method
- `_stress_fallback()` method
- `_AUTO_CUPY_ATTEMPTED` global and auto-install logic (`_attempt_auto_install_cupy`, `_pip_install`, `_detect_cuda_major_from_driver`)

Keep and modify:
- `StabilityResult` — add `correctness_passed: bool = True` field
- `StabilityTester.__init__` — keep as-is
- `StabilityTester.run()` — simplify to CuPy-only path. If CuPy unavailable, set `failure_reason = "CuPy is required but not available. Run the installer to set up CuPy with the correct CUDA version."` and return immediately. Remove all fallback chains.
- `_stress_cupy()` — keep the matmul loop but add periodic correctness checks every 5 seconds
- `_cupy_available()` — simplify: just probe, no auto-install. Keep `_configure_cuda_path_env()`.

Add new:
- `_verify_correctness(cpu_result: np.ndarray, gpu_result: np.ndarray) -> bool` — compares two numpy arrays with `np.allclose(cpu_result, gpu_result, rtol=1e-3)`. This is a pure function (no CuPy dependency) for testability.
- `_check_computation_correctness(cp, device_idx: int) -> bool` — creates 64x64 random float32 matrices on CPU, copies to GPU, computes matmul on GPU, copies result back via `cp.asnumpy()`, calls `_verify_correctness()`. Returns True if results match.
- In `_stress_cupy()`, call `_check_computation_correctness()` every 5 seconds during the stress loop. If it fails, set `result.correctness_passed = False` and `result.failure_reason = "GPU computation correctness check failed (memory instability detected)"`, then abort.
- Update `result.passed` condition in `run()` to also require `result.correctness_passed`

- [ ] **Step 4: Run tests to verify they pass**

Run: `python -m pytest tests/test_stability.py -v`
Expected: All PASS

- [ ] **Step 5: Commit**

```bash
git add src/stability.py tests/test_stability.py
git commit -m "feat: strip to CuPy-only stress testing with computation correctness verification"
```

---

### Task 3: Add read-back verification to NVAPI, remove VF curve code

**Files:**
- Modify: `src/backends/nvapi.py`
- Modify: `src/backends/base.py`
- Create: `tests/test_nvapi.py`

- [ ] **Step 1: Write tests for read-back verification**

```python
# tests/test_nvapi.py
from unittest.mock import patch, MagicMock
from src.backends.base import AppliedSettings

def test_applied_settings_has_verified_field():
    s = AppliedSettings()
    assert hasattr(s, "verified")
    assert s.verified is False

def test_verify_returns_actual_offsets():
    """verify() should read back P-state20 and return actual offsets."""
    from src.backends.nvapi import NVAPIBackend
    backend = NVAPIBackend()
    # If NVAPI isn't available (e.g. CI), verify should return None
    result = backend.verify(gpu_index=0)
    # On non-Windows or without NVIDIA, result is None
    # Just test the interface exists and returns the right type
    assert result is None or isinstance(result, dict)
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `python -m pytest tests/test_nvapi.py -v`
Expected: FAIL — `verified` field and `verify()` method don't exist

- [ ] **Step 3: Update base.py**

In `src/backends/base.py`:
- Add `verified: bool = False` field to `AppliedSettings`
- Add `def verify(self, gpu_index: int) -> dict | None` method to `GPUBackend` base class (non-abstract, returns `None` by default). Return dict should have keys: `core_offset_khz`, `mem_offset_khz`, `volt_offset_uv`.

- [ ] **Step 4: Update nvapi.py**

In `src/backends/nvapi.py`:

Remove entirely:
- `NV_GPU_VFP_CURVE_ENTRY` struct
- `NV_GPU_VFP_CURVE` struct
- `_VFP_CURVE_VERSION` constant
- `_NVAPI_GPU_GET_VFP_CURVE` and `_NVAPI_GPU_SET_VFP_CURVE` function IDs
- `_NVAPILoader.get_vfp_curve()` method
- `_NVAPILoader.set_vfp_curve()` method
- `NVAPIBackend.flatten_vf_curve()` method
- `NVAPIBackend.get_vf_curve_info()` method
- Fix `_NVAPI_GPU_GET_FULL_NAME`: delete the wrong constant (`0xCEEE8D8C`, which duplicates `_NVAPI_ERROR`) and rename `_NVAPI_GPU_GET_FULL_NAME2` to `_NVAPI_GPU_GET_FULL_NAME` (correct value `0xCEEBE66E`)

Add:
- `NVAPIBackend.verify(self, gpu_index: int) -> dict | None`:
  - Call `self._loader.get_pstate20(gpu_index)`
  - Read back the P0 state clocks[0].freqDelta_value (core offset kHz) and clocks[1].freqDelta_value (mem offset kHz) and baseVoltages[0].voltDelta_value (volt offset µV)
  - Return `{"core_offset_khz": ..., "mem_offset_khz": ..., "volt_offset_uv": ...}` or `None` if read fails

Modify `NVAPIBackend.apply()`:
- After `set_pstate20()`, call `verify()` and compare returned offsets against requested offsets
- Set `AppliedSettings.verified = True` only if read-back matches (within tolerance of ±1000 kHz / ±1000 µV for rounding)
- If read-back doesn't match, set `notes` to include "WARNING: Read-back verification failed"

- [ ] **Step 5: Run tests to verify they pass**

Run: `python -m pytest tests/test_nvapi.py -v`
Expected: All PASS

- [ ] **Step 6: Commit**

```bash
git add src/backends/base.py src/backends/nvapi.py tests/test_nvapi.py
git commit -m "feat: add NVAPI read-back verification, remove VF curve code"
```

---

### Task 4: Simplify nvidia_smi.py to power-limit-only

**Files:**
- Modify: `src/backends/nvidia_smi.py`

- [ ] **Step 1: Simplify nvidia_smi.py**

In `src/backends/nvidia_smi.py`:
- Remove the `_run_smi` helper's usage for power limit setting (keep it for `is_available()` check only)
- Keep `is_available()`, `apply()`, `reset()`
- In `apply()`: remove the nvidia-smi CLI fallback for power limits — only use pynvml. If pynvml fails, report failure. This simplifies the code and pynvml is always available (it's a required dependency).
- In `apply()`: set `AppliedSettings.verified = True` on success. Power-limit-only operations are self-verifying via pynvml (the call either succeeds or raises). This is necessary because the optimizer's `_apply()` will check the `verified` flag, and the SMI backend is used for safe mode and as a power-limit fallback.
- Make sure `supports_voltage`, `supports_core_oc`, `supports_mem_oc` all return `False` (already do)
- No test file needed — this backend is simple and tested via integration

- [ ] **Step 2: Commit**

```bash
git add src/backends/nvidia_smi.py
git commit -m "refactor: simplify nvidia-smi backend to pynvml power-limit-only"
```

---

### Task 5: Add measure-under-load helper to monitor.py

**Depends on:** None (independent of Tasks 1-4, can be parallelized)

**Files:**
- Modify: `src/monitor.py`
- Create: `tests/test_monitor.py`

- [ ] **Step 1: Write tests for sample_average_under_load**

```python
# tests/test_monitor.py
from unittest.mock import MagicMock, patch
from src.monitor import GPUMetrics, sample_average_under_load

def test_filters_out_idle_samples():
    """Only samples with gpu_util >= threshold should be included."""
    monitor = MagicMock()
    samples = [
        GPUMetrics(gpu_index=0, gpu_util_pct=10, core_clock_mhz=300),   # idle — discard
        GPUMetrics(gpu_index=0, gpu_util_pct=95, core_clock_mhz=1800),  # load — keep
        GPUMetrics(gpu_index=0, gpu_util_pct=99, core_clock_mhz=1820),  # load — keep
    ]
    monitor.read_once.side_effect = samples
    with patch("src.monitor.time") as mock_time:
        mock_time.time.side_effect = [0.0, 0.5, 1.0, 1.5]  # 3 reads, then past deadline
        mock_time.sleep = MagicMock()
        result = sample_average_under_load(monitor, duration_sec=1.0, min_util_pct=80)
    assert result.core_clock_mhz == 1810  # avg of 1800 and 1820
    assert result.samples_used == 2

def test_returns_all_samples_if_none_qualify():
    """If no samples meet threshold, return raw average with samples_used=0."""
    monitor = MagicMock()
    samples = [
        GPUMetrics(gpu_index=0, gpu_util_pct=10, core_clock_mhz=300),
        GPUMetrics(gpu_index=0, gpu_util_pct=5, core_clock_mhz=280),
    ]
    monitor.read_once.side_effect = samples
    with patch("src.monitor.time") as mock_time:
        mock_time.time.side_effect = [0.0, 0.5, 1.0]
        mock_time.sleep = MagicMock()
        result = sample_average_under_load(monitor, duration_sec=0.5, min_util_pct=80)
    assert result.samples_used == 0
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `python -m pytest tests/test_monitor.py -v`
Expected: FAIL — `sample_average_under_load` and `samples_used` don't exist yet

- [ ] **Step 3: Add `sample_average_under_load()` to monitor.py**

Add a new function to `src/monitor.py`:

```python
def sample_average_under_load(
    monitor: GPUMonitor,
    duration_sec: float,
    min_util_pct: int = 80,
) -> GPUMetrics:
    """
    Poll monitor for `duration_sec`, discard samples where gpu_util_pct < min_util_pct,
    return averaged metrics from only the under-load samples.
    If no samples meet the threshold, return the raw average with a flag.
    """
```

This filters out idle/ramp-up samples so the averaged result reflects actual under-load performance. Set `gpu_util_pct` on the returned metrics to the average of qualifying samples. Add a `samples_used: int` field to `GPUMetrics` (default 0) so callers know if the measurement was meaningful.

- [ ] **Step 4: Run tests to verify they pass**

Run: `python -m pytest tests/test_monitor.py -v`
Expected: All PASS

- [ ] **Step 5: Commit**

```bash
git add src/monitor.py tests/test_monitor.py
git commit -m "feat: add sample_average_under_load helper for under-load measurements"
```

---

### Task 6: Fix optimizer.py — measurements under load, clock verification, longer tests

**Depends on:** Task 3 (base.py `verified` field), Task 5 (monitor.py `sample_average_under_load`)

**Files:**
- Modify: `src/optimizer.py`
- Create: `tests/test_optimizer.py`

This is the largest task — it fixes the three core bugs:
1. Measurements taken at idle
2. `_apply()` return value ignored
3. Binary search doesn't verify offsets took effect

- [ ] **Step 1: Write tests for optimizer verification logic**

```python
# tests/test_optimizer.py
import pytest
from unittest.mock import MagicMock, patch
from src.config import RiskLevel, RISK_PROFILES
from src.backends.base import AppliedSettings

@pytest.fixture
def mock_optimizer():
    """Create a GPUOptimizer with mocked backend (avoids real NVAPI/GPU calls)."""
    mock_gpu = MagicMock()
    mock_gpu.index = 0
    mock_gpu.name = "Test GPU"
    mock_gpu.supports_uv = True

    mock_backend = MagicMock()
    mock_backend.is_available.return_value = True
    mock_backend.supports_core_oc.return_value = True
    mock_backend.supports_mem_oc.return_value = True
    mock_backend.supports_voltage.return_value = True

    with patch("src.optimizer._best_backend", return_value=mock_backend):
        from src.optimizer import GPUOptimizer
        opt = GPUOptimizer(mock_gpu, RiskLevel.BALANCED)
    return opt

def test_apply_checks_return_value(mock_optimizer):
    """Optimizer should raise if backend.apply() returns success=False."""
    mock_optimizer._backend.apply.return_value = AppliedSettings(
        success=False, notes="NVAPI failed"
    )
    with pytest.raises(RuntimeError, match="NVAPI failed"):
        mock_optimizer._apply()

def test_apply_checks_verification_when_oc_offsets_set(mock_optimizer):
    """Optimizer should raise if read-back verification fails with OC offsets."""
    mock_optimizer._core_offset_mhz = 100  # non-zero OC offset
    mock_optimizer._backend.apply.return_value = AppliedSettings(
        success=True, verified=False, notes="Read-back mismatch"
    )
    with pytest.raises(RuntimeError, match="(?i)verification"):
        mock_optimizer._apply()

def test_apply_skips_verification_for_power_only(mock_optimizer):
    """Power-limit-only changes should not require read-back verification."""
    mock_optimizer._core_offset_mhz = 0
    mock_optimizer._mem_offset_mhz = 0
    mock_optimizer._voltage_offset_mv = 0
    mock_optimizer._backend.apply.return_value = AppliedSettings(
        success=True, verified=False, notes="Power only"
    )
    # Should NOT raise — no OC offsets, so verification is skipped
    result = mock_optimizer._apply()
    assert result.success is True

def test_measure_under_load_method_exists(mock_optimizer):
    """_measure_under_load should exist as a method."""
    assert hasattr(mock_optimizer, "_measure_under_load")

def test_cancel_mechanism(mock_optimizer):
    """Optimizer should have a cancel event that stops binary search."""
    assert hasattr(mock_optimizer, '_cancel_event')
    assert not mock_optimizer._cancel_event.is_set()
    mock_optimizer.cancel()
    assert mock_optimizer._cancel_event.is_set()
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `python -m pytest tests/test_optimizer.py -v`
Expected: FAIL

- [ ] **Step 3: Rewrite optimizer.py**

Key changes to `src/optimizer.py`:

**Backend selection:**
- Remove `AfterburnerBackend` from imports and `_best_backend()` candidates
- Keep only `NVAPIBackend` and `NvidiaSMIBackend`

**Remove `optimize_all_gpus()` function** — the GUI optimizes one GPU at a time

**`_apply()` method — must check return value and verification:**
```python
def _apply(self) -> AppliedSettings:
    result = self._backend.apply(
        gpu_index=self._gpu.index,
        core_offset_mhz=self._core_offset_mhz,
        mem_offset_mhz=self._mem_offset_mhz,
        voltage_offset_mv=self._voltage_offset_mv,
        power_limit_pct=self._power_limit_pct,
        thermal_limit_c=self._profile["thermal_limit_max_c"],
    )
    if not result.success:
        raise RuntimeError(f"Backend apply failed: {result.notes}")
    # Only enforce read-back verification when OC offsets are being applied.
    # Power-limit-only operations (safe mode, SMI backend) are self-verifying
    # via pynvml — the call either succeeds or raises.
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
```

**New `_measure_under_load()` method:**
- Starts a CuPy stress workload in a background thread
- Waits 10 seconds for thermal ramp-up
- Calls `sample_average_under_load(self._monitor, duration_sec)` while stress is running
- Stops the stress thread
- Returns the averaged under-load metrics

**Baseline measurement (`run()` method):**
- Replace `self._measure(duration_sec=20)` with `self._measure_under_load(duration_sec=20)` — measures baseline UNDER LOAD

**Final measurement (`run()` method):**
- Replace `self._measure(duration_sec=15)` with `self._measure_under_load(duration_sec=20)` — measures final UNDER LOAD after the last stress test

**Binary search — add clock verification:**

In `_binary_search_core()`, after applying an offset and running the stability test:
```python
# After stability test passes, verify the clock actually changed
m = self._monitor.read_once()
# Allow 50 MHz tolerance for GPU Boost dynamics
if mid > 0 and m.core_clock_mhz <= baseline_core + 10:
    self._emit(f"Core search: +{mid} MHz offset applied but clock did not increase — backend may have failed", ...)
    break
```

Same pattern for `_binary_search_mem()`.

**Store baseline core clock in `run()`** so binary search methods can reference it.

**Test durations:**
- Change minimum per-step test from `max(20, ...)` to `max(30, ...)` for core and voltage searches
- Change minimum per-step test from `max(15, ...)` to `max(30, ...)` for memory search
- Final verification: change to `max(300, self._profile["test_duration_sec"] * 2)` (5 minutes minimum)

**Final verification:**
- After the 5-minute stress test, add a thermal soak: the stress runs for 5 minutes total, measurements taken from the last 60 seconds only (GPU has reached thermal equilibrium by then)

- [ ] **Step 4: Run tests to verify they pass**

Run: `python -m pytest tests/test_optimizer.py -v`
Expected: All PASS

- [ ] **Step 5: Commit**

```bash
git add src/optimizer.py tests/test_optimizer.py
git commit -m "fix: measure under load, verify apply results, verify clock changes, longer test durations"
```

---

### Task 7: Delete dead code and old CLI files

**Files:**
- Delete: `src/backends/afterburner.py`
- Delete: `src/backends/amd.py`
- Delete: `src/ui.py`
- Delete: `run_with_log.py`
- Delete: `run_with_log.bat`
- Delete: `run_safe.bat`, `run_balanced.bat`, `run_performance.bat`, `run_monitor.bat`, `run_reset.bat`
- Delete: `install.bat`
- Modify: `src/detector.py` (remove AMD placeholder comment)
- Modify: `src/backends/__init__.py` (leave empty, just verify no broken imports)

- [ ] **Step 1: Delete files**

```bash
git rm src/backends/afterburner.py
git rm src/backends/amd.py
git rm src/ui.py
git rm run_with_log.py
git rm run_with_log.bat
git rm run_safe.bat run_balanced.bat run_performance.bat run_monitor.bat run_reset.bat
git rm install.bat
```

- [ ] **Step 2: Clean up detector.py**

In `src/detector.py`:
- Remove the comment `# AMD – placeholder; extend in future` and the commented-out `# gpus.extend(_detect_amd())` line (lines 86-88)
- Remove `_supports_voltage_curve()` function — this was for VF curve support which is removed. The `supports_uv` field in `GPUInfo` should now refer to voltage offset support (which all Turing+ GPUs support via NVAPI P-state20). Change `supports_uv=_supports_voltage_curve(arch)` to `supports_uv=arch in ("Turing", "Ampere", "Ada Lovelace")` inline.

- [ ] **Step 3: Verify no broken imports**

Run: `python -c "from src.optimizer import GPUOptimizer; print('OK')"`
Expected: `OK` (no import errors)

- [ ] **Step 4: Run all tests**

Run: `python -m pytest tests/ -v`
Expected: All PASS

- [ ] **Step 5: Commit**

```bash
git add -A
git commit -m "chore: remove afterburner/AMD backends, CLI, bat files, and dead code"
```

---

## Phase 2: Infrastructure (Logging, Boot-Apply, Scheduler)

### Task 8: Create centralized logging with rotation and size cap

**Files:**
- Create: `src/logging_config.py`
- Create: `tests/test_logging_config.py`

- [ ] **Step 1: Write tests for logging setup**

```python
# tests/test_logging_config.py
import os
import tempfile
from pathlib import Path

def test_setup_logging_creates_log_dir(tmp_path):
    from src.logging_config import setup_logging
    log_dir = tmp_path / "logs"
    logger = setup_logging(log_dir=str(log_dir), name="test")
    assert log_dir.exists()

def test_optimization_log_has_timestamp_name(tmp_path):
    from src.logging_config import create_optimization_log
    log_dir = tmp_path / "logs"
    log_dir.mkdir()
    path = create_optimization_log(str(log_dir))
    assert "optimize_" in os.path.basename(path)
    assert path.endswith(".log")

def test_prune_logs_removes_oldest(tmp_path):
    from src.logging_config import prune_logs
    log_dir = tmp_path / "logs"
    log_dir.mkdir()
    # Create 15 fake log files totaling > 50 MB
    for i in range(15):
        f = log_dir / f"optimize_{i:04d}.log"
        f.write_bytes(b"x" * (4 * 1024 * 1024))  # 4 MB each = 60 MB total

    prune_logs(str(log_dir), max_total_bytes=50 * 1024 * 1024)
    total = sum(f.stat().st_size for f in log_dir.iterdir())
    assert total <= 50 * 1024 * 1024

def test_prune_keeps_max_n_optimization_logs(tmp_path):
    from src.logging_config import prune_logs
    log_dir = tmp_path / "logs"
    log_dir.mkdir()
    for i in range(15):
        f = log_dir / f"optimize_{i:04d}.log"
        f.write_text("small")

    prune_logs(str(log_dir), max_optimization_logs=10)
    opt_logs = [f for f in log_dir.iterdir() if f.name.startswith("optimize_")]
    assert len(opt_logs) <= 10
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `python -m pytest tests/test_logging_config.py -v`
Expected: FAIL

- [ ] **Step 3: Implement logging_config.py**

Create `src/logging_config.py`:
- `setup_logging(log_dir: str, name: str) -> logging.Logger`: creates log dir, returns configured logger
- `create_optimization_log(log_dir: str) -> str`: returns path like `logs/optimize_20260325_143022.log`
- `setup_boot_apply_log(log_dir: str) -> logging.Logger`: rotating file handler for `boot_apply.log`, rotate at 1 MB, keep 3 backups
- `prune_logs(log_dir: str, max_total_bytes: int = 50*1024*1024, max_optimization_logs: int = 10)`: delete oldest optimization logs exceeding count, then delete oldest files until total size is under cap
- Use `logging.handlers.RotatingFileHandler` for boot log
- Use standard `logging.FileHandler` for per-run optimization logs (one file per run, pruned by count)

- [ ] **Step 4: Run tests to verify they pass**

Run: `python -m pytest tests/test_logging_config.py -v`
Expected: All PASS

- [ ] **Step 5: Commit**

```bash
git add src/logging_config.py tests/test_logging_config.py
git commit -m "feat: centralized logging with rotation, count limits, and 50 MB cap"
```

---

### Task 9: Create boot-apply headless script with 3-strike logic

**Files:**
- Create: `src/boot_apply.py`
- Create: `tests/test_boot_apply.py`

- [ ] **Step 1: Write tests for 3-strike logic**

```python
# tests/test_boot_apply.py
from unittest.mock import patch, MagicMock
from src.config import UserConfig, BootApplyState

def test_skip_if_auto_apply_disabled():
    from src.boot_apply import should_apply
    cfg = UserConfig()
    cfg.auto_apply_on_boot = False
    result = should_apply(cfg)
    assert result.skip is True
    assert "disabled" in result.reason.lower()

def test_skip_if_3_consecutive_failures():
    from src.boot_apply import should_apply
    cfg = UserConfig()
    cfg.auto_apply_on_boot = True
    cfg.boot_apply.consecutive_failures = 3
    cfg.boot_apply.disabled = True
    result = should_apply(cfg)
    assert result.skip is True
    assert "3" in result.reason or "strike" in result.reason.lower()

def test_skip_if_gpu_uuid_changed():
    from src.boot_apply import should_apply
    cfg = UserConfig()
    cfg.auto_apply_on_boot = True
    cfg.boot_apply.gpu_uuid = "GPU-old-uuid"
    result = should_apply(cfg, current_gpu_uuid="GPU-new-uuid")
    assert result.skip is True
    assert "hardware" in result.reason.lower() or "uuid" in result.reason.lower()

def test_allow_if_driver_changed():
    from src.boot_apply import should_apply
    cfg = UserConfig()
    cfg.auto_apply_on_boot = True
    cfg.boot_apply.gpu_uuid = "GPU-abc"
    cfg.boot_apply.driver_version = "555.00"
    result = should_apply(cfg, current_gpu_uuid="GPU-abc", current_driver="560.00")
    assert result.skip is False
    assert "driver" in result.warning.lower()

def test_increment_failure_on_stress_fail():
    from src.boot_apply import record_boot_result
    cfg = UserConfig()
    cfg.boot_apply.consecutive_failures = 1
    record_boot_result(cfg, success=False, details="Stress test failed")
    assert cfg.boot_apply.consecutive_failures == 2
    assert cfg.boot_apply.disabled is False

def test_disable_on_third_failure():
    from src.boot_apply import record_boot_result
    cfg = UserConfig()
    cfg.boot_apply.consecutive_failures = 2
    record_boot_result(cfg, success=False, details="Stress test failed")
    assert cfg.boot_apply.consecutive_failures == 3
    assert cfg.boot_apply.disabled is True

def test_reset_failures_on_success():
    from src.boot_apply import record_boot_result
    cfg = UserConfig()
    cfg.boot_apply.consecutive_failures = 2
    record_boot_result(cfg, success=True, details="OK")
    assert cfg.boot_apply.consecutive_failures == 0
    assert cfg.boot_apply.disabled is False

def test_no_saved_results_skips():
    from src.boot_apply import should_apply
    cfg = UserConfig()
    cfg.auto_apply_on_boot = True
    cfg.per_gpu_results = {}
    result = should_apply(cfg)
    assert result.skip is True
    assert "no saved" in result.reason.lower()
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `python -m pytest tests/test_boot_apply.py -v`
Expected: FAIL

- [ ] **Step 3: Implement boot_apply.py**

Create `src/boot_apply.py`:

- `@dataclass ApplyDecision`: `skip: bool`, `reason: str`, `warning: str = ""`
- `should_apply(cfg: UserConfig, current_gpu_uuid: str = "", current_driver: str = "") -> ApplyDecision`:
  - Check `auto_apply_on_boot` is True
  - Check `boot_apply.disabled` is False
  - Check `per_gpu_results` is not empty
  - Check GPU UUID matches (if saved). If mismatch → skip, reason "GPU hardware changed"
  - Check driver version (if saved). If mismatch → allow but set `warning = "Driver version changed from X to Y"`
  - Return `ApplyDecision(skip=False, ...)`

- `record_boot_result(cfg: UserConfig, success: bool, details: str)`:
  - If success: reset `consecutive_failures = 0`, `disabled = False`
  - If fail: increment `consecutive_failures`. If >= 3: set `disabled = True`
  - Append to `boot_apply.boot_log` (keep last 20 entries)
  - Set `last_apply_time` and `last_apply_result`

- `def run_boot_apply()`: main entry point for headless script
  - Load config
  - Detect GPU, get UUID and driver version
  - Call `should_apply()`
  - If skip: log reason, show Windows toast notification if disabled, exit
  - If proceed: import NVAPI backend, apply saved offsets, run 20-second CuPy stress at below-normal priority (`BELOW_NORMAL_PRIORITY_CLASS`)
  - Record result
  - Save config
  - Show toast notification (success: brief, failure: visible warning)

- `if __name__ == "__main__": run_boot_apply()` — direct execution support

Toast notifications: use `win10toast` or `winotify` library. Add to requirements. If unavailable, silently skip toast (non-critical).

- [ ] **Step 4: Run tests to verify they pass**

Run: `python -m pytest tests/test_boot_apply.py -v`
Expected: All PASS

- [ ] **Step 5: Commit**

```bash
git add src/boot_apply.py tests/test_boot_apply.py
git commit -m "feat: headless boot-apply with 3-strike logic, UUID check, driver warning"
```

---

### Task 10: Create Windows Task Scheduler integration

**Files:**
- Create: `src/scheduler.py`
- Create: `tests/test_scheduler.py`

- [ ] **Step 1: Write tests for scheduler command construction**

```python
# tests/test_scheduler.py
from unittest.mock import patch, MagicMock
import subprocess

def test_register_constructs_correct_schtasks_command():
    """Verify the schtasks /create command is built correctly."""
    from src.scheduler import register_boot_task, TASK_NAME
    with patch("src.scheduler.subprocess.run") as mock_run:
        mock_run.return_value = MagicMock(returncode=0)
        result = register_boot_task(
            python_exe=r"C:\Python\pythonw.exe",
            script_path=r"C:\App\boot_apply.py",
        )
        assert result is True
        args = mock_run.call_args_list[-1][0][0]  # last call's positional arg
        assert "schtasks" in args[0].lower()
        assert "/create" in args
        assert TASK_NAME in " ".join(args)
        assert r"C:\Python\pythonw.exe" in " ".join(args)

def test_unregister_calls_schtasks_delete():
    from src.scheduler import unregister_boot_task, TASK_NAME
    with patch("src.scheduler.subprocess.run") as mock_run:
        mock_run.return_value = MagicMock(returncode=0)
        result = unregister_boot_task()
        assert result is True
        args = mock_run.call_args[0][0]
        assert "/delete" in args
        assert TASK_NAME in args

def test_is_registered_returns_true_when_task_exists():
    from src.scheduler import is_task_registered
    with patch("src.scheduler.subprocess.run") as mock_run:
        mock_run.return_value = MagicMock(returncode=0)
        assert is_task_registered() is True

def test_is_registered_returns_false_when_missing():
    from src.scheduler import is_task_registered
    with patch("src.scheduler.subprocess.run") as mock_run:
        mock_run.return_value = MagicMock(returncode=1)
        assert is_task_registered() is False
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `python -m pytest tests/test_scheduler.py -v`
Expected: FAIL — scheduler module doesn't exist yet

- [ ] **Step 3: Implement scheduler.py**

Create `src/scheduler.py`:

- `TASK_NAME = "GPUOptimizer_BootApply"`
- `register_boot_task(python_exe: str, script_path: str) -> bool`:
  - Uses `subprocess.run(["schtasks", "/create", ...])` to create a task that:
    - Runs at user logon
    - Runs with highest privileges (for NVAPI admin access)
    - Executes: `<python_exe> <script_path>`
    - Task name: `TASK_NAME`
  - Returns True on success
  - If task already exists, delete and recreate

- `unregister_boot_task() -> bool`:
  - `schtasks /delete /tn TASK_NAME /f`
  - Returns True on success

- `is_task_registered() -> bool`:
  - `schtasks /query /tn TASK_NAME`
  - Returns True if exit code 0

- [ ] **Step 4: Run tests to verify they pass**

Run: `python -m pytest tests/test_scheduler.py -v`
Expected: All PASS

- [ ] **Step 5: Commit**

```bash
git add src/scheduler.py tests/test_scheduler.py
git commit -m "feat: Windows Task Scheduler integration for boot-apply"
```

---

## Phase 3: GUI

### Task 11: Set up Tkinter with Sun Valley theme

**Files:**
- Create: `src/gui/__init__.py`
- Create: `src/gui/theme.py`

- [ ] **Step 1: Create GUI package and theme setup**

`src/gui/__init__.py`: empty file

`src/gui/theme.py`:
```python
"""Sun Valley theme initialization for Tkinter."""
import tkinter as tk
from tkinter import ttk

def apply_theme(root: tk.Tk, mode: str = "dark") -> None:
    """Apply Sun Valley ttk theme. mode = 'dark' or 'light'."""
    import sv_ttk
    sv_ttk.set_theme(mode)

def create_root(title: str = "GPU Optimizer") -> tk.Tk:
    """Create and configure the root Tk window."""
    root = tk.Tk()
    root.title(title)
    root.geometry("900x650")
    root.minsize(800, 550)
    apply_theme(root, mode="dark")
    # Set window icon if available
    try:
        # Icon will be added later
        pass
    except Exception:
        pass
    return root
```

- [ ] **Step 2: Commit**

```bash
git add src/gui/__init__.py src/gui/theme.py
git commit -m "feat: GUI package with Sun Valley dark theme"
```

---

### Task 12: Create shared widgets

**Files:**
- Create: `src/gui/widgets.py`

- [ ] **Step 1: Implement shared widgets**

Create `src/gui/widgets.py` with reusable components:

- `StatusIndicator(parent, size=16)` — canvas circle that can be set to green/yellow/red/grey via `set_status(color)` method
- `GPUMetricsCard(parent)` — labeled frame showing live GPU stats (core clock, mem clock, temp, power, fan, util). Has `update(metrics: GPUMetrics)` method.
- `LogViewer(parent, max_lines=100)` — scrollable text widget for displaying log entries. Has `append(text)` and `clear()` methods. Read-only.
- `OffsetDisplay(parent)` — shows current applied offsets (Core +X MHz, Mem +X MHz, Volt -X mV, Power X%). Has `update(result: GPUOptimizationResult)` method.
- `StepProgressList(parent)` — treeview/listbox showing per-step optimization results with pass/fail indicators. Has `add_step(description, passed)` and `clear()` methods.

- [ ] **Step 2: Commit**

```bash
git add src/gui/widgets.py
git commit -m "feat: shared GUI widgets (status indicator, metrics card, log viewer)"
```

---

### Task 13: Create main app shell with screen navigation

**Files:**
- Create: `src/gui/app.py`

- [ ] **Step 1: Implement main app window**

Create `src/gui/app.py`:

```python
"""Main GUI application — manages screen navigation and lifecycle."""
import tkinter as tk
from tkinter import ttk
from .theme import create_root

class GPUOptimizerApp:
    """Main application class. Manages the window, navigation, and screen switching."""

    def __init__(self) -> None:
        self.root = create_root()
        self._screens: dict[str, ttk.Frame] = {}
        self._current_screen: str = ""
        self._setup_layout()

    def _setup_layout(self) -> None:
        # Sidebar navigation (left)
        self._nav_frame = ttk.Frame(self.root, width=180)
        self._nav_frame.pack(side="left", fill="y", padx=(10, 0), pady=10)
        self._nav_frame.pack_propagate(False)

        # Content area (right)
        self._content_frame = ttk.Frame(self.root)
        self._content_frame.pack(side="left", fill="both", expand=True, padx=10, pady=10)

        # Navigation buttons
        nav_items = [
            ("Dashboard", "dashboard"),
            ("Optimize", "optimization"),
            ("Results", "results"),
            ("Settings", "settings"),
        ]
        for label, screen_id in nav_items:
            btn = ttk.Button(
                self._nav_frame,
                text=label,
                command=lambda s=screen_id: self.show_screen(s),
            )
            btn.pack(fill="x", pady=2)

    def register_screen(self, screen_id: str, frame: ttk.Frame) -> None:
        self._screens[screen_id] = frame

    def show_screen(self, screen_id: str) -> None:
        if self._current_screen and self._current_screen in self._screens:
            self._screens[self._current_screen].pack_forget()
        if screen_id in self._screens:
            self._screens[screen_id].pack(in_=self._content_frame, fill="both", expand=True)
            self._current_screen = screen_id

    def run(self) -> None:
        self.show_screen("dashboard")
        self.root.mainloop()
```

Screens will be registered in Task 14-17. The app shell should work standalone (empty content area) for testing.

- [ ] **Step 2: Test manually**

Run: `python -c "from src.gui.app import GPUOptimizerApp; app = GPUOptimizerApp(); app.run()"`
Expected: Window opens with dark theme and sidebar navigation. Closing the window exits.

- [ ] **Step 3: Commit**

```bash
git add src/gui/app.py
git commit -m "feat: main GUI shell with sidebar navigation"
```

---

### Task 14: Create Dashboard screen

**Files:**
- Create: `src/gui/dashboard.py`

- [ ] **Step 1: Implement dashboard**

Create `src/gui/dashboard.py`:

```python
"""Dashboard screen — live GPU stats, applied settings, auto-apply status, boot log."""
```

The dashboard has 4 sections in a grid layout:

1. **GPU Status** (top-left) — `GPUMetricsCard` with live-updating stats. Uses a `root.after(1000, ...)` polling loop to call `GPUMonitor.read_once()` and update the card every second.

2. **Applied Settings** (top-right) — `OffsetDisplay` showing current offsets from last optimization. Loaded from config on screen show. Shows "No optimization run yet" if no saved results.

3. **Auto-Apply Status** (bottom-left) — `StatusIndicator` + labels showing:
   - Enabled/Disabled toggle state
   - Strike count (0/3, 1/3, 2/3)
   - Last boot-apply result and timestamp
   - Driver version warning if applicable

4. **Boot Log** (bottom-right) — `LogViewer` showing last 10 boot-apply log entries from config. Each entry: timestamp, action, result.

Class: `DashboardScreen(ttk.Frame)` with:
- `__init__(self, parent, config: UserConfig, monitor: GPUMonitor)`
- `refresh(self)` — reload config and update all sections
- `_start_live_monitor(self)` — starts the 1-second polling loop
- `_stop_live_monitor(self)` — stops polling when screen is hidden

- [ ] **Step 2: Register screen in app.py**

Add import and registration in `GPUOptimizerApp.__init__()` after `_setup_layout()`.

- [ ] **Step 3: Test manually**

Launch app, verify dashboard shows GPU stats updating live.

- [ ] **Step 4: Commit**

```bash
git add src/gui/dashboard.py src/gui/app.py
git commit -m "feat: dashboard screen with live GPU stats and boot-apply status"
```

---

### Task 15: Create Optimization screen

**Files:**
- Create: `src/gui/optimization.py`

- [ ] **Step 1: Implement optimization screen**

Create `src/gui/optimization.py`:

The optimization screen has 3 states:

**State 1: Pre-optimization (risk selection)**
- Risk profile cards (safe/balanced/performance/extreme) with descriptions and limits
- Each card shows: label, description, core/mem/voltage limits
- Select button on each card
- Warning dialog after selection (for non-safe profiles) with confirm/cancel

**State 2: Running (live progress)**
- Current phase label (e.g., "Phase 2/5 — Core clock search")
- Overall progress bar
- `StepProgressList` showing each tested offset and result
- Live `GPUMetricsCard` showing current metrics
- Cancel button (applies best settings found so far)
- Runs `GPUOptimizer.run()` in a background thread to avoid freezing the GUI
- The progress callback (`_emit` in optimizer.py) is connected to update the GUI via `root.after()` thread-safe calls

**State 3: Complete**
- Shows summary of applied settings
- "View Results" button → switches to Results screen
- "Return to Dashboard" button

Class: `OptimizationScreen(ttk.Frame)` with:
- `__init__(self, parent, app_ref)` — reference to app for screen switching
- `_select_risk(self, risk: RiskLevel)` — shows warning, starts optimization
- `_run_optimization(self, risk: RiskLevel)` — launches optimizer in thread
- `_on_progress(self, phase, step, total, metrics)` — thread-safe GUI update
- `_on_complete(self, result)` — show completion state
- `_cancel(self)` — signals optimizer to stop (needs a cancel mechanism in optimizer)

**Important:** The optimizer currently has no cancel mechanism. Add a `self._cancelled = threading.Event()` to `GPUOptimizer` and check it at the start of each binary search iteration. If set, return best-so-far.

- [ ] **Step 2: Add cancel support to optimizer.py**

In `src/optimizer.py`:
- Add `self._cancel_event = threading.Event()` in `__init__`
- Add `def cancel(self): self._cancel_event.set()`
- In each binary search loop (`_binary_search_core`, `_binary_search_mem`, `_binary_search_voltage`), check `if self._cancel_event.is_set(): break` at the top of the while loop
- In `_tune_power_limit`, same check in the for loop

- [ ] **Step 3: Register screen in app.py**

- [ ] **Step 4: Test manually**

Launch app, navigate to Optimize, select Balanced. Verify progress updates appear and the optimization runs to completion (or cancel works).

- [ ] **Step 5: Commit**

```bash
git add src/gui/optimization.py src/optimizer.py src/gui/app.py
git commit -m "feat: optimization screen with live progress, cancel support"
```

---

### Task 16: Create Results screen

**Files:**
- Create: `src/gui/results.py`

- [ ] **Step 1: Implement results screen**

Create `src/gui/results.py`:

Displays the before/after comparison from the last optimization run.

Layout:
- **Header**: GPU name, risk level, timestamp
- **Comparison table** (ttk.Treeview or grid of labels):

  | Metric | Baseline | Achieved | Delta |
  |--------|----------|----------|-------|
  | Core Clock (under load) | 1800 MHz | 1920 MHz | +120 MHz |
  | Memory Clock | 7000 MHz | 7200 MHz | +200 MHz |
  | Temperature | 78 °C | 72 °C | -6 °C |
  | Power Draw | 175 W | 155 W | -20 W |

  Delta column: green for improvements, red for regressions.

- **Applied Offsets**: Core +X, Mem +X, Volt -X, Power X%
- **Stability Details**: test duration, correctness checks passed, stress backend, notes
- **Status badge**: "Stable" (green) or "Unstable — rolled back" (red)

Class: `ResultsScreen(ttk.Frame)` with:
- `__init__(self, parent)`
- `show_result(self, result: GPUOptimizationResult)` — populate all fields
- `_load_last_result(self)` — load from config if available

If no results exist yet, show a centered message: "No optimization results yet. Run an optimization from the Optimize tab."

- [ ] **Step 2: Register screen in app.py**

- [ ] **Step 3: Commit**

```bash
git add src/gui/results.py src/gui/app.py
git commit -m "feat: results screen with before/after comparison table"
```

---

### Task 17: Create Settings screen

**Files:**
- Create: `src/gui/settings_screen.py`

- [ ] **Step 1: Implement settings screen**

Create `src/gui/settings_screen.py`:

Layout:
- **Auto-Apply on Boot** section:
  - Toggle switch (ttk.Checkbutton) for enable/disable
  - When toggled on: registers Task Scheduler task via `src/scheduler.py`
  - When toggled off: unregisters task
  - Status label showing current Task Scheduler state

- **Boot-Apply Log** section:
  - `LogViewer` widget showing full boot log (last 20 entries)
  - "Clear Log" button

- **GPU Controls** section:
  - "Reset to Stock" button — calls backend.reset(), updates config, shows confirmation
  - Shows current GPU name and UUID

- **About** section:
  - App version
  - Python version
  - CUDA version (from nvidia-smi)
  - CuPy version and status
  - NVAPI status (available/unavailable)

Class: `SettingsScreen(ttk.Frame)` with:
- `__init__(self, parent, config: UserConfig)`
- `_toggle_auto_apply(self)` — register/unregister scheduler task
- `_reset_to_stock(self)` — reset GPU and update config
- `_clear_boot_log(self)` — clear boot log entries

- [ ] **Step 2: Register screen in app.py**

- [ ] **Step 3: Commit**

```bash
git add src/gui/settings_screen.py src/gui/app.py
git commit -m "feat: settings screen with auto-apply toggle, boot log viewer, reset"
```

---

### Task 18: Create system tray icon

**Files:**
- Create: `src/tray.py`

- [ ] **Step 1: Implement tray icon**

Create `src/tray.py`:

Uses `pystray` library for cross-platform (Windows) system tray.

```python
"""System tray icon with status display and context menu."""
import threading
from PIL import Image, ImageDraw

def _create_icon_image(color: str = "grey", size: int = 64) -> Image.Image:
    """Generate a simple colored circle icon."""
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    draw = ImageDraw.Draw(img)
    colors = {
        "green": (76, 175, 80),
        "yellow": (255, 193, 7),
        "red": (244, 67, 54),
        "grey": (158, 158, 158),
    }
    rgb = colors.get(color, colors["grey"])
    draw.ellipse([4, 4, size-4, size-4], fill=(*rgb, 255))
    return img
```

Class: `TrayIcon`:
- `__init__(self, on_open_gui: Callable, on_exit: Callable)`
- `start(self)` — creates pystray.Icon in a daemon thread
- `stop(self)` — stops the tray icon
- `set_status(self, color: str, tooltip: str)` — updates icon color and tooltip
- `show_notification(self, title: str, message: str)` — Windows toast notification via pystray's `notify()` method

Context menu items:
- "Status: {status_text}" (informational, greyed out)
- Separator
- "Open GPU Optimizer" → calls `on_open_gui()`
- "Reset to Stock" → calls reset logic
- "Disable Auto-Apply" / "Enable Auto-Apply" → toggle
- Separator
- "Exit" → calls `on_exit()`

- [ ] **Step 2: Commit**

```bash
git add src/tray.py
git commit -m "feat: system tray icon with status states and context menu"
```

---

### Task 19: Wire up tray icon with GUI app — minimize to tray

**Files:**
- Modify: `src/gui/app.py`
- Modify: `gpu_optimizer.py`

- [ ] **Step 1: Update app.py to support tray integration**

In `src/gui/app.py`:
- Add `self._tray = TrayIcon(on_open_gui=self._show_window, on_exit=self._quit)`
- Override window close (`root.protocol("WM_DELETE_WINDOW", self._hide_to_tray)`)
- `_hide_to_tray(self)` — `root.withdraw()` (hides window, tray stays)
- `_show_window(self)` — `root.deiconify()` + `root.lift()`
- `_quit(self)` — stops tray, destroys root
- Start tray icon in `run()` method
- Update tray status color based on config state (green if settings applied, etc.)

- [ ] **Step 2: Rewrite gpu_optimizer.py as GUI launcher**

Replace all CLI code in `gpu_optimizer.py` with:

```python
#!/usr/bin/env python3
"""GPU Optimizer — launch the GUI application."""
import sys
sys.path.insert(0, str(__import__("pathlib").Path(__file__).parent))

from src.gui.app import GPUOptimizerApp

def main():
    app = GPUOptimizerApp()
    app.run()

if __name__ == "__main__":
    main()
```

- [ ] **Step 3: Test manually**

Run: `python gpu_optimizer.py`
Expected: Window opens with tray icon. Closing window hides to tray. Right-click tray → "Open" shows window. "Exit" quits.

- [ ] **Step 4: Commit**

```bash
git add src/gui/app.py gpu_optimizer.py src/tray.py
git commit -m "feat: tray icon integration, window minimize to tray, GUI launcher"
```

---

## Phase 4: Distribution

### Task 20: Create smart installer

**Files:**
- Create: `installer.py` (project root)

- [ ] **Step 1: Implement installer**

Create `installer.py` at the project root (not inside `src/` — avoids copying itself into install dir):

This is a self-contained script that:

1. **Checks prerequisites**: Python version >= 3.8, nvidia-smi present, admin rights
2. **Detects CUDA version**: runs `nvidia-smi -q` and parses "CUDA Version" line
3. **Creates app directory**: `%LOCALAPPDATA%\GPUOptimizer\` with `app\`, `config\`, `logs\` subdirs
4. **Copies source files**: copies `src/`, `gpu_optimizer.py`, `requirements.txt` to `app\`
5. **Creates venv**: `python -m venv %LOCALAPPDATA%\GPUOptimizer\venv`
6. **Installs dependencies**: `pip install -r requirements.txt` + correct CuPy wheel for detected CUDA version
7. **Verifies CuPy works**: runs a quick CuPy probe (allocate small tensor, matmul, check result)
8. **Creates shortcuts**:
   - Desktop shortcut (.lnk) pointing to: `venv\Scripts\pythonw.exe app\gpu_optimizer.py`
   - Start Menu shortcut in `%APPDATA%\Microsoft\Windows\Start Menu\Programs\GPU Optimizer.lnk`
   - Uses `win32com.client` (pywin32) to create .lnk files
9. **Registers Task Scheduler task** for boot-apply (if user opts in during install)
10. **Prints summary**: install location, CuPy version, CUDA version, shortcut locations

Entry point: `if __name__ == "__main__"` with simple text-based prompts (the installer itself can be CLI since it runs once).

- [ ] **Step 2: Test manually**

Run: `python installer.py`
Expected: Creates directory structure, installs deps, creates shortcuts.

- [ ] **Step 3: Commit**

```bash
git add installer.py
git commit -m "feat: smart installer with CUDA detection, CuPy setup, shortcuts"
```

---

### Task 21: Update requirements.txt and final cleanup

**Files:**
- Modify: `requirements.txt`

- [ ] **Step 1: Update requirements.txt**

```
nvidia-ml-py>=12.0.0
psutil>=5.9.0
pywin32>=305; sys_platform == "win32"
sv-ttk>=2.0
pystray>=0.19.0
Pillow>=9.0.0
winotify>=1.0.0; sys_platform == "win32"
```

Note: `rich`, `click`, and `requests` are removed. CuPy is NOT in requirements.txt because it's CUDA-version-specific — the installer handles it.

- [ ] **Step 2: Run full test suite**

Run: `python -m pytest tests/ -v`
Expected: All tests pass

- [ ] **Step 3: Test full flow manually**

1. Run installer: `python installer.py`
2. Launch app from installed location
3. Verify dashboard shows GPU stats
4. Run a balanced optimization
5. Verify results screen shows before/after under load
6. Enable auto-apply in settings
7. Verify tray icon shows green status
8. Close window → verify tray icon persists
9. Reboot → verify boot-apply runs and applies settings

- [ ] **Step 4: Commit**

```bash
git add requirements.txt
git commit -m "chore: update dependencies, remove CLI deps, add GUI deps"
```

- [ ] **Step 5: Final commit — update README**

Update `README.md` to reflect:
- GUI-based workflow (not CLI)
- Installation via `python installer.py`
- System tray presence
- Auto-apply on boot feature
- Remove all CLI command references
- Remove bat file references

```bash
git add README.md
git commit -m "docs: update README for GUI-based workflow"
```
