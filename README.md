<div align="center">

# GPU Auto Optimizer

[![CI](https://github.com/Rovey/gpu-auto-optimizer/actions/workflows/ci.yml/badge.svg)](https://github.com/Rovey/gpu-auto-optimizer/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/github/license/Rovey/gpu-auto-optimizer?color=blue)](LICENSE)
![Windows 10/11](https://img.shields.io/badge/Windows-10%20%7C%2011-0078D4)
![NVIDIA](https://img.shields.io/badge/NVIDIA-Pascal%2B-76B900?logo=nvidia&logoColor=white)

**A native C++ NVIDIA GPU tuning tool, currently mid-rewrite.**

</div>

> [!IMPORTANT]
> `main` is being rewritten from Python to C++ (see [the design spec](docs/superpowers/specs/2026-09-20-cpp-rewrite-design.md)). What's here today is the repository skeleton and the hardware layer only: it can read live telemetry and write clock offsets and fan speed, verified against the driver. **There is no tuning engine, no stress test, no search and no GUI yet** -- those are later phases. v1.0 will be the finished C++ application. The previous, working Python version (search engine, risk profiles, tray GUI) is preserved at the `v0.9-python` git tag and still works as it always did; it just isn't what lives on `main` anymore.

> [!WARNING]
> The commands below that write to the GPU (`--set-core`, `--set-mem`, `--set-fan`, `--reset`) change clocks and fan behavior directly through the driver. Every write is verified by reading the value back rather than trusting the driver's return code, but a wrong offset can still make a card unstable. Use at your own risk.

## Quick start

Requires an x64 Windows machine with MSVC (Visual Studio, with the C++ and CMake workload) and CMake >= 3.28. From a Developer PowerShell, in the repo root:

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

That builds `.\build\Release\gao.exe` -- a command-line tool; there is no GUI yet.

```powershell
.\build\Release\gao.exe --version
.\build\Release\gao.exe --probe
```

`--probe` prints live telemetry (core/memory clock, temperature, fan %, power) read through NVML; it needs no elevation. `gao` also has write commands -- `--set-core <mhz>`, `--set-mem <mhz>`, `--set-fan <pct>`, `--reset` -- that go through NVAPI and each verify the change by reading it back rather than trusting the call's return code. These need an **elevated** (administrator) shell, and are unverified on real hardware so far; see [`docs/hardware-checks.md`](docs/hardware-checks.md) for the checklist this project runs manually and its current state. **Fan control does not work on this project's own reference RTX 4070**: the legacy NVAPI cooler API that `gao` calls (`NvAPI_GPU_GetCoolerSettings`) answers `NVAPI_NOT_SUPPORTED` on that card -- NVIDIA dropped it on Turing-and-later GPUs -- and `gao` reports that honestly instead of pretending the fan moved.

### Requirements

- Windows 10/11, 64-bit
- MSVC (Visual Studio) with the C++ and CMake components, or CMake >= 3.28 with MSVC on `PATH`
- An NVIDIA GPU with a recent driver, for `--probe` and the write commands (NVML/NVAPI are loaded at runtime; nothing in the build itself needs a GPU, and CI builds the full executable on a GPU-less runner)

## Project structure

```
src/core/     pure logic -- no windows.h, no driver calls, no D3D. Unit-tested in CI.
  types.hpp       Telemetry, FanCurve, GpuControl -- the hardware seam
  objectives.*    presets as data (see below)
  config.*        JSON config round-trip
src/hw/       the only code that touches hardware: NVML and NVAPI.
  nvml.*          telemetry (clocks, temp, fan %, power) via NVML
  nvapi.*         clock offsets and fan control via NVAPI, verified by read-back
  gpu_control.*   wires nvml/nvapi into the GpuControl struct core code uses
src/app/      entry point -- currently a command-line probe (gao.exe); an ImGui UI is a later phase.
tests/        doctest, core only.
third_party/  doctest, nlohmann/json -- vendored as source, no package manager.
docs/hardware-checks.md   the manual checklist for what CI can't test (the runner has no GPU)
```

## Presets -- data today, not yet a feature

`src/core/objectives.cpp` defines four presets as plain data (a thermal ceiling, a fan ceiling, how hard to push clocks, and which of core/memory/power tuning each one enables):

| Preset | max temp | max fan | perf push | core OC | mem OC | power |
|---|---|---|---|---|---|---|
| BestOfMyGpu (default) | 75 °C | 60% | 0.7 | on | on | on |
| Quiet | 80 °C | 40% | 0.4 | on | on | on |
| CoolAndEfficient | 65 °C | 70% | 0.3 | off | off | on |
| MaxPerformance | 83 °C | 100% | 1.0 | on | on | on |

**These aren't selectable anywhere yet.** There is no search to apply them to and no UI to pick them from; they exist now so the tuning engine (a later phase) has ceilings to search within once it's built. Undervolting is a separate opt-in flag, off in every preset above, and stays off by default even once tuning exists.

## Development

```powershell
cmake -S . -B build -A x64
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

`core_tests` is the only test binary. It covers `src/core/` -- presets, config round-trip, and the `GpuControl` seam that lets tests drive fake hardware through lambdas -- and needs no GPU, which is why CI runs it on `windows-latest`. `src/hw/` (NVML, NVAPI) is verified manually on real hardware instead, through [`docs/hardware-checks.md`](docs/hardware-checks.md); CI still builds the full `gao.exe` so a link error in the hardware layer is caught there rather than on a developer's machine.

## Status and roadmap

This is phases 0 and 1 of the rewrite: repository migration, the CMake/CI skeleton, and the hardware layer (NVML telemetry, NVAPI clock offsets, NVAPI fan control). The phases after this one -- the DX11 stress load and stability verdict, the search and crash-safe journal, persistence and boot-apply, the ImGui screens, and finally the release workflow -- are not started. See [the design spec](docs/superpowers/specs/2026-09-20-cpp-rewrite-design.md) for the full phase table and the reasoning behind each decision (why NVAPI + NVML directly, why the Afterburner route was abandoned, why undervolting ships opt-in).

## License

[MIT](LICENSE) © Rovey
