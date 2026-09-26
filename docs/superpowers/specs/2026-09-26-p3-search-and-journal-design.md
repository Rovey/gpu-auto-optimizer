# P3 — Search and Crash-Safe Journal

Date: 2026-09-26
Status: approved (design), not yet implemented
Parent: `2026-09-20-cpp-rewrite-design.md` §6 (this refines it). Builds on P2
(`2026-09-26-p2-stress-and-verdict-design.md`): `run_stability` is the only way the search
judges a candidate.

## 1. Goal

`gao --optimize [best|quiet|cool|max]` finds the power limit, core offset and memory offset
that are actually stable on this card for the chosen preset, applies them, and survives a
freeze: a candidate that froze the machine is never tried again.

From here the project actually tunes. Results are volatile (lost on reboot) until P4 adds
persistence and boot-apply.

Success criteria:
- `gao --optimize best` on the RTX 4070 ends with an applied result whose 60 s soak is STABLE,
  and a following `gao --stress 60` is STABLE too.
- A simulated freeze (an unmatched `begin` line in the journal) produces a ceiling the next run
  respects.
- Ctrl+C at any point leaves the card at stock.
- All search and journal logic is unit-tested in CI without a GPU.

## 2. Decisions

| Decision | Choice | Why |
|---|---|---|
| Scope | Power limit + core + memory. Fan skipped when unsupported | All four presets work, including CoolAndEfficient (power only). Fan control is `NOT_SUPPORTED` on the 4070; an empty `set_fan_pct` callback skips it. |
| `perf_push` meaning | **Safety margin**: the search always finds the highest stable offset; the applied offset is `perf_push × max`, rounded down to a step, and always at least one step below max | One search run per dimension, predictable results, and a margin against instability a 3 s probe misses. User-approved. |
| Freeze memory | Journal lines become **ceilings**: a freeze at core +165 means nothing ≥ +165 is tried again | A value that froze the machine once is not worth a second freeze; neither is anything above it. |
| Entry point | CLI `gao --optimize`, elevated | The UI is P5; the CLI is what P3 is verified with. |

## 3. Flow

```
0. Load journal → ceilings; warn about any freeze found.
1. reset_to_stock (offsets 0, power default).
2. Baseline: 30 s probe at stock. Not STABLE → abort, nothing to tune.
3. Power (if objectives.power and a power range exists and min < max):
   a. thermal cap  = highest pct (5 % steps, within range) whose 20 s probe has
                     peak_temp_c <= objectives.max_temp_c
   b. perf_push < 0.5 (Quiet, CoolAndEfficient):
                     lowest pct (<= thermal cap) whose 20 s score >= 98 % of the score at
                     the thermal cap
      otherwise:     the thermal cap
   Apply and verify by read-back.
4. Core (if objectives.core_oc): highest stable offset in 15 MHz steps, 0..+300, below the
   journal ceiling; each candidate set → read back → 3 s probe. Apply the margin value.
5. Memory (if objectives.mem_oc): same, 50 MHz steps, 0..+1500, with the chosen core applied.
6. Soak: 60 s probe on the final settings. On failure: core and mem each one step down (not
   below 0), soak again; at most 3 retries. Still failing → reset_to_stock, report failure.
7. Leave the result applied; print before/after (baseline vs soak: score, clocks, temp, power).
```

A candidate is **unstable** when its probe verdict is WrongResult, DeviceLost or TooHot (a
clock that pushes the card past its thermal ceiling is too far). NoTelemetry aborts the whole
run: without temperature the search is blind. After DeviceLost the search restores stock
settings before the next candidate.

Probe temperature limits: clock and soak probes run with `max_temp_c = objectives.max_temp_c`;
power probes run with a hard safety limit of 85 °C and judge `peak_temp_c` against the
objective, because the point of that step is to measure the temperature.

Timing, roughly: baseline 30 s + power ~5 × 20 s + core ~5 × 3 s + mem ~5 × 3 s + soak 60 s
≈ 4–5 minutes. A 20 s peak is a lower bound, not a settled temperature; the 60 s soak runs at
`max_temp_c` too, so it doubles as the thermal check.

## 4. Search math

`highest_stable(lo, hi, step, ceiling, is_stable)` — binary search over the values
`lo, lo+step, …, <= hi` that are also `< ceiling`, assuming stability is monotonic (if a value is
unstable, everything above it is). `lo` is assumed stable (it is stock, already verified by the
baseline) and is never probed. Returns the highest stable value.

Margin: `applied = floor(perf_push * max / step) * step`, then `min(applied, max - step)`, then
`max(applied, 0)`. `max == 0` → `0`.

Power efficiency search uses the same binary search on "score >= 98 % of reference", monotonic
in the other direction (lower power = lower score), so it finds the lowest passing value.

## 5. Journal

File: `%LOCALAPPDATA%\GpuAutoOptimizer\journal.jsonl`, JSON lines.

```
{"id":7,"core":165,"state":"begin"}                 written and flushed BEFORE the set
{"id":7,"state":"complete","verdict":"STABLE"}       written after the probe
{"id":9,"core":105,"mem":700,"state":"begin"}       soak: both values
```

- `begin` is flushed to disk (`FlushFileBuffers`) before the hardware is touched. A freeze
  therefore always leaves the `begin` of the candidate that caused it.
- On load: every `begin` without a matching `complete` sets a ceiling:
  `ceiling[core] = min(ceiling[core], core)`, and likewise for `mem`. Core-search lines carry only
  `core`; memory-search lines only `mem` (the core value is already proven by then); soak lines
  both, so a soak freeze lowers both ceilings.
- Malformed lines (a half-written last line after a power cut) are ignored.
- `id` = highest id in the file + 1.
- Power limits are not journaled: a lower power limit does not freeze a card.
- The file only grows, a few hundred bytes per run. `ponytail:` no rotation; add it if it ever
  matters.

Core stays platform-free: `core::Journal` builds and parses lines and exposes
`std::function<bool(const std::string&)> append` / the loaded lines; `hw/journal_file.*` does
the directory creation and the durable append.

## 6. Hardware seam changes

- `GpuControl::set_power_limit(int pct)` — implemented via NVML
  (`nvmlDeviceSetPowerManagementLimit`, milliwatts from `pct` of
  `nvmlDeviceGetPowerManagementDefaultLimit`), verified by reading
  `nvmlDeviceGetPowerManagementLimit` back.
- New `std::function<std::pair<int, int>()> power_limit_range_pct` — min/max from
  `nvmlDeviceGetPowerManagementLimitConstraints`, as percent of the default limit.
- `reset_to_stock` also restores the default power limit when power control is available, and
  `gao --reset` does the same.
- Empty callback = not supported, as before.

## 7. Components

```
src/core/journal.*     line building/parsing, ceilings, next id. Pure.
src/core/search.*      highest_stable, apply_margin, optimize(). Pure; hardware via
                       GpuControl, probe via std::function<StabilityResult(double s, int max_temp)>,
                       abort via std::function<bool()>, output via std::function<void(std::string)>.
src/hw/nvml.*          power limit: default, constraints, set, read back.
src/hw/gpu_control.*   wire set_power_limit, power_limit_range_pct, reset incl. power.
src/hw/journal_file.*  create %LOCALAPPDATA%\GpuAutoOptimizer, durable append, read lines.
src/app/main.cpp       gao --optimize [best|quiet|cool|max]; elevation check; Ctrl+C handler.
```

`optimize()` returns:

```cpp
struct OptimizeResult {
    bool ok = false;
    std::string reason;          // why it stopped, when !ok
    int power_pct = 100;
    int core_mhz = 0, mem_mhz = 0;
    int core_max_stable = 0, mem_max_stable = 0;
    StabilityResult baseline, soak;
};
```

## 8. Safety and error handling

- **Ctrl+C:** a console control handler sets an atomic abort flag and returns TRUE (the
  process is not killed). `optimize()` checks the flag between probes; when set, it calls
  `reset_to_stock` and returns `!ok` with reason "aborted". Worst-case latency: one probe (20 s
  during the power step).
- **Any failed set or read-back mismatch:** `reset_to_stock`, return `!ok` with the reason.
- **Not elevated:** `gao --optimize` says so and exits 1 before touching anything.
- **Any exit path with `!ok`** leaves the card at stock.

## 9. Testing

Unit tests (CI, no GPU) with a fake `GpuControl` and a fake probe that models a card
("stable up to core +150 and mem +800; temp = 40 + power_pct × 0.3; score ∝ power up to 90 %"):

- `highest_stable`: finds the edge; respects the ceiling; never probes `lo`; single-value range.
- `apply_margin`: the table from §4 (1.0 → max − step, 0.7, 0.4, max 0 → 0).
- Journal: build/parse round-trip; unmatched begin → ceiling; matched begin → none; soak line
  lowers both; malformed line ignored; next id.
- `optimize()`:
  - converges to the modelled edges and applies the margin;
  - respects journal ceilings;
  - writes `begin` before calling the setter (the fake setter checks the journal);
  - baseline not STABLE → `!ok`, stock;
  - soak fails once → one step down, then ok; fails 4 times → stock, `!ok`;
  - abort flag → stock, `!ok`, "aborted";
  - empty `set_power_limit` → power step skipped;
  - quiet preset picks the lowest power within 2 % score; max preset picks the thermal cap;
  - a setter returning false → stock, `!ok`.

New rows in `docs/hardware-checks.md`:

| # | Check | Expected |
|---|---|---|
| 13 | `gao --optimize best` (elevated) | Result applied, soak STABLE; a following `gao --stress 60` STABLE |
| 14 | Add `{"id":999,"core":150,"state":"begin"}` to the journal, run `gao --optimize best` | Warns about the freeze at core +150; no core candidate ≥ +150 is tried |
| 15 | Ctrl+C during the core search | Card at stock: `gao --reset` read-back 0/0, power at default |
| 16 | Power limit set + read back (via `--optimize quiet` log or a direct run) | Read-back matches the requested watts |

## 10. Out of scope

Fan curve (unsupported on the reference card; code skips it), undervolt, persistence and
boot-apply (P4), UI (P5), multi-GPU.
