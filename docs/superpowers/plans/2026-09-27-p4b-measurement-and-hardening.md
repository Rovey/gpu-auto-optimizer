# GPU Auto Optimizer — P4b (Measurement Fixes and Boot-Apply Hardening) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The memory search picks the bandwidth peak instead of the stability ceiling, every applied edge passes a 30 s confirmation, and boot-apply runs an admin-only copy of the exe against admin-only state with sane task settings and no DLL planting.

**Architecture:** Two new pure search primitives (`best_bandwidth_offset`, `confirm_edge`) and a pure `boot_task_xml` live in `core` and are unit-tested; `optimize()` uses them through a new `OptimizeIo::bandwidth` callback. `hw::Stress` gains a DX11 copy kernel timed with GPU timestamp queries. `hw/app_files` moves all state to a protected `%ProgramData%\GpuAutoOptimizer\`; `hw/boot_task` installs the exe into Program Files and registers the task from XML. `main` calls `SetDefaultDllDirectories` first and delay-loads `d3dcompiler_47.dll`.

**Tech Stack:** C++20, MSVC, CMake ≥ 3.28, doctest, D3D11 (compute + timestamp queries), Win32 security APIs (`ConvertStringSecurityDescriptorToSecurityDescriptorW`, `GetNamedSecurityInfoW`, `SetNamedSecurityInfoW`), `schtasks /XML`.

**Spec:** `docs/superpowers/specs/2026-09-27-p4b-measurement-and-hardening-design.md`

## Global Constraints

- English only in the repository (see `CLAUDE.md`). C++20, MSVC, x64. `src/core/` has no Windows/NVML/NVAPI/D3D headers.
- Every hardware write verified by read-back; every clock candidate journaled before it is applied; any `!ok` ends at stock.
- Constants: core step 15 / max 300; mem step 50 / max 1500; clock probe 3 s; confirm probe 30 s; confirm tries 3; bandwidth drop 1 % (`kBandwidthDrop = 0.01`); bandwidth tie 0.5 % (`kBandwidthTie = 0.005`); bandwidth median of 3 runs.
- State folder `%ProgramData%\GpuAutoOptimizer\`, SDDL `O:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1200a9;;;BU)`; refuse if it exists with an owner other than Administrators/SYSTEM.
- Installed exe `%ProgramFiles%\GpuAutoOptimizer\gao.exe`; task `\GpuAutoOptimizer\BootApply`; legacy task `\GpuAutoOptimizer` removed by `--boot on/off`.
- `cmake`/`ctest` live in `C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\`.
- Push after every commit.

## Review Focus

1. **Bandwidth noise on a flat curve** — the result must not creep upward on noise; the lowest offset within 0.5 % of the best wins. (Task 1 test: "a flat curve keeps the lowest offset".)
2. **A bandwidth measurement that fails** (device lost mid-measurement) — must count as unusable and never raise memory. (Task 2 test: "a failing bandwidth measurement never raises memory".)
3. **The state folder pre-created by a normal user** before the first elevated run — `gao` must refuse, not adopt it. (Task 6 manual step 1.)
4. **An exe path with XML special characters** (`&`, `<`, `>`, `"`) in the task XML. (Task 4 test: "special characters in the path are escaped".)
5. **`--boot on` run from the installed copy itself** — copying a file onto itself must not fail the command. (Task 5 code: `equivalent` check; Task 6 manual step.)

---

## File Structure

| File | Change |
|---|---|
| `src/core/search.hpp/.cpp` | + `MemSample`, `best_bandwidth_offset`, `confirm_edge`, constants; `OptimizeIo::bandwidth`; `OptimizeResult::core_confirmed/mem_confirmed`; `optimize()` uses both |
| `src/core/task_xml.hpp/.cpp` | new: `boot_task_xml` |
| `src/hw/stress.hpp/.cpp` | + `MeasureBandwidth()` |
| `src/hw/app_files.hpp/.cpp` | ProgramData, `ensure_app_dir`, `legacy_journal_path` |
| `src/hw/boot_task.hpp/.cpp` | install/uninstall exe, XML task create, legacy removal, UTF-16 writer, user SID, `files_equal` |
| `src/app/main.cpp` | DLL setup, `--bandwidth`, wiring, state prep + migration, `--boot` rewrite, status lines |
| `CMakeLists.txt` | new sources; delay-load flags |
| `tests/test_search.cpp`, `tests/test_task_xml.cpp` | tests |
| `docs/hardware-checks.md`, `README.md` | rows 23–28, paths |

---

### Task 1: Bandwidth scan and edge confirmation primitives

**Files:** Modify `src/core/search.hpp`, `src/core/search.cpp`, `tests/test_search.cpp`

**Interfaces — Produces:**
```cpp
inline constexpr double kBandwidthDrop = 0.01;
inline constexpr double kBandwidthTie = 0.005;
struct MemSample { bool stable = false; double gbps = 0; };
int best_bandwidth_offset(int lo, int hi, int step, int ceiling, const std::function<MemSample(int)>& sample);
int confirm_edge(int edge, int lo, int step, int tries, const std::function<bool(int)>& holds);
```

- [ ] **Step 1: Failing tests** — append to `tests/test_search.cpp`:
```cpp
TEST_CASE("bandwidth scan stops at the peak of a rising-then-falling curve") {
    int max_sampled = -1;
    const int r = best_bandwidth_offset(0, 1500, 50, INT_MAX, [&](int v) {
        max_sampled = std::max(max_sampled, v);
        return MemSample{true, v <= 600 ? 500 + v * 0.1 : 560 - (v - 600) * 0.3};
    });
    CHECK(r == 600);
    CHECK(max_sampled == 650);   // stopped at the first step more than 1 % below the peak
}

TEST_CASE("bandwidth scan stops at the first unstable step") {
    int max_sampled = -1;
    const int r = best_bandwidth_offset(0, 1500, 50, INT_MAX, [&](int v) {
        max_sampled = std::max(max_sampled, v);
        return MemSample{v <= 400, 500 + v * 0.1};
    });
    CHECK(r == 400);
    CHECK(max_sampled == 450);
}

TEST_CASE("a flat curve keeps the lowest offset") {
    const int r = best_bandwidth_offset(0, 1500, 50, INT_MAX, [](int v) {
        return MemSample{true, 500.0 + ((v / 50) % 2)};   // +-1 GB/s noise
    });
    CHECK(r == 0);
}

TEST_CASE("bandwidth scan samples lo and respects the ceiling") {
    std::vector<int> sampled;
    const int r = best_bandwidth_offset(0, 1500, 50, 300, [&](int v) {
        sampled.push_back(v);
        return MemSample{true, 500 + v * 0.1};
    });
    REQUIRE_FALSE(sampled.empty());
    CHECK(sampled.front() == 0);
    CHECK(sampled.back() == 250);
    CHECK(r == 250);
    CHECK(best_bandwidth_offset(0, 1500, 50, 300, [](int) { return MemSample{false, 0}; }) == 0);
}

TEST_CASE("confirm_edge keeps a holding edge and steps down otherwise") {
    int calls = 0;
    CHECK(confirm_edge(150, 0, 15, 3, [&](int) { ++calls; return true; }) == 150);
    CHECK(calls == 1);
    CHECK(confirm_edge(150, 0, 15, 3, [](int v) { return v <= 135; }) == 135);
    calls = 0;
    CHECK(confirm_edge(150, 0, 15, 3, [&](int) { ++calls; return false; }) == 0);
    CHECK(calls == 3);
    calls = 0;
    CHECK(confirm_edge(0, 0, 15, 3, [&](int) { ++calls; return false; }) == 0);
    CHECK(calls == 0);
    CHECK(confirm_edge(15, 0, 15, 3, [](int) { return false; }) == 0);
}
```

- [ ] **Step 2: Run** `cmake --build build --config Debug` → FAIL: `best_bandwidth_offset` / `MemSample` undeclared.

- [ ] **Step 3: Implement** — in `src/core/search.hpp`, after `apply_margin`:
```cpp
// Memory overclocks fail by losing bandwidth before they fail by producing
// errors: GDDR6X/GDDR6 retry bad transfers. The memory search therefore looks
// for the bandwidth peak, not the stability ceiling.
inline constexpr double kBandwidthDrop = 0.01;   // stop once 1 % below the best
inline constexpr double kBandwidthTie = 0.005;   // within 0.5 % of the best counts as the best

struct MemSample {
    bool stable = false;
    double gbps = 0;
};

// Scans lo, lo+step, ... (<= hi, < ceiling), sampling lo too. Stops at the
// first unstable sample or once gbps falls more than kBandwidthDrop below the
// best so far. Returns the lowest stable offset within kBandwidthTie of the
// best, so noise on a flat curve never drifts the result upward; lo when no
// sample was stable.
int best_bandwidth_offset(int lo, int hi, int step, int ceiling, const std::function<MemSample(int)>& sample);

// "Search fast, confirm long": re-checks edge with a long probe; on failure
// steps down by step and retries, at most `tries` probes, never probing lo
// (stock). Returns the first value that holds, or lo.
int confirm_edge(int edge, int lo, int step, int tries, const std::function<bool(int)>& holds);
```
In `src/core/search.cpp` add `#include <utility>` and `#include <vector>`, and after `apply_margin`:
```cpp
int best_bandwidth_offset(int lo, int hi, int step, int ceiling, const std::function<MemSample(int)>& sample) {
    const int top = std::min(hi, ceiling - 1);
    std::vector<std::pair<int, double>> seen;
    double best = 0;
    for (int v = lo; v <= top; v += step) {
        const MemSample s = sample(v);
        if (!s.stable) break;
        seen.emplace_back(v, s.gbps);
        best = std::max(best, s.gbps);
        if (s.gbps < best * (1 - kBandwidthDrop)) break;
    }
    for (const auto& [v, gbps] : seen)
        if (gbps >= best * (1 - kBandwidthTie)) return v;
    return lo;
}

int confirm_edge(int edge, int lo, int step, int tries, const std::function<bool(int)>& holds) {
    for (int t = 0, v = edge; t < tries && v > lo; ++t, v -= step)
        if (holds(v)) return v;
    return lo;
}
```

- [ ] **Step 4: Run** `cmake --build build --config Debug; .\build\Debug\core_tests.exe` → `Status: SUCCESS!`.

- [ ] **Step 5: Commit + push** — `feat(core): bandwidth-peak scan and long-probe edge confirmation`.

---

### Task 2: `optimize()` uses bandwidth and confirmation

**Files:** Modify `src/core/search.hpp`, `src/core/search.cpp`, `tests/test_search.cpp`

**Interfaces — Consumes:** Task 1. **Produces:**
```cpp
// OptimizeIo:
std::function<std::optional<double>()> bandwidth;   // GB/s at the current settings; nullopt = failed
// OptimizeResult:
int core_confirmed = 0;
int mem_confirmed = 0;
```

- [ ] **Step 1: Failing tests** — in `FakeCard` add fields
```cpp
    std::vector<int> confirm_fail_core;   // a 30 s probe at these core offsets fails
    std::function<double(int)> bw_curve;  // GB/s by memory offset; empty = no bandwidth
    bool bw_fails = false;                // every bandwidth measurement fails
    int max_mem_seen = 0;
```
change `g.set_mem_offset` to `[this](int v) { mem = v; max_mem_seen = std::max(max_mem_seen, v); return true; }`, and in the probe lambda, before the `if (stock_unstable || ...)` line, add:
```cpp
            if (seconds == 30 && std::find(confirm_fail_core.begin(), confirm_fail_core.end(), core) != confirm_fail_core.end()) {
                r.verdict = Verdict::WrongResult;
                return r;
            }
```
In `Run::go`, after `io.log = ...`, add:
```cpp
        if (card.bw_curve)
            io.bandwidth = [this]() -> std::optional<double> {
                if (card.bw_fails) return std::nullopt;
                return card.bw_curve(card.mem);
            };
```
In the existing test "best preset converges to the card's edges and applies the margin" add:
```cpp
    CHECK(r.core_confirmed == 150);
    CHECK(r.mem_confirmed == 800);
```
Append:
```cpp
TEST_CASE("memory search stops at the bandwidth peak, not the stability edge") {
    Run run;
    run.card.mem_edge = 1400;
    run.card.bw_curve = [](int m) { return m <= 900 ? 500 + m * 0.1 : 590 - (m - 900) * 0.2; };
    const auto r = run.go(Preset::BestOfMyGpu);
    REQUIRE(r.ok);
    CHECK(r.mem_max_stable == 900);
    CHECK(r.mem_confirmed == 900);
    CHECK(r.mem_mhz == 600);
    CHECK(run.card.max_mem_seen <= 950);
}

TEST_CASE("the margin applies to the confirmed core edge") {
    Run run;
    run.card.confirm_fail_core = {150};
    const auto r = run.go(Preset::BestOfMyGpu);
    REQUIRE(r.ok);
    CHECK(r.core_max_stable == 150);
    CHECK(r.core_confirmed == 135);
    CHECK(r.core_mhz == 90);
}

TEST_CASE("a failing bandwidth measurement never raises memory") {
    Run run;
    run.card.bw_curve = [](int m) { return 500 + m * 0.1; };
    run.card.bw_fails = true;
    const auto r = run.go(Preset::BestOfMyGpu);
    REQUIRE(r.ok);
    CHECK(r.mem_mhz == 0);
}
```

- [ ] **Step 2: Run** → FAIL: `core_confirmed` / `bandwidth` not members.

- [ ] **Step 3: Implement**

`search.hpp`: add `#include <optional>`; in `OptimizeIo` after `log`:
```cpp
    // GB/s at the currently applied settings; nullopt when the measurement
    // failed. Empty: the memory search falls back to stability only.
    std::function<std::optional<double>()> bandwidth;
```
In `OptimizeResult` after `mem_max_stable`:
```cpp
    int core_confirmed = 0;   // edge that held a 30 s probe; the margin applies to this
    int mem_confirmed = 0;
```

`search.cpp`: constants block add
```cpp
constexpr double kConfirmProbeS = 30;
constexpr int kConfirmTries = 3;
```
Replace the `clock_candidate` lambda with:
```cpp
    // One clock candidate: journal first, then hardware, then the probe.
    // `extra` runs while the candidate is still applied and stable (e.g. a
    // bandwidth measurement), before the journal entry is closed.
    auto clock_candidate = [&](std::optional<int> core_j, std::optional<int> mem_j, int core, int mem,
                               double seconds, const std::function<void()>& extra = {}) {
        if (!stopped.empty()) return false;
        if (io.aborted && io.aborted()) { stopped = "aborted"; return false; }
        const int id = journal.begin(core_j, mem_j);
        if (id < 0) { stopped = "could not write the journal"; return false; }
        if (!set_state(r.power_pct, core, mem)) { journal.complete(id, "SET FAILED"); return false; }
        const auto s = probe(seconds, obj.max_temp_c);
        const bool stable = s && s->verdict == Verdict::Stable;
        if (stable && extra) extra();
        journal.complete(id, s ? verdict_name(s->verdict) : "NOT RUN");
        if (!s) return false;
        log(std::string(seconds == kConfirmProbeS ? "confirm " : "") + "core +" + std::to_string(core) +
            " / mem +" + std::to_string(mem) + ": " + describe(*s));
        return stable;
    };
```
Replace the `if (obj.core_oc) { ... }` and `if (obj.mem_oc) { ... }` blocks with:
```cpp
    if (obj.core_oc) {
        r.core_max_stable = highest_stable(0, kCoreMax, kCoreStep, journal.ceilings().core_mhz,
                                           [&](int v) { return clock_candidate(v, std::nullopt, v, 0, kClockProbeS); });
        if (!stopped.empty()) return finish_fail(stopped);
        r.core_confirmed = confirm_edge(r.core_max_stable, 0, kCoreStep, kConfirmTries, [&](int v) {
            return clock_candidate(v, std::nullopt, v, 0, kConfirmProbeS);
        });
        if (!stopped.empty()) return finish_fail(stopped);
        r.core_mhz = apply_margin(r.core_confirmed, kCoreStep, obj.perf_push);
        log("core: highest stable +" + std::to_string(r.core_max_stable) + ", confirmed +" +
            std::to_string(r.core_confirmed) + ", applying +" + std::to_string(r.core_mhz));
    }
    if (obj.mem_oc) {
        const int ceiling = journal.ceilings().mem_mhz;
        if (io.bandwidth) {
            r.mem_max_stable = best_bandwidth_offset(0, kMemMax, kMemStep, ceiling, [&](int v) {
                MemSample m;
                std::optional<double> gbps;
                m.stable = clock_candidate(std::nullopt, v, r.core_mhz, v, kClockProbeS, [&] { gbps = io.bandwidth(); });
                // A failed measurement (device lost) makes the step unusable.
                if (m.stable && !gbps) m.stable = false;
                if (m.stable) {
                    m.gbps = *gbps;
                    char buf[64];
                    std::snprintf(buf, sizeof(buf), "mem +%d: bandwidth %.1f GB/s", v, m.gbps);
                    log(buf);
                }
                return m;
            });
        } else {
            r.mem_max_stable = highest_stable(0, kMemMax, kMemStep, ceiling, [&](int v) {
                return clock_candidate(std::nullopt, v, r.core_mhz, v, kClockProbeS);
            });
        }
        if (!stopped.empty()) return finish_fail(stopped);
        r.mem_confirmed = confirm_edge(r.mem_max_stable, 0, kMemStep, kConfirmTries, [&](int v) {
            return clock_candidate(std::nullopt, v, r.core_mhz, v, kConfirmProbeS);
        });
        if (!stopped.empty()) return finish_fail(stopped);
        r.mem_mhz = apply_margin(r.mem_confirmed, kMemStep, obj.perf_push);
        log(std::string("mem: ") + (io.bandwidth ? "bandwidth peak +" : "highest stable +") +
            std::to_string(r.mem_max_stable) + ", confirmed +" + std::to_string(r.mem_confirmed) +
            ", applying +" + std::to_string(r.mem_mhz));
    }
```

- [ ] **Step 4: Run** `cmake --build build --config Debug; .\build\Debug\core_tests.exe` → `Status: SUCCESS!` (all existing tests still pass; recompute an expectation from the FakeCard model before touching code if one differs).

- [ ] **Step 5: Commit + push** — `feat(core): memory search by bandwidth peak; 30 s confirmation of both edges`.

---

### Task 3: GPU bandwidth measurement

**Files:** Modify `src/hw/stress.hpp`, `src/hw/stress.cpp`, `src/app/main.cpp`

**Interfaces — Produces:** `std::optional<double> Stress::MeasureBandwidth();` and CLI `gao --bandwidth` (prints GB/s, no elevation). **Consumes:** `OptimizeIo::bandwidth` (Task 2).

No unit test (hardware). Deliverable: builds warning-free; `gao --bandwidth` prints a plausible number (RTX 4070 datasheet: 504 GB/s; expect roughly 350–500).

- [ ] **Step 1: Header** — in `src/hw/stress.hpp` add `#include <optional>` and public:
```cpp
    // Device-memory bandwidth in GB/s (read + write) from a 256 MB buffer copy,
    // timed with GPU timestamps; median of 3 runs. nullopt when the device was
    // lost or the timing was unusable.
    std::optional<double> MeasureBandwidth();
```

- [ ] **Step 2: Implementation** — in `src/hw/stress.cpp`:

Add to the anonymous namespace:
```cpp
constexpr UINT kBwBytes = 256u * 1024 * 1024;   // per buffer; far above the 4070's 36 MB L2
constexpr UINT kBwGroups = 1024;
constexpr int kBwDispatches = 64;
constexpr int kBwRuns = 3;

// Grid-stride copy of 16-byte elements between two raw buffers.
const char kCopyShader[] = R"(
#define COUNT (256 * 1024 * 1024 / 16)
#define THREADS (1024 * 256)
RWByteAddressBuffer Src : register(u0);
RWByteAddressBuffer Dst : register(u1);
[numthreads(256, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    for (uint i = id.x; i < COUNT; i += THREADS) Dst.Store4(i * 16, Src.Load4(i * 16));
}
)";
```
Add to `Stress::Impl`:
```cpp
    ComPtr<ID3D11ComputeShader> copy_cs;
    ComPtr<ID3D11Buffer> bw_src, bw_dst;
    ComPtr<ID3D11UnorderedAccessView> bw_src_uav, bw_dst_uav;
    ComPtr<ID3D11Query> q_disjoint, q_begin, q_end;
```
In `CreateDevice()`, extend the reset line with `d.copy_cs.Reset(); d.bw_src.Reset(); d.bw_dst.Reset(); d.bw_src_uav.Reset(); d.bw_dst_uav.Reset(); d.q_disjoint.Reset(); d.q_begin.Reset(); d.q_end.Reset();`.

Add before the closing namespace brace:
```cpp
std::optional<double> Stress::MeasureBandwidth() {
    Impl& d = *impl_;
    if (!d.device && !CreateDevice()) return std::nullopt;
    if (!d.copy_cs) {   // created lazily: the stress path never needs 512 MB of buffers
        ComPtr<ID3DBlob> code, log;
        HRESULT hr = D3DCompile(kCopyShader, sizeof(kCopyShader) - 1, "copy.hlsl", nullptr, nullptr, "main",
                                "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &log);
        if (FAILED(hr)) { error_ = Hr("D3DCompile (copy)", hr); return std::nullopt; }
        if (FAILED(hr = d.device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &d.copy_cs))) {
            error_ = Hr("CreateComputeShader (copy)", hr); return std::nullopt;
        }
        auto make = [&](ComPtr<ID3D11Buffer>& buf, ComPtr<ID3D11UnorderedAccessView>& uav) {
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = kBwBytes;
            bd.Usage = D3D11_USAGE_DEFAULT;
            bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
            bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
            HRESULT h = d.device->CreateBuffer(&bd, nullptr, &buf);
            if (FAILED(h)) return h;
            D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
            ud.Format = DXGI_FORMAT_R32_TYPELESS;
            ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
            ud.Buffer.NumElements = kBwBytes / 4;
            ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
            return d.device->CreateUnorderedAccessView(buf.Get(), &ud, &uav);
        };
        if (FAILED(hr = make(d.bw_src, d.bw_src_uav)) || FAILED(hr = make(d.bw_dst, d.bw_dst_uav))) {
            error_ = Hr("bandwidth buffers", hr); return std::nullopt;
        }
        D3D11_QUERY_DESC qd{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
        D3D11_QUERY_DESC qt{D3D11_QUERY_TIMESTAMP, 0};
        if (FAILED(hr = d.device->CreateQuery(&qd, &d.q_disjoint)) || FAILED(hr = d.device->CreateQuery(&qt, &d.q_begin)) ||
            FAILED(hr = d.device->CreateQuery(&qt, &d.q_end))) {
            error_ = Hr("timestamp queries", hr); return std::nullopt;
        }
    }
    auto wait = [&](ID3D11Asynchronous* q, void* out, UINT size) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        for (;;) {
            const HRESULT hr = d.ctx->GetData(q, out, size, 0);
            if (hr == S_OK) return true;
            if (FAILED(hr) || d.device->GetDeviceRemovedReason() != S_OK ||
                std::chrono::steady_clock::now() > deadline) return false;
            Sleep(1);
        }
    };
    ID3D11UnorderedAccessView* uavs[] = {d.bw_src_uav.Get(), d.bw_dst_uav.Get()};
    ID3D11UnorderedAccessView* none[] = {nullptr, nullptr};
    std::vector<double> runs;
    for (int run = 0; run < kBwRuns; ++run) {
        d.ctx->CSSetShader(d.copy_cs.Get(), nullptr, 0);
        d.ctx->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
        d.ctx->Begin(d.q_disjoint.Get());
        d.ctx->End(d.q_begin.Get());
        for (int i = 0; i < kBwDispatches; ++i) d.ctx->Dispatch(kBwGroups, 1, 1);
        d.ctx->End(d.q_end.Get());
        d.ctx->End(d.q_disjoint.Get());
        d.ctx->CSSetUnorderedAccessViews(0, 2, none, nullptr);
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
        UINT64 t0 = 0, t1 = 0;
        if (!wait(d.q_disjoint.Get(), &dj, sizeof(dj)) || !wait(d.q_begin.Get(), &t0, sizeof(t0)) ||
            !wait(d.q_end.Get(), &t1, sizeof(t1))) {
            if (d.device->GetDeviceRemovedReason() != S_OK) d.device.Reset();
            return std::nullopt;
        }
        if (dj.Disjoint || dj.Frequency == 0 || t1 <= t0) continue;   // clock changed mid-run: discard
        const double seconds = double(t1 - t0) / double(dj.Frequency);
        runs.push_back(2.0 * kBwBytes * kBwDispatches / seconds / 1e9);
    }
    if (runs.empty()) return std::nullopt;
    std::sort(runs.begin(), runs.end());
    return runs[runs.size() / 2];
}
```
Add `#include <algorithm>`, `#include <vector>` if missing.

- [ ] **Step 3: Wire into the app** — in `src/app/main.cpp`:
- in `optimize()` after `io.log = ...;`:
```cpp
    io.bandwidth = [&] { return load.MeasureBandwidth(); };
```
- change the `RESULT:` printf to:
```cpp
    std::printf("RESULT: power %d %%, core +%d MHz (confirmed +%d), mem +%d MHz (confirmed +%d)\n",
                r.power_pct, r.core_mhz, r.core_confirmed, r.mem_mhz, r.mem_confirmed);
```
- add a command before `main()`:
```cpp
static int bandwidth() {
    gao::Stress load;
    if (!load.Init()) { std::printf("stress init failed: %s\n", load.Error().c_str()); return 1; }
    const auto gbps = load.MeasureBandwidth();
    if (!gbps) { std::printf("bandwidth measurement failed: %s\n", load.Error().c_str()); return 1; }
    std::printf("memory bandwidth: %.1f GB/s (%s)\n", *gbps, load.AdapterName().c_str());
    return 0;
}
```
and in `main()`: `if (argc > 1 && std::strcmp(argv[1], "--bandwidth") == 0) return bandwidth();`; add `| --bandwidth` to the usage line after `--stress ...`.

- [ ] **Step 4: Build and smoke** — `cmake --build build --config Release; ctest --test-dir build -C Release`; then `.\build\Release\gao.exe --bandwidth` three times → a plausible GB/s each time, spread within ~1 %.

- [ ] **Step 5: Commit + push** — `feat(hw): measure memory bandwidth with a timed DX11 copy kernel`.

---

### Task 4: Task XML

**Files:** Create `src/core/task_xml.hpp`, `src/core/task_xml.cpp`, `tests/test_task_xml.cpp`; modify `CMakeLists.txt`

**Interfaces — Produces:** `std::string boot_task_xml(const std::string& exe_path_utf8, const std::string& user_id);`

- [ ] **Step 1: Failing tests** — `tests/test_task_xml.cpp`:
```cpp
#include "doctest/doctest.h"
#include "core/task_xml.hpp"
#include <string>

using namespace gao;

namespace {
bool has(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }
}

TEST_CASE("the boot task XML carries every required setting") {
    const std::string x = boot_task_xml(R"(C:\Program Files\GpuAutoOptimizer\gao.exe)", "S-1-5-21-1-2-3-1001");
    CHECK(has(x, "<LogonTrigger>"));
    CHECK(has(x, "<Delay>PT15S</Delay>"));
    CHECK(has(x, "<UserId>S-1-5-21-1-2-3-1001</UserId>"));
    CHECK(has(x, "<LogonType>InteractiveToken</LogonType>"));
    CHECK(has(x, "<RunLevel>HighestAvailable</RunLevel>"));
    CHECK(has(x, "<DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>"));
    CHECK(has(x, "<StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>"));
    CHECK(has(x, "<ExecutionTimeLimit>PT5M</ExecutionTimeLimit>"));
    CHECK(has(x, "<MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>"));
    CHECK(has(x, R"(<Command>C:\Program Files\GpuAutoOptimizer\gao.exe</Command>)"));
    CHECK(has(x, "<Arguments>--boot-apply</Arguments>"));
    CHECK(x.rfind("<?xml", 0) == 0);
}

TEST_CASE("special characters in the path are escaped") {
    const std::string x = boot_task_xml(R"(C:\A&B <x> "q"\gao.exe)", "S-1");
    CHECK(has(x, R"(<Command>C:\A&amp;B &lt;x&gt; &quot;q&quot;\gao.exe</Command>)"));
    CHECK_FALSE(has(x, "A&B"));
}
```
Register `src/core/task_xml.hpp src/core/task_xml.cpp` in `core` and the test in `core_tests`; create the header with `#pragma once` and an empty `.cpp`.

- [ ] **Step 2: Run** → FAIL: `boot_task_xml` undeclared.

- [ ] **Step 3: Implement** — `src/core/task_xml.hpp`:
```cpp
#pragma once
#include <string>

namespace gao {

// Task Scheduler definition for the logon task: starts on battery, at most
// 5 minutes, one instance, 15 s after logon of `user_id` (a SID string),
// with highest privileges. The caller writes it as UTF-16LE with a BOM.
// The path goes in <Command> on its own (arguments are separate), so it
// needs XML escaping, not shell quoting.
std::string boot_task_xml(const std::string& exe_path_utf8, const std::string& user_id);

}
```
`src/core/task_xml.cpp`:
```cpp
#include "core/task_xml.hpp"

namespace gao {

namespace {
std::string xml_escape(const std::string& s) {
    std::string out;
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default: out += c;
        }
    }
    return out;
}
}

std::string boot_task_xml(const std::string& exe_path_utf8, const std::string& user_id) {
    const std::string user = xml_escape(user_id);
    return "<?xml version=\"1.0\" encoding=\"UTF-16\"?>\n"
           "<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\n"
           "  <RegistrationInfo><Description>Re-applies the saved GPU Auto Optimizer profile at logon.</Description></RegistrationInfo>\n"
           "  <Triggers>\n"
           "    <LogonTrigger><Enabled>true</Enabled><UserId>" + user + "</UserId><Delay>PT15S</Delay></LogonTrigger>\n"
           "  </Triggers>\n"
           "  <Principals>\n"
           "    <Principal id=\"Author\"><UserId>" + user + "</UserId><LogonType>InteractiveToken</LogonType>"
           "<RunLevel>HighestAvailable</RunLevel></Principal>\n"
           "  </Principals>\n"
           "  <Settings>\n"
           "    <MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>\n"
           "    <DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>\n"
           "    <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>\n"
           "    <ExecutionTimeLimit>PT5M</ExecutionTimeLimit>\n"
           "    <Enabled>true</Enabled>\n"
           "  </Settings>\n"
           "  <Actions Context=\"Author\">\n"
           "    <Exec><Command>" + xml_escape(exe_path_utf8) + "</Command><Arguments>--boot-apply</Arguments></Exec>\n"
           "  </Actions>\n"
           "</Task>\n";
}

}
```

- [ ] **Step 4: Run** → `Status: SUCCESS!`.

- [ ] **Step 5: Commit + push** — `feat(core): Task Scheduler XML for the logon task`.

Note: the spec's §7 bullet "quotes the path" is superseded here: `<Command>` holds only the program path (arguments are a separate element), so it takes XML escaping, not quotes.

---

### Task 5: Protected state, installed exe, XML task, DLL loading

**Files:** Modify `src/hw/app_files.hpp/.cpp`, `src/hw/boot_task.hpp/.cpp`, `src/app/main.cpp`, `CMakeLists.txt`

**Interfaces — Produces:**
```cpp
// app_files.hpp
std::filesystem::path app_dir();              // now %ProgramData%\GpuAutoOptimizer
std::filesystem::path legacy_journal_path();  // %LOCALAPPDATA%\GpuAutoOptimizer\journal.jsonl
bool ensure_app_dir(std::string* why);        // create/verify protected folder (elevated callers)
// boot_task.hpp
std::filesystem::path installed_exe_path();   // %ProgramFiles%\GpuAutoOptimizer\gao.exe
bool install_exe(const std::filesystem::path& self, std::string* why);
void uninstall_exe();
bool files_equal(const std::filesystem::path& a, const std::filesystem::path& b);
std::string current_user_sid();
bool write_utf16_file(const std::filesystem::path& p, const std::string& utf8);
int boot_task_create_xml(const std::filesystem::path& xml_file);
int boot_task_remove();          // \GpuAutoOptimizer\BootApply
int boot_task_remove_legacy();   // \GpuAutoOptimizer
bool boot_task_exists();
```
(`boot_task_create(exe)` is removed.)

No unit tests (OS). Deliverable: warning-free build, green suite, `--status` works unelevated.

- [ ] **Step 1: `app_files`** — in `app_files.hpp` add the two declarations above (`legacy_journal_path`, `ensure_app_dir`) and change the comment to say `%ProgramData%\GpuAutoOptimizer`, readable by users and writable only by administrators. In `app_files.cpp`:
  - add `#include <aclapi.h>` and `#include <sddl.h>` after `<windows.h>`;
  - `app_dir()` reads `_wgetenv(L"ProgramData")` instead of `LOCALAPPDATA`;
  - add:
```cpp
std::filesystem::path legacy_journal_path() {
    const wchar_t* base = _wgetenv(L"LOCALAPPDATA");
    if (!base || !*base) return {};
    return std::filesystem::path(base) / L"GpuAutoOptimizer" / L"journal.jsonl";
}

// Owner Administrators; protected DACL: SYSTEM and Administrators full
// control, Users read & execute, inherited by everything inside.
static constexpr wchar_t kAppDirSddl[] = L"O:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1200a9;;;BU)";

bool ensure_app_dir(std::string* why) {
    auto fail = [&](const std::string& w) { if (why) *why = w; return false; };
    const auto dir = app_dir();
    if (dir.empty()) return fail("%ProgramData% is not set");
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(kAppDirSddl, SDDL_REVISION_1, &sd, nullptr))
        return fail("could not build the folder's security descriptor");
    SECURITY_ATTRIBUTES sa{sizeof(sa), sd, FALSE};
    bool ok = true;
    if (!CreateDirectoryW(dir.c_str(), &sa)) {
        if (GetLastError() != ERROR_ALREADY_EXISTS) {
            ok = fail("could not create " + dir.string());
        } else {
            // Someone may have created it first to plant files the elevated
            // process would trust: only adopt a folder an admin owns.
            PSID owner = nullptr;
            PSECURITY_DESCRIPTOR existing = nullptr;
            if (GetNamedSecurityInfoW(dir.c_str(), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION, &owner, nullptr,
                                      nullptr, nullptr, &existing) != ERROR_SUCCESS) {
                ok = fail("could not read the owner of " + dir.string());
            } else {
                const bool trusted = IsWellKnownSid(owner, WinBuiltinAdministratorsSid) || IsWellKnownSid(owner, WinLocalSystemSid);
                LocalFree(existing);
                if (!trusted) {
                    ok = fail(dir.string() + " exists but is not owned by Administrators or SYSTEM; delete it and try again");
                } else {
                    PACL dacl = nullptr;
                    BOOL present = FALSE, defaulted = FALSE;
                    GetSecurityDescriptorDacl(sd, &present, &dacl, &defaulted);
                    std::wstring name = dir.wstring();
                    if (SetNamedSecurityInfoW(name.data(), SE_FILE_OBJECT,
                                              DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr,
                                              nullptr, dacl, nullptr) != ERROR_SUCCESS)
                        ok = fail("could not secure " + dir.string());
                }
            }
        }
    }
    LocalFree(sd);
    return ok;
}
```
  - `write_file_atomic` and `append_line_durable` no longer call `create_directories` (the folder is created by `ensure_app_dir`); remove those two lines each.

- [ ] **Step 2: `boot_task`** — replace `src/hw/boot_task.hpp` with the declarations in the Interfaces block (with short comments) and rewrite `src/hw/boot_task.cpp`:
```cpp
#include "hw/boot_task.hpp"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <sddl.h>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace gao {

static int run_schtasks(const std::wstring& args) {
    // Full path: --boot runs elevated, and a bare name would let the current
    // or the exe's folder supply a different schtasks.exe.
    wchar_t sys[MAX_PATH];
    const UINT n = GetSystemDirectoryW(sys, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return -1;
    const std::wstring app = std::wstring(sys) + L"\\schtasks.exe";
    std::wstring cmd = L"\"" + app + L"\" " + args;
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(app.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return -1;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return static_cast<int>(code);
}

std::filesystem::path installed_exe_path() {
    const wchar_t* pf = _wgetenv(L"ProgramFiles");
    if (!pf || !*pf) return {};
    return std::filesystem::path(pf) / L"GpuAutoOptimizer" / L"gao.exe";
}

bool install_exe(const std::filesystem::path& self, std::string* why) {
    const auto dst = installed_exe_path();
    if (dst.empty()) { if (why) *why = "%ProgramFiles% is not set"; return false; }
    std::error_code ec;
    if (std::filesystem::equivalent(self, dst, ec)) return true;   // running the installed copy already
    std::filesystem::create_directories(dst.parent_path(), ec);
    if (!CopyFileW(self.c_str(), dst.c_str(), FALSE)) {
        if (why) *why = "could not copy gao.exe to " + dst.string() + " (error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    return true;
}

void uninstall_exe() {
    const auto dst = installed_exe_path();
    if (dst.empty()) return;
    std::error_code ec;
    std::filesystem::remove(dst, ec);
    std::filesystem::remove(dst.parent_path(), ec);   // only succeeds when empty
}

bool files_equal(const std::filesystem::path& a, const std::filesystem::path& b) {
    std::ifstream fa(a, std::ios::binary), fb(b, std::ios::binary);
    if (!fa || !fb) return false;
    return std::vector<char>(std::istreambuf_iterator<char>(fa), {}) ==
           std::vector<char>(std::istreambuf_iterator<char>(fb), {});
}

std::string current_user_sid() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return {};
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<unsigned char> buf(size);
    std::string out;
    if (size && GetTokenInformation(token, TokenUser, buf.data(), size, &size)) {
        LPSTR text = nullptr;
        if (ConvertSidToStringSidA(reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid, &text)) {
            out = text;
            LocalFree(text);
        }
    }
    CloseHandle(token);
    return out;
}

bool write_utf16_file(const std::filesystem::path& p, const std::string& utf8) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), w.data(), n);
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    const unsigned char bom[] = {0xFF, 0xFE};
    out.write(reinterpret_cast<const char*>(bom), 2);
    out.write(reinterpret_cast<const char*>(w.data()), static_cast<std::streamsize>(w.size() * sizeof(wchar_t)));
    return static_cast<bool>(out);
}

int boot_task_create_xml(const std::filesystem::path& xml_file) {
    return run_schtasks(L"/Create /F /TN \\GpuAutoOptimizer\\BootApply /XML \"" + xml_file.wstring() + L"\"");
}
int boot_task_remove() { return run_schtasks(L"/Delete /F /TN \\GpuAutoOptimizer\\BootApply"); }
int boot_task_remove_legacy() { return run_schtasks(L"/Delete /F /TN GpuAutoOptimizer"); }
bool boot_task_exists() { return run_schtasks(L"/Query /TN \\GpuAutoOptimizer\\BootApply") == 0; }

}
```

- [ ] **Step 3: `main.cpp`**
  - First statement of `main()`: `SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);` with the comment `// Before anything loads a DLL: System32 only (the delay-loaded d3dcompiler_47.dll included).`
  - Include `"core/task_xml.hpp"`.
  - Add after `boot_log`:
```cpp
// Every elevated command that writes state calls this first: creates or
// verifies the protected folder, and imports the pre-P4b journal once so its
// freeze ceilings survive the move.
static bool prepare_state(std::string* why) {
    if (!gao::ensure_app_dir(why)) return false;
    std::error_code ec;
    const auto legacy = gao::legacy_journal_path();
    if (!std::filesystem::exists(gao::journal_path(), ec) && !legacy.empty() && std::filesystem::exists(legacy, ec))
        for (const auto& line : gao::read_lines(legacy)) gao::append_line_durable(gao::journal_path(), line);
    return true;
}
```
  - `optimize()`: right after the elevation check, `std::string why; if (!prepare_state(&why)) { std::printf("%s\n", why.c_str()); return 1; }` (move it before `journal_path()` is used). Update the `LOCALAPPDATA is not set` message to `ProgramData is not set`.
  - `boot_apply()`: after `FreeConsole();`, `std::string why; if (!prepare_state(&why)) return 1;` (nothing can be logged without the folder).
  - Replace `boot(bool on)`:
```cpp
static int boot(bool on) {
    if (!IsElevated()) { std::printf("--boot needs an elevated (administrator) shell\n"); return 1; }
    std::string why;
    if (!prepare_state(&why)) { std::printf("%s\n", why.c_str()); return 1; }
    gao::boot_task_remove_legacy();   // the pre-P4b task, if any
    if (!on) {
        const int code = gao::boot_task_remove();
        gao::uninstall_exe();
        std::printf(code == 0 ? "boot-apply off: task and installed copy removed\n"
                              : "task not removed (schtasks exit %d); installed copy removed\n", code);
        return code == 0 ? 0 : 1;
    }
    wchar_t self[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, self, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) { std::printf("could not find gao.exe's own path\n"); return 1; }
    if (!gao::install_exe(self, &why)) { std::printf("%s\n", why.c_str()); return 1; }
    const auto exe = gao::installed_exe_path();
    const auto u8 = exe.u8string();
    const std::string xml = gao::boot_task_xml(std::string(u8.begin(), u8.end()), gao::current_user_sid());
    const auto xml_path = gao::app_dir() / L"BootApply.xml";
    if (!gao::write_utf16_file(xml_path, xml)) { std::printf("could not write %s\n", xml_path.string().c_str()); return 1; }
    const int code = gao::boot_task_create_xml(xml_path);
    std::error_code ec;
    std::filesystem::remove(xml_path, ec);
    if (code != 0) { std::printf("could not create the task (schtasks exit %d)\n", code); return 1; }
    gao::Config cfg = load_config();
    cfg.boot_strikes = 0;
    if (!save_config(cfg)) { std::printf("task created, but could not reset the strike counter\n"); return 1; }
    std::printf("boot-apply on: %s runs at every logon (strikes reset)\n", exe.string().c_str());
    if (!cfg.profile) std::printf("note: there is no saved profile yet; run `gao --optimize` first\n");
    return 0;
}
```
  - `status()`: after the `boot-apply:` line add:
```cpp
    const auto installed = gao::installed_exe_path();
    wchar_t self[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::error_code ec;
    if (!std::filesystem::exists(installed, ec)) std::printf("boot copy:  not installed\n");
    else if (n && n < MAX_PATH && gao::files_equal(self, installed)) std::printf("boot copy:  up to date\n");
    else std::printf("boot copy:  OUTDATED -- run `gao --boot on` to install this build\n");
```

- [ ] **Step 4: CMake** — after `target_link_libraries(gao PRIVATE core hw)` add:
```cmake
# d3dcompiler_47.dll is not a KnownDLL: delay-load it so it resolves after
# SetDefaultDllDirectories(SYSTEM32) in main(), never from the exe's folder.
target_link_libraries(gao PRIVATE delayimp)
target_link_options(gao PRIVATE /DELAYLOAD:d3dcompiler_47.dll /DEPENDENTLOADFLAG:0x800)
```
Register `src/core/task_xml.*` (done in Task 4).

- [ ] **Step 5: Build and smoke** — Release build warning-free; `ctest` green; `dumpbin /dependents build\Release\gao.exe` lists `d3dcompiler_47.dll` under "Image has the following delay load dependencies"; `.\build\Release\gao.exe --status` (not elevated) runs; `.\build\Release\gao.exe --stress 3` → STABLE (delay-load resolves).

- [ ] **Step 6: Docs** — append rows 23–28 from the spec §7 to `docs/hardware-checks.md` (Elevated column: 23/24/25/28 yes, 26/27 no) and update its paths from `%LOCALAPPDATA%` to `%ProgramData%`; in `README.md` replace `%LOCALAPPDATA%\GpuAutoOptimizer` with `%ProgramData%\GpuAutoOptimizer` (protected: users read, admins write), say `--boot on` installs the exe to `%ProgramFiles%\GpuAutoOptimizer\` and `--status` shows whether that copy is current, replace the CAUTION block with a short NOTE that boot-apply runs an admin-only copy, add `--bandwidth` to Quick start, and describe the memory search as "stops at the bandwidth peak".

- [ ] **Step 7: Commit + push** — `feat: protected state folder, installed boot copy, XML logon task, System32-only DLLs`.

---

### Task 6: Hardware verification (with the user)

- [ ] **Step 1: Check 26 part 1 (planted folder)** — before any elevated run: as the normal user `mkdir "%ProgramData%\GpuAutoOptimizer"`; run `gao --optimize best` elevated → must refuse with "not owned by Administrators or SYSTEM"; delete the folder as the user.
- [ ] **Step 2: Elevated script** (per-step log files, no tails): `--optimize best` (checks 23, 24, 28: log shows bandwidth lines, confirm lines, and the warning list from the migrated journal is empty unless the old journal had freezes; count journal lines), `icacls "%ProgramData%\GpuAutoOptimizer"` (26), `--boot on`, `schtasks /Query /TN \GpuAutoOptimizer\BootApply /XML` (25), `dir "%ProgramFiles%\GpuAutoOptimizer"`, `--status`.
- [ ] **Step 3: Check 27** — copy `C:\Windows\System32\version.dll` to `build\Release\d3dcompiler_47.dll`; `.\build\Release\gao.exe --stress 5` must print STABLE (a loaded fake would fail delay-load and crash); delete the fake.
- [ ] **Step 4: Check 25 logon** — ask the user to log off and on, wait 2 min, `--status` → `last boot: ... applied`, strikes 0, `boot copy: up to date`.
- [ ] **Step 5: Run `--boot on` from the installed copy** (`"%ProgramFiles%\GpuAutoOptimizer\gao.exe" --boot on`, elevated) → succeeds (Review Focus 5).
- [ ] **Step 6:** fill the Result column; commit `docs: record P4b hardware checks on the RTX 4070`; push.
