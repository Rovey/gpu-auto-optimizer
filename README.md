# GPU Auto Optimizer

Automatically tune NVIDIA GPUs by combining overclocking, undervolting, and power-limit optimization with built-in stability testing.

This project is for users who want better performance-per-watt without manually tweaking dozens of settings.

## What You Get

- Automatic tuning with step-by-step stability checks
- Risk profiles (`safe`, `balanced`, `performance`, `extreme`)
- Rollback on instability during tuning
- Per-GPU result summary with final offsets and status
- Live monitor mode (`--monitor`) and reset mode (`--reset`)

## Who This Is For

- Windows users with NVIDIA GTX/RTX GPUs
- People who want practical gains without manual trial-and-error
- Builders who prefer a CLI workflow and clear telemetry

## Quick Start (Visitor Path)

### 1. Clone and install

```bat
git clone <your-repo-url>
cd 22_auto-optimize-gpu
install.bat
```

### 2. Run recommended profile

```bat
run_balanced.bat
```

### 3. Check results

At the end of a successful run, you should see:

- `Results: <Your GPU Name>`
- `Applied Settings` with core/memory/voltage/power values
- `Optimization complete!`

## Core Commands

```bat
python gpu_optimizer.py                 # interactive menu
python gpu_optimizer.py --risk balanced # direct risk selection
python gpu_optimizer.py --gpu 0 --risk balanced
python gpu_optimizer.py --list
python gpu_optimizer.py --backends
python gpu_optimizer.py --monitor
python gpu_optimizer.py --reset
```

Convenience scripts:

```bat
run_balanced.bat
run_safe.bat
run_performance.bat
run_monitor.bat
run_reset.bat
```

## Risk Profiles

### Safe

- Focus: lower temperatures and power draw
- Typical effect: less heat, quieter operation, minimal risk

### Balanced (recommended)

- Focus: daily-driver performance-per-watt
- Typical effect: moderate performance gain with reduced thermals/power

### Performance

- Focus: higher performance targets
- Expect more instability during the search phase (tool auto-handles retries/rollback)

### Extreme

- Focus: maximum tuning headroom
- Highest risk profile; not recommended for most users

## Requirements

- Windows 10/11 (64-bit)
- Python 3.8+
- NVIDIA driver with `nvidia-smi`
- Administrator rights recommended for full control capabilities

Optional tools (auto-detected when installed):

- MSI Afterburner
- FurMark / Heaven
- CuPy + CUDA runtime packages

## How It Works (Short Version)

1. Detect GPU(s)
2. Pick best available control backend
3. Measure baseline
4. Run binary-search tuning for core/memory/voltage (by profile)
5. Tune power limit
6. Run final verification
7. Save and show best stable settings

The optimizer only accepts runs with sustained stress load quality (high average and peak GPU utilization) to avoid false-positive "stable" results.

## Project Structure

```text
gpu_optimizer.py
src/
  config.py
  detector.py
  monitor.py
  stability.py
  optimizer.py
  ui.py
  backends/
```

## Safety Note

Overclocking/undervolting can reduce hardware lifespan and may affect warranty coverage. Use this tool at your own risk.

