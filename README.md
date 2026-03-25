# GPU Auto Optimizer

Automatically tune NVIDIA GPUs by combining overclocking, undervolting, and power-limit optimization with built-in stability testing.

## Features

- Automatic tuning with step-by-step stability checks and correctness verification
- Risk profiles: Safe, Balanced, Performance, Extreme
- Rollback on instability during tuning
- Before/after performance comparison under load
- System tray icon with persistent status
- Auto-apply settings on boot with 3-strike safety
- Modern GUI with Sun Valley dark theme

## Requirements

- Windows 10/11 (64-bit)
- Python 3.8+
- NVIDIA GPU with driver and nvidia-smi
- Administrator rights (recommended for full OC control)

## Installation

```bash
git clone <your-repo-url>
cd <repo-folder>
python installer.py
```

The installer will:
1. Detect your CUDA version
2. Set up a virtual environment
3. Install all dependencies including CuPy
4. Create desktop and Start Menu shortcuts
5. Optionally enable auto-apply on boot

## Usage

Launch via the desktop shortcut, Start Menu entry, or:

```bash
python gpu_optimizer.py
```

The app runs in the system tray. Close the window to minimize; right-click the tray icon for options.

### GUI Screens

- **Dashboard** — Live GPU stats, applied settings, auto-apply status, boot log
- **Optimize** — Select a risk profile and run the optimization
- **Results** — Before/after comparison under load
- **Settings** — Auto-apply toggle, boot log viewer, reset to stock

## Risk Profiles

| Profile | Core OC | Mem OC | Undervolt | Power | Risk |
|---------|---------|--------|-----------|-------|------|
| Safe | — | — | — | -20% to 0% | None |
| Balanced | +150 MHz | +800 MHz | -150 mV | -10% to +15% | Low |
| Performance | +250 MHz | +1500 MHz | -200 mV | -5% to +25% | Medium |
| Extreme | +400 MHz | +2000 MHz | -250 mV | 0% to +50% | High |

## How It Works

1. Detect GPU and select best control backend (NVAPI preferred)
2. Measure baseline performance under load
3. Binary-search for max stable core/memory/voltage offsets
4. Each step verified: settings applied, clocks confirmed, computation correctness checked
5. 5-minute final verification with thermal soak
6. Save and display results

## Safety

- Every overclock step is stability-tested with CuPy GPU computation
- Computation correctness verification detects memory instability
- Automatic rollback on any failure
- Boot-apply uses a 3-strike model: 3 consecutive failures disables auto-apply
- GPU hardware changes are detected and auto-apply is paused

## Project Structure

```
gpu_optimizer.py          — GUI launcher
installer.py              — Smart installer
src/
  config.py               — Risk profiles, settings, persistence
  detector.py             — GPU detection via pynvml
  monitor.py              — Real-time GPU monitoring
  stability.py            — CuPy stress testing with correctness verification
  optimizer.py            — Binary-search optimization pipeline
  boot_apply.py           — Headless boot-apply with 3-strike logic
  scheduler.py            — Windows Task Scheduler integration
  logging_config.py       — Log rotation and size management
  tray.py                 — System tray icon
  gui/
    app.py                — Main application window
    theme.py              — Sun Valley theme setup
    widgets.py            — Shared UI components
    dashboard.py          — Dashboard screen
    optimization.py       — Optimization screen
    results.py            — Results screen
    settings_screen.py    — Settings screen
  backends/
    base.py               — Backend interface
    nvapi.py              — NVAPI direct control (primary)
    nvidia_smi.py         — Power limit control via pynvml
```

## Safety Note

Overclocking and undervolting can reduce hardware lifespan and may affect warranty coverage. Use at your own risk.
