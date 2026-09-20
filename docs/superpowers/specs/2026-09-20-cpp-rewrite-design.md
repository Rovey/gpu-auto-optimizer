# GPU Auto Optimizer — C++ Rewrite Design

Date: 2026-09-20
Status: approved (design), not yet implemented
Supersedes: `2026-06-29-cpp-afterburner-rewrite-design.md` (Afterburner control plane — abandoned, see §2.1)

## 1. Goal

One native Windows executable that tunes an NVIDIA GPU on its own. The user picks a
preset — by default **"Make the best of my GPU"** — presses Optimize, and the app finds
the fan curve, power limit and clock offsets that are actually stable on that card,
verifying every step against the hardware.

Non-goals for v1: AMD/Intel GPUs, Linux, per-game profiles, auto-update, an installer,
telemetry, translations.

## 2. Decisions

| Decision | Choice | Why |
|---|---|---|
| Control plane | **NVAPI + NVML, direct** | The Afterburner route was already tried and abandoned in this repo; its public interface (MAHM shared memory) is monitoring, and profile-file editing never reached the hardware. NVAPI is what the Python version proved works. |
| Scope of v1 | Full rewrite: engine **and** GUI | One `.exe` to download, no Python, no venv, no CUDA wheels. |
| GUI | **Dear ImGui + DX11** | Single small binary, no runtime dependency, live graphs are trivial, and DX11 is in the process anyway for the stress load. |
| User model | 4 presets + 3 advanced sliders | One button for most people; performance / noise / temperature sliders for the rest. |
| v1 tuning scope | Fan curve + power limit + core/mem OC. Undervolt **opt-in** | The V/F lock froze the test GPU; the reshape path is untested on hardware. It ships behind an explicit switch, off by default. |
| Persistence | Tray + boot-apply with 3-strike | Settings are volatile by design; boot-apply is what makes "set it and forget it" true. |
| Repo | Same repo, Python removed | `main` becomes pure C++; the Python app stays reachable via the `v0.9-python` tag. |
| Language | English everywhere | Code, comments, commits, UI strings, docs, issues. |

### 2.1 Why not Afterburner

This repo already contains `cpp/src/hw/afterburner.cpp`,
`cpp/src/core/ab_profile.cpp` and `cpp/src/tools/abctl_spike_main.cpp` — a spike whose stated
goal was to prove that editing an Afterburner profile and triggering a re-apply reaches the
hardware. The project moved back to NVAPI right after, and the README records the verdict:
editing profile `.cfg` files does not apply to hardware. Afterburner also gives no per-step
verification, which the entire search depends on. Those three files are deleted in the migration.

## 3. Architecture

One process, three layers, no IPC.

```
src/core/     pure logic. No windows.h, no driver calls. Unit-tested.
src/hw/       the only code that touches hardware: NVAPI, NVML, D3D11.
src/app/      entry point, ImGui screens, tray, --boot-apply.
tests/        doctest, core only.
third_party/  imgui, doctest, nlohmann/json — vendored, no package manager.
```

`core` reaches hardware through one struct of callbacks, filled by `hw` in production and by
lambdas in tests:

```cpp
struct GpuControl {
    std::function<Telemetry()>            read;               // clocks, temp, fan %, watts
    std::function<bool(int)>              set_core_offset;    // MHz, verified by read-back
    std::function<bool(int)>              set_mem_offset;     // MHz, verified by read-back
    std::function<bool(int)>              set_power_limit;    // % of default
    std::function<bool(FanCurve const&)>  set_fan_curve;      // 4 points, temp -> %
    std::function<bool()>                 reset_to_stock;
};
```

No interface with one implementation, no backend registry: a second vendor would add a second
filler for this struct, not an abstraction layer.

## 4. Control layer and the verification rule

- NVAPI is loaded at runtime (`LoadLibrary("nvapi64.dll")` + `NvAPI_QueryInterface` by
  interface id), carrying only the handful of structs actually called. No NVAPI SDK as a
  build dependency.
- Clock offsets and the V/F curve go through NVAPI (PState20). Power limit and telemetry go
  through NVML. Fans go through the NVAPI cooler API.

**The rule the whole app rests on: every set is followed by a read-back, and a mismatch is a
failure.** NVAPI returns success for changes it silently does not apply; treating the return
code as truth is how the Python version produced results that did not exist.

Fan control is the one genuinely uncertain API: older cards use `SetCoolerLevels`, newer ones
the client fan-cooler interface. Strategy: try the new one, fall back to the old one, and if
neither verifies, **disable fan tuning and say so in the UI** — never pretend it worked.

## 5. Stress load and the stability verdict

A DX11 compute shader (the device is already there for ImGui) runs an FP32 matmul over fixed
input whose checksum is known in advance.

- **Wrong checksum = unstable.** A GPU computes wrong before it crashes; this is the earliest
  and cheapest instability signal available.
- **`DXGI_ERROR_DEVICE_REMOVED` = unstable.** That is Windows' TDR stepping in — recoverable,
  unlike a freeze.
- Search probe: ~3 s per candidate. Final soak: 60 s (configurable).
- Thermal abort above 85 °C (configurable).
- The kernel's iterations/second **is** the score. No separate benchmark.

## 6. The tuner

### 6.1 Presets are data

```cpp
struct Objectives {
    int   max_temp_c;    // thermal ceiling under load
    int   max_fan_pct;   // noise proxy
    float perf_push;     // 0..1, how hard to chase clocks
    bool  core_oc, mem_oc, power, undervolt;
};
```

| Preset | max_temp | max_fan | perf_push | core/mem | power | undervolt |
|---|---|---|---|---|---|---|
| Make the best of my GPU (default) | 75 °C | 60 % | 0.7 | on | on | off |
| Quiet | 80 °C | 40 % | 0.4 | on | on | off |
| Cool & efficient | 65 °C | 70 % | 0.3 | off | on (lowered) | off |
| Max performance | 83 °C | 100 % | 1.0 | on | on | off |

The three advanced sliders (performance, noise, temperature) overwrite fields in the same
struct. Undervolt is a separate opt-in toggle, never set by a preset.

### 6.2 Search order

Envelope first, clocks second. The reverse is the classic mistake: tune clocks, then lower the
power limit, and the result you measured no longer exists.

1. **Fan curve** — four points (temp → %); per point find the lowest speed that holds
   `max_temp_c` under load. Four points, not a spline: the driver interpolates.
2. **Power limit** — binary-search the lowest cap costing < 2 % score (quiet/efficient) or the
   highest permitted (performance).
3. **Core offset** — binary search; each step applied, read back, probed.
4. **Memory offset** — same. VRAM has no ECC here: it corrupts silently rather than crashing,
   which is exactly what the checksum catches.
5. **Final soak**, then save.

### 6.3 Crash-safe journal

Unchanged from the Python design, because it earned its place: `begin → fsync → apply →
complete`, written before the hardware is touched. On startup, any entry without `complete` is
by definition the setting that froze the machine → blacklisted permanently. One JSON-lines
file. This is the difference between a freeze costing one run and costing every run.

## 7. Persistence, boot-apply, tray

- `%LOCALAPPDATA%\GpuAutoOptimizer\gao.json` — last applied profile, fan curve, blacklist.
- Boot-apply is the same executable with `--boot-apply`, registered as a Task Scheduler task at
  logon with highest privileges. No service, no Run key.
- 3-strike: increment a counter before applying, clear it once the app has run 2 minutes without
  crashing. At 3, auto-apply disables itself and the UI says why.
- Tray: temperature and clock in the tooltip; right-click gives Apply, Revert to stock, Open, Exit.

## 8. UI

Three screens, one window (~900×600), dark theme.

1. **Home** — GPU name, live telemetry, four preset cards, one **Optimize** button, and an
   "Advanced" expander holding the three sliders plus the undervolt opt-in.
2. **Run** — current phase, current candidate, verdict per step, live graph, and an **Abort**
   that always works.
3. **Results** — before/after (score, clocks, temp, fan %), with Apply, Revert to stock, and
   Apply at boot.

No settings screen: the only real settings (thermal ceiling, soak duration) live in Advanced.

## 9. Testing and CI

`core` is fully testable because it only knows `hw` through the callback struct. Coverage that
must exist:

- search converges and respects objectives and the blacklist;
- fan-curve math;
- journal recovery after a simulated freeze;
- preset → objectives mapping;
- config round-trip.

CI on `windows-latest`: MSVC + CMake configure → build → `ctest`. The runner needs no GPU
because only `core` is tested, but the **full executable is built** so link errors surface in CI
rather than on the developer's machine.

Hardware verification is a checklist in `docs/hardware-checks.md`, run manually on the RTX 4070:
read-back matches, fan responds, TDR is recognised, a freeze results in a blacklist entry, and
boot-apply survives a reboot.

## 10. Release

Tag → GitHub Actions builds Release x64 and attaches `GpuAutoOptimizer.exe` plus its SHA-256 to
the release. Unsigned, so the README carries the same SmartScreen note as `claude-usage-tray`.

## 11. Migration and phases

1. `git tag v0.9-python` on the current state — the Python app stays retrievable forever.
2. One commit removes the Python tree and flattens `cpp/` to the repository root
   (`src/`, `tests/`, `third_party/`, `CMakeLists.txt`). The Python CI workflow is replaced by
   the C++ one. `core/ab_profile`, `hw/afterburner` and `tools/abctl_spike_main` are deleted.

| Phase | Done when |
|---|---|
| P0 | Skeleton, CMake, CI green, empty app starts |
| P1 | `hw`: detect GPU, read telemetry, set offsets with verified read-back |
| P2 | DX11 stress load and the stability verdict |
| P3 | Search + journal — **from here it actually tunes** |
| P4 | Persistence, boot-apply, tray |
| P5 | ImGui screens |
| P6 | README, hero image, release workflow, first release |

Stopping after P3 still leaves something useful; that is why the UI is last.

## 12. Risks

| Risk | Mitigation |
|---|---|
| NVAPI fan control differs per generation and is undocumented | Try new API, fall back to old, disable fan tuning if neither verifies |
| A freeze during search | Write-ahead journal + blacklist; this is why it exists |
| NVAPI reports success without applying | Read-back verification on every set |
| Undervolt freezing the machine again | Off by default, explicit opt-in, and it inherits the journal protection |
| TDR kills the D3D device mid-probe | Treated as an instability verdict, device recreated |
| Antivirus/SmartScreen flags an unsigned exe that writes clocks | Documented in the README, SHA-256 published per release |

## 13. Explicitly out of scope

Installer, auto-update, per-game profiles, cloud sync, multi-GPU tuning (detect all, tune one),
undo history beyond "Revert to stock", i18n, telemetry.
