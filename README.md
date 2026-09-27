<div align="center">

# GPU Auto Optimizer

[![CI](https://github.com/Rovey/gpu-auto-optimizer/actions/workflows/ci.yml/badge.svg)](https://github.com/Rovey/gpu-auto-optimizer/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/github/license/Rovey/gpu-auto-optimizer?color=blue)](LICENSE)
![Windows 10/11](https://img.shields.io/badge/Windows-10%20%7C%2011-0078D4)
![NVIDIA](https://img.shields.io/badge/NVIDIA-Pascal%2B-76B900?logo=nvidia&logoColor=white)

**A native C++ NVIDIA GPU tuning tool, currently mid-rewrite.**

</div>

> [!IMPORTANT]
> `main` is being rewritten from Python to C++ (see [the design spec](docs/superpowers/specs/2026-09-20-cpp-rewrite-design.md)). What's here today is the repository skeleton, the hardware layer and the stress load: it can read live telemetry, write clock offsets and fan speed verified against the driver, and run a DX11 load that checks every value it computes and ends in a stability verdict. It can also tune: `--optimize` searches power limit, core and memory offsets against that verdict, saves the result, and `--boot on` re-applies it at every logon. `GpuAutoOptimizer.exe` is the window and tray app on top of the same engine; its tray process keeps the tune applied for the whole session. v1.0 will be the finished C++ application. The previous, working Python version (search engine, risk profiles, tray GUI) is preserved at the `v0.9-python` git tag and still works as it always did; it just isn't what lives on `main` anymore.

> [!WARNING]
> The commands below that write to the GPU (`--set-core`, `--set-mem`, `--set-fan`, `--reset`, `--optimize`, `--apply`, `--boot`) change clocks, power limit and fan behavior directly through the driver. Every write is verified by reading the value back rather than trusting the driver's return code, but a wrong offset can still make a card unstable. Use at your own risk.

## Quick start

### From a release

Download `GpuAutoOptimizer-<version>-win-x64.zip` from [Releases](https://github.com/Rovey/gpu-auto-optimizer/releases), check it and unzip it anywhere:

```powershell
(Get-FileHash .\GpuAutoOptimizer-<version>-win-x64.zip -Algorithm SHA256).Hash   # compare with the .sha256 file
gh attestation verify .\GpuAutoOptimizer-<version>-win-x64.zip --repo Rovey/gpu-auto-optimizer   # optional: proves CI built it from this repo
```

The executables are not code-signed, so Windows SmartScreen may say it "protected your PC" on the first start: choose **More info**, then **Run anyway**. Turning on apply-at-logon copies both executables to `%ProgramFiles%\GpuAutoOptimizer\`, so the unzipped folder can be deleted afterwards.

### From source

Requires an x64 Windows machine with Visual Studio 2026 (C++ and CMake workload), whose bundled CMake (4.2 or newer) knows the VS 2026 generator. From a Developer PowerShell, in the repo root:

```powershell
cmake --workflow --preset ci    # configure, build Release, run the tests
```

That builds two programs in `.\build\Release\`: `GpuAutoOptimizer.exe`, the window and tray app, and `gao.exe`, the command-line tool. Both use the same engine and the same saved state.

### The app

Start `GpuAutoOptimizer.exe`. The window shows the GPU, live telemetry, the saved tune and what is applied right now; pick a preset and press **Optimize** (it offers to restart as administrator first, because tuning writes clocks and power limits). The run screen shows temperature, power and every probe as it happens; **Abort** restores stock. The results screen compares before and after and can turn on **apply at every logon**.

With apply-at-logon on, Windows starts the app in the tray at every logon. It applies the saved tune and then keeps it applied: every 30 seconds it reads back what the driver reports. If the tune was reset (a driver reset or TDR), it applies it again; if that happens four times within an hour it stops and tells you the tune is probably not stable; if another program such as MSI Afterburner changed the settings, it leaves them alone and tells you; after a driver update it does not apply the old tune and tells you to optimize again. While it keeps a tune applied (or a run is in progress), closing the window keeps it in the tray and **Exit** in the tray menu quits; otherwise closing the window quits. **Revert to stock** in the tray menu, or `gao --reset` from a shell, puts the card at stock and the watchdog leaves it there until you apply the tune again (window, tray menu or `gao --apply`). Only one optimize runs at a time, from the window or the command line.

### The command line

```powershell
.\build\Release\gao.exe --version
.\build\Release\gao.exe --probe
.\build\Release\gao.exe --stress 60
.\build\Release\gao.exe --bandwidth
.\build\Release\gao.exe --optimize best   # elevated
.\build\Release\gao.exe --apply           # elevated: re-apply the saved result
.\build\Release\gao.exe --boot on         # elevated: re-apply at every logon
.\build\Release\gao.exe --status
```

`--optimize` runs baseline → power → core → memory → 60 s soak (~10 minutes), leaves the result applied until reboot (exit code 0; 2 if it was applied but could not be saved), and keeps a crash journal so a setting that froze the machine is never tried again. Ctrl+C restores stock. The core and memory edges it finds are each re-checked with a 30 s probe before the safety margin is applied, and the memory search stops at the **bandwidth peak** rather than at the highest offset that does not produce errors: GDDR6X and GDDR6 retry failed transfers, so an excessive memory clock costs speed long before it produces a wrong result. `--bandwidth` prints the current memory bandwidth.

All state (`gao.json`, the crash journal, `boot.log`) lives in `%ProgramData%\GpuAutoOptimizer`, which users can read but only administrators can write. `--boot on` copies both executables to `%ProgramFiles%\GpuAutoOptimizer\` and registers a logon task (`\GpuAutoOptimizer\BootApply`) that starts that copy of the app in the tray, which re-applies the saved profile and keeps it applied; `--status` says whether the installed copies match your build; if the machine crashes within 2 minutes of three logons in a row, or the NVIDIA driver version changes, boot-apply stops and `--status` / `boot.log` say why. A profile only applies to the card it was tuned on (NVML UUID).

> [!NOTE]
> The logon task runs with administrator rights, so everything it touches is admin-only: the exe in Program Files and the state folder in ProgramData (a folder someone else created there first, or a link in its place, is refused). Both locations come from the registry, not from environment variables. The CRT is linked statically and every non-system DLL (`d3d11`, `dxgi`, `d3dcompiler_47`) is delay-loaded from System32, so nothing placed next to the exe is ever loaded. Profile values outside the search range are refused.

`--stress` runs a DX11 compute load that checks every value it computes and ends with a verdict (`STABLE`, `WRONG RESULT`, `DEVICE LOST`, `TOO HOT`, `NO TELEMETRY`); it changes no settings and needs no elevation.

`--probe` prints live telemetry (core/memory clock, temperature, fan %, power) read through NVML; it needs no elevation. `gao` also has write commands -- `--set-core <mhz>`, `--set-mem <mhz>`, `--set-fan <pct>`, `--reset` -- that go through NVAPI and each verify the change by reading it back rather than trusting the call's return code. These need an **elevated** (administrator) shell, and their read-back has been verified on an RTX 4070; see [`docs/hardware-checks.md`](docs/hardware-checks.md) for the checklist this project runs manually and its current state. **Fan control does not work on this project's own reference RTX 4070**: the legacy NVAPI cooler API that `gao` calls (`NvAPI_GPU_GetCoolerSettings`) answers `NVAPI_NOT_SUPPORTED` on that card -- NVIDIA dropped it on Turing-and-later GPUs -- and `gao` reports that honestly instead of pretending the fan moved.

### Requirements

- Windows 10/11, 64-bit
- To build: Visual Studio 2026 with the C++ and CMake components (CMake 4.2 or newer)
- An NVIDIA GPU with a recent driver, for `--probe` and the write commands (NVML/NVAPI are loaded at runtime; nothing in the build itself needs a GPU, and CI builds the full executable on a GPU-less runner)

## Project structure

```
src/core/     pure logic -- no windows.h, no driver calls, no D3D. Unit-tested in CI.
  types.hpp       Telemetry, FanCurve, GpuControl -- the hardware seam
  objectives.*    presets as data (see below)
  config.*        JSON config round-trip
  stress_math.*   exact-float stress inputs and the CPU reference result
  stability.*     the stress run loop and its verdict (stable / wrong result / TDR / too hot / no telemetry)
  journal.*       write-ahead log of clock candidates; a freeze becomes a ceiling
  search.*        the tuner: baseline, power, core, memory, soak
  boot.*          when boot-apply may run (strikes, driver) and the verified profile apply
src/hw/       the only code that touches hardware: NVML, NVAPI and D3D11.
  nvml.*          telemetry (clocks, temp, fan %, power) via NVML
  nvapi.*         clock offsets and fan control via NVAPI, verified by read-back
  gpu_control.*   wires nvml/nvapi into the GpuControl struct core code uses
  stress.*        DX11 compute stress load; the GPU checks every value it computes
  app_files.*     gao.json (atomic writes), the crash journal and boot.log, all flushed to disk
  boot_task.*     the logon task, via schtasks.exe
src/app/      common.*: application logic both programs share (optimize, boot-apply, logon task)
  main.cpp        gao.exe, the command line
  gui/            GpuAutoOptimizer.exe: window, tray, optimize worker thread, watchdog loop
tests/        doctest, core only.
third_party/  doctest, nlohmann/json, Dear ImGui -- vendored as source, no package manager.
docs/hardware-checks.md   the manual checklist for what CI can't test (the runner has no GPU)
```

## Presets

`src/core/objectives.cpp` defines four presets as plain data (a thermal ceiling, a fan ceiling, how hard to push clocks, and which of core/memory/power tuning each one enables):

| Preset | max temp | max fan | perf push | core OC | mem OC | power |
|---|---|---|---|---|---|---|
| BestOfMyGpu (default) | 75 °C | 60% | 0.7 | on | on | on |
| Quiet | 80 °C | 40% | 0.4 | on | on | on |
| CoolAndEfficient | 65 °C | 70% | 0.3 | off | off | on |
| MaxPerformance | 83 °C | 100% | 1.0 | on | on | on |

Pick one with `--optimize best|quiet|cool|max`. Perf push is a safety margin: the search finds the highest stable offset and applies that fraction of it, always at least one step below. Quiet and CoolAndEfficient (perf push below 0.5) also look for the lowest power limit that costs less than 2 % score. Fan tuning is skipped on cards without fan control (including the reference RTX 4070). Undervolting is a separate opt-in flag, off in every preset above.

## Development

```powershell
cmake --preset default
cmake --build --preset debug
ctest --preset debug

cmake --preset asan               # AddressSanitizer build in build-asan/
cmake --build --preset asan
ctest --preset asan               # needs the MSVC bin\Hostx64\x64 folder on PATH
```

Our code builds at `/W4 /permissive-` with warnings as errors; `third_party/` is a system include. All of it is compiled with Control Flow Guard and SDL checks; both executables are linked CET-compatible and carry an application manifest.

`core_tests` is the only test binary. It covers `src/core/` -- presets, config round-trip, and the `GpuControl` seam that lets tests drive fake hardware through lambdas -- and needs no GPU, which is why CI runs it on a GitHub-hosted `windows-2025` runner, once normally and once under AddressSanitizer. `src/hw/` (NVML, NVAPI) is verified manually on real hardware instead, through [`docs/hardware-checks.md`](docs/hardware-checks.md); CI still builds the full `gao.exe` so a link error in the hardware layer is caught there rather than on a developer's machine.

## Status and roadmap

Phases 0-5 of the rewrite are done: repository migration, the CMake/CI skeleton, the hardware layer (NVML telemetry and power limit, NVAPI clock offsets, NVAPI fan control), the DX11 stress load with its stability verdict, the search with its crash-safe journal, persistence with boot-apply, and the window and tray app with its tune watchdog. Phase 6 is under way: the executables carry an icon and version information, and a `v*` tag builds a draft release (zip, SHA-256, build provenance attestation); the first release is still to be tagged. See [the design spec](docs/superpowers/specs/2026-09-20-cpp-rewrite-design.md) for the full phase table and the reasoning behind each decision (why NVAPI + NVML directly, why the Afterburner route was abandoned, why undervolting ships opt-in).

## License

[MIT](LICENSE) © Rovey
