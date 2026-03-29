# Code Review Fixes Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix all 17 issues identified by the comprehensive code review, grouped into 6 independent workstreams that can be executed in parallel.

**Architecture:** Each workstream touches a disjoint set of files, so all 6 can run concurrently without merge conflicts. Changes are surgical fixes — no refactoring beyond what's needed.

**Tech Stack:** Python 3.8+, pynvml, CuPy, Tkinter, pytest

---

## File Map

| Workstream | Files Modified | Files Created |
|---|---|---|
| A: Stability & Monitor | `src/stability.py`, `src/monitor.py` | — |
| B: Backends & Detection | `src/backends/nvapi.py`, `src/backends/nvidia_smi.py`, `src/detector.py` | — |
| C: Optimizer | `src/optimizer.py` | — |
| D: Config & Boot-Apply | `src/config.py`, `src/boot_apply.py`, `gpu_optimizer.py` | — |
| E: GUI | `src/gui/optimization.py`, `src/gui/dashboard.py`, `src/gui/app.py` | — |
| F: Misc | `requirements.txt`, `installer.py` | — |

---

### Task A: Stability & Monitor Fixes

**Files:**
- Modify: `src/stability.py:308-331`
- Modify: `src/monitor.py:256-329`

**Issues:** #1 (buffer mutation), #3 (monitor in hot loop), #5 (sample mutation)

- [ ] **Step 1: Fix stress buffer mutation in stability.py**

In `src/stability.py`, lines 310-311, the second matmul overwrites buffer `b`. Change to use `c` as output for both, preserving `a` and `b`:

```python
# BEFORE (line 310-311):
cp.matmul(a, b, out=c)
cp.matmul(c, a, out=b)

# AFTER:
cp.matmul(a, b, out=c)
cp.matmul(b, a, out=c)
```

- [ ] **Step 2: Fix monitor creation in hot loop in stability.py**

In `src/stability.py`, the `_stress_cupy` method creates a new `GPUMonitor` on every progress callback tick (lines 328-330). Create the monitor once before the loop and reuse it:

```python
# BEFORE (inside the while loop, lines 328-330):
if self._cb:
    mon = GPUMonitor(self._idx)
    self._cb(elapsed, duration_sec, mon.read_once())

# AFTER (add before the while loop, after line 307):
cb_monitor = GPUMonitor(self._idx) if self._cb else None

# Then inside the while loop replace lines 328-330 with:
if self._cb and cb_monitor:
    self._cb(elapsed, duration_sec, cb_monitor.read_once())
```

- [ ] **Step 3: Fix sample_average mutating last sample in monitor.py**

In `src/monitor.py`, function `sample_average` (lines 256-283), replace the mutation of `ref` with creating a new `GPUMetrics` instance:

```python
# BEFORE (lines 272-283):
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

# AFTER:
return GPUMetrics(
    gpu_index=samples[-1].gpu_index,
    timestamp=samples[-1].timestamp,
    core_clock_mhz=int(_avg("core_clock_mhz")),
    mem_clock_mhz=int(_avg("mem_clock_mhz")),
    temp_c=_avg("temp_c"),
    power_w=_avg("power_w"),
    power_limit_w=samples[-1].power_limit_w,
    fan_speed_pct=int(_avg("fan_speed_pct")),
    gpu_util_pct=int(_avg("gpu_util_pct")),
    mem_util_pct=int(_avg("mem_util_pct")),
    mem_used_mb=samples[-1].mem_used_mb,
    mem_total_mb=samples[-1].mem_total_mb,
    ecc_errors=max(s.ecc_errors for s in samples),
    is_throttling=any(s.is_throttling for s in samples),
    is_thermal_limit=any(s.is_thermal_limit for s in samples),
    is_power_limit=any(s.is_power_limit for s in samples),
)
```

- [ ] **Step 4: Fix sample_average_under_load mutating last sample in monitor.py**

In `src/monitor.py`, function `sample_average_under_load` (lines 290-329), apply same fix:

```python
# BEFORE (lines 317-329):
ref = samples[-1]
ref.core_clock_mhz = int(_avg("core_clock_mhz"))
ref.mem_clock_mhz = int(_avg("mem_clock_mhz"))
ref.temp_c = _avg("temp_c")
ref.power_w = _avg("power_w")
ref.fan_speed_pct = int(_avg("fan_speed_pct"))
ref.gpu_util_pct = int(_avg("gpu_util_pct"))
ref.ecc_errors = max(s.ecc_errors for s in samples)
ref.is_throttling = any(s.is_throttling for s in samples)
ref.is_thermal_limit = any(s.is_thermal_limit for s in samples)
ref.is_power_limit = any(s.is_power_limit for s in samples)
ref.samples_used = len(qualifying)
return ref

# AFTER:
return GPUMetrics(
    gpu_index=samples[-1].gpu_index,
    timestamp=samples[-1].timestamp,
    core_clock_mhz=int(_avg("core_clock_mhz")),
    mem_clock_mhz=int(_avg("mem_clock_mhz")),
    temp_c=_avg("temp_c"),
    power_w=_avg("power_w"),
    power_limit_w=samples[-1].power_limit_w,
    fan_speed_pct=int(_avg("fan_speed_pct")),
    gpu_util_pct=int(_avg("gpu_util_pct")),
    mem_util_pct=int(_avg("mem_util_pct")),
    mem_used_mb=samples[-1].mem_used_mb,
    mem_total_mb=samples[-1].mem_total_mb,
    ecc_errors=max(s.ecc_errors for s in samples),
    is_throttling=any(s.is_throttling for s in samples),
    is_thermal_limit=any(s.is_thermal_limit for s in samples),
    is_power_limit=any(s.is_power_limit for s in samples),
    samples_used=len(qualifying),
)
```

- [ ] **Step 5: Run tests**

Run: `pytest tests/test_stability.py tests/test_monitor.py -v`
Expected: All tests PASS

- [ ] **Step 6: Commit**

```bash
git add src/stability.py src/monitor.py
git commit -m "fix: stability buffer mutation, monitor hot-loop creation, sample mutation"
```

---

### Task B: Backends & Detection Fixes

**Files:**
- Modify: `src/backends/nvapi.py:288-354`
- Modify: `src/backends/nvidia_smi.py:56-108`
- Modify: `src/detector.py:22-42`

**Issues:** #2 (pynvml init leak), #6 (thermal limit docs), #7 (fragile arch detection), #11 (nvml shutdown), #13b (unused `_NVIDIA_ARCH_MAP`)

- [ ] **Step 1: Add pynvml shutdown to nvapi.py apply method**

In `src/backends/nvapi.py`, the `apply()` method calls `pynvml.nvmlInit()` at line 305 but never shuts down. Wrap in try/finally:

```python
# BEFORE (lines 302-314):
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

# AFTER:
try:
    import pynvml
    pynvml.nvmlInit()
    try:
        h         = pynvml.nvmlDeviceGetHandleByIndex(gpu_index)
        default_mw = pynvml.nvmlDeviceGetPowerManagementDefaultLimit(h)
        min_mw, max_mw = pynvml.nvmlDeviceGetPowerManagementLimitConstraints(h)
        target_mw = int(default_mw * power_limit_pct / 100)
        target_mw = max(min_mw, min(max_mw, target_mw))
        pynvml.nvmlDeviceSetPowerManagementLimit(h, target_mw)
        notes.append(f"Power limit → {target_mw // 1000} W")
    finally:
        pynvml.nvmlShutdown()
except Exception as e:
    notes.append(f"Power limit skipped ({e})")
```

- [ ] **Step 2: Add pynvml shutdown to nvidia_smi.py apply and reset methods**

In `src/backends/nvidia_smi.py`, `apply()` method (line 71) and `reset()` method (line 100) both call `pynvml.nvmlInit()` without shutdown. Add try/finally to both:

For `apply()` (lines 70-85):
```python
# BEFORE:
try:
    pynvml.nvmlInit()
    h = pynvml.nvmlDeviceGetHandleByIndex(gpu_index)
    ...
    notes.append(f"Power limit set to {target_mw // 1000} W via pynvml")
except Exception as exc:
    notes.append(f"pynvml power limit failed: {exc}")
    applied.success = False

# AFTER:
try:
    pynvml.nvmlInit()
    try:
        h = pynvml.nvmlDeviceGetHandleByIndex(gpu_index)
        ...
        notes.append(f"Power limit set to {target_mw // 1000} W via pynvml")
    finally:
        pynvml.nvmlShutdown()
except Exception as exc:
    notes.append(f"pynvml power limit failed: {exc}")
    applied.success = False
```

For `reset()` (lines 99-107):
```python
# BEFORE:
try:
    pynvml.nvmlInit()
    h           = pynvml.nvmlDeviceGetHandleByIndex(gpu_index)
    default_mw  = pynvml.nvmlDeviceGetPowerManagementDefaultLimit(h)
    pynvml.nvmlDeviceSetPowerManagementLimit(h, default_mw)
    return True
except Exception:
    pass

# AFTER:
try:
    pynvml.nvmlInit()
    try:
        h           = pynvml.nvmlDeviceGetHandleByIndex(gpu_index)
        default_mw  = pynvml.nvmlDeviceGetPowerManagementDefaultLimit(h)
        pynvml.nvmlDeviceSetPowerManagementLimit(h, default_mw)
        return True
    finally:
        pynvml.nvmlShutdown()
except Exception:
    pass
```

- [ ] **Step 3: Add comment documenting thermal limit is software-only**

In `src/backends/nvapi.py`, add a comment at line 295 (inside `apply()`):

```python
# NOTE: thermal_limit_c is tracked in AppliedSettings but not applied to
# hardware via NVAPI. It is enforced as a software ceiling by the
# StabilityTester during optimization. Hardware thermal limits are
# managed by the GPU's own firmware.
```

Also add same comment in `src/backends/nvidia_smi.py` at line 91:

```python
# NOTE: thermal_limit_c is tracked but not applied to hardware.
# Enforced as a software ceiling by StabilityTester during optimization.
applied.thermal_limit_c   = thermal_limit_c
```

- [ ] **Step 4: Fix fragile architecture detection and remove unused map**

In `src/detector.py`, replace `_NVIDIA_ARCH_MAP` (lines 23-25) and `_infer_nvidia_arch` (lines 27-42):

```python
# BEFORE (lines 22-42):
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

# AFTER:
def _infer_nvidia_arch(name: str) -> str:
    """Best-effort architecture detection from GPU name string."""
    name_up = name.upper()
    if any(x in name_up for x in ("RTX 50", "5090", "5080", "5070", "5060")):
        return "Blackwell"
    if any(x in name_up for x in ("RTX 40", "4090", "4080", "4070", "4060", "4050")):
        return "Ada Lovelace"
    if any(x in name_up for x in ("RTX 30", "3090", "3080", "3070", "3060", "3050")):
        return "Ampere"
    if any(x in name_up for x in ("RTX 20", "2080", "2070", "2060")):
        return "Turing"
    if any(x in name_up for x in ("GTX 10", "1080", "1070", "1060", "1050")):
        return "Pascal"
    if any(x in name_up for x in ("GTX 9", "980", "970", "960", "950")):
        return "Maxwell"
    if "TITAN" in name_up:
        return "Ampere/Turing/Pascal"
    return "Unknown"
```

Key changes:
- Removed unused `_NVIDIA_ARCH_MAP` dict
- Checks prefixed patterns first (`"RTX 50"` before `"5090"`) so family match is tried before model number
- Added Blackwell (RTX 50 series) support
- Removed bare `"40"` match that could false-positive
- Added `supports_uv` for Blackwell in both `_detect_nvidia()` (line 139) and `_detect_via_nvidia_smi()` (line 196):

```python
# BEFORE:
supports_uv=arch in ("Turing", "Ampere", "Ada Lovelace"),

# AFTER:
supports_uv=arch in ("Turing", "Ampere", "Ada Lovelace", "Blackwell"),
```

- [ ] **Step 5: Run tests**

Run: `pytest tests/test_nvapi.py -v`
Expected: All tests PASS

- [ ] **Step 6: Commit**

```bash
git add src/backends/nvapi.py src/backends/nvidia_smi.py src/detector.py
git commit -m "fix: pynvml shutdown leaks, fragile arch detection, add Blackwell support"
```

---

### Task C: Optimizer Fix

**Files:**
- Modify: `src/optimizer.py:434-471`

**Issue:** #4 (useless power limit loop)

- [ ] **Step 1: Fix the useless power limit warmup loop**

In `src/optimizer.py`, `_tune_power_limit` method (lines 434-471). The loop at lines 437-441 iterates setting power limits with no stability test, only the final value persists. Replace with a single set:

```python
# BEFORE (lines 436-441):
min_delta, max_delta = self._profile["power_limit_delta_pct"]
# First try raising slightly for overclock headroom
for delta in range(max_delta, -1, -5):
    self._power_limit_pct = 100 + delta
    self._apply()
    time.sleep(0.5)

# AFTER:
min_delta, max_delta = self._profile["power_limit_delta_pct"]
# Start at elevated power limit for overclock headroom
self._power_limit_pct = 100 + max_delta
self._apply()
time.sleep(0.5)
```

- [ ] **Step 2: Run tests**

Run: `pytest tests/test_optimizer.py -v`
Expected: All tests PASS

- [ ] **Step 3: Commit**

```bash
git add src/optimizer.py
git commit -m "fix: replace useless power limit warmup loop with single set"
```

---

### Task D: Config & Boot-Apply Fixes

**Files:**
- Modify: `src/config.py:170-172, 196-221, 246-253`
- Modify: `src/boot_apply.py` (add entry point)
- Modify: `gpu_optimizer.py` (call migrate)

**Issues:** #8 (silent error swallowing), #10 (empty LOCALAPPDATA), #13a (migrate never called), #17 (missing boot-apply entry point)

- [ ] **Step 1: Fix silent error swallowing in load_config**

In `src/config.py`, `load_config` function (line 219), replace the bare `except Exception: pass` with logging:

```python
# BEFORE (lines 218-220):
        except Exception:
            pass
    return UserConfig()

# AFTER:
        except json.JSONDecodeError as exc:
            import logging
            logging.getLogger(__name__).warning(
                "Config file %s is corrupted (JSON parse error: %s). Using defaults.",
                path, exc,
            )
        except Exception as exc:
            import logging
            logging.getLogger(__name__).warning(
                "Failed to load config from %s: %s. Using defaults.",
                path, exc,
            )
    return UserConfig()
```

- [ ] **Step 2: Fix get_app_dir empty LOCALAPPDATA fallback**

In `src/config.py`, `get_app_dir` function (line 172):

```python
# BEFORE:
def get_app_dir() -> str:
    """Return the root application data directory (%LOCALAPPDATA%/GPUOptimizer)."""
    return os.path.join(os.environ.get("LOCALAPPDATA", ""), "GPUOptimizer")

# AFTER:
def get_app_dir() -> str:
    """Return the root application data directory (%LOCALAPPDATA%/GPUOptimizer)."""
    local_app = os.environ.get("LOCALAPPDATA")
    if not local_app:
        local_app = os.path.join(os.path.expanduser("~"), "AppData", "Local")
    return os.path.join(local_app, "GPUOptimizer")
```

- [ ] **Step 3: Call migrate_config_if_needed in gpu_optimizer.py**

In `gpu_optimizer.py`, add the migration call before launching the app:

```python
# BEFORE (lines 28-34):
def main():
    if not _is_admin():
        _request_admin_and_relaunch()

    from src.gui.app import GPUOptimizerApp
    app = GPUOptimizerApp()
    app.run()

# AFTER:
def main():
    if not _is_admin():
        _request_admin_and_relaunch()

    from src.config import migrate_config_if_needed
    migrate_config_if_needed()

    from src.gui.app import GPUOptimizerApp
    app = GPUOptimizerApp()
    app.run()
```

- [ ] **Step 4: Add boot-apply entry point to boot_apply.py**

In `src/boot_apply.py`, add the actual apply-on-boot logic after the existing `record_boot_result` function:

```python
def _apply_on_boot() -> None:
    """Entry point for headless boot-apply. Intended to run via Task Scheduler."""
    import logging
    import sys

    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s [boot_apply] %(message)s",
    )
    log = logging.getLogger(__name__)

    cfg = load_config()

    # Detect current GPU
    from .detector import detect_gpus
    gpus = detect_gpus()
    if not gpus:
        log.warning("No GPUs detected. Skipping boot-apply.")
        record_boot_result(cfg, False, "No GPUs detected")
        save_config(cfg)
        return

    gpu = gpus[0]
    decision = should_apply(cfg, current_gpu_uuid=gpu.uuid, current_driver=gpu.driver_version)

    if decision.skip:
        log.info("Skipping boot-apply: %s", decision.reason)
        return

    if decision.warning:
        log.warning("Boot-apply warning: %s", decision.warning)

    # Find the saved result for this GPU
    saved = cfg.per_gpu_results.get(gpu.name)
    if not saved:
        log.warning("No saved result for GPU '%s'. Skipping.", gpu.name)
        record_boot_result(cfg, False, f"No saved result for {gpu.name}")
        save_config(cfg)
        return

    # Apply via best available backend
    from .optimizer import _best_backend
    backend = _best_backend(gpu)

    try:
        result = backend.apply(
            gpu_index=gpu.index,
            core_offset_mhz=saved.get("core_offset_mhz", 0),
            mem_offset_mhz=saved.get("mem_offset_mhz", 0),
            voltage_offset_mv=saved.get("voltage_offset_mv", 0),
            power_limit_pct=saved.get("power_limit_pct", 100),
            thermal_limit_c=saved.get("thermal_limit_c", 83),
        )
        if result.success:
            log.info("Boot-apply succeeded: %s", result.notes)
            record_boot_result(cfg, True, result.notes)
        else:
            log.error("Boot-apply failed: %s", result.notes)
            record_boot_result(cfg, False, result.notes)
    except Exception as exc:
        log.error("Boot-apply exception: %s", exc)
        record_boot_result(cfg, False, str(exc))

    # Update GPU UUID and driver for future checks
    cfg.boot_apply.gpu_uuid = gpu.uuid
    cfg.boot_apply.driver_version = gpu.driver_version
    save_config(cfg)


if __name__ == "__main__":
    import sys
    from pathlib import Path
    # Ensure project root is importable
    sys.path.insert(0, str(Path(__file__).parent.parent))
    _apply_on_boot()
```

- [ ] **Step 5: Run tests**

Run: `pytest tests/test_config.py tests/test_boot_apply.py -v`
Expected: All tests PASS

- [ ] **Step 6: Commit**

```bash
git add src/config.py src/boot_apply.py gpu_optimizer.py
git commit -m "fix: config error logging, LOCALAPPDATA fallback, migration call, boot-apply entry point"
```

---

### Task E: GUI Fixes

**Files:**
- Modify: `src/gui/optimization.py:64-89`
- Modify: `src/gui/dashboard.py:68-80`
- Modify: `src/gui/app.py:33-35, 140-155`

**Issues:** #9 (race condition + lost traceback), #18 (dashboard last result), #19 (hardcoded GPU index)

- [ ] **Step 1: Fix race condition and lost traceback in optimization.py**

In `src/gui/optimization.py`, the `_run` inner function (lines 68-86). Wrap `self.after()` in try/except and log full traceback:

```python
# BEFORE (lines 68-86):
def _run():
    try:
        from src.detector import detect_gpus
        from src.optimizer import GPUOptimizer
        from src.config import save_config, load_config, save_result

        gpus = detect_gpus()
        if not gpus:
            self.after(0, lambda: self._on_error("No NVIDIA GPUs detected."))
            return

        gpu = gpus[0]  # optimize first GPU
        self._optimizer = GPUOptimizer(gpu, risk_level, self._on_progress)
        result = self._optimizer.run()
        cfg = load_config()
        save_result(cfg, result)
        self.after(0, lambda: self._on_complete(result))
    except Exception as exc:
        self.after(0, lambda e=str(exc): self._on_error(e))

# AFTER:
def _run():
    try:
        from src.detector import detect_gpus
        from src.optimizer import GPUOptimizer
        from src.config import save_config, load_config, save_result

        gpus = detect_gpus()
        if not gpus:
            try:
                self.after(0, lambda: self._on_error("No NVIDIA GPUs detected."))
            except RuntimeError:
                pass
            return

        gpu = gpus[0]  # optimize first GPU
        self._optimizer = GPUOptimizer(gpu, risk_level, self._on_progress)
        result = self._optimizer.run()
        cfg = load_config()
        save_result(cfg, result)
        try:
            self.after(0, lambda: self._on_complete(result))
        except RuntimeError:
            pass
    except Exception as exc:
        import logging
        import traceback
        logging.getLogger(__name__).error(
            "Optimization failed:\n%s", traceback.format_exc()
        )
        try:
            self.after(0, lambda e=str(exc): self._on_error(e))
        except RuntimeError:
            pass
```

- [ ] **Step 2: Fix dashboard "last result" ordering**

In `src/gui/dashboard.py`, `refresh` method (line 74). Instead of relying on dict insertion order, find the most recent result by checking all entries:

```python
# BEFORE (line 74):
last_result = list(cfg.per_gpu_results.values())[-1]

# AFTER:
# Use the first GPU's result if available, otherwise take any result
results = cfg.per_gpu_results
if self._monitor is not None:
    gpu_idx = self._monitor._index
    # Try to find a result matching the current GPU by index
    last_result = None
    for val in results.values():
        if isinstance(val, dict) and val.get("gpu_index") == gpu_idx:
            last_result = val
            break
    if last_result is None:
        last_result = list(results.values())[-1]
else:
    last_result = list(results.values())[-1]
```

- [ ] **Step 3: Fix hardcoded GPU index 0 in app.py tray reset**

In `src/gui/app.py`, `_reset_to_stock` method (lines 140-155). Use detected GPU index instead of hardcoded `0`:

```python
# BEFORE (lines 140-155):
def _reset_to_stock(self) -> None:
    """Reset GPU from tray menu."""
    # Import here to avoid circular imports
    from ..backends.nvapi import NVAPIBackend
    from ..backends.nvidia_smi import NvidiaSMIBackend

    backends = [NVAPIBackend(), NvidiaSMIBackend()]
    for b in sorted(backends, key=lambda x: -x.priority):
        if b.is_available() and b.reset(0):
            self._tray.show_notification(
                "GPU Optimizer", f"GPU reset via {b.name}"
            )
            return
    self._tray.show_notification(
        "GPU Optimizer", "Reset failed -- no backend available"
    )

# AFTER:
def _reset_to_stock(self) -> None:
    """Reset GPU from tray menu."""
    from ..backends.nvapi import NVAPIBackend
    from ..backends.nvidia_smi import NvidiaSMIBackend

    gpu_index = self._monitor._index if self._monitor else 0
    backends = [NVAPIBackend(), NvidiaSMIBackend()]
    for b in sorted(backends, key=lambda x: -x.priority):
        if b.is_available() and b.reset(gpu_index):
            self._tray.show_notification(
                "GPU Optimizer", f"GPU reset via {b.name}"
            )
            return
    self._tray.show_notification(
        "GPU Optimizer", "Reset failed -- no backend available"
    )
```

- [ ] **Step 4: Commit**

```bash
git add src/gui/optimization.py src/gui/dashboard.py src/gui/app.py
git commit -m "fix: GUI race condition, dashboard result ordering, hardcoded GPU index"
```

---

### Task F: Misc Fixes

**Files:**
- Modify: `requirements.txt`
- Modify: `installer.py:177-185`

**Issues:** #16 (CUDA 13 forward-compat), #20 (missing numpy)

- [ ] **Step 1: Add numpy to requirements.txt**

```
# BEFORE:
nvidia-ml-py>=12.0.0
psutil>=5.9.0
pywin32>=305; sys_platform == "win32"
sv-ttk>=2.0
pystray>=0.19.0
Pillow>=9.0.0
winotify>=1.0.0; sys_platform == "win32"

# AFTER:
nvidia-ml-py>=12.0.0
numpy>=1.24.0
psutil>=5.9.0
pywin32>=305; sys_platform == "win32"
sv-ttk>=2.0
pystray>=0.19.0
Pillow>=9.0.0
winotify>=1.0.0; sys_platform == "win32"
```

- [ ] **Step 2: Fix installer CUDA version mapping**

In `installer.py`, function `_cupy_package_for_cuda` (lines 177-185). Remove the speculative CUDA 13 entry and add a warning for unknown versions:

```python
# BEFORE (lines 177-185):
def _cupy_package_for_cuda(cuda_major: str | None) -> str | None:
    """Return the correct CuPy pip package name for the CUDA version."""
    if cuda_major == "13":
        return "cupy-cuda13x"
    elif cuda_major == "12":
        return "cupy-cuda12x"
    elif cuda_major == "11":
        return "cupy-cuda11x"
    return None

# AFTER:
def _cupy_package_for_cuda(cuda_major: str | None) -> str | None:
    """Return the correct CuPy pip package name for the CUDA version."""
    mapping = {
        "12": "cupy-cuda12x",
        "11": "cupy-cuda11x",
    }
    return mapping.get(cuda_major)
```

- [ ] **Step 3: Commit**

```bash
git add requirements.txt installer.py
git commit -m "fix: add numpy to requirements, remove speculative CUDA 13 mapping"
```

---

## Verification

After all workstreams complete:

- [ ] Run full test suite: `pytest tests/ -v`
- [ ] Verify no regressions: all 37 tests pass
- [ ] Final commit if any merge needed
