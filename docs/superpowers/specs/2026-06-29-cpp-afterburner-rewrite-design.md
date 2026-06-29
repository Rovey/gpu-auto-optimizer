# Native C++ GPU Auto-Optimizer — Design Spec

**Date:** 2026-06-29
**Status:** Draft for review
**Supersedes:** the Python implementation (kept as reference spec, not reused as code)

## 1. Goal

A native Windows desktop app that makes NVIDIA GPU tuning fully automatic. The user
picks one of three risk presets — **Safe**, **Balanced**, **Performance** — presses one
**Optimize** button, and the app finds and applies the best stable settings on its own,
then keeps them applied across reboots.

Target hardware for v1: RTX 4070 (Ada), driver 610.62. Designed to generalize to
RTX 30/40/50 series.

### Why this rewrite

The previous Python app crashed before touching the GPU because its stress backend
(CuPy/CUDA) was a fragile native dependency that broke on a packaging mismatch. The
control path (direct NVAPI writes) was also unverified and full of fallbacks. This
rewrite removes both risk sources:

- **No CUDA toolchain** — stress runs on DirectX 12 compute, which ships with Windows.
- **No direct NVAPI writes** — hardware control is delegated to MSI Afterburner, a
  battle-tested engine, via its profile files.

## 2. Non-goals (YAGNI)

- No "Extreme" preset (removed — too risky for a one-button tool).
- No direct NVAPI/NVML *writes* for OC/UV. NVML is read-only telemetry.
- No multi-GPU orchestration in v1 (architect for a GPU index, ship single-GPU).
- No fan-curve tuning in v1 (Afterburner default fan behavior is fine).
- No cloud, no telemetry upload, no account system.

## 3. Architecture

Two processes, one shared library boundary:

```
┌────────────────────────────────────────────────────────┐
│ optimizer.exe  (main app, C++ / Dear ImGui on DX12)     │
│                                                          │
│  GUI (ImGui)  ──  Optimizer core  ──  AfterburnerCtl     │
│      │                 │                    │            │
│   Telemetry         spawns          writes profile .cfg  │
│   (NVML read)      stress.exe       + triggers AB apply  │
└────────┼────────────────┼───────────────────┼───────────┘
         │                │                    │
     nvml.dll        stress.exe          MSIAfterburner.exe
   (read clocks,   (DX12 compute,        (applies OC/UV/power
    temp, power,    separate process,     to the hardware)
    util, ECC)      crash-isolated)
```

**Why stress is a separate process:** an unstable overclock can trigger a driver
Timeout Detection & Recovery (TDR) or a hard GPU hang. If that happens in-process it
takes the GUI down with it. As a child process, a TDR kills only `stress.exe`; the main
app observes the death (non-zero exit / lost heartbeat), treats it as a stability
failure, and rolls back. (This is the same reason NV-UV runs its stress out-of-process.)

### Components

| Component | Responsibility |
|---|---|
| `Telemetry` | NVML wrapper. Read clocks, temp, power, util, ECC errors, throttle reasons. |
| `AfterburnerController` | Locate AB install + profile cfg; read/write cfg keys + `VFCurve=`; trigger apply; verify. |
| `VFCurve` | Parse/encode the Afterburner `VFCurve=` hex blob; compute undervolt curves. |
| `StressClient` | Spawn `stress.exe`, stream results, detect death/TDR. |
| `Optimizer` | The search pipeline (port of the Python algorithm). Presets, binary search, rollback. |
| `Persistence` | JSON config: last result, per-GPU settings, boot-apply state, 3-strike counter. |
| `BootApply` | Re-apply saved settings at boot with 3-strike safety. |
| `Gui` | ImGui screens: Dashboard, Optimize, Results, Settings. |
| `stress.exe` | Standalone DX12 compute stress + correctness check. |

## 4. Control plane — Afterburner integration

### 4.1 Profile files

- Location: `…\MSI Afterburner\Profiles\VEN_10DE&DEV_<id>&SUBSYS_…&BUS_…&DEV_…&FN_….cfg`
  (resolve by matching the GPU's PCI VEN/DEV/SUBSYS/bus from NVML/setupapi).
- Plain INI-ish text. Relevant keys under a profile section:
  - `PowerLimit=` (percent)
  - `ThermalLimit=` (°C)
  - `ThermalPrioritize=`
  - `CoreClkBoost=` (core offset, units to confirm in spike — kHz vs MHz×1000)
  - `MemClkBoost=` (memory offset, same unit family)
  - `CoreVoltageBoost=`
  - `VFCurve=<hexstring>` (the undervolt curve — see 4.2)
- The app reserves a dedicated profile slot (**Profile 5** by default; confirm it is
  unused or let the user pick). It never edits the user's other profiles.

### 4.2 VFCurve hex format (cracked — reference: VFCurveEditor, MIT C#)

The `VFCurve=` value encodes the full voltage/frequency curve as a hex string:

```
VFCurve = header(3 × int32 = 24 hex chars)
        + N points, each = 3 float32 values × 8 hex chars = 24 hex chars
        + zero-terminator point (voltage=freq=offset=0)
        + trailing bytes
```

Each point is `(Voltage, Frequency, Offset)`:
- `Voltage` — millivolts (identifies the curve point).
- `Frequency` — MHz, **visualization only**, not applied.
- `Offset` — MHz applied **relative to the GPU's built-in curve**. This is the only
  field that changes hardware behavior. NVIDIA OC/UV is offset-only.

Each float is serialized as its raw IEEE-754 bytes hex-encoded. Port the exact
serialization from VFCurveEditor `CurveReader.cs` (`ToHexString`/`FromHexString`) and
**validate against a real profile exported by Afterburner on this machine** before
trusting it (endianness must match byte-for-byte).

**Undervolt algorithm:** to lock the GPU at voltage `V` with target frequency `F`,
raise the offset at the point nearest `V` so its effective frequency reaches `F`, then
flatten all points above `V` to that same effective frequency (so the GPU stops
boosting to higher voltages). Reference: VFCurveEditor `OffsetPointMethod` /
`SmoothSlopeMethod`. The optimizer searches `V` (and optionally a small `F` bump) for
the lowest stable voltage — same logic as the Python `_search_optimal_vf_point`.

### 4.3 Apply trigger (the main integration spike)

VFCurveEditor only edits the file; a human then loads the profile in AB. We must make AB
apply an edited profile **programmatically and reliably**. The spike must determine
which of these works on this machine, in priority order:

1. `MSIAfterburner.exe -Profile5` while AB is running — does AB re-read the cfg from
   disk and apply? (Preferred: cheapest.)
2. If AB caches profiles in memory: write cfg, then restart AB (`taskkill` + relaunch
   with `-Profile5` / `-startup`).
3. Use AB's "apply profile on startup" setting so a relaunch always applies our slot.

Verify application by reading back clocks/power/voltage via NVML and comparing to intent.
**Acceptance for the spike:** a programmatic change to `VFCurve=`/`PowerLimit=`
demonstrably changes NVML-observed power/voltage/clocks without manual clicks.

### 4.4 Dependency & failure handling

- Afterburner is a **hard dependency**. On launch, detect install path (registry /
  known paths) and running state.
- If missing: clear guidance ("Install MSI Afterburner") and disable Optimize.
- If not running: offer to launch it (must run as admin to apply).
- Back up the reserved profile slot before first write; offer "reset to stock"
  (clear our profile / apply a zeroed curve + 100% power).

## 5. Stress engine — `stress.exe`

- Standalone C++ DX12 console exe. Args: `--gpu <luid/index> --seconds <n> --json`.
- Workload: sustained compute-shader GEMM-style FMA loop sized to a safe share of VRAM.
- **Correctness check:** periodically run a compute pass with a known input and compare
  the GPU result against a CPU reference within tolerance. A mismatch = memory
  instability → report failure (this is what catches a bad memory OC that doesn't crash).
- Output: stream JSON lines (or a final JSON) with `{started, peak_util, completed,
  correctness_ok, error}`. Exit code: 0 = clean finish, non-zero = workload error.
- The main app additionally watches: process death (TDR), NVML temp ceiling, ECC errors,
  throttle reasons, and clocks dropping to 0 (TDR heuristic). Any of these = fail.

### IPC contract (main app ↔ stress.exe)

- Spawn with stdout piped; parse JSON lines for progress + result.
- Heartbeat: if no output for `T` seconds and process alive, treat as hang → kill + fail.
- Timeout: hard kill at `seconds + margin`.

## 6. Optimizer pipeline (port of existing algorithm)

Presets (ported from Python `config.py`, Extreme removed):

| Preset | Core OC | Mem OC | Undervolt target | Power | Risk |
|---|---|---|---|---|---|
| Safe | — | — | — | reduce to −20%..0% | none |
| Balanced | up to +150 MHz | up to +800 MHz | down to ~−150 mV equiv | −10%..+15% | low |
| Performance | up to +250 MHz | up to +1500 MHz | down to ~−200 mV equiv | −5%..+25% | medium |

Pipeline:
1. Reset reserved profile to stock; apply; settle.
2. Baseline measure under stress load (~20 s): boost clock, temp, power.
3. **Preflight:** run stress once; fail fast if load isn't sustained or stress.exe can't
   run (clear message — no silent pass).
4. **Safe path:** binary-search the lowest power limit that holds clocks within 2%.
5. **Full path (Balanced/Performance):**
   - V/F undervolt search: lowest stable voltage for the baseline boost frequency
     (binary search over curve voltage steps), then optional small frequency bump.
   - Memory OC: binary search max stable `MemClkBoost` (100 MHz steps).
   - Power limit: lower until performance drops >2%, then step back.
6. **Final verification:** 5-minute stress soak at chosen settings; one-step rollback on
   failure.
7. Save result to JSON; show before/after.

Every step: apply → stress-test → on failure roll back one step and reduce the search
range. Apply a safety margin below the absolute max found (one voltage/clock step).

## 7. Telemetry — NVML

Read-only via `nvml.dll` (bundled with the driver). Sample clocks, temp, power, util,
ECC error counts, and throttle reasons at ~3 Hz for the dashboard and during stress.
Used to verify Afterburner actually applied settings (read-back vs intent).

## 8. GUI — Dear ImGui on DX12

Single window, dark theme, four screens (tabs/sidebar):
- **Dashboard** — live clocks/temp/power/util graphs, current applied settings,
  Afterburner status, boot-apply status.
- **Optimize** — three preset cards (Safe/Balanced/Performance) + a big **Optimize**
  button; live progress log + current phase during a run; Cancel.
- **Results** — before/after comparison (boost MHz, temp, power) from the last run.
- **Settings** — boot-apply toggle, reserved profile slot picker, reset-to-stock,
  Afterburner path, log viewer.

ImGui chosen because the app already renders DX12 (for stress dev/telemetry viz),
live graphs are native to ImGui, and it ships as one self-contained exe with no runtime
or licensing burden.

## 9. Persistence & boot-apply

- Config + results: JSON in `%LOCALAPPDATA%\GpuAutoOptimizer\`.
- **Boot-apply:** on user opt-in, register a Windows Task Scheduler task (at logon, admin)
  that re-applies the saved profile. Prefer leaning on Afterburner's own
  "apply on startup" for the reserved slot where possible (most reliable), with the
  scheduled task as the trigger/guard.
- **3-strike safety:** a counter persisted to JSON. Three consecutive boot-apply failures
  (detected via a "did we boot cleanly after applying?" marker) disables auto-apply and
  notifies the user. GPU hardware change (PCI id mismatch) pauses auto-apply.

## 10. Build & toolchain

- **Language:** C++20.
- **Build:** CMake, MSVC (Visual Studio 2022 toolset).
- **Deps via vcpkg:** Dear ImGui, a JSON lib (nlohmann/json), DirectX headers (Windows
  SDK). NVML headers from the CUDA/driver SDK (link `nvml.dll` at runtime via
  `LoadLibrary` to avoid a hard SDK build dependency).
- **Output:** `optimizer.exe` + `stress.exe`, shipped together. No installer framework
  required for v1 (a zip + shortcut script); revisit packaging later.

## 11. Risks & spikes (ordered by risk)

1. **AB apply trigger (§4.3)** — highest risk. Must prove a file edit → applied hardware
   change without manual clicks. Spike this **first**; the whole route depends on it.
2. **VFCurve format validation (§4.2)** — port serialization, validate byte-for-byte vs a
   real exported profile. Risk: per-driver point count / format drift.
3. **Reserved profile slot management** — don't clobber user profiles; back up first.
4. **DX12 stress correctness check** — must reliably catch memory instability, not just
   crashes.
5. **Admin elevation** — applying needs AB running as admin; the app likely needs
   elevation too. Decide manifest (`requireAdministrator`) vs on-demand.
6. **TDR detection latency** — confirm child-process death + NVML clock-zero heuristic
   catch hangs fast enough.

## 12. Testing strategy

- **Unit:** VFCurve parse/encode round-trip (against captured real profiles); offset/
  undervolt math; cfg read/write; JSON persistence; 3-strike state machine; search-loop
  logic with a mocked stress+telemetry.
- **Integration (manual, on real hardware, as admin):**
  - Spike harness: edit `PowerLimit=`/`VFCurve=`, trigger AB, confirm via NVML.
  - `stress.exe` standalone: produces sustained load + correctness pass/fail.
  - Full Safe run end-to-end; then Balanced.
- **Safety gates:** never exceed 95 °C; always apply a margin below max-stable; roll back
  on any verification failure.

## 13. Build order (high level — detailed plan to follow)

1. Spike: AB apply trigger + VFCurve read/write/validate (de-risk before building UI).
2. `stress.exe` (DX12 compute + correctness + JSON IPC).
3. Telemetry (NVML) + AfterburnerController + VFCurve lib.
4. Optimizer core (Safe path first, then full path) with mocked then real stress.
5. ImGui shell + 4 screens wired to the core.
6. Persistence + boot-apply + 3-strike.
7. End-to-end hardware validation + polish.

## 14. Open questions

- Confirm `CoreClkBoost`/`MemClkBoost` units in the cfg (spike will reveal).
- Which profile slot to reserve by default (5?) and how to detect it's free.
- Admin model: always-elevated manifest vs relaunch-elevated on Optimize.
- Minimum supported Afterburner version (curve format stability).
