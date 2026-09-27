# Fan curve — design

Date: 2026-09-27. Status: approved in conversation, pending written review.

## Goal

Let GPU Auto Optimizer control the fans with a temperature curve, so a user can have a quieter card, a cooler card, a curve per profile, or a curve of their own, like MSI Afterburner offers. The curve is part of the tune: an optimize run uses the profile's curve, so the clocks it finds are stable at the temperatures that curve produces.

## Feasibility (measured)

`fan-probe.ps1` on the reference RTX 4070, driver 610.74, elevated:

- NVML reports 2 fans and a range of 30-100 %.
- `nvmlDeviceSetFanSpeed_v2(60)` returns success; the fans reached 58 % and the policy switched to manual (1). `nvmlDeviceGetTargetFanSpeed` reads back 60.
- 0 % and 20 % are accepted and stop the fans, but the target reads back 30 %: a value below the minimum cannot be verified by read-back.
- `nvmlDeviceSetDefaultFanSpeed_v2` returns control to the driver; the policy reads back 0 (automatic). At idle the driver keeps these fans stopped.
- Unelevated, every set call returns `NVML_ERROR_NO_PERMISSION`; reads work.

This is the public NVML API, documented for Maxwell and newer. The removed NVAPI cooler code used an API NVIDIA dropped on RTX 20-series and newer; nothing of it comes back.

## Behaviour

### The curve

- 2-6 points of (temperature °C, fan %), strictly ascending in temperature, non-decreasing in fan %. Between points the fan follows linearly; below the first point it holds the first point's value, above the last it holds the last.
- An optional **fan-stop threshold**. Below it the fans are handed to the driver (`SetDefaultFanSpeed`), which stops them at idle on cards that support it. The app never sets 0 % itself: if it is killed, the driver still controls the fans.
- **Stopping without cycling:** the curve takes over at the threshold. The fans go back to the driver only when the card is 8 °C below the threshold and draws under 30 W for a hold time: 60 s, doubled (up to 15 min) whenever the fans had to restart within 5 min of stopping, and back to 60 s after 10 min stopped. (Revised after hardware check 38: a 3 °C gap made the fans cycle every few seconds under light load.)
- **Fast up, slow down:** a higher target is applied at once; a lower target only after the temperature has stayed lower for 5 seconds.
- A new manual value is written only when it differs from the current one by 2 % or more, or when it is 100 %.

### Guard rails, whatever the curve says

- Never below the card's minimum (`nvmlDeviceGetMinMaxFanSpeed`) outside the fan-stop zone.
- 100 % at or above the profile's temperature limit (`Objectives::max_temp_c`).
- No temperature reading (`temp_c == -1`) means driver control; the app never keeps a manual speed without a reading.

### Default curves

| Profile | Fan-stop below | Points (°C → %) |
|---|---|---|
| Quiet | 50 °C | 50→30, 65→40, 75→55, 80→75 |
| Best of my GPU | 50 °C | 50→35, 60→45, 70→65, 75→100 |
| Cool & efficient | 45 °C | 45→40, 55→55, 65→80 |
| Max performance | none | 40→40, 60→60, 75→85, 83→100 |

Points below the card's minimum are raised to it by the guard rail, not rejected.

### Relation to the tune

- `--optimize` and the window's Optimize run with the profile's default curve active. The saved profile records that curve as the **tested curve**.
- The **active curve** starts as the tested curve and can be edited. If it is quieter than the tested curve at any temperature (a lower fan % at some temperature from 30 °C up to the profile's limit, or a fan-stop threshold higher than the tested one), the app warns that the tune was tested with a different curve and suggests optimizing again. It does not block the edit.
- Fan control runs while the tray app runs elevated, which is what apply-at-logon starts. Otherwise the driver controls the fans.

## Fan presets

Four curves independent of the tuning profile: Silent, Normal, Cool and Aggressive (the Quiet, Best, Cool & efficient and Max performance defaults). The Optimize page and `gao --optimize <profile> --fan-curve <preset>` choose the curve a run uses (default: the profile's own); the Fan page applies a preset as the active curve. The profile's temperature limit still forces 100 %.

## Components

### `src/core/fan_curve.*` (pure, unit-tested)

```cpp
struct FanPoint { int temp_c; int pct; };
struct FanCurve {
    std::optional<int> stop_below_c;   // driver control below this
    std::vector<FanPoint> points;      // 2-6, ascending temp, non-decreasing pct
};
FanCurve default_curve(Preset preset);
bool valid(const FanCurve& c);
int curve_pct(const FanCurve& c, int temp_c);           // interpolation only
bool quieter_than(const FanCurve& a, const FanCurve& b, int max_temp_c);

struct FanCommand { bool driver = true; int pct = 0; };  // driver control, or manual pct
class FanController {
public:
    FanController(FanCurve curve, int min_pct, int max_temp_c);
    FanCommand decide(int temp_c, std::chrono::steady_clock::time_point now);
};
```

`FanController` holds the hysteresis and fast-up/slow-down state and applies the guard rails.

### `src/core/types.hpp`

`GpuControl` gains `std::function<bool(int)> set_fan_pct` (manual, verified by the target read-back), `std::function<bool()> set_fan_auto` (verified by the policy read-back) and `std::function<int()> fan_min_pct`. Empty when the card has no controllable fans.

### `src/hw/nvml.*`

`FanCount`, `FanMinMaxPct`, `SetFanPct` (every fan; verifies `GetTargetFanSpeed == pct`), `SetFanAuto` (every fan; verifies `GetFanControlPolicy_v2 == automatic`), `FanPolicyManual` (for foreign-tool detection). `gpu_control.cpp` wires them in when `FanCount() > 0`.

### `src/core/config.*`

- `Profile` gains `FanCurve fan_curve` (the tested curve).
- `Config` gains `std::optional<FanCurve> fan_curve` (the active curve; empty means the profile's tested curve) and `bool fan_control = true`.
- JSON round-trip; an invalid curve reads as absent, like a half profile.

### `src/app/common.*`

A `FanDriver` that owns a `FanController`, calls `GpuControl` only when the command changes, and records the last value it wrote, for foreign-tool detection. `run_optimize` builds one from the profile's default curve and ticks it from the stress load's telemetry read (already about once a second), then hands the fans to the driver when the run ends, including on abort and on an exception.

### Tray app (`src/app/gui/main_gui.cpp`)

- Ticks the `FanDriver` on the 1 s telemetry timer when elevated, a profile exists, fan control is on and no search is running.
- Hands the fans to the driver on: Exit, `WM_ENDSESSION`, `WM_POWERBROADCAST`/`PBT_APMSUSPEND`, the crash handler (after the dump, before the clock reset), `hw_lost`, and fan control switched off. After resume the next tick takes over again.

### UI (`src/app/gui/ui.*`)

- A **Fan** page in the sidebar:
  - the curve as a graph with draggable points;
  - add/remove point buttons (2-6 points);
  - a fan-stop threshold slider with an off position;
  - a Fan control on/off switch and a "Profile default" button;
  - the "quieter than tested" warning;
  - the current mode: curve at X %, driver control, or off.
- The Fan tile on the dashboard shows the mode under the speed.
- Edits are saved to `gao.json` on release of a point or slider, not on every frame.

### CLI (`src/app/main.cpp`)

- `gao --fan auto` hands every fan to the driver (elevated); an emergency exit.
- `gao --status` prints the fan mode and the active curve.
- Ctrl+C during `--optimize` hands the fans to the driver along with stock clocks.

## Error handling

| Situation | Response |
|---|---|
| A fan write does not read back | Hand the fans to the driver, switch fan control off for the session, note it in the log and a balloon |
| No temperature reading | Driver control until a reading returns |
| Policy back to automatic without us (driver reset, TDR) | Treated as a reset: the next tick applies the curve again |
| Policy manual at a value we did not write (another tool) | Stop controlling, hand nothing back (the other tool owns it), say so once |
| Exit, logoff, sleep, crash | Driver control first |
| App killed hard | The fans stay at the last manual value, which is never below what the curve asked at that temperature; the next tray start takes over |
| Not elevated | No fan control; the Fan page says why |

## Testing

- **Unit tests** (`tests/test_fan_curve.cpp`, CI):
  - interpolation and clamping;
  - `valid()`;
  - fan-stop zone: 8 °C gap, idle power, growing hold;
  - fast up, slow down with an injected clock;
  - guard rails (minimum, 100 % at the limit, missing reading);
  - `quieter_than`;
  - the default curves are valid and end at 100 % by the profile's limit (via the guard rail where the last point is lower);
  - JSON round-trip including invalid curves;
  - `run_optimize`-level fan ticks through a fake `GpuControl`.
- **Hardware checks** (new rows in `docs/hardware-checks.md`, reference RTX 4070):
  - the curve follows the temperature under `--stress`;
  - the fan-stop zone hands control to the driver (policy 0);
  - Exit, sleep and a forced TDR leave the fans correct;
  - an Afterburner fan change makes the app step aside;
  - `gao --fan auto` works;
  - `gao --optimize` leaves the fans on driver control after Ctrl+C.

## Out of scope

- Per-fan curves (both fans follow one curve).
- Fan control without the tray app running (a Windows service).
- Tuning the curve automatically for noise.
