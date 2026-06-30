# GPU Auto Optimizer

Automatically tune NVIDIA GPUs by combining overclocking, power-limit optimization,
and (experimental) undervolting with built-in CUDA stability testing. Pick a profile,
press **Optimize**, and the tool searches for the best stable settings on its own.

## Quick start

```bat
setup.bat   :: one-time: builds .venv (Python 3.12) + installs CuPy/NVAPI deps
start.bat   :: launches the app (self-elevates for hardware control)
```

`setup.bat` creates a project-local `.venv` and installs everything from
`requirements.txt` (CuPy CUDA-12 + the `nvidia-*-cu12` runtime wheels — **no system
CUDA toolkit required**). `start.bat` runs the GUI via the venv's `pythonw.exe` and
requests administrator rights (needed for NVAPI clock/power control).

> `installer.py` (CUDA auto-detect + shortcuts + boot-apply) still works as an
> alternative, but the `setup.bat`/`start.bat` venv flow is the supported path.

## Requirements

- Windows 10/11 (64-bit)
- Python 3.12 (`py -3.12` launcher)
- NVIDIA GPU, recent driver — Pascal (GTX 10) or newer for full control
- Administrator rights (required for clock/voltage control via NVAPI)

## How control works

| Layer    | Backend            | What it does                                        |
|----------|--------------------|-----------------------------------------------------|
| Primary  | **NVAPI** (PState20)| Core + memory clock offsets, read-back verification |
| Power    | **pynvml/NVML**    | Power-limit cap (watts)                              |
| Fallback | **nvidia-smi**     | Power limit only, when NVAPI is unavailable          |
| Stress   | **CuPy**           | CUDA matmul load + result-correctness checks         |

> MSI Afterburner orchestration was evaluated and **abandoned** — editing AB profile
> `.cfg` files does not apply to hardware. Direct NVAPI is the control plane.

## Risk profiles

| Profile     | Core OC | Mem OC  | Power      | Undervolt† | Risk   |
|-------------|---------|---------|------------|-----------|--------|
| Safe        | —       | —       | -20% … 0%  | —         | None   |
| Balanced    | +150 MHz| +800 MHz| -10% … +15%| -150 mV   | Low    |
| Performance | +250 MHz| +1500 MHz| -5% … +25%| -200 mV   | Medium |
| Extreme     | +400 MHz| +2000 MHz| 0% … +50% | -250 mV   | High   |

† **Undervolt is currently gated OFF** (`optimizer.ENABLE_VF_CURVE_UNDERVOLT = False`).
The single-point V/F-curve *lock* hard-froze the test GPU (RTX 4070) even near stock
voltage, so Balanced/Performance currently apply core + memory OC + power only. A
gentler V/F **curve-reshape** undervolt (no hard lock, keeps dynamic boost) is the
next step — see *Undervolt status* below.

## How optimize works

1. Detect GPU, select best control backend (NVAPI preferred).
2. Measure baseline performance under CuPy load.
3. Binary-search the max stable core / memory offset, each step verified:
   settings applied → clocks confirmed via read-back → computation correctness checked.
4. Apply the power-limit target for the profile.
5. Final stability soak; save and display before/after results.

## Safety

- **Crash-safe journal** (`src/search_journal.py`): every risky apply is written to a
  write-ahead log (`begin → fsync → apply → complete`) *before* it touches hardware.
  If the PC freezes mid-apply, the next launch sees the un-completed entry and
  **blacklists that setting** so it is never retried.
- **Clock-hold gate**: rejects any undervolt point whose under-load clock collapses
  (prevents over-aggressive voltage cuts that tank performance).
- **Automatic rollback** on any failed stability step.
- **NVML handle recovery**: the monitor re-acquires its NVML handle on invalidation
  instead of crashing.
- **Volatile by design**: OC/UV settings clear on reboot. Boot-apply (optional) uses a
  3-strike model — 3 consecutive boot failures disables auto-apply.
- GPU hardware changes are detected and auto-apply is paused.

## Undervolt status

The high-value RTX 40-series tune is a **V/F curve reshape**: raise the curve so the
target frequency is reached at a lower voltage, flatten everything above that point to
cap voltage, and **do not** set a hard voltage lock — so the GPU keeps scaling
dynamically below the cap. This avoids the freeze caused by the older single-point lock
(`apply_vf_lock`). The reshape path (`apply_vf_reshape`) is implemented and unit-tested
but stays **gated off by default** until validated on hardware — flip
`ENABLE_VF_CURVE_UNDERVOLT = True` only for a deliberate, supervised live test.

## Project structure

```
gpu_optimizer.py          — GUI launcher
setup.bat / start.bat     — venv setup + elevated launch
installer.py              — alternative installer (CUDA detect + shortcuts)
src/
  config.py               — risk profiles, settings, persistence
  detector.py             — GPU detection via pynvml
  monitor.py              — real-time GPU monitoring (NVML-recovery hardened)
  stability.py            — CuPy stress testing with correctness verification
  optimizer.py            — binary-search optimization pipeline
  search_journal.py       — crash-safe write-ahead journal (freeze safety)
  boot_apply.py           — headless boot-apply with 3-strike logic
  scheduler.py            — Windows Task Scheduler integration
  tray.py                 — system tray icon
  gui/                    — Sun Valley dark-theme screens
  backends/
    base.py               — backend interface
    nvapi.py              — NVAPI PState20 control (primary)
    nvapi_vfcurve.py      — NVAPI V/F-curve undervolt (gated; reshape path)
    nvidia_smi.py         — power-limit fallback via pynvml
```

## Safety note

Overclocking and undervolting can reduce hardware lifespan and may affect warranty
coverage. Settings are volatile (cleared on reboot). Use at your own risk.
