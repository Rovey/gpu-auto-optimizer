# V/F Curve Undervolting Design Spec

## Goal

Replace the flat PState20 clock offset approach with voltage-frequency curve manipulation, enabling automatic undervolting that finds the lowest stable voltage for a target clock frequency. This yields higher sustained boost clocks, lower temperatures, and reduced power consumption.

## Problem

The current optimizer applies a uniform frequency offset via NVAPI PState20 (`SetPstates20`). This shifts all V/F points up equally, increasing power draw and heat. The GPU then thermally throttles, negating much of the gain. On an RTX 2060 Super, the optimizer achieves +125 MHz offset but the GPU only sustains 1845 MHz under load due to thermal/power limits.

Community-standard undervolting (via MSI Afterburner's curve editor) achieves 1900-1950 MHz at 900-950mV by locking the GPU to a specific voltage-frequency point on the V/F curve. This approach reduces temperature by 10-15C and power draw by 20-30% while often slightly increasing performance.

## Architecture

### Hybrid Backend Strategy

A new `NVAPIVFCurveBackend` handles V/F curve manipulation for Pascal through Ada GPUs (GTX 10 through RTX 40 series). The existing `NVAPIBackend` (PState20) serves as fallback for Maxwell and Blackwell GPUs where V/F curve write is unavailable.

Backend selection priority:

| Priority | Backend | Capabilities | GPU Support |
|---|---|---|---|
| 40 | `NVAPIVFCurveBackend` | V/F curve + power limit | Pascal, Turing, Ampere, Ada |
| 30 | `NVAPIBackend` | PState20 offsets + power limit | Maxwell, Blackwell fallback |
| 10 | `NvidiaSMIBackend` | Power limit only | Universal fallback |

### NVAPI V/F Curve Functions

All are undocumented, accessed via `nvapi_QueryInterface`:

| Function | ID | Purpose |
|---|---|---|
| `GetClockBoostMask` | `0x507B4B59` | Discover which V/F points exist |
| `GetVFPCurve` | `0x21537AD4` | Read base voltage-frequency pairs (read-only) |
| `GetClockBoostTable` | `0x23F1B133` | Read per-point frequency deltas |
| `SetClockBoostTable` | `0x0733E009` | Write per-point frequency deltas |
| `GetClockBoostLock` | `0xE440B867` | Read voltage lock state |
| `SetClockBoostLock` | `0x39442CFB` | Lock GPU to specific voltage point |

### Buffer Formats

**NV_GPU_CLOCK_MASKS** (6188 bytes, version `0x0001182C`):
- `+0`: uint32 version
- `+4`: 64 bytes mask data
- `+68`: 255 entries x 24 bytes each: `{uint32 clockType, uint8 enabled, 19 bytes padding}`

**NV_GPU_VFP_CURVE** (7208 bytes, version `0x00011C28`):
- `+0`: uint32 version
- `+4`: 64 bytes mask (copy from ClockBoostMask)
- `+68`: 255 entries x 28 bytes each: `{uint32 clockType, uint32 frequencyKHz, uint32 voltageUV, 16 bytes padding}`

**NV_GPU_CLOCK_TABLE** (9248 bytes, version `0x00012420`):
- `+0`: uint32 version
- `+4`: 64 bytes mask (copy from ClockBoostMask)
- `+68`: 255 entries x 36 bytes each: `{uint32 clockType, 16 bytes padding, int32 frequencyDeltaKHz, 12 bytes padding}`

**NV_GPU_CLOCK_LOCK** (variable size):
- `+0`: uint32 version, uint32 flags, uint32 count
- Entries: `{uint32 index, uint32 unknown, uint32 mode (0=NONE/3=MANUAL), uint32 unknown, uint32 voltageUV, uint32 unknown}`

**Critical:** ClockBoostTable `frequencyDeltaKHz` is stored at 2x scale internally. Divide by 2 when reading, multiply by 2 when writing.

## New Backend: `src/backends/nvapi_vfcurve.py`

### Reading the V/F Curve

1. Call `GetClockBoostMask` -- returns bitmask of enabled V/F points
2. Call `GetVFPCurve` (pass mask) -- returns base voltage (uV) and frequency (kHz) per point
3. Call `GetClockBoostTable` (pass mask) -- returns current frequency delta per point
4. Filter for graphics domain (`clockType == 0`) and `enabled == 1`
5. Return list of `VFPoint(voltage_uv, base_freq_khz, delta_khz)` sorted by voltage

### Writing the Curve (Undervolting)

Given target voltage `V_target` and target frequency `F_target`:

1. For each V/F point at or below `V_target`: set delta so `base_freq + delta >= F_target` (allow GPU to hit target clock)
2. For each V/F point above `V_target`: set delta to flatten frequency to `F_target` or below (prevent GPU from using higher voltages)
3. Call `SetClockBoostTable` with modified deltas (remember 2x scaling)
4. Call `SetClockBoostLock` with `mode=3` (MANUAL) and `voltageUV = V_target * 1000` to hard-lock

### Reset

1. Zero all deltas in ClockBoostTable, call `SetClockBoostTable`
2. Call `SetClockBoostLock` with `mode=0` (NONE) to unlock voltage

### Availability Detection

Call `GetClockBoostMask` at init. If it returns `NVAPI_OK` with >0 enabled graphics points, V/F curves are supported. If it fails (Blackwell restriction, Maxwell lack of support), `is_available()` returns False.

### Backend Interface

Implements the existing `GPUBackend` abstract class:
- `apply()`: writes V/F curve + power limit via pynvml
- `reset()`: zeros curve + unlocks voltage + resets power limit
- `verify()`: reads back curve and confirms target V/F point matches
- `supports_voltage()`: returns True
- `supports_core_oc()`: returns True (V/F curve implicitly controls frequency)
- `supports_mem_oc()`: returns False (memory OC still via PState20)

New methods (not on base class):
- `read_vf_curve()`: returns list of current V/F points for the optimizer to inspect
- `apply_vf_lock(voltage_uv, freq_khz)`: apply V/F curve flattening + voltage lock (used by optimizer's binary search without going through the full `apply()` interface)

**Memory OC coordination:** When V/F curve backend is active, memory OC still goes through PState20 (`NVAPIBackend`). The optimizer calls `_best_backend()` for the primary backend (V/F curve) and keeps a separate PState20 backend reference for memory offsets. The `apply()` method on `NVAPIVFCurveBackend` accepts `mem_offset_mhz` and delegates it to PState20 internally.

## Optimizer Changes

### New Phase Order

When V/F curve backend is selected:

**Phase 1: Baseline** (unchanged) -- measure stock clocks under load

**Phase 2: V/F Curve Search** (new, replaces old phases 2+4) -- find optimal voltage-frequency point:
1. Read stock V/F curve
2. Set target frequency = baseline boost clock (what the GPU naturally boosts to under load)
3. Binary search on voltage:
   - Range: `voltage_min_mv` (from risk profile) to stock voltage for target freq
   - Test midpoint: apply V/F curve locked at `target_freq @ mid_voltage`, run stability test
   - Stable: try lower voltage. Unstable: try higher voltage.
   - Step size: 12-13mV (one V/F curve point, ~12.5mV spacing typical)
4. Once minimum stable voltage found, optionally try bumping frequency +25/+50 MHz at that voltage
5. Apply 1-step safety margin (one V/F point higher voltage than absolute minimum)

**Phase 3: Memory OC** (unchanged) -- binary search via PState20

**Phase 4: Power Limit** (improved) -- with undervolt active, start reducing power limit. The GPU needs much less power at lower voltage, so this phase finds bigger savings.

**Phase 5: Final Verification** (unchanged)

### PState20 Fallback

When V/F curve backend is NOT available, the optimizer uses the existing phase order:
`Core OC -> Mem OC -> (skip voltage) -> Power Limit`

This is detected via `backend.supports_voltage()`. No code path changes needed for the fallback -- it's the current behavior.

### New Optimizer Method

```python
def _search_optimal_vf_point(self) -> tuple[int, int]:
    """Find lowest stable voltage for target frequency.
    Returns (voltage_mv, frequency_mhz)."""
```

Replaces `_binary_search_core()` and `_binary_search_voltage()` when V/F curves are available.

## Config Changes

### GPUOptimizationResult (2 new fields)

```python
target_voltage_mv: int = 0    # voltage GPU is locked to (0 = not using V/F curve)
target_freq_mhz: int = 0      # frequency at that voltage point
```

### Risk Profile Additions

```python
RiskLevel.SAFE: {
    # No voltage changes -- power limit only (unchanged)
    "voltage_min_mv": 0,   # 0 = V/F curve disabled for this risk level
}
RiskLevel.BALANCED: {
    "voltage_min_mv": 800,  # won't go below 800mV
}
RiskLevel.PERFORMANCE: {
    "voltage_min_mv": 725,
}
RiskLevel.EXTREME: {
    "voltage_min_mv": 650,
}
```

## Boot-Apply Changes

The `_apply_on_boot()` function checks `target_voltage_mv` in saved results:
- If > 0: use `NVAPIVFCurveBackend` to re-apply V/F curve at saved voltage/frequency
- If == 0: use existing PState20 path

When re-applying V/F curve on boot:
1. Read the *current* V/F curve fresh (don't replay raw bytes -- curve may differ after driver update)
2. Find the V/F point closest to saved `target_voltage_mv`
3. Apply curve flattening at that point for `target_freq_mhz`

This handles driver updates gracefully -- the curve point spacing may change but the target voltage/frequency intent is preserved.

## Safety

**Voltage floors per risk level** -- enforced in the optimizer, not the backend. BALANCED never goes below 800mV even if lower might be stable.

**TDR recovery** -- when the GPU crashes during V/F testing, the driver resets all NVAPI overrides. The optimizer detects this via the existing TDR detection in StabilityTester and moves to the next voltage candidate.

**3-strike boot-apply** -- unchanged. If V/F curve settings cause 3 consecutive boot failures, auto-apply disables.

**Blackwell detection** -- `NVAPIVFCurveBackend.is_available()` attempts `GetClockBoostMask`. If NVIDIA has locked this on RTX 50 series, it returns an error and the backend reports unavailable. Falls back to PState20 automatically.

## Files Changed

| File | Change |
|---|---|
| `src/backends/nvapi_vfcurve.py` | **New.** V/F curve backend (~300 lines) |
| `src/backends/nvapi.py` | Add memory OC support for hybrid use (V/F curve does core, PState20 does memory) |
| `src/optimizer.py` | Add `_search_optimal_vf_point()`, modify `_run_full_mode()` phase order |
| `src/config.py` | Add `target_voltage_mv`, `target_freq_mhz` to result; add `voltage_min_mv` to profiles |
| `src/boot_apply.py` | V/F curve re-apply path in `_apply_on_boot()` |
| `tests/test_vfcurve.py` | **New.** Tests for V/F curve reading, delta calculation, 2x scaling |

## Out of Scope

- GUI changes (the optimization screen already shows generic phase progress)
- Fan curve control
- Per-game profiles
- RTX 50 Blackwell workaround (falls back to PState20; revisit if NVIDIA re-enables V/F curve write)
