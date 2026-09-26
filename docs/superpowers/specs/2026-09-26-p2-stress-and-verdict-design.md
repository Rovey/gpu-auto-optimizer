# P2 — Stress Load and Stability Verdict

Date: 2026-09-26
Status: approved (design), not yet implemented
Parent: `2026-09-20-cpp-rewrite-design.md` §5 (this refines it; nothing here contradicts it)

## 1. Goal

A DX11 compute load that pulls the GPU to its power limit, checks every result it
computes, and turns a run into one verdict plus a score. P3's search calls this once
per candidate (~3 s probe); the final soak calls it for 60 s. P2 also exposes it on the
command line as `gao --stress`, which finally lets hardware checks 3 and 5 do their
boost-clock cross-check under load.

Success criteria:
- `gao --stress 60` on the stock RTX 4070 reports `STABLE` and draws ≥ 95 % of the power limit.
- A corrupted result, a TDR, overheating and a lost temperature sensor each produce their
  own verdict and stop the run immediately.
- All verdict logic is unit-tested in CI without a GPU.

## 2. Correctness check: floats that stay exact

Inputs are small integers stored as `float`, drawn from `-8..8` by a fixed-seed generator.
For `N = 1024`, the largest possible dot product is `1024 · 8 · 8 = 65536`, far below `2^24`,
so every FP32 product and partial sum is exactly representable. The result is therefore
identical regardless of summation order or FMA use, and a CPU-computed reference is exact.

Rejected alternatives:
- *Golden reference from a stock run* — needs bit-identical GPU output across runs (likely,
  not guaranteed), costs a stock run per session, and is wrong if stock itself is unstable.
- *Integer matmul* — exact, but does not load the FP32 units a clock offset stresses, and
  draws less power.

A `static_assert` in `stress_math` enforces `N · 64 < 2^24`, so a later change to `N` or the
value range cannot silently break exactness.

## 3. Components

```
src/core/stress_math.*   input generation (fixed seed, ints -8..8 as float) and the
                         CPU reference matmul. Pure, unit-tested.
src/core/stability.*     the run loop and the verdict. Reaches hardware only through
                         callbacks, like GpuControl. Unit-tested with fake batches.
src/hw/stress.*          D3D11: device on the NVIDIA adapter (vendor id 0x10DE),
                         shader compile, batch dispatch, TDR handling.
src/app/main.cpp         new command: gao --stress <sec> [--max-temp <c>]
```

The seam between core and hw:

```cpp
struct StressBatch {            // result of one hw batch (~250 ms of GPU work)
    long long iterations;       // matmuls completed in this batch
    int  wrong_values;          // counted on the GPU against the reference
    bool device_lost;           // DXGI_ERROR_DEVICE_REMOVED (TDR)
    double elapsed_ms;
};

enum class Verdict { Stable, WrongResult, DeviceLost, TooHot, NoTelemetry };

struct StabilityResult {
    Verdict verdict;
    double  score;              // iterations per second over the whole run
    double  seconds;            // run time actually covered
    int     peak_temp_c;
    int     avg_power_w;
    int     avg_core_mhz;
    int     avg_mem_mhz;
};

StabilityResult run_stability(std::function<StressBatch()> batch,
                              std::function<Telemetry()> read,
                              double seconds, int max_temp_c);
```

## 4. Data flow

1. `hw::Stress::Init` creates a D3D11 device on the first adapter with vendor id `0x10DE`,
   compiles the HLSL (embedded as a string in `stress.cpp`) with `D3DCompile`
   (`d3dcompiler_47.dll` ships with Windows 10/11), and uploads A, B and the reference C.
2. Calibration: the number of dispatches per batch doubles from 1 until one batch takes
   ≥ 250 ms. A single dispatch (~2 GFLOP) is ~1 ms, far below the 2 s TDR limit.
3. Each dispatch runs a tiled matmul (`groupshared` 16×16 tiles, `numthreads(16,16)`),
   compares every output element against the reference, and `InterlockedAdd`s mismatches
   into a counter. Only that 4-byte counter is read back per batch; the readback also
   synchronises, so `elapsed_ms` is real GPU time.
4. `run_stability` loops: call `batch()`, then `read()`, then judge. Run time is the sum of
   `elapsed_ms`, not the wall clock, which keeps the tests deterministic.
5. It stops at the first of: `wrong_values > 0` → WrongResult; `device_lost` → DeviceLost;
   `!telemetry.ok` → NoTelemetry; `temp_c > max_temp_c` → TooHot; accumulated time ≥
   `seconds` → Stable. No further batch runs after a stop.

The power target (≥ 95 % of the limit) is measured, not assumed. If the first shader falls
short on the 4070, the fix is more arithmetic per memory load (larger tiles, several outputs
per thread), measured again.

## 5. Error handling

- **Device lost:** the batch reports `device_lost = true`; `hw::Stress` releases the device and
  recreates it on its next batch. Core never deals with D3D.
- **No telemetry:** stop. Stress without a working temperature reading is not allowed to
  continue — the thermal abort would be blind.
- **Init failure** (no NVIDIA adapter, shader compile error, no NVML): the CLI prints the
  reason and exits 1. Nothing was written to the GPU, so there is nothing to undo.
- **Ctrl+C:** the stress load changes no settings, so killing it is always safe. No handler.

## 6. CLI

```
gao --stress 60 [--max-temp 85]
  t=1s  score=812 it/s  core=2745 MHz  mem=10501 MHz  temp=58 C  power=196/200 W  errors=0
  ...
VERDICT: STABLE  score=815 it/s  peak=67 C  avg power=197 W
```

One line per second of accumulated run time. `--max-temp` defaults to 85. Exit codes:
`0` Stable, `2` WrongResult or DeviceLost, `3` TooHot or NoTelemetry, `1` init failure.

Hidden self-test flag for hardware checks that stock hardware cannot trigger:
- `--stress-selftest wrong` flips one value of the uploaded reference; the run must end
  in WrongResult. This proves the whole chain from shader counter to verdict.
- `--stress-selftest tdr` issues one deliberately long dispatch (> 2 s) so Windows resets the
  driver (screen black for 1–2 s, then recovers); the run must end in DeviceLost, and a
  following `gao --stress` must work again. Approved by the user; it touches no settings.

## 7. Testing

Unit tests (`core_tests`, CI, no GPU):
- `stress_math`: same seed → same input; all values in `-8..8`; the reference equals a naive
  `double` computation on a small matrix.
- `stability`, with fake batches:
  - 12 good batches of 250 ms → Stable, `seconds == 3`, score = total iterations / 3;
  - stops at the first WrongResult / DeviceLost / TooHot / NoTelemetry, and no batch is
    called after the stop;
  - averages and peak temperature are computed from the telemetry actually read.

New rows in `docs/hardware-checks.md`:

| # | Check | Expected |
|---|---|---|
| 10 | `gao --stress 60` on stock | STABLE, avg power ≥ 95 % of limit |
| 11 | `gao --stress 10 --stress-selftest wrong` | WrongResult, exit 2 |
| 12 | `gao --stress 10 --stress-selftest tdr` | DeviceLost, exit 2; next `--stress` works |

Checks 3 and 5 get their open cross-check completed: run `gao --stress` in a second window
and compare clocks before and after `--set-core 25` / `--set-mem 100`.

## 8. Out of scope for P2

The search, the journal and anything that changes settings (P3); the UI (P5). `run_stability`
is the only entry point P3 needs.
