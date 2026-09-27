# Development

How the code is organised, how to build and test it, and the rules the design depends on. For using the app, see the [README](../README.md).

## Build and test

Visual Studio 2026 with the C++ and CMake components (CMake 4.2 or newer, which knows the VS 2026 generator). From a Developer PowerShell in the repository root:

```powershell
cmake --workflow --preset ci      # configure, build Release, run the tests

cmake --preset default
cmake --build --preset debug
ctest --preset debug

cmake --preset asan               # AddressSanitizer build in build-asan/
cmake --build --preset asan
ctest --preset asan               # needs the MSVC bin\Hostx64\x64 folder on PATH
```

Our code builds at `/W4 /permissive-` with warnings as errors; `third_party/` is a system include. Everything is compiled with Control Flow Guard and SDL checks, and both executables are linked CET-compatible and carry an application manifest.

## Layout

```
src/core/     pure logic: no windows.h, no driver calls, no D3D. Unit-tested in CI.
  types.hpp       Telemetry, GpuControl (the hardware seam), AppliedState
  objectives.*    the four profiles as data
  config.*        gao.json round-trip
  stress_math.*   exact-float stress inputs and the CPU reference result
  stability.*     the stress run loop and its verdict
  journal.*       write-ahead log of clock candidates; a freeze becomes a ceiling
  search.*        the tuner: baseline, power, core, memory, confirmation, soak
  boot.*          when a logon apply may run (strikes, driver, card) and the verified apply
  watchdog.*      the tray app's decisions: re-apply, give up, back off, driver changed
  task_xml.*      the logon task definition
src/hw/       the only code that touches hardware or the OS state folders.
  nvml.*          telemetry and power limit via NVML
  nvapi.*         clock offsets and fan control via NVAPI, verified by read-back
  gpu_control.*   wires NVML and NVAPI into GpuControl
  stress.*        the DX11 compute load; the GPU checks every value it computes
  app_files.*     gao.json (atomic writes), the crash journal and boot.log, flushed to disk
  boot_task.*     the logon task and the Program Files copy
src/app/
  common.*        logic both programs share: optimize, logon apply, apply-at-logon on/off
  main.cpp        gao.exe, the command line
  gui/            GpuAutoOptimizer.exe: window, tray, optimize worker thread, watchdog loop
tests/        doctest, core only
third_party/  doctest, nlohmann/json, Dear ImGui: vendored as source, no package manager
```

## Profiles as data

`src/core/objectives.cpp` defines each profile as a thermal ceiling, a fan ceiling, a perf push (the fraction of the highest stable offset that is applied) and which of core, memory and power tuning it enables:

| Profile | `--optimize` | Max temp | Max fan | Perf push | Core | Memory | Power |
|---|---|---|---|---|---|---|---|
| BestOfMyGpu (default) | `best` | 75 °C | 60 % | 0.7 | on | on | on |
| Quiet | `quiet` | 80 °C | 40 % | 0.4 | on | on | on |
| CoolAndEfficient | `cool` | 65 °C | 70 % | 0.3 | off | off | on |
| MaxPerformance | `max` | 83 °C | 100 % | 1.0 | on | on | on |

The applied offset is always at least one step below the confirmed edge. Profiles with a perf push below 0.5 also look for the lowest power limit that costs less than 2 % score. Fan tuning is skipped on cards without fan control.

## Rules the design depends on

- `src/core/` is pure logic. It reaches hardware only through `GpuControl` and callbacks, so CI tests it without a GPU.
- Every hardware write is verified by reading it back; a mismatch is a failure. Any failed apply ends at stock.
- The crash journal's `begin` line is flushed to disk before a clock candidate touches the hardware.
- Everything the elevated logon task touches is admin-only: the copy in `%ProgramFiles%` and the state folder in `%ProgramData%`. A state folder someone else created first, or a link in its place, is refused. Both locations come from the registry, not from environment variables.
- The CRT is linked statically and every non-system DLL (`d3d11`, `dxgi`, `d3dcompiler_47`, `dwmapi`) is delay-loaded from System32, so nothing placed next to the executable is loaded.

## Testing

`core_tests` covers `src/core/` and runs in CI on a GitHub-hosted `windows-2025` runner, once normally and once under AddressSanitizer. CI also builds both executables, so a link error in the hardware layer is caught there.

The hardware layer cannot run in CI (the runner has no GPU). It is checked by hand on the reference RTX 4070 through [hardware-checks.md](hardware-checks.md); record the results there.

## Releases

The version lives in `src/core/version.hpp`. Pushing a `v*` tag that matches it builds, tests and packages both executables into a **draft** GitHub release with a SHA-256 file and a build provenance attestation; publishing the draft is a manual step. Running the Release workflow by hand is a dry run that only uploads the zip as an artifact.
