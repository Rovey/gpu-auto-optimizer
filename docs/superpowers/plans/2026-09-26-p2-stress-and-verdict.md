# GPU Auto Optimizer — Phase 2 (Stress Load and Stability Verdict) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A DX11 compute stress load that checks every value it computes, a pure-logic run loop that turns batches plus telemetry into one verdict and a score, and a `gao --stress` command that exposes both.

**Architecture:** `src/core/stress_math.*` generates exact-float inputs and the CPU reference. `src/core/stability.*` owns the loop and the verdict and reaches hardware only through two callbacks (`std::function<StressBatch()>`, `std::function<Telemetry()>`), so CI tests it with fakes. `src/hw/stress.*` is the only D3D11 code: device on the NVIDIA adapter, runtime-compiled HLSL, batches that grow to ~250 ms, device recreation after a TDR. `src/app/main.cpp` wires them into `gao --stress`.

**Tech Stack:** C++20, MSVC (VS 2026), CMake ≥ 3.28, doctest (vendored), D3D11 + DXGI + `D3DCompile` (Windows SDK, `d3dcompiler_47.dll` ships with Windows 10/11), NVML (existing `gao::Nvml`).

**Spec:** `docs/superpowers/specs/2026-09-26-p2-stress-and-verdict-design.md` (parent: `docs/superpowers/specs/2026-09-20-cpp-rewrite-design.md`)

## Global Constraints

- C++20, MSVC, x64 only.
- English everywhere: code, comments, commit messages, CLI output, docs.
- `src/core/` must not include `windows.h`, D3D, DXGI, NVML or NVAPI headers.
- No package manager, no new third-party dependency. D3D11/DXGI/d3dcompiler come from the Windows SDK.
- Hardware code (`src/hw/`) is never unit-tested in CI; it is verified through `docs/hardware-checks.md` on the RTX 4070.
- The stress load changes no GPU settings. Nothing in P2 writes clocks, power or fans.
- Matrix size `N = 1024`, input values `-8..8`, and `N · 8 · 8 < 2^24` must hold (enforced by `static_assert`).
- Default thermal ceiling for `--stress`: 85 °C.
- Exit codes of `gao --stress`: `0` Stable, `2` WrongResult or DeviceLost, `3` TooHot or NoTelemetry, `1` init/argument failure.
- `cmake` is not on `PATH` on the dev machine. Either run from a VS Developer PowerShell, or use the bundled one:
  `$cmake = "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"` and `$ctest` likewise (`...\bin\ctest.exe`). Commands below write `cmake`/`ctest`; substitute `& $cmake`/`& $ctest` where needed.

## Review Focus

1. **A batch that is both `device_lost` and has `wrong_values > 0`** — after a TDR the error counter is garbage; the verdict must be DeviceLost, not WrongResult. (Task 2 test: "device lost wins over a garbage error count".)
2. **`seconds <= 0` or a batch reporting `elapsed_ms <= 0`** — the loop must still terminate and judge at least one batch rather than spin forever or divide by zero. (Task 2 tests: "a zero-length run still judges one batch", "a batch reporting no elapsed time cannot hang the loop".)
3. **Temperature exactly at the ceiling** — `temp_c == max_temp_c` is allowed; only strictly above is TooHot. (Task 2 test: "temperature at the ceiling is not too hot".)
4. **Telemetry fields that are unknown (`-1`) while `ok` is true** (power, mem clock) — averages must skip unknown samples and report `-1` when none were known, never average a `-1` into a real number. (Task 2 test: "unknown power is excluded from the average".)
5. **Bad CLI input** — `--stress abc`, `--stress 0`, `--stress -5`, `--max-temp 200`, an unknown selftest name: all must be rejected with a message and exit 1, never start a load. (Task 4 manual step 5.)

---

## File Structure

| File | Responsibility |
|---|---|
| `src/core/stress_math.hpp/.cpp` | Constants, deterministic input matrix, exact CPU reference matmul |
| `src/core/stability.hpp/.cpp` | `StressBatch`, `Verdict`, `StabilityResult`, `run_stability`, `verdict_name` |
| `src/hw/stress.hpp/.cpp` | `gao::Stress`: D3D11 device, HLSL, batches, TDR recovery, selftests |
| `src/app/main.cpp` | New `--stress` command |
| `tests/test_stress_math.cpp`, `tests/test_stability.cpp` | Unit tests for the two core units |
| `CMakeLists.txt` | Register new sources/tests; link `d3d11 dxgi d3dcompiler` into `hw` |
| `docs/hardware-checks.md` | Rows 10–12; cross-check steps for 3 and 5 |
| `README.md` | Mention `--stress` |

---

### Task 1: Exact-float stress inputs and CPU reference

**Files:**
- Create: `src/core/stress_math.hpp`, `src/core/stress_math.cpp`
- Test: `tests/test_stress_math.cpp`
- Modify: `CMakeLists.txt` (add both to `core`, the test to `core_tests`)

**Interfaces:**
- Consumes: nothing.
- Produces:
  ```cpp
  namespace gao {
  inline constexpr int kStressN = 1024;
  inline constexpr int kStressMaxAbs = 8;
  std::vector<float> make_stress_matrix(std::uint32_t seed, int n = kStressN);   // n*n, row-major
  std::vector<float> reference_matmul(const std::vector<float>& a,
                                      const std::vector<float>& b, int n = kStressN); // C = A*B, row-major
  }
  ```

- [ ] **Step 1: Write the failing test**

`tests/test_stress_math.cpp`:
```cpp
#include "doctest/doctest.h"
#include "core/stress_math.hpp"

using namespace gao;

TEST_CASE("stress matrix is deterministic per seed and stays in range") {
    const auto a = make_stress_matrix(1, 32);
    const auto again = make_stress_matrix(1, 32);
    const auto other = make_stress_matrix(2, 32);
    REQUIRE(a.size() == 32u * 32u);
    CHECK(a == again);
    CHECK(a != other);
    bool has_negative = false, has_positive = false;
    for (float v : a) {
        CHECK(v >= -kStressMaxAbs);
        CHECK(v <= kStressMaxAbs);
        CHECK(v == static_cast<float>(static_cast<int>(v)));   // whole numbers only
        has_negative |= v < 0;
        has_positive |= v > 0;
    }
    CHECK(has_negative);
    CHECK(has_positive);
}

TEST_CASE("reference matmul equals a naive double computation") {
    const int n = 8;
    const auto a = make_stress_matrix(3, n);
    const auto b = make_stress_matrix(4, n);
    const auto c = reference_matmul(a, b, n);
    REQUIRE(c.size() == static_cast<size_t>(n * n));
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j) {
            double sum = 0;
            for (int k = 0; k < n; ++k) sum += double(a[i * n + k]) * double(b[k * n + j]);
            CHECK(c[i * n + j] == static_cast<float>(sum));
        }
}
```

Add to `CMakeLists.txt`: `src/core/stress_math.hpp` and `src/core/stress_math.cpp` in the `core` source list, `tests/test_stress_math.cpp` in `core_tests`.

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake -S . -B build -A x64; cmake --build build --config Debug`
Expected: build FAILS with `cannot open include file 'core/stress_math.hpp'`.

- [ ] **Step 3: Write minimal implementation**

`src/core/stress_math.hpp`:
```cpp
#pragma once
#include <cstdint>
#include <vector>

namespace gao {

// The stress load multiplies two N x N matrices of small whole numbers stored
// as float. With |values| <= 8, every product is <= 64 and every dot product
// is <= N * 64, which must stay below 2^24 -- the largest range in which FP32
// represents every integer exactly. Inside that range the GPU's result does
// not depend on summation order or FMA use, so a CPU reference is exact and
// any difference is a real computation error.
inline constexpr int kStressN = 1024;
inline constexpr int kStressMaxAbs = 8;
static_assert(kStressN * kStressMaxAbs * kStressMaxAbs < (1 << 24),
              "stress inputs would leave the exact-FP32 range");

// n*n values in -kStressMaxAbs..kStressMaxAbs, row-major. Same seed -> same
// matrix on every compiler: std::mt19937's output sequence is fixed by the
// standard (the distribution classes are not, so none is used).
std::vector<float> make_stress_matrix(std::uint32_t seed, int n = kStressN);

// C = A * B, row-major, n x n.
std::vector<float> reference_matmul(const std::vector<float>& a,
                                    const std::vector<float>& b, int n = kStressN);

}
```

`src/core/stress_math.cpp`:
```cpp
#include "core/stress_math.hpp"
#include <random>

namespace gao {

std::vector<float> make_stress_matrix(std::uint32_t seed, int n) {
    std::mt19937 rng(seed);
    std::vector<float> m(static_cast<size_t>(n) * n);
    constexpr std::uint32_t span = 2 * kStressMaxAbs + 1;
    for (float& v : m) v = static_cast<float>(static_cast<int>(rng() % span) - kStressMaxAbs);
    return m;
}

std::vector<float> reference_matmul(const std::vector<float>& a,
                                    const std::vector<float>& b, int n) {
    std::vector<float> c(static_cast<size_t>(n) * n, 0.0f);
    // i-k-j order walks B and C row by row (cache friendly); the result is
    // exact in float regardless of order -- see the header.
    for (int i = 0; i < n; ++i)
        for (int k = 0; k < n; ++k) {
            const float aik = a[static_cast<size_t>(i) * n + k];
            const float* brow = &b[static_cast<size_t>(k) * n];
            float* crow = &c[static_cast<size_t>(i) * n];
            for (int j = 0; j < n; ++j) crow[j] += aik * brow[j];
        }
    return c;
}

}
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `cmake --build build --config Debug; ctest --test-dir build -C Debug --output-on-failure`
Expected: `100% tests passed`.

- [ ] **Step 5: Commit**

```bash
git add src/core/stress_math.hpp src/core/stress_math.cpp tests/test_stress_math.cpp CMakeLists.txt
git commit -m "feat(core): exact-float stress inputs and CPU reference"
```

---

### Task 2: Run loop and stability verdict

**Files:**
- Create: `src/core/stability.hpp`, `src/core/stability.cpp`
- Test: `tests/test_stability.cpp`
- Modify: `CMakeLists.txt` (add to `core` and `core_tests`)

**Interfaces:**
- Consumes: `gao::Telemetry` from `src/core/types.hpp` (fields `ok`, `core_mhz`, `mem_mhz`, `temp_c`, `power_w`; `-1` = unknown).
- Produces:
  ```cpp
  namespace gao {
  struct StressBatch { long long iterations = 0; int wrong_values = 0; bool device_lost = false; double elapsed_ms = 0; };
  enum class Verdict { Stable, WrongResult, DeviceLost, TooHot, NoTelemetry };
  struct StabilityResult { Verdict verdict = Verdict::Stable; double score = 0; double seconds = 0;
                           int peak_temp_c = -1; int avg_power_w = -1; int avg_core_mhz = -1; int avg_mem_mhz = -1; };
  StabilityResult run_stability(const std::function<StressBatch()>& batch,
                                const std::function<Telemetry()>& read,
                                double seconds, int max_temp_c);
  const char* verdict_name(Verdict v);   // "STABLE", "WRONG RESULT", "DEVICE LOST", "TOO HOT", "NO TELEMETRY"
  }
  ```

- [ ] **Step 1: Write the failing tests**

`tests/test_stability.cpp`:
```cpp
#include "doctest/doctest.h"
#include "core/stability.hpp"
#include <string>

using namespace gao;

namespace {
Telemetry tel(int temp, int power = 190, int core = 2700, int mem = 10500) {
    Telemetry t;
    t.ok = true; t.temp_c = temp; t.power_w = power; t.core_mhz = core; t.mem_mhz = mem;
    return t;
}
StressBatch good(long long its = 100, double ms = 250) { return {its, 0, false, ms}; }
}

TEST_CASE("twelve good 250 ms batches make a stable 3 s run") {
    int calls = 0;
    const auto r = run_stability([&] { ++calls; return good(); },
                                 [] { return tel(60); }, 3.0, 85);
    CHECK(r.verdict == Verdict::Stable);
    CHECK(calls == 12);
    CHECK(r.seconds == doctest::Approx(3.0));
    CHECK(r.score == doctest::Approx(1200 / 3.0));
    CHECK(r.peak_temp_c == 60);
    CHECK(r.avg_power_w == 190);
    CHECK(r.avg_core_mhz == 2700);
    CHECK(r.avg_mem_mhz == 10500);
}

TEST_CASE("the first wrong value stops the run") {
    int calls = 0;
    const auto r = run_stability([&] { ++calls; return calls == 3 ? StressBatch{100, 1, false, 250} : good(); },
                                 [] { return tel(60); }, 60.0, 85);
    CHECK(r.verdict == Verdict::WrongResult);
    CHECK(calls == 3);
}

TEST_CASE("a lost device stops the run") {
    int calls = 0;
    const auto r = run_stability([&] { ++calls; return calls == 2 ? StressBatch{0, 0, true, 2000} : good(); },
                                 [] { return tel(60); }, 60.0, 85);
    CHECK(r.verdict == Verdict::DeviceLost);
    CHECK(calls == 2);
}

TEST_CASE("device lost wins over a garbage error count") {
    const auto r = run_stability([] { return StressBatch{0, 12345, true, 2000}; },
                                 [] { return tel(60); }, 60.0, 85);
    CHECK(r.verdict == Verdict::DeviceLost);
}

TEST_CASE("overheating stops the run and records the peak") {
    int reads = 0;
    const auto r = run_stability([] { return good(); },
                                 [&] { ++reads; return tel(reads == 4 ? 86 : 70); }, 60.0, 85);
    CHECK(r.verdict == Verdict::TooHot);
    CHECK(reads == 4);
    CHECK(r.peak_temp_c == 86);
}

TEST_CASE("temperature at the ceiling is not too hot") {
    const auto r = run_stability([] { return good(); }, [] { return tel(85); }, 1.0, 85);
    CHECK(r.verdict == Verdict::Stable);
}

TEST_CASE("losing telemetry stops the run") {
    int calls = 0;
    const auto r = run_stability([&] { ++calls; return good(); },
                                 [] { return Telemetry{}; }, 60.0, 85);   // ok == false
    CHECK(r.verdict == Verdict::NoTelemetry);
    CHECK(calls == 1);
}

TEST_CASE("unknown power is excluded from the average") {
    int reads = 0;
    const auto r = run_stability([] { return good(); },
                                 [&] { ++reads; return tel(60, reads % 2 ? -1 : 200); }, 1.0, 85);
    CHECK(r.avg_power_w == 200);
    const auto none = run_stability([] { return good(); }, [] { return tel(60, -1); }, 1.0, 85);
    CHECK(none.avg_power_w == -1);
}

TEST_CASE("a zero-length run still judges one batch") {
    int calls = 0;
    const auto r = run_stability([&] { ++calls; return good(); }, [] { return tel(60); }, 0.0, 85);
    CHECK(calls == 1);
    CHECK(r.verdict == Verdict::Stable);
    CHECK(r.score > 0);
}

TEST_CASE("a batch reporting no elapsed time cannot hang the loop") {
    int calls = 0;
    // 0.0095 s, not 0.01: ten 1 ms steps summed in floating point may land a
    // hair below 0.01 and make the count flaky.
    const auto r = run_stability([&] { ++calls; return good(100, 0); }, [] { return tel(60); }, 0.0095, 85);
    CHECK(r.verdict == Verdict::Stable);
    CHECK(calls == 10);   // each batch counts as at least 1 ms
}

TEST_CASE("every verdict has a name") {
    CHECK(std::string(verdict_name(Verdict::Stable)) == "STABLE");
    CHECK(std::string(verdict_name(Verdict::WrongResult)) == "WRONG RESULT");
    CHECK(std::string(verdict_name(Verdict::DeviceLost)) == "DEVICE LOST");
    CHECK(std::string(verdict_name(Verdict::TooHot)) == "TOO HOT");
    CHECK(std::string(verdict_name(Verdict::NoTelemetry)) == "NO TELEMETRY");
}
```

Add `src/core/stability.hpp`, `src/core/stability.cpp` to `core` and `tests/test_stability.cpp` to `core_tests` in `CMakeLists.txt`.

- [ ] **Step 2: Run tests to verify they fail**

Run: `cmake --build build --config Debug`
Expected: build FAILS with `cannot open include file 'core/stability.hpp'`.

- [ ] **Step 3: Write minimal implementation**

`src/core/stability.hpp`:
```cpp
#pragma once
#include "core/types.hpp"
#include <functional>

namespace gao {

// What hw reports for one batch of stress work (~250 ms of GPU time).
struct StressBatch {
    long long iterations = 0;   // matmuls completed in this batch
    int wrong_values = 0;       // output elements that differed from the reference
    bool device_lost = false;   // DXGI_ERROR_DEVICE_REMOVED: Windows reset the driver (TDR)
    double elapsed_ms = 0;
};

enum class Verdict { Stable, WrongResult, DeviceLost, TooHot, NoTelemetry };

struct StabilityResult {
    Verdict verdict = Verdict::Stable;
    double score = 0;          // iterations per second over the covered time
    double seconds = 0;        // run time actually covered (sum of batch times)
    int peak_temp_c = -1;
    int avg_power_w = -1;      // -1 when no sample reported the value
    int avg_core_mhz = -1;
    int avg_mem_mhz = -1;
};

// Runs batches until `seconds` of batch time are covered or something fails,
// whichever comes first. Always runs at least one batch. Stops at the first
// of: lost device, wrong value, missing telemetry, temp_c > max_temp_c.
// Time is the sum of batch elapsed_ms, not the wall clock, so tests are exact.
StabilityResult run_stability(const std::function<StressBatch()>& batch,
                              const std::function<Telemetry()>& read,
                              double seconds, int max_temp_c);

const char* verdict_name(Verdict v);

}
```

`src/core/stability.cpp`:
```cpp
#include "core/stability.hpp"
#include <algorithm>

namespace gao {

namespace {
// Averages only the samples that were actually reported (-1 = unknown).
struct Avg {
    long long sum = 0;
    int n = 0;
    void add(int v) { if (v >= 0) { sum += v; ++n; } }
    int get() const { return n ? static_cast<int>(sum / n) : -1; }
};
}

StabilityResult run_stability(const std::function<StressBatch()>& batch,
                              const std::function<Telemetry()>& read,
                              double seconds, int max_temp_c) {
    StabilityResult r;
    long long iterations = 0;
    Avg power, core, mem;
    do {
        const StressBatch b = batch();
        // A batch that claims no time would never advance the loop; count it
        // as 1 ms so a broken timer ends the run instead of hanging it.
        r.seconds += std::max(b.elapsed_ms, 1.0) / 1000.0;
        // Device lost first: after a TDR the error counter is garbage.
        if (b.device_lost) { r.verdict = Verdict::DeviceLost; break; }
        if (b.wrong_values > 0) { r.verdict = Verdict::WrongResult; break; }
        iterations += b.iterations;
        const Telemetry t = read();
        if (!t.ok) { r.verdict = Verdict::NoTelemetry; break; }
        r.peak_temp_c = std::max(r.peak_temp_c, t.temp_c);
        power.add(t.power_w);
        core.add(t.core_mhz);
        mem.add(t.mem_mhz);
        if (t.temp_c > max_temp_c) { r.verdict = Verdict::TooHot; break; }
    } while (r.seconds < seconds);
    r.score = static_cast<double>(iterations) / r.seconds;
    r.avg_power_w = power.get();
    r.avg_core_mhz = core.get();
    r.avg_mem_mhz = mem.get();
    return r;
}

const char* verdict_name(Verdict v) {
    switch (v) {
        case Verdict::Stable: return "STABLE";
        case Verdict::WrongResult: return "WRONG RESULT";
        case Verdict::DeviceLost: return "DEVICE LOST";
        case Verdict::TooHot: return "TOO HOT";
        case Verdict::NoTelemetry: return "NO TELEMETRY";
    }
    return "UNKNOWN";
}

}
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `cmake --build build --config Debug; ctest --test-dir build -C Debug --output-on-failure`
Expected: `100% tests passed`.

- [ ] **Step 5: Commit**

```bash
git add src/core/stability.hpp src/core/stability.cpp tests/test_stability.cpp CMakeLists.txt
git commit -m "feat(core): stress run loop and stability verdict"
```

---

### Task 3: D3D11 stress load

**Files:**
- Create: `src/hw/stress.hpp`, `src/hw/stress.cpp`
- Modify: `CMakeLists.txt` (add to `hw`; `target_link_libraries(hw PUBLIC core d3d11 dxgi d3dcompiler)`)

**Interfaces:**
- Consumes: `gao::StressBatch` (Task 2), `gao::make_stress_matrix`, `gao::reference_matmul`, `gao::kStressN` (Task 1).
- Produces:
  ```cpp
  namespace gao {
  enum class StressSelftest { None, WrongResult, Tdr };
  class Stress {
  public:
      Stress(); ~Stress();
      bool Init(StressSelftest selftest = StressSelftest::None);   // false -> Error() says why
      StressBatch Batch();                                           // recreates the device after a loss
      const std::string& Error() const;
      const std::string& AdapterName() const;
  };
  }
  ```

No unit test: this is hardware code (Global Constraints). It is verified in Task 5. The deliverable here is that it compiles, links, and the whole suite still passes.

- [ ] **Step 1: Write the header**

`src/hw/stress.hpp`:
```cpp
#pragma once
#include "core/stability.hpp"
#include <memory>
#include <string>

namespace gao {

// Hidden test modes for hardware checks 11 and 12: stock hardware never
// produces a wrong value or a TDR on its own.
enum class StressSelftest {
    None,
    WrongResult,   // one reference value is corrupted before upload
    Tdr,           // the first batch is one dispatch far longer than the 2 s TDR limit
};

// The DX11 compute stress load. Multiplies two exact-float matrices (see
// core/stress_math.hpp) and has the GPU count every output element that
// differs from the uploaded CPU reference. Changes no GPU settings.
class Stress {
public:
    Stress();
    ~Stress();
    bool Init(StressSelftest selftest = StressSelftest::None);
    // One batch of dispatches. Batches start at one dispatch and double until
    // they take ~250 ms. After a lost device, the next call recreates it.
    StressBatch Batch();
    const std::string& Error() const { return error_; }
    const std::string& AdapterName() const { return adapter_name_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;   // keeps D3D headers out of every includer
    StressSelftest selftest_ = StressSelftest::None;
    std::string error_;
    std::string adapter_name_;
    int dispatches_ = 1;
    bool CreateDevice();
};

}
```

- [ ] **Step 2: Write the implementation**

`src/hw/stress.cpp`:
```cpp
#include "hw/stress.hpp"
#include "core/stress_math.hpp"

#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <chrono>
#include <climits>
#include <cstdio>
#include <cstring>
#include <string>

using Microsoft::WRL::ComPtr;

namespace gao {

namespace {

constexpr UINT kNvidiaVendorId = 0x10DE;
constexpr int kTile = 16;
constexpr double kTargetBatchMs = 250.0;
constexpr int kMaxDispatches = 4096;
// ~1 ms per pass on an RTX 4070; 20000 passes is far beyond the 2 s TDR limit.
constexpr UINT kTdrPasses = 20000;

// Tiled matmul. Every thread computes one element of C = A * B, compares it
// with the reference, and counts a mismatch. `passes` repeats the whole
// computation; it is 1 except for the TDR self-test.
const char kShader[] = R"(
#define N 1024
#define T 16
StructuredBuffer<float> A : register(t0);
StructuredBuffer<float> B : register(t1);
StructuredBuffer<float> Ref : register(t2);
RWStructuredBuffer<uint> Errors : register(u0);
cbuffer Params : register(b0) { uint passes; uint3 pad; };
groupshared float As[T][T];
groupshared float Bs[T][T];
[numthreads(T, T, 1)]
void main(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID) {
    uint row = gid.y * T + tid.y;
    uint col = gid.x * T + tid.x;
    float acc = 0;
    [loop] for (uint p = 0; p < passes; ++p) {
        acc = 0;
        [loop] for (uint k0 = 0; k0 < N; k0 += T) {
            As[tid.y][tid.x] = A[row * N + k0 + tid.x];
            Bs[tid.y][tid.x] = B[(k0 + tid.y) * N + col];
            GroupMemoryBarrierWithGroupSync();
            [unroll] for (uint k = 0; k < T; ++k) acc = mad(As[tid.y][k], Bs[k][tid.x], acc);
            GroupMemoryBarrierWithGroupSync();
        }
    }
    if (acc != Ref[row * N + col]) InterlockedAdd(Errors[0], 1);
}
)";

std::string Narrow(const wchar_t* w) {
    char buf[256];
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, sizeof(buf), nullptr, nullptr);
    return n > 0 ? std::string(buf) : std::string("?");
}

std::string Hr(const char* what, HRESULT hr) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%s failed (hr=0x%08lX)", what, static_cast<unsigned long>(hr));
    return buf;
}

}

struct Stress::Impl {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11ComputeShader> cs;
    ComPtr<ID3D11ShaderResourceView> a, b, ref;
    ComPtr<ID3D11Buffer> errors, staging, params;
    ComPtr<ID3D11UnorderedAccessView> errors_uav;
    std::vector<float> ha, hb, href;   // host copies, kept for device recreation
};

Stress::Stress() : impl_(std::make_unique<Impl>()) {}
Stress::~Stress() = default;

bool Stress::Init(StressSelftest selftest) {
    selftest_ = selftest;
    impl_->ha = make_stress_matrix(1);
    impl_->hb = make_stress_matrix(2);
    impl_->href = reference_matmul(impl_->ha, impl_->hb);
    if (selftest_ == StressSelftest::WrongResult) impl_->href[0] += 1.0f;
    return CreateDevice();
}

bool Stress::CreateDevice() {
    Impl& d = *impl_;
    d.device.Reset(); d.ctx.Reset(); d.cs.Reset();
    d.a.Reset(); d.b.Reset(); d.ref.Reset();
    d.errors.Reset(); d.staging.Reset(); d.params.Reset(); d.errors_uav.Reset();

    ComPtr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) { error_ = Hr("CreateDXGIFactory1", hr); return false; }
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        if (desc.VendorId == kNvidiaVendorId) { adapter_name_ = Narrow(desc.Description); break; }
        adapter.Reset();
    }
    if (!adapter) { error_ = "no NVIDIA adapter found"; return false; }

    const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, &level, 1,
                           D3D11_SDK_VERSION, &d.device, nullptr, &d.ctx);
    if (FAILED(hr)) { error_ = Hr("D3D11CreateDevice", hr); return false; }

    ComPtr<ID3DBlob> code, log;
    hr = D3DCompile(kShader, sizeof(kShader) - 1, "stress.hlsl", nullptr, nullptr, "main", "cs_5_0",
                    D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &log);
    if (FAILED(hr)) {
        error_ = Hr("D3DCompile", hr);
        if (log) error_ += std::string(": ") + static_cast<const char*>(log->GetBufferPointer());
        return false;
    }
    hr = d.device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &d.cs);
    if (FAILED(hr)) { error_ = Hr("CreateComputeShader", hr); return false; }

    auto make_srv = [&](const std::vector<float>& host, ComPtr<ID3D11ShaderResourceView>& out) {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = static_cast<UINT>(host.size() * sizeof(float));
        bd.Usage = D3D11_USAGE_IMMUTABLE;
        bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        bd.StructureByteStride = sizeof(float);
        D3D11_SUBRESOURCE_DATA init{host.data()};
        ComPtr<ID3D11Buffer> buf;
        HRESULT h = d.device->CreateBuffer(&bd, &init, &buf);
        if (SUCCEEDED(h)) h = d.device->CreateShaderResourceView(buf.Get(), nullptr, &out);
        return h;
    };
    if (FAILED(hr = make_srv(d.ha, d.a)) || FAILED(hr = make_srv(d.hb, d.b)) ||
        FAILED(hr = make_srv(d.href, d.ref))) {
        error_ = Hr("input buffer", hr); return false;
    }

    D3D11_BUFFER_DESC ed{};
    ed.ByteWidth = sizeof(UINT);
    ed.Usage = D3D11_USAGE_DEFAULT;
    ed.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    ed.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    ed.StructureByteStride = sizeof(UINT);
    if (FAILED(hr = d.device->CreateBuffer(&ed, nullptr, &d.errors)) ||
        FAILED(hr = d.device->CreateUnorderedAccessView(d.errors.Get(), nullptr, &d.errors_uav))) {
        error_ = Hr("error counter", hr); return false;
    }
    D3D11_BUFFER_DESC sd{};
    sd.ByteWidth = sizeof(UINT);
    sd.Usage = D3D11_USAGE_STAGING;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(hr = d.device->CreateBuffer(&sd, nullptr, &d.staging))) {
        error_ = Hr("staging buffer", hr); return false;
    }
    D3D11_BUFFER_DESC pd{};
    pd.ByteWidth = 16;
    pd.Usage = D3D11_USAGE_DEFAULT;
    pd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(hr = d.device->CreateBuffer(&pd, nullptr, &d.params))) {
        error_ = Hr("constant buffer", hr); return false;
    }
    return true;
}

StressBatch Stress::Batch() {
    StressBatch out;
    const auto start = std::chrono::steady_clock::now();
    auto finish = [&] {
        out.elapsed_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count();
        return out;
    };
    Impl& d = *impl_;
    if (!d.device && !CreateDevice()) { out.device_lost = true; return finish(); }

    const bool tdr = selftest_ == StressSelftest::Tdr;
    selftest_ = tdr ? StressSelftest::None : selftest_;   // the TDR self-test fires once
    const UINT params[4] = {tdr ? kTdrPasses : 1u, 0, 0, 0};
    d.ctx->UpdateSubresource(d.params.Get(), 0, nullptr, params, 0, 0);

    const UINT zero[4] = {0, 0, 0, 0};
    d.ctx->ClearUnorderedAccessViewUint(d.errors_uav.Get(), zero);
    d.ctx->CSSetShader(d.cs.Get(), nullptr, 0);
    ID3D11ShaderResourceView* srvs[] = {d.a.Get(), d.b.Get(), d.ref.Get()};
    d.ctx->CSSetShaderResources(0, 3, srvs);
    ID3D11UnorderedAccessView* uavs[] = {d.errors_uav.Get()};
    d.ctx->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
    ID3D11Buffer* cbs[] = {d.params.Get()};
    d.ctx->CSSetConstantBuffers(0, 1, cbs);

    const int dispatches = tdr ? 1 : dispatches_;
    constexpr UINT groups = kStressN / kTile;
    for (int i = 0; i < dispatches; ++i) d.ctx->Dispatch(groups, groups, 1);
    d.ctx->CopyResource(d.staging.Get(), d.errors.Get());

    // Map blocks until the GPU has finished, so elapsed time is GPU time.
    D3D11_MAPPED_SUBRESOURCE mapped{};
    const HRESULT hr = d.ctx->Map(d.staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr) || d.device->GetDeviceRemovedReason() != S_OK) {
        out.device_lost = true;
        d.device.Reset();   // next Batch() recreates everything
        return finish();
    }
    UINT count = 0;
    std::memcpy(&count, mapped.pData, sizeof(count));
    // The GPU counter is unsigned and spans every dispatch in the batch; clamp
    // so a huge count can never wrap to a negative "no errors".
    out.wrong_values = count > INT_MAX ? INT_MAX : static_cast<int>(count);
    d.ctx->Unmap(d.staging.Get(), 0);
    out.iterations = dispatches;
    finish();
    if (!tdr && out.elapsed_ms < kTargetBatchMs * 0.6 && dispatches_ < kMaxDispatches) dispatches_ *= 2;
    return out;
}

}
```

Add to `CMakeLists.txt`:
```cmake
add_library(hw STATIC
  src/hw/nvml.hpp src/hw/nvml.cpp
  src/hw/nvapi.hpp src/hw/nvapi.cpp
  src/hw/gpu_control.hpp src/hw/gpu_control.cpp
  src/hw/stress.hpp src/hw/stress.cpp
)
target_include_directories(hw PUBLIC src)
target_link_libraries(hw PUBLIC core d3d11 dxgi d3dcompiler)
```

- [ ] **Step 3: Build and run the suite**

Run: `cmake --build build --config Debug; cmake --build build --config Release; ctest --test-dir build -C Release --output-on-failure`
Expected: both configs build with no warnings from `stress.cpp`; `100% tests passed`.

- [ ] **Step 4: Commit**

```bash
git add src/hw/stress.hpp src/hw/stress.cpp CMakeLists.txt
git commit -m "feat(hw): DX11 compute stress load with on-GPU result check"
```

---

### Task 4: `gao --stress` command and docs

**Files:**
- Modify: `src/app/main.cpp` (new `stress()` function + argument parsing + usage line)
- Modify: `docs/hardware-checks.md` (rows 10–12, cross-check wording for 3 and 5)
- Modify: `README.md` (quick-start mentions `--stress`)

**Interfaces:**
- Consumes: `gao::Stress`, `gao::StressSelftest` (Task 3); `gao::run_stability`, `gao::verdict_name`, `gao::Verdict`, `gao::StressBatch` (Task 2); `gao::Nvml` (existing: `Init()`, `Read(unsigned)`, `Error()`); `ParseIntArg`, `FormatField`, `kGpu` (existing in `main.cpp`).
- Produces: CLI `gao --stress <sec> [--max-temp <c>] [--stress-selftest wrong|tdr]` with the exit codes in Global Constraints.

- [ ] **Step 1: Add the command**

In `src/app/main.cpp`, add includes:
```cpp
#include "core/stability.hpp"
#include "hw/stress.hpp"
```

Add after `set_fan()`:
```cpp
// Runs the DX11 stress load for `seconds` of GPU time, printing one line per
// second, then the verdict. Changes no settings, so Ctrl+C is always safe.
// Assumes the first NVIDIA DXGI adapter is NVML device kGpu -- true on a
// single-GPU machine, which is all this CLI supports (see kGpu).
static int stress(int seconds, int max_temp_c, gao::StressSelftest selftest) {
    gao::Nvml nvml;
    if (!nvml.Init()) { std::printf("NVML init failed: %s\n", nvml.Error().c_str()); return 1; }
    gao::Stress load;
    if (!load.Init(selftest)) { std::printf("stress init failed: %s\n", load.Error().c_str()); return 1; }
    std::printf("stress: %s, %d s, abort above %d C\n", load.AdapterName().c_str(), seconds, max_temp_c);

    double t = 0, next_print = 1.0;
    long long window_its = 0;
    double window_s = 0;
    int last_wrong = 0;
    auto batch = [&] {
        const gao::StressBatch b = load.Batch();
        t += b.elapsed_ms / 1000.0;
        window_s += b.elapsed_ms / 1000.0;
        window_its += b.iterations;
        last_wrong = b.wrong_values;
        return b;
    };
    auto read = [&] {
        const gao::Telemetry tel = nvml.Read(kGpu);
        if (t >= next_print) {
            char core[8], mem[8], temp[8], power[8];
            FormatField(core, sizeof(core), tel.core_mhz, "");
            FormatField(mem, sizeof(mem), tel.mem_mhz, "");
            FormatField(temp, sizeof(temp), tel.temp_c, "");
            FormatField(power, sizeof(power), tel.power_w, "");
            std::printf("  t=%.0fs  score=%.0f it/s  core=%s MHz  mem=%s MHz  temp=%s C  power=%s/%d W  errors=%d\n",
                        t, window_its / window_s, core, mem, temp, power, tel.power_limit_w, last_wrong);
            window_its = 0;
            window_s = 0;
            next_print = t + 1.0;
        }
        return tel;
    };
    const gao::StabilityResult r = gao::run_stability(batch, read, seconds, max_temp_c);
    std::printf("VERDICT: %s  score=%.0f it/s  %.1f s  peak=%d C  avg power=%d W  avg core=%d MHz  avg mem=%d MHz\n",
                gao::verdict_name(r.verdict), r.score, r.seconds, r.peak_temp_c, r.avg_power_w,
                r.avg_core_mhz, r.avg_mem_mhz);
    switch (r.verdict) {
        case gao::Verdict::Stable: return 0;
        case gao::Verdict::WrongResult:
        case gao::Verdict::DeviceLost: return 2;
        case gao::Verdict::TooHot:
        case gao::Verdict::NoTelemetry: return 3;
    }
    return 1;
}
```

In `main()`, before the usage line:
```cpp
    if (argc > 2 && std::strcmp(argv[1], "--stress") == 0) {
        int seconds = 0;
        if (!ParseIntArg(argv[2], &seconds) || seconds < 1 || seconds > 3600) {
            std::printf("--stress expects a duration of 1-3600 seconds, got '%s'\n", argv[2]);
            return 1;
        }
        int max_temp = 85;
        auto selftest = gao::StressSelftest::None;
        for (int i = 3; i < argc; i += 2) {
            if (i + 1 >= argc) { std::printf("%s expects a value\n", argv[i]); return 1; }
            if (std::strcmp(argv[i], "--max-temp") == 0) {
                if (!ParseIntArg(argv[i + 1], &max_temp) || max_temp < 40 || max_temp > 95) {
                    std::printf("--max-temp expects 40-95 C, got '%s'\n", argv[i + 1]);
                    return 1;
                }
            } else if (std::strcmp(argv[i], "--stress-selftest") == 0) {
                if (std::strcmp(argv[i + 1], "wrong") == 0) selftest = gao::StressSelftest::WrongResult;
                else if (std::strcmp(argv[i + 1], "tdr") == 0) selftest = gao::StressSelftest::Tdr;
                else { std::printf("--stress-selftest expects 'wrong' or 'tdr', got '%s'\n", argv[i + 1]); return 1; }
            } else {
                std::printf("unknown --stress option '%s'\n", argv[i]);
                return 1;
            }
        }
        return stress(seconds, max_temp, selftest);
    }
```

Replace the usage line with:
```cpp
    std::printf("usage: gao [--version | --probe | --set-core <mhz> | --set-mem <mhz> | --reset | --set-fan <pct>\n"
                "            | --stress <sec> [--max-temp <c>]]\n");
```
(`--stress-selftest` stays out of the usage line on purpose: it exists for the hardware checklist.)

- [ ] **Step 2: Build**

Run: `cmake --build build --config Release; ctest --test-dir build -C Release --output-on-failure`
Expected: builds; `100% tests passed`.

- [ ] **Step 3: Smoke-run on the dev machine (no elevation needed)**

Run: `.\build\Release\gao.exe --stress 5`
Expected: 5 lines `t=1s` … `t=5s` with `errors=0`, then `VERDICT: STABLE`, exit code 0 (`$LASTEXITCODE`). If the GPU is absent (CI-like machine) expect `stress init failed: no NVIDIA adapter found`, exit 1.

- [ ] **Step 4: Update the docs**

In `docs/hardware-checks.md`, append to the table:
```markdown
| 10 | Stress load is stable on stock and loads the card | `.\build\Release\gao.exe --stress 60` (after `--reset`) | no | `VERDICT: STABLE`, exit 0, `avg power` ≥ 95 % of the power limit printed on each line, `errors=0` on every line | |
| 11 | A wrong result is detected | `.\build\Release\gao.exe --stress 10 --stress-selftest wrong` | no | `VERDICT: WRONG RESULT`, exit 2, within the first second | |
| 12 | A TDR is detected and survived | `.\build\Release\gao.exe --stress 10 --stress-selftest tdr`, then `.\build\Release\gao.exe --stress 5` | no | Screen goes black for 1–2 s while Windows resets the driver; first command prints `VERDICT: DEVICE LOST`, exit 2; the second prints `VERDICT: STABLE` | |
```
and change the Command column of checks 3 and 5 to say: "run `gao --stress 120` in a second window first; compare the `core=`/`mem=` of its lines before and after the offset". Add a note above the table: "Checks 10–12 need no elevation: the stress load only computes, it writes no settings."

In `README.md`, in the quick-start block after `--probe`, add:
```powershell
.\build\Release\gao.exe --stress 60
```
and one sentence: "`--stress` runs a DX11 compute load that checks every value it computes and ends with a verdict (`STABLE`, `WRONG RESULT`, `DEVICE LOST`, `TOO HOT`, `NO TELEMETRY`); it changes no settings." Update the "There is no tuning engine, no stress test" sentence in the IMPORTANT block and the Status section to say P2 (stress load and verdict) is done.

- [ ] **Step 5: Check bad input**

Run each; every one must print a message and exit 1 without starting a load:
`gao --stress abc`, `gao --stress 0`, `gao --stress -5`, `gao --stress 10 --max-temp 200`, `gao --stress 10 --stress-selftest boom`, `gao --stress 10 --max-temp`.

- [ ] **Step 6: Commit**

```bash
git add src/app/main.cpp docs/hardware-checks.md README.md
git commit -m "feat(app): gao --stress with verdict and exit codes"
```

---

### Task 5: Hardware verification (with the user, on the RTX 4070)

**Files:**
- Modify: `docs/hardware-checks.md` (Result column), possibly `src/hw/stress.cpp` (step 3)

**Interfaces:**
- Consumes: `gao --stress` (Task 4), `gao --set-core/--set-mem/--reset` (existing, need elevation).
- Produces: filled rows 3, 5, 10, 11, 12.

- [ ] **Step 1: Check 10** — `.\build\Release\gao.exe --reset` (elevated) then `.\build\Release\gao.exe --stress 60`. Record verdict, score, peak temp, avg power vs limit.

- [ ] **Step 2: If avg power < 95 % of the limit**, replace the shader in `src/hw/stress.cpp` with the register-blocked variant below (32×32 tiles, each thread computes 2×2 outputs: four times the arithmetic per shared-memory load), change `constexpr int kTile = 16;` to `constexpr int kTile = 32;` (the dispatch becomes `1024/32 = 32` groups per axis; `numthreads` stays 16×16), rebuild, and repeat step 1. If still < 95 %, stop and report the measured numbers to the user before changing anything else.

```hlsl
#define N 1024
#define T 32
StructuredBuffer<float> A : register(t0);
StructuredBuffer<float> B : register(t1);
StructuredBuffer<float> Ref : register(t2);
RWStructuredBuffer<uint> Errors : register(u0);
cbuffer Params : register(b0) { uint passes; uint3 pad; };
groupshared float As[T][T];
groupshared float Bs[T][T];
[numthreads(16, 16, 1)]
void main(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID) {
    uint r0 = gid.y * T + tid.y * 2;
    uint c0 = gid.x * T + tid.x * 2;
    float c00 = 0, c01 = 0, c10 = 0, c11 = 0;
    [loop] for (uint p = 0; p < passes; ++p) {
        c00 = 0; c01 = 0; c10 = 0; c11 = 0;
        [loop] for (uint k0 = 0; k0 < N; k0 += T) {
            [unroll] for (uint i = 0; i < 2; ++i)
                [unroll] for (uint j = 0; j < 2; ++j) {
                    As[tid.y * 2 + i][tid.x * 2 + j] = A[(r0 + i) * N + k0 + tid.x * 2 + j];
                    Bs[tid.y * 2 + i][tid.x * 2 + j] = B[(k0 + tid.y * 2 + i) * N + c0 + j];
                }
            GroupMemoryBarrierWithGroupSync();
            [unroll] for (uint k = 0; k < T; ++k) {
                float x0 = As[tid.y * 2][k], x1 = As[tid.y * 2 + 1][k];
                float y0 = Bs[k][tid.x * 2], y1 = Bs[k][tid.x * 2 + 1];
                c00 = mad(x0, y0, c00); c01 = mad(x0, y1, c01);
                c10 = mad(x1, y0, c10); c11 = mad(x1, y1, c11);
            }
            GroupMemoryBarrierWithGroupSync();
        }
    }
    uint errs = (c00 != Ref[r0 * N + c0]) + (c01 != Ref[r0 * N + c0 + 1])
              + (c10 != Ref[(r0 + 1) * N + c0]) + (c11 != Ref[(r0 + 1) * N + c0 + 1]);
    if (errs) InterlockedAdd(Errors[0], errs);
}
```

- [ ] **Step 3: Check 11** — `.\build\Release\gao.exe --stress 10 --stress-selftest wrong`. Expect `VERDICT: WRONG RESULT`, exit 2.

- [ ] **Step 4: Check 12** — warn the user the screen will go black for 1–2 s, then `.\build\Release\gao.exe --stress 10 --stress-selftest tdr`, then `.\build\Release\gao.exe --stress 5`. Expect DEVICE LOST (exit 2), then STABLE.

- [ ] **Step 5: Cross-checks 3 and 5** — elevated shell A: `--reset`; shell B: `.\build\Release\gao.exe --stress 120`. After ~20 s note the steady `core=`/`mem=`; in A run `--set-core 25`, wait ~15 s, note `core=`; `--reset`; `--set-mem 100`, wait ~15 s, note `mem=`; `--reset`. Expect core to rise by roughly the offset (a power-limited card may show less than the full 25 MHz — record the actual numbers) and mem by roughly 100 MHz.

- [ ] **Step 6: Record results and commit**

Fill the Result column for rows 3, 5, 10, 11, 12 with the measured numbers and date.
```bash
git add docs/hardware-checks.md src/hw/stress.cpp
git commit -m "docs: record P2 hardware checks on the RTX 4070"
```
