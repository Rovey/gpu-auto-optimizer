# P5 — Window, Tray and Tune Watchdog

Date: 2026-09-27
Status: approved (design) under the user's overnight mandate; not yet implemented
Parents: `2026-09-20-cpp-rewrite-design.md` §8, `2026-09-26-p4-persistence-and-boot-apply-design.md`,
`2026-09-27-p4b-measurement-and-hardening-design.md`. Research: ImGui/tray/crash notes of 2026-09-27.

## 1. Goal

A desktop app next to the CLI: pick a preset, press Optimize, watch it run, see the result —
and a tray process that re-applies the tune at logon and keeps it applied for the whole
session. The user asked for the last part explicitly: a tune that silently disappeared (TDR,
driver reset, another tool) must not cost a whole game session.

## 2. Decisions

| Decision | Choice | Why |
|---|---|---|
| Executables | `gao.exe` (CLI, console subsystem, unchanged) + **`GpuAutoOptimizer.exe`** (window + tray, Windows subsystem), both linking `core` and `hw` | One console-subsystem exe flashes a console for the GUI; one Windows-subsystem exe breaks CLI output and exit codes in a shell. Two small exes is the least fragile. |
| Logon | The logon task runs `GpuAutoOptimizer.exe --tray` (elevated) instead of `gao.exe --boot-apply`. It does the boot decision (strikes, driver, GPU), applies, clears the strike after 2 minutes, and **stays resident** in the tray with the watchdog | One resident elevated process instead of a one-shot plus a second tray process. No console flash at logon. Task time limit becomes unlimited (`PT0S`). `gao --boot-apply` remains for compatibility. |
| Watchdog | Every 30 s: read back the applied core/mem offsets and power limit, compare with the profile | The user's requirement. |
| Watchdog cases | matches → nothing; **all stock, same driver/GPU** → re-apply (a TDR or driver reset), but after 3 resets within an hour stop re-applying and notify ("keeps getting reset — likely unstable, re-optimize"); **neither stock nor ours** → another tool (Afterburner, NVIDIA App) owns the GPU: back off, notify once, do not fight; **driver or GPU changed** → do not apply the old tune, notify "driver changed — re-optimize?" once | Evidence from the research: fighting another tuner causes write wars; repeated resets are an instability signal. |
| Screens | Home (GPU, live telemetry, profile/boot status, four preset cards, Optimize), Run (phase log, live score/temperature graph, Abort), Results (before/after, Apply at boot toggle) | Parent spec §8. The three "advanced sliders" are dropped (YAGNI; presets cover the objectives). |
| ImGui | Dear ImGui **v1.92.9b** (tagged master, not docking), vendored under `third_party/imgui` with the Win32 + DX11 backends; no ImPlot (`ImGui::PlotLines` is enough for two series) | Research: docking/multi-viewport are overkill; 1.92 brings dynamic fonts and `FontScaleDpi`. |
| Rendering | Own D3D11 device for the UI (separate from the stress device); render only when needed: block in `MsgWaitForMultipleObjectsEx`, ~4 Hz on Home, ~10 Hz on Run, a few frames after input, nothing while hidden in the tray | The tool measures the GPU; its own UI must not load it. DXGI occlusion is unreliable on Windows 11 — track visibility ourselves. |
| Work | Optimize runs on a `std::jthread`; the UI reads a snapshot under a mutex; Abort sets the same abort flag Ctrl+C sets | A 10-minute run must never block the window. |
| Elevation | Unelevated launch works (telemetry, status); Optimize / Apply / Boot toggle show "Restart as administrator" (`ShellExecuteExW` with `runas`) | Elevation stays the user's choice (manifest `asInvoker`). |
| Tray | `Shell_NotifyIcon` with hWnd+uID (no GUID: the exe is unsigned and may move), `NOTIFYICON_VERSION_4` + `NIF_SHOWTIP`, tooltip = temp/clock, menu Open / Re-apply / Revert to stock / Exit; `TaskbarCreated` re-adds the icon; `ChangeWindowMessageFilterEx` lets Explorer's messages reach the elevated window; balloons with `NIIF_RESPECT_QUIET_TIME` | Research, Microsoft Learn. |
| Single instance | Named mutex; a second launch activates the first window and exits | Two watchdogs would fight each other. |
| Crash dumps | `SetUnhandledExceptionFilter` → `MiniDumpWriteDump` into the state folder (`crash-<time>.dmp`), written from a pre-created thread | Last log lines plus a dump are the most useful evidence after a driver crash. |

## 3. Core additions (unit-tested)

```cpp
struct AppliedState { int core_mhz = 0; int mem_mhz = 0; int power_pct = 100; };
enum class WatchAction { None, Reapply, GiveUpUnstable, BackOffForeign, NotifyDriverChanged };
struct Watchdog {
    // Pure decision; the caller does the reading and acting.
    WatchAction check(const Profile& p, const std::optional<AppliedState>& seen,
                      bool driver_matches, bool gpu_matches, std::chrono::steady_clock::time_point now);
};
```
- `seen` equal to the profile → None. `seen` all stock → Reapply, unless 3 Reapply decisions fall
  within the last hour → GiveUpUnstable (once; afterwards None). Anything else → BackOffForeign
  (once; afterwards None until the state matches again). Driver or GPU mismatch →
  NotifyDriverChanged (once). `seen` nullopt (read failed) → None.
- Tolerance: power within ±1 %, offsets exact.
- `GpuControl` gains `std::function<std::optional<AppliedState>()> read_applied`.

## 4. hw / app additions

- `Nvapi::ReadOffsetsMhz` and `Nvml` power read-back are wired into `read_applied` (power as percent
  of default).
- `src/app/gui/`: `main_gui.cpp` (WinMain, single instance, tray, message loop, render loop),
  `screens.cpp` (the three screens), `worker.cpp` (optimize thread + snapshot). CLI code shared by
  both exes (profile text, config load/save, boot decision text) moves to `src/app/common.*`.
- Task XML: the action becomes `GpuAutoOptimizer.exe --tray` with `ExecutionTimeLimit PT0S`;
  `gao --boot on` and the GUI's boot toggle both install **both** exes into Program Files.

## 5. Testing

- Unit: every `Watchdog` case, the one-hour window, the "once" behaviour, read failure; the
  updated task XML.
- Manual (hardware checks 29+): window renders and idles (GPU load of the idle window ~0 %),
  Optimize from the GUI, Abort, Results, tray menu, balloon, single instance, logon → tray
  resident and applied, forced TDR (`dxcap -forcetdr`) → watchdog re-applies within 30 s,
  Afterburner applying its own offsets → watchdog backs off and notifies.

## 6. Out of scope

Advanced sliders, fan curve UI (fan control unsupported on the reference card), undervolt UI,
localisation, auto-update, installer (P6 decides packaging).
