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
  search.*        the tuner: baseline, power, memory, core, soak; the recovery after a driver reset
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
| Instability = a wrong result or a lost device | A GPU computes wrong before it crashes, so a self-checking compute load catches instability earliest and cheapest. A TDR (`DXGI_ERROR_DEVICE_REMOVED`) is recoverable, unlike a freeze. The load's iterations per second double as the score. A third way to fail is `STALLED`: no wrong value and no lost device, but almost nothing computed (see the `STALLED` row). The search takes a lost device, a stall and lost telemetry as a driver reset, not as proof that the candidate itself is unstable: another program, or a forced reset in a test, gives the same verdict. |
| Memory stops at the bandwidth peak | GDDR6/GDDR6X retry failed transfers, so memory overclocks lose bandwidth long before they return wrong results. |
| Edges are confirmed, then backed off | A 3 s probe can pass by luck; the edges get 30 s probes before the safety margin is applied, and the result must pass a 300 s soak. |
| The search climbs to a per-card bound | A fixed +300 MHz cap decided the result on cards that hold more. The bound is the offset range the driver reports, accepted only when it is plausible (spans stock, contains the applied offset, below a sanity limit). Bisecting such a range would start hundreds of MHz too high, so the search climbs from stock. |
| Both searches go one grid step at a time (15 MHz core, 50 MHz memory); nothing is bisected | The first value that fails is then exactly one step above the last one that passed, for an ordinary failure and for a driver reset alike, and the climb probes nothing more after a value that failed or reset the driver. The confirm probe then runs one step below an ordinary failure (at the last value that passed) and, after a reset, four steps below the last value that passed. The earlier scheme, 60 MHz strides and a bisect after the first failure, could leave the last passing value up to 45 MHz below the edge and bisected right below a value that had just reset the driver. The cost is more probes and a longer run; how much longer has not been measured. |
| Memory is searched before the core | The first driver reset ends exploring (see the reset budget row). The assumption, not measured, is that a reset is far more likely in the core climb than in the memory scan: memory normally loses bandwidth long before it fails, and both resets seen so far came from the core. With the core first, its reset would leave the memory untuned. The core climb runs with the chosen memory offset applied; memory is tuned at stock core clocks, and the combination is proven by the soak. If the memory search itself has a reset, the core is not searched in that run. |
| The optimize run reconnects after a driver reset | The edge is found by crossing it, and past it the driver resets. A call into NVML faulted after a reset, in the tray app (hardware check 33) and in an optimize run (check 51), so the run owns guarded connections: a fault inside a driver DLL becomes a failed call, a reconnect re-creates both libraries, and it never happens with a journal entry open. A fault that still escapes ends the run with the entry closed as `CRASHED`, and the card is then reset to stock. |
| A probe that computes almost nothing is `STALLED` | Past its edge an RTX 5070 passed a probe at 3 % of its baseline score with no wrong value and no lost device (hardware check 51). Below a quarter of the baseline a "stable" probe is `STALLED`. A running probe ends as soon as its last three seconds fall below that floor, so a 300 s soak does not keep loading a card that stopped computing; three seconds, so that one delayed batch does not look like a dead card. Probes shorter than that are judged after they end. |
| A run stops exploring at the first driver reset and ends at the second | Nothing can guarantee that a probe will not reset the driver, so resets are counted. What counts: a probe that ends `DEVICE LOST`, `STALLED` or `NO TELEMETRY`; a candidate, power or soak write that fails; a failed bandwidth measurement on an overclocked candidate. At the first one in a clock search that search ends at the last value that passed (in a bandwidth scan: the bandwidth peak of the values that passed), a core search that had not started is skipped, confirmation starts four steps lower, and a reset during a confirm probe or the soak moves the next try four steps down instead of one. At the second the run ends at stock, nothing saved, nothing else started. With no clock offset applied (the baseline, the power step, a soak at stock clocks) there is nothing to back off to, and the first one ends the run. Only `DEVICE LOST` proves a reset; the others suggest one, so `gao` can count a reset the Windows log does not show. |
| After a reset: rest, then prove the card is back | In round A of hardware check 51 the first probe after a forced reset, at core +30 and about two seconds after the driver restarted, scored 325 it/s at 45 W and was judged `STALLED`; the desktop was unusable afterwards. Why that probe scored so low is not known: the stress load re-creates its D3D device and recompiles its shader in the first batch after a lost device, and that time is counted in the probe's score; the card itself may or may not have been back. So before any further candidate the search reconnects, writes stock, leaves the card without load for 20 s, and runs a 5 s probe at stock that must be `STABLE` at 90 % of the baseline or better: up to three such probes, a rest before each. A card that does not come back ends the run, and so does a reset during that check: no step of the recovery starts another recovery. The numbers are first choices, not measured. |
| Known limits of the health check | After a reset the stress load is rebuilt once, after the rest and before the health probe, outside its timing: it creates a new D3D device and recompiles its shader, so the probe's score no longer includes that. A rebuild that fails counts as a failed try of the health check (the rest and the check are tried up to three times) and its reason is logged (`the stress load could not be rebuilt: ...`); a fault during the rebuild ends the run through the crashed path (journal entry closed, card reset to stock). The load is rebuilt even when it did not notice the reset itself. Not yet measured on hardware. Still unknown: how long the rebuild takes (`gao --stress-recreate` measures it), whether the first health probe then passes at once, and whether the rest keeps the desktop usable. |
| Nothing about a reset is kept for later runs | A reset says nothing certain about the candidate, and a stored one would let a single invalid probe lower the bound for good. The cost: a run on a card it can push past its edge crosses that edge again every time. Only a freeze is remembered, through the crash journal. |
| An entry at stock clocks never becomes a ceiling | A ceiling of zero would mean "never search this clock", which no freeze at stock can justify. The soak and the +0 sample of the memory scan do not journal a clock that is at stock, and an open entry with an offset of 0 is reported as a freeze but lowers nothing. |
| No undervolting | Locking a voltage point hard-froze the reference RTX 4070, and reshaping the curve gave no measurable gain on a power-limited card. |
| Fans through NVML, stop zone via the driver | NVIDIA's legacy NVAPI fan API is gone on RTX 20-series and newer; NVML's `nvmlDeviceSetFanSpeed_v2` is public and verified by reading the target back. Below the stop threshold the driver owns the fans, so no failure of this app can leave them stopped. |
| Tray app plus logon task | Driver settings are volatile: a reboot or driver reset clears them. The logon task starts the tray app, whose watchdog keeps the tune applied; three crashing logons in a row switch it off. |
| Dear ImGui on DX11 | One small binary with no runtime, and the D3D11 device is in the process anyway for the stress load. |

**Not yet verified on hardware.** The handling of a driver reset during an optimize run (the rows above on the reconnect, the in-probe `STALLED`, the reset budget, the rest and health check, and the memory-first single-step search) describes what the code does. It is implemented and unit-tested. On hardware only this much has been seen, in round A of [hardware check 51](hardware-checks.md), on a build that reconnected but took no rest: `gao` survived a forced reset, reconnected and left the journal clean, and the desktop was unusable afterwards. Whether the rest keeps the desktop usable is not known.

## Rules the design depends on

- `src/core/` is pure logic. It reaches hardware only through `GpuControl` and callbacks, so CI tests it without a GPU.
- Every hardware write is verified by reading it back; a mismatch is a failure. Any failed apply ends at stock.
- The crash journal's `begin` line is flushed to disk before a clock candidate touches the hardware.
- A reconnect to the driver never happens while a journal entry is open.
- A run stops exploring at the first driver reset and ends at the second. Every reset event is counted in one number, the recovery's own health probe included.
- After a reset event no candidate is probed until the card has rested and passed a health probe at stock. No step of that recovery starts another recovery.
- A probe that ended `DEVICE LOST`, `STALLED` or `NO TELEMETRY` is never used as a measured failure of its candidate: the search does not bisect below it and stores nothing about it for later runs.
- A journal entry at stock clocks never becomes a ceiling.
- A stop request ends the running probe within one batch (about 250 ms); the candidate's journal entry is closed as `ABORTED` and the run ends at stock.
- Everything the elevated logon task touches is admin-only: the copy in `%ProgramFiles%` and the state folder in `%ProgramData%`. A state folder someone else created first, or a link in its place, is refused. Both locations come from the registry, not from environment variables.
- The CRT is linked statically and every non-system DLL (`d3d11`, `dxgi`, `d3dcompiler_47`, `dwmapi`) is delay-loaded from System32, so nothing placed next to the executable is loaded.

## Testing

`core_tests` covers `src/core/` and runs in CI on a GitHub-hosted `windows-2025` runner, once normally and once under AddressSanitizer. CI also builds both executables, so a link error in the hardware layer is caught there.

The hardware layer cannot run in CI (the runner has no GPU). It is checked by hand on the reference RTX 4070 through [hardware-checks.md](hardware-checks.md); record the results there.

## Releases

The version lives in `src/core/version.hpp`. Pushing a `v*` tag that matches it builds, tests and packages both executables into a **draft** GitHub release with a SHA-256 file and a build provenance attestation; publishing the draft is a manual step. Running the Release workflow by hand is a dry run that only uploads the zip as an artifact.
