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
  fan_curve.*     fan curves, the per-second controller and the FanDriver
src/hw/       the only code that touches hardware or the OS state folders.
  nvml.*          telemetry and power limit via NVML
  nvapi.*         clock offsets via NVAPI, verified by read-back
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

`src/core/objectives.cpp` defines each profile as a thermal ceiling, a perf push (the fraction of the highest stable offset that is applied) and which of core, memory and power tuning it enables:

| Profile | `--optimize` | Max temp | Perf push | Core | Memory | Power |
|---|---|---|---|---|---|---|
| BestOfMyGpu (default) | `best` | 75 °C | 0.7 | on | on | on |
| Quiet | `quiet` | 80 °C | 0.4 | on | on | on |
| CoolAndEfficient | `cool` | 65 °C | 0.3 | off | off | on |
| MaxPerformance | `max` | 83 °C | 1.0 | on | on | on |

The applied offset is always at least one step below the confirmed edge. Profiles with a perf push below 0.5 also look for the lowest power limit that costs less than 2 % score.

## Design decisions

| Decision | Why |
|---|---|
| NVAPI and NVML directly, not MSI Afterburner | An earlier version drove Afterburner by editing its profile files; the edits never reached the hardware, and Afterburner offers no per-step read-back, which the whole search depends on. |
| Instability = a wrong result or a lost device | A GPU computes wrong before it crashes, so a self-checking compute load catches instability earliest and cheapest. A TDR (`DXGI_ERROR_DEVICE_REMOVED`) is recoverable, unlike a freeze. The load's iterations per second double as the score. |
| Memory stops at the bandwidth peak | GDDR6/GDDR6X retry failed transfers, so memory overclocks lose bandwidth long before they return wrong results. |
| Edges are confirmed, then backed off | A 3 s probe can pass by luck; the edges get 30 s probes before the safety margin is applied, and the result must pass a 300 s soak. |
| The search climbs to a per-card bound | A fixed +300 MHz cap decided the result on cards that hold more. The bound is the offset range the driver reports, accepted only when it is plausible (spans stock, contains the applied offset, below a sanity limit). Bisecting such a range would start hundreds of MHz too high, so the search climbs in strides and bisects only between the last pass and the first failure. |
| The optimize run reconnects after a driver reset | The edge is found by crossing it, and past it the driver resets. NVML and NVAPI are stale or fault after a reset (hardware checks 33 and 51), so the run owns guarded connections, re-creates them, and never does so with a journal entry open. |
| A probe that computes almost nothing is `STALLED` | Past its edge an RTX 5070 passed a probe at 3 % of its baseline score with no wrong value and no lost device (hardware check 51). Below a quarter of the baseline a "stable" probe counts as failed. |
| No undervolting | Locking a voltage point hard-froze the reference RTX 4070, and reshaping the curve gave no measurable gain on a power-limited card. |
| Fans through NVML, stop zone via the driver | NVIDIA's legacy NVAPI fan API is gone on RTX 20-series and newer; NVML's `nvmlDeviceSetFanSpeed_v2` is public and verified by reading the target back. Below the stop threshold the driver owns the fans, so no failure of this app can leave them stopped. |
| Tray app plus logon task | Driver settings are volatile: a reboot or driver reset clears them. The logon task starts the tray app, whose watchdog keeps the tune applied; three crashing logons in a row switch it off. |
| Dear ImGui on DX11 | One small binary with no runtime, and the D3D11 device is in the process anyway for the stress load. |

## Rules the design depends on

- `src/core/` is pure logic. It reaches hardware only through `GpuControl` and callbacks, so CI tests it without a GPU.
- Every hardware write is verified by reading it back; a mismatch is a failure. Any failed apply ends at stock.
- The crash journal's `begin` line is flushed to disk before a clock candidate touches the hardware.
- A reconnect to the driver never happens while a journal entry is open.
- A stop request ends the running probe within one batch (about 250 ms); the candidate's journal entry is closed as `ABORTED` and the run ends at stock.
- Everything the elevated logon task touches is admin-only: the copy in `%ProgramFiles%` and the state folder in `%ProgramData%`. A state folder someone else created first, or a link in its place, is refused. Both locations come from the registry, not from environment variables.
- The CRT is linked statically and every non-system DLL (`d3d11`, `dxgi`, `d3dcompiler_47`, `dwmapi`) is delay-loaded from System32, so nothing placed next to the executable is loaded.

## Testing

`core_tests` covers `src/core/` and runs in CI on a GitHub-hosted `windows-2025` runner, once normally and once under AddressSanitizer. CI also builds both executables, so a link error in the hardware layer is caught there.

The hardware layer cannot run in CI (the runner has no GPU). It is checked by hand on the reference RTX 4070 through [hardware-checks.md](hardware-checks.md); record the results there.

## Releases

The version lives in `src/core/version.hpp`. Pushing a `v*` tag that matches it builds, tests and packages both executables into a **draft** GitHub release with a SHA-256 file and a build provenance attestation; publishing the draft is a manual step. Running the Release workflow by hand is a dry run that only uploads the zip as an artifact.
