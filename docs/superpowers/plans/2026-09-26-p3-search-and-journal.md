# GPU Auto Optimizer — Phase 3 (Search and Crash-Safe Journal) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `gao --optimize [best|quiet|cool|max]` searches power limit, core offset and memory offset against the stress verdict, applies the result, and never retries a candidate that froze the machine.

**Architecture:** Two new pure `core` units — `journal` (write-ahead log lines → ceilings) and `search` (`highest_stable`, `lowest_passing`, `apply_margin`, `optimize`) — reach hardware only through `GpuControl`, a probe callback wrapping `run_stability`, an abort callback and a log callback, so CI tests the whole search against a fake card. `hw` gains NVML power-limit control (verified by read-back) and a durable journal file. `app` adds `--optimize` with an elevation check and a Ctrl+C handler that lets the search restore stock before exiting.

**Tech Stack:** C++20, MSVC, CMake ≥ 3.28, doctest, nlohmann/json (vendored, `third_party/nlohmann/json.hpp`), NVML (runtime-loaded), Win32 (`FlushFileBuffers`, `SetConsoleCtrlHandler`, token elevation).

**Spec:** `docs/superpowers/specs/2026-09-26-p3-search-and-journal-design.md` (parents: `2026-09-26-p2-stress-and-verdict-design.md`, `2026-09-20-cpp-rewrite-design.md`)

## Global Constraints

- C++20, MSVC, x64 only. English everywhere.
- `src/core/` must not include `windows.h`, NVML, NVAPI or D3D headers.
- Every hardware write is verified by read-back; a mismatch is a failure.
- Hardware code is never unit-tested in CI; verified via `docs/hardware-checks.md` on the RTX 4070.
- Search constants (spec §3): core 15 MHz steps 0..+300; mem 50 MHz steps 0..+1500; power 5 % steps on a grid anchored at 100 %; baseline 30 s; power probes 20 s at 85 °C safety limit; clock probes 3 s; soak 60 s; soak retries 3; efficiency threshold 98 % of reference score; efficiency mode when `perf_push < 0.5`.
- Any `!ok` exit of `optimize()` leaves the card at stock (offsets 0, default power).
- Journal `begin` is durably written **before** the hardware is touched.
- `cmake` is not on `PATH`: use `C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe` (and `ctest.exe` beside it). Commands below say `cmake`/`ctest`.
- Pushing to `origin` is always allowed in this repo; push after each commit.

## Review Focus

1. **Journal file with blank lines, CRLF endings, or a half-written last line** — must parse the good lines and ignore the rest, never crash or invent a ceiling. (Task 1 test: "blank, CRLF and malformed lines are ignored".)
2. **A power range that does not contain 100 %** (odd firmware) — the power step must be skipped with a log line, not search a nonsense grid. (Task 3 test: "power range without 100 % skips the power step".)
3. **A ceiling at or below the search floor** (e.g. a soak froze at core 0) — `highest_stable` must return the floor without probing anything. (Task 2 test: "ceiling at or below lo probes nothing".)
4. **`perf_push` as float** (`0.7f * 150 / 15` is 6.9999…) — the margin must still be 105, not 90. (Task 2 test: "margin survives float rounding".)
5. **Abort during the power step** — power limit already changed; the stop path must restore default power too. (Task 3 test: "abort during the power step restores stock power".)

---

## File Structure

| File | Responsibility |
|---|---|
| `src/core/types.hpp` | + `power_limit_range_pct` callback on `GpuControl` |
| `src/core/journal.hpp/.cpp` | `Ceilings`, `Journal`: parse lines, build begin/complete lines, ceilings, freezes, next id |
| `src/core/search.hpp/.cpp` | `highest_stable`, `lowest_passing`, `apply_margin`, `Probe`, `OptimizeIo`, `OptimizeResult`, `optimize` |
| `src/hw/nvml.hpp/.cpp` | + `PowerLimitRangePct`, `SetPowerLimitPct` (read-back verified) |
| `src/hw/gpu_control.cpp` | wire power callbacks; `reset_to_stock` restores default power |
| `src/hw/journal_file.hpp/.cpp` | `journal_path`, `read_lines`, `append_line_durable` |
| `src/app/main.cpp` | `--optimize`; `--reset` also restores power |
| `tests/test_journal.cpp`, `tests/test_search.cpp` | unit tests |
| `docs/hardware-checks.md`, `README.md` | rows 13–16, `--optimize` docs |

---

### Task 1: Journal

**Files:**
- Create: `src/core/journal.hpp`, `src/core/journal.cpp`, `tests/test_journal.cpp`
- Modify: `CMakeLists.txt` (add to `core`, `core_tests`; `core` already has `third_party` as a private include dir)

**Interfaces:**
- Consumes: nothing new.
- Produces:
  ```cpp
  namespace gao {
  struct Ceilings { int core_mhz = INT_MAX; int mem_mhz = INT_MAX; };   // exclusive upper bounds
  class Journal {
  public:
      using Append = std::function<bool(const std::string&)>;
      Journal(const std::vector<std::string>& existing_lines, Append append);
      const Ceilings& ceilings() const;
      const std::vector<std::string>& freezes() const;   // e.g. "core +165", "core +105 / mem +700"
      int next_id() const;
      int begin(std::optional<int> core, std::optional<int> mem);   // id, or -1 if append failed
      bool complete(int id, const std::string& verdict);
  };
  }
  ```

- [ ] **Step 1: Write the failing tests**

`tests/test_journal.cpp`:
```cpp
#include "doctest/doctest.h"
#include "core/journal.hpp"
#include <climits>
#include <string>
#include <vector>

using namespace gao;

namespace {
struct Sink {
    std::vector<std::string> lines;
    bool ok = true;
    Journal::Append append() { return [this](const std::string& l) { if (ok) lines.push_back(l); return ok; }; }
};
}

TEST_CASE("an empty journal has no ceilings and starts at id 1") {
    Sink s;
    Journal j({}, s.append());
    CHECK(j.ceilings().core_mhz == INT_MAX);
    CHECK(j.ceilings().mem_mhz == INT_MAX);
    CHECK(j.freezes().empty());
    CHECK(j.next_id() == 1);
}

TEST_CASE("begin and complete write lines that parse back as a finished candidate") {
    Sink s;
    Journal j({}, s.append());
    const int id = j.begin(165, std::nullopt);
    CHECK(id == 1);
    REQUIRE(s.lines.size() == 1);
    CHECK(s.lines[0].find("\"core\":165") != std::string::npos);
    CHECK(s.lines[0].find("\"begin\"") != std::string::npos);
    CHECK(j.complete(id, "STABLE"));
    Journal reread(s.lines, s.append());
    CHECK(reread.ceilings().core_mhz == INT_MAX);
    CHECK(reread.freezes().empty());
    CHECK(reread.next_id() == 2);
}

TEST_CASE("a begin without complete becomes a ceiling") {
    Sink s;
    Journal first({}, s.append());
    first.begin(165, std::nullopt);   // the machine "froze" here
    Journal after(s.lines, s.append());
    CHECK(after.ceilings().core_mhz == 165);
    CHECK(after.ceilings().mem_mhz == INT_MAX);
    REQUIRE(after.freezes().size() == 1);
    CHECK(after.freezes()[0] == "core +165");
}

TEST_CASE("a soak freeze lowers both ceilings and the lowest freeze wins") {
    Sink s;
    Journal first({}, s.append());
    first.begin(105, 700);
    first.begin(200, std::nullopt);
    Journal after(s.lines, s.append());
    CHECK(after.ceilings().core_mhz == 105);
    CHECK(after.ceilings().mem_mhz == 700);
    CHECK(after.freezes().size() == 2);
    CHECK(after.freezes()[0] == "core +105 / mem +700");
}

TEST_CASE("blank, CRLF and malformed lines are ignored") {
    Sink s;
    const std::vector<std::string> lines = {
        "",
        "{\"id\":4,\"mem\":900,\"state\":\"begin\"}\r",
        "garbage",
        "{\"id\":5,\"co",                     // half-written after a power cut
        "{\"state\":\"begin\",\"core\":60}",  // no id
    };
    Journal j(lines, s.append());
    CHECK(j.ceilings().mem_mhz == 900);
    CHECK(j.ceilings().core_mhz == INT_MAX);
    CHECK(j.next_id() == 5);
}

TEST_CASE("next id follows the highest id seen, including completes") {
    Sink s;
    Journal j({"{\"id\":9,\"state\":\"complete\",\"verdict\":\"STABLE\"}"}, s.append());
    CHECK(j.next_id() == 10);
    CHECK(j.begin(15, std::nullopt) == 10);
    CHECK(j.next_id() == 11);
}

TEST_CASE("a failed append reports -1 and does not advance") {
    Sink s;
    s.ok = false;
    Journal j({}, s.append());
    CHECK(j.begin(15, std::nullopt) == -1);
    CHECK(j.next_id() == 1);
}
```

Add `src/core/journal.hpp`, `src/core/journal.cpp` to `core` and `tests/test_journal.cpp` to `core_tests` in `CMakeLists.txt`; create both source files empty (`#pragma once` in the header) so the build reaches the test.

- [ ] **Step 2: Run to verify it fails**

Run: `cmake -S . -B build -A x64; cmake --build build --config Debug`
Expected: FAIL — `Journal` / `Ceilings` undeclared in `test_journal.cpp`.

- [ ] **Step 3: Implement**

`src/core/journal.hpp`:
```cpp
#pragma once
#include <climits>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace gao {

// Exclusive upper bounds the search must stay below. INT_MAX = no limit.
struct Ceilings {
    int core_mhz = INT_MAX;
    int mem_mhz = INT_MAX;
};

// Write-ahead log of every clock candidate. begin() is written (and, in
// production, flushed to disk) before the candidate is applied; complete()
// after its probe. A begin with no complete therefore marks the candidate
// that froze the machine, and it becomes a ceiling for every later run.
// Pure: the file lives in hw/journal_file, reached through `append`.
class Journal {
public:
    using Append = std::function<bool(const std::string&)>;

    Journal(const std::vector<std::string>& existing_lines, Append append);

    const Ceilings& ceilings() const { return ceilings_; }
    // Human-readable description of each unfinished candidate, in file order.
    const std::vector<std::string>& freezes() const { return freezes_; }
    int next_id() const { return next_id_; }

    // Returns the entry id, or -1 when the line could not be written -- in
    // which case the caller must not touch the hardware.
    int begin(std::optional<int> core, std::optional<int> mem);
    bool complete(int id, const std::string& verdict);

private:
    Append append_;
    Ceilings ceilings_;
    std::vector<std::string> freezes_;
    int next_id_ = 1;
};

}
```

`src/core/journal.cpp`:
```cpp
#include "core/journal.hpp"
#include "nlohmann/json.hpp"
#include <algorithm>
#include <map>

namespace gao {

namespace {
struct Pending {
    std::optional<int> core, mem;
};

std::string describe(const Pending& p) {
    std::string s;
    if (p.core) s += "core +" + std::to_string(*p.core);
    if (p.mem) s += (s.empty() ? "" : " / ") + std::string("mem +") + std::to_string(*p.mem);
    return s.empty() ? "settings" : s;
}

std::optional<int> int_field(const nlohmann::json& j, const char* key) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_number_integer()) return std::nullopt;
    return it->get<int>();
}
}

Journal::Journal(const std::vector<std::string>& existing_lines, Append append)
    : append_(std::move(append)) {
    std::map<int, Pending> pending;   // ordered by id = file order
    int max_id = 0;
    for (std::string line : existing_lines) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const nlohmann::json j = nlohmann::json::parse(line, nullptr, /*allow_exceptions=*/false);
        if (j.is_discarded() || !j.is_object()) continue;
        const auto id = int_field(j, "id");
        const auto state = j.find("state");
        if (!id || state == j.end() || !state->is_string()) continue;
        max_id = std::max(max_id, *id);
        if (*state == "begin") pending[*id] = {int_field(j, "core"), int_field(j, "mem")};
        else if (*state == "complete") pending.erase(*id);
    }
    for (const auto& [id, p] : pending) {
        if (p.core) ceilings_.core_mhz = std::min(ceilings_.core_mhz, *p.core);
        if (p.mem) ceilings_.mem_mhz = std::min(ceilings_.mem_mhz, *p.mem);
        freezes_.push_back(describe(p));
    }
    next_id_ = max_id + 1;
}

int Journal::begin(std::optional<int> core, std::optional<int> mem) {
    nlohmann::json j = {{"id", next_id_}, {"state", "begin"}};
    if (core) j["core"] = *core;
    if (mem) j["mem"] = *mem;
    if (!append_(j.dump())) return -1;
    return next_id_++;
}

bool Journal::complete(int id, const std::string& verdict) {
    const nlohmann::json j = {{"id", id}, {"state", "complete"}, {"verdict", verdict}};
    return append_(j.dump());
}

}
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `cmake --build build --config Debug; .\build\Debug\core_tests.exe`
Expected: `Status: SUCCESS!`, no failures.

- [ ] **Step 5: Commit and push**

```bash
git add src/core/journal.hpp src/core/journal.cpp tests/test_journal.cpp CMakeLists.txt
git commit -m "feat(core): crash-safe journal that turns freezes into ceilings"
git push origin main
```

---

### Task 2: Search primitives

**Files:**
- Create: `src/core/search.hpp`, `src/core/search.cpp`, `tests/test_search.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: nothing new yet.
- Produces:
  ```cpp
  namespace gao {
  int highest_stable(int lo, int hi, int step, int ceiling, const std::function<bool(int)>& is_stable);
  int lowest_passing(int lo, int hi, int step, const std::function<bool(int)>& passes);
  int apply_margin(int max, int step, float perf_push);
  }
  ```

- [ ] **Step 1: Write the failing tests**

`tests/test_search.cpp`:
```cpp
#include "doctest/doctest.h"
#include "core/search.hpp"
#include <climits>
#include <vector>

using namespace gao;

TEST_CASE("highest_stable finds the edge of a monotonic range") {
    std::vector<int> probed;
    const int r = highest_stable(0, 300, 15, INT_MAX, [&](int v) { probed.push_back(v); return v <= 150; });
    CHECK(r == 150);
    CHECK(probed.size() <= 5);   // 20 candidates -> binary search
    for (int v : probed) CHECK(v % 15 == 0);
}

TEST_CASE("highest_stable never probes lo and returns lo when nothing above is stable") {
    std::vector<int> probed;
    const int r = highest_stable(0, 300, 15, INT_MAX, [&](int v) { probed.push_back(v); return false; });
    CHECK(r == 0);
    for (int v : probed) CHECK(v != 0);
}

TEST_CASE("highest_stable stays below the ceiling") {
    int max_probed = 0;
    const int r = highest_stable(0, 300, 15, 120, [&](int v) { max_probed = std::max(max_probed, v); return true; });
    CHECK(r == 105);
    CHECK(max_probed == 105);
}

TEST_CASE("ceiling at or below lo probes nothing") {
    int calls = 0;
    CHECK(highest_stable(0, 300, 15, 0, [&](int) { ++calls; return true; }) == 0);
    CHECK(calls == 0);
}

TEST_CASE("highest_stable with everything stable returns the top of the grid") {
    CHECK(highest_stable(50, 120, 5, INT_MAX, [](int) { return true; }) == 120);
    CHECK(highest_stable(0, 1500, 50, INT_MAX, [](int) { return true; }) == 1500);
}

TEST_CASE("lowest_passing finds the lowest value that still passes and never probes hi") {
    std::vector<int> probed;
    const int r = lowest_passing(50, 115, 5, [&](int v) { probed.push_back(v); return v >= 90; });
    CHECK(r == 90);
    for (int v : probed) CHECK(v != 115);
    CHECK(lowest_passing(50, 115, 5, [](int) { return true; }) == 50);
    CHECK(lowest_passing(50, 115, 5, [](int) { return false; }) == 115);
}

TEST_CASE("apply_margin scales and keeps one step of headroom") {
    CHECK(apply_margin(150, 15, 1.0f) == 135);
    CHECK(apply_margin(150, 15, 0.4f) == 60);
    CHECK(apply_margin(800, 50, 0.7f) == 550);
    CHECK(apply_margin(0, 15, 1.0f) == 0);
    CHECK(apply_margin(15, 15, 1.0f) == 0);
}

TEST_CASE("margin survives float rounding") {
    CHECK(apply_margin(150, 15, 0.7f) == 105);   // 0.7f * 150 / 15 = 6.99999...
}
```

Register `src/core/search.hpp/.cpp` in `core` and `tests/test_search.cpp` in `core_tests`; create empty stubs so the build reaches the test.

- [ ] **Step 2: Run to verify it fails**

Run: `cmake -S . -B build -A x64; cmake --build build --config Debug`
Expected: FAIL — `highest_stable` identifier not found.

- [ ] **Step 3: Implement**

`src/core/search.hpp`:
```cpp
#pragma once
#include <functional>

namespace gao {

// Highest value in lo, lo+step, ... (<= hi, < ceiling) for which is_stable
// holds, assuming stability is monotonic: once a value fails, every higher
// one fails too. lo itself is assumed stable (it is stock) and never probed.
int highest_stable(int lo, int hi, int step, int ceiling, const std::function<bool(int)>& is_stable);

// Lowest value in hi, hi-step, ... (>= lo) for which passes holds, assuming
// passing is monotonic upward. hi is the reference and is assumed to pass;
// it is never probed.
int lowest_passing(int lo, int hi, int step, const std::function<bool(int)>& passes);

// The offset to apply: perf_push * max, rounded down to a step, and always at
// least one step below max so a 3 s probe's blind spot has headroom.
int apply_margin(int max, int step, float perf_push);

}
```

`src/core/search.cpp`:
```cpp
#include "core/search.hpp"
#include <algorithm>
#include <cmath>

namespace gao {

int highest_stable(int lo, int hi, int step, int ceiling, const std::function<bool(int)>& is_stable) {
    const int top = std::min(hi, ceiling - 1);
    const int n = top > lo ? (top - lo) / step : 0;   // candidates lo+step .. lo+n*step
    int good = 0, bad = n + 1;                          // indices; 0 = lo, assumed stable
    while (bad - good > 1) {
        const int mid = (good + bad) / 2;
        if (is_stable(lo + mid * step)) good = mid;
        else bad = mid;
    }
    return lo + good * step;
}

int lowest_passing(int lo, int hi, int step, const std::function<bool(int)>& passes) {
    const int n = hi > lo ? (hi - lo) / step : 0;       // candidates hi - n*step .. hi
    int good = n, bad = -1;                             // index n = hi, assumed passing
    while (good - bad > 1) {
        const int mid = (good + bad) / 2;
        if (passes(hi - (n - mid) * step)) good = mid;
        else bad = mid;
    }
    return hi - (n - good) * step;
}

int apply_margin(int max, int step, float perf_push) {
    if (max <= 0) return 0;
    // +1e-4: perf_push is a float, and 0.7f * 150 / 15 is 6.9999999, which a
    // bare floor would turn into one step less than intended.
    const int steps = static_cast<int>(std::floor(double(perf_push) * max / step + 1e-4));
    return std::max(0, std::min(steps * step, max - step));
}

}
```

- [ ] **Step 4: Run tests**

Run: `cmake --build build --config Debug; .\build\Debug\core_tests.exe`
Expected: `Status: SUCCESS!`.

- [ ] **Step 5: Commit and push**

```bash
git add src/core/search.hpp src/core/search.cpp tests/test_search.cpp CMakeLists.txt
git commit -m "feat(core): binary search and safety margin for the tuner"
git push origin main
```

---

### Task 3: `optimize()`

**Files:**
- Modify: `src/core/types.hpp` (add `power_limit_range_pct`), `src/core/search.hpp`, `src/core/search.cpp`, `tests/test_search.cpp`

**Interfaces:**
- Consumes: `Journal` (Task 1), `highest_stable`/`lowest_passing`/`apply_margin` (Task 2), `StabilityResult`/`Verdict`/`verdict_name` (P2, `core/stability.hpp`), `Objectives` (`core/objectives.hpp`), `GpuControl` (`core/types.hpp`).
- Produces:
  ```cpp
  // types.hpp, in GpuControl:
  std::function<std::pair<int, int>()> power_limit_range_pct;   // {min, max} % of default
  // search.hpp:
  using Probe = std::function<StabilityResult(double seconds, int max_temp_c)>;
  struct OptimizeIo { Probe probe; std::function<bool()> aborted; std::function<void(const std::string&)> log; };
  struct OptimizeResult { bool ok = false; std::string reason; int power_pct = 100;
                          int core_mhz = 0, mem_mhz = 0, core_max_stable = 0, mem_max_stable = 0;
                          StabilityResult baseline, soak; };
  OptimizeResult optimize(const GpuControl& gpu, const Objectives& obj, Journal& journal, const OptimizeIo& io);
  ```

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_search.cpp` (add includes `core/journal.hpp`, `core/objectives.hpp`, `<string>`, `<algorithm>` at the top):
```cpp
namespace {
// A modelled card: stable up to core +150 / mem +800; peak temp = 40 +
// 0.3 * power%; score rises with power up to 90 %, flat above.
struct FakeCard {
    int power = 100, core = 0, mem = 0;
    int core_edge = 150, mem_edge = 800;
    bool stock_unstable = false;
    int soak_failures = 0;           // how many 60 s probes fail before one passes
    int fail_core_set_at = -1;       // set_core_offset(this) returns false
    int max_core_seen = 0;
    int probes = 0, power_probes = 0;
    std::vector<std::string> journal;
    std::string last_set_begin_ok;   // "" if every set had its begin line first

    GpuControl gpu(bool with_power = true, std::pair<int, int> range = {50, 120}) {
        GpuControl g;
        g.read = [this] { Telemetry t; t.ok = true; t.temp_c = 40; t.core_mhz = 2800; return t; };
        g.set_core_offset = [this](int v) {
            // Every non-stock clock must be preceded by an open journal line.
            if (v != 0 && (journal.empty() || journal.back().find("\"begin\"") == std::string::npos))
                last_set_begin_ok = "core " + std::to_string(v) + " set without begin";
            if (v == fail_core_set_at) return false;
            core = v; max_core_seen = std::max(max_core_seen, v); return true;
        };
        g.set_mem_offset = [this](int v) { mem = v; return true; };
        g.reset_to_stock = [this] { power = 100; core = 0; mem = 0; return true; };
        if (with_power) {
            g.set_power_limit = [this](int p) { power = p; return true; };
            g.power_limit_range_pct = [range] { return range; };
        }
        return g;
    }
    Probe probe() {
        return [this](double seconds, int max_temp) {
            ++probes;
            if (seconds == 20) ++power_probes;
            StabilityResult r;
            r.seconds = seconds;
            r.peak_temp_c = static_cast<int>(40 + 0.3 * power);
            r.score = 1000.0 * std::min(power, 90) / 90;
            if (stock_unstable || core > core_edge || mem > mem_edge) r.verdict = Verdict::WrongResult;
            else if (seconds == 60 && soak_failures > 0) { --soak_failures; r.verdict = Verdict::WrongResult; }
            else if (r.peak_temp_c > max_temp) r.verdict = Verdict::TooHot;
            return r;
        };
    }
};

struct Run {
    FakeCard card;
    std::vector<std::string> log;
    int abort_after_probes = -1;
    OptimizeResult go(Preset preset, GpuControl gpu) {
        Journal j(card.journal, [this](const std::string& l) { card.journal.push_back(l); return true; });
        OptimizeIo io;
        io.probe = card.probe();
        io.aborted = [this] { return abort_after_probes >= 0 && card.probes >= abort_after_probes; };
        io.log = [this](const std::string& m) { log.push_back(m); };
        return optimize(gpu, objectives_for(preset), j, io);
    }
    OptimizeResult go(Preset preset) { return go(preset, card.gpu()); }
};
}

TEST_CASE("best preset converges to the card's edges and applies the margin") {
    Run run;
    const auto r = run.go(Preset::BestOfMyGpu);
    REQUIRE(r.ok);
    CHECK(r.power_pct == 115);          // highest with 40 + 0.3p <= 75
    CHECK(r.core_max_stable == 150);
    CHECK(r.core_mhz == 105);
    CHECK(r.mem_max_stable == 800);
    CHECK(r.mem_mhz == 550);
    CHECK(run.card.core == 105);        // left applied
    CHECK(run.card.mem == 550);
    CHECK(run.card.power == 115);
    CHECK(r.soak.verdict == Verdict::Stable);
    CHECK(run.card.last_set_begin_ok.empty());
}

TEST_CASE("quiet preset picks the lowest power within 2 % of the reference score") {
    Run run;
    const auto r = run.go(Preset::Quiet);
    REQUIRE(r.ok);
    CHECK(r.power_pct == 90);
    CHECK(r.core_mhz == 60);
    CHECK(r.mem_mhz == 300);
}

TEST_CASE("max preset takes the thermal cap and one step below the clock edges") {
    Run run;
    const auto r = run.go(Preset::MaxPerformance);
    REQUIRE(r.ok);
    CHECK(r.power_pct == 120);
    CHECK(r.core_mhz == 135);
    CHECK(r.mem_mhz == 750);
}

TEST_CASE("cool preset tunes power only") {
    Run run;
    const auto r = run.go(Preset::CoolAndEfficient);
    REQUIRE(r.ok);
    CHECK(r.power_pct == 85);   // peak int(40 + 25.5) = 65 <= 65; 80 would cost 6 % score
    CHECK(r.core_mhz == 0);
    CHECK(r.mem_mhz == 0);
    CHECK(run.card.max_core_seen == 0);
}

TEST_CASE("journal ceilings are respected") {
    Run run;
    run.card.journal = {"{\"id\":3,\"core\":120,\"state\":\"begin\"}"};
    const auto r = run.go(Preset::BestOfMyGpu);
    REQUIRE(r.ok);
    CHECK(r.core_max_stable == 105);
    CHECK(run.card.max_core_seen < 120);
}

TEST_CASE("an unstable stock card aborts before tuning") {
    Run run;
    run.card.stock_unstable = true;
    const auto r = run.go(Preset::BestOfMyGpu);
    CHECK_FALSE(r.ok);
    CHECK(r.reason.find("stock") != std::string::npos);
    CHECK(run.card.probes == 1);
    CHECK(run.card.core == 0);
    CHECK(run.card.power == 100);
}

TEST_CASE("a failed soak steps both clocks down once and succeeds") {
    Run run;
    run.card.soak_failures = 1;
    const auto r = run.go(Preset::BestOfMyGpu);
    REQUIRE(r.ok);
    CHECK(r.core_mhz == 90);
    CHECK(r.mem_mhz == 500);
}

TEST_CASE("four failed soaks give up and restore stock") {
    Run run;
    run.card.soak_failures = 4;
    const auto r = run.go(Preset::BestOfMyGpu);
    CHECK_FALSE(r.ok);
    CHECK(run.card.core == 0);
    CHECK(run.card.mem == 0);
    CHECK(run.card.power == 100);
}

TEST_CASE("abort during the clock search restores stock") {
    Run run;
    run.abort_after_probes = 6;   // baseline + ~3 power probes + into core
    const auto r = run.go(Preset::BestOfMyGpu);
    CHECK_FALSE(r.ok);
    CHECK(r.reason == "aborted");
    CHECK(run.card.core == 0);
    CHECK(run.card.power == 100);
}

TEST_CASE("abort during the power step restores stock power") {
    Run run;
    run.abort_after_probes = 2;   // baseline + first power probe
    const auto r = run.go(Preset::BestOfMyGpu);
    CHECK_FALSE(r.ok);
    CHECK(r.reason == "aborted");
    CHECK(run.card.power == 100);
}

TEST_CASE("no power control skips the power step") {
    Run run;
    const auto r = run.go(Preset::BestOfMyGpu, run.card.gpu(/*with_power=*/false));
    REQUIRE(r.ok);
    CHECK(r.power_pct == 100);
    CHECK(run.card.power_probes == 0);
}

TEST_CASE("power range without 100 % skips the power step") {
    Run run;
    const auto r = run.go(Preset::BestOfMyGpu, run.card.gpu(true, {110, 120}));
    REQUIRE(r.ok);
    CHECK(r.power_pct == 100);
    CHECK(run.card.power_probes == 0);
}

TEST_CASE("a setter that fails stops the run at stock") {
    Run run;
    run.card.fail_core_set_at = 150;
    const auto r = run.go(Preset::BestOfMyGpu);
    CHECK_FALSE(r.ok);
    CHECK(r.reason.find("core") != std::string::npos);
    CHECK(run.card.core == 0);
    CHECK(run.card.power == 100);
}

TEST_CASE("every clock candidate is closed in the journal") {
    Run run;
    run.go(Preset::BestOfMyGpu);
    Journal reread(run.card.journal, [](const std::string&) { return true; });
    CHECK(reread.freezes().empty());
}
```

In `src/core/types.hpp`, add `#include <utility>` and to `GpuControl` after `set_power_limit`:
```cpp
    std::function<std::pair<int, int>()> power_limit_range_pct;   // {min, max}, percent of default
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build build --config Debug`
Expected: FAIL — `optimize`, `OptimizeIo`, `Probe` undeclared.

- [ ] **Step 3: Implement**

Append to `src/core/search.hpp` (add includes `core/journal.hpp`, `core/objectives.hpp`, `core/stability.hpp`, `core/types.hpp`, `<string>`):
```cpp
// One stress run: seconds of load, aborting above max_temp_c.
using Probe = std::function<StabilityResult(double seconds, int max_temp_c)>;

struct OptimizeIo {
    Probe probe;
    std::function<bool()> aborted;                      // polled between probes
    std::function<void(const std::string&)> log;
};

struct OptimizeResult {
    bool ok = false;
    std::string reason;        // why it stopped, when !ok
    int power_pct = 100;
    int core_mhz = 0;
    int mem_mhz = 0;
    int core_max_stable = 0;
    int mem_max_stable = 0;
    StabilityResult baseline;
    StabilityResult soak;
};

// The whole tuning run (spec §3): baseline, power, core, memory, soak. Leaves
// the result applied when ok; any !ok return leaves the card at stock.
OptimizeResult optimize(const GpuControl& gpu, const Objectives& obj, Journal& journal, const OptimizeIo& io);
```

Append to `src/core/search.cpp` (add includes `<climits>`, `<cstdio>`, `<map>`, `<optional>`):
```cpp
namespace {
constexpr int kCoreStep = 15, kCoreMax = 300;
constexpr int kMemStep = 50, kMemMax = 1500;
constexpr int kPowerStep = 5;
constexpr double kBaselineS = 30, kPowerProbeS = 20, kClockProbeS = 3, kSoakS = 60;
constexpr int kSafetyTempC = 85;
constexpr int kSoakRetries = 3;
constexpr double kEfficiencyScore = 0.98;
constexpr float kEfficiencyBelowPush = 0.5f;

std::string describe(const StabilityResult& s) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%s  score=%.0f it/s  peak=%d C  power=%d W",
                  verdict_name(s.verdict), s.score, s.peak_temp_c, s.avg_power_w);
    return buf;
}
}

OptimizeResult optimize(const GpuControl& gpu, const Objectives& obj, Journal& journal, const OptimizeIo& io) {
    OptimizeResult r;
    std::string stopped;   // non-empty once the run must end; later probes become no-ops
    auto log = [&](const std::string& m) { if (io.log) io.log(m); };
    auto finish_fail = [&](const std::string& why) {
        if (gpu.reset_to_stock) gpu.reset_to_stock();
        r.ok = false;
        r.reason = why;
        log("stopped: " + why + " -- card restored to stock");
        return r;
    };
    bool power_ctl = obj.power && gpu.set_power_limit && gpu.power_limit_range_pct;   // false once skipped

    // Applies the full state for every candidate, so a TDR that reset the
    // driver (or a previous candidate) can never leave stale settings behind.
    auto set_state = [&](int power, int core, int mem) -> bool {
        if (power_ctl && !gpu.set_power_limit(power)) { stopped = "setting power " + std::to_string(power) + " % failed"; return false; }
        if ((obj.core_oc || core != 0) && !(gpu.set_core_offset && gpu.set_core_offset(core))) {
            stopped = "setting core +" + std::to_string(core) + " failed"; return false;
        }
        if ((obj.mem_oc || mem != 0) && !(gpu.set_mem_offset && gpu.set_mem_offset(mem))) {
            stopped = "setting mem +" + std::to_string(mem) + " failed"; return false;
        }
        return true;
    };
    // One probe, with the abort and blindness checks every step shares.
    auto probe = [&](double seconds, int max_temp) -> std::optional<StabilityResult> {
        if (!stopped.empty()) return std::nullopt;
        if (io.aborted && io.aborted()) { stopped = "aborted"; return std::nullopt; }
        const StabilityResult s = io.probe(seconds, max_temp);
        if (s.verdict == Verdict::NoTelemetry) { stopped = "lost telemetry"; return std::nullopt; }
        return s;
    };

    if (!gpu.reset_to_stock || !gpu.reset_to_stock()) return finish_fail("could not reset to stock");
    log("baseline: 30 s at stock");
    const auto base = probe(kBaselineS, kSafetyTempC);
    if (!base) return finish_fail(stopped);
    r.baseline = *base;
    log("baseline: " + describe(*base));
    if (base->verdict != Verdict::Stable) return finish_fail(std::string("stock is not stable (") + verdict_name(base->verdict) + ")");

    // Power.
    if (power_ctl) {
        const auto [min_pct, max_pct] = gpu.power_limit_range_pct();
        if (min_pct > 100 || max_pct < 100 || min_pct >= max_pct) {
            log("power: range " + std::to_string(min_pct) + "-" + std::to_string(max_pct) + " % has no room around 100 %, skipped");
            power_ctl = false;
        } else {
            const int grid_lo = 100 - (100 - min_pct) / kPowerStep * kPowerStep;
            std::map<int, StabilityResult> seen;
            auto run_power = [&](int pct) -> std::optional<StabilityResult> {
                if (!stopped.empty()) return std::nullopt;
                if (io.aborted && io.aborted()) { stopped = "aborted"; return std::nullopt; }
                if (!set_state(pct, 0, 0)) return std::nullopt;
                const auto s = probe(kPowerProbeS, kSafetyTempC);
                if (s) { seen[pct] = *s; log("power " + std::to_string(pct) + " %: " + describe(*s)); }
                return s;
            };
            const int cap = highest_stable(grid_lo, max_pct, kPowerStep, INT_MAX, [&](int pct) {
                const auto s = run_power(pct);
                return s && s->verdict == Verdict::Stable && s->peak_temp_c <= obj.max_temp_c;
            });
            if (!stopped.empty()) return finish_fail(stopped);
            r.power_pct = cap;
            if (obj.perf_push < kEfficiencyBelowPush) {
                const auto ref = seen.count(cap) ? std::optional<StabilityResult>(seen[cap]) : run_power(cap);
                if (!ref) return finish_fail(stopped);
                r.power_pct = lowest_passing(grid_lo, cap, kPowerStep, [&](int pct) {
                    const auto s = run_power(pct);
                    return s && s->verdict == Verdict::Stable && s->score >= kEfficiencyScore * ref->score;
                });
                if (!stopped.empty()) return finish_fail(stopped);
            }
            log("power: " + std::to_string(r.power_pct) + " %");
        }
    }

    // One clock candidate: journal first, then hardware, then probe.
    auto clock_candidate = [&](std::optional<int> core_j, std::optional<int> mem_j, int core, int mem) {
        if (!stopped.empty()) return false;
        if (io.aborted && io.aborted()) { stopped = "aborted"; return false; }
        const int id = journal.begin(core_j, mem_j);
        if (id < 0) { stopped = "could not write the journal"; return false; }
        if (!set_state(r.power_pct, core, mem)) { journal.complete(id, "SET FAILED"); return false; }
        const auto s = probe(kClockProbeS, obj.max_temp_c);
        journal.complete(id, s ? verdict_name(s->verdict) : "NOT RUN");
        if (!s) return false;
        log("core +" + std::to_string(core) + " / mem +" + std::to_string(mem) + ": " + describe(*s));
        return s->verdict == Verdict::Stable;
    };

    if (obj.core_oc) {
        r.core_max_stable = highest_stable(0, kCoreMax, kCoreStep, journal.ceilings().core_mhz,
                                           [&](int v) { return clock_candidate(v, std::nullopt, v, 0); });
        if (!stopped.empty()) return finish_fail(stopped);
        r.core_mhz = apply_margin(r.core_max_stable, kCoreStep, obj.perf_push);
        log("core: highest stable +" + std::to_string(r.core_max_stable) + ", applying +" + std::to_string(r.core_mhz));
    }
    if (obj.mem_oc) {
        r.mem_max_stable = highest_stable(0, kMemMax, kMemStep, journal.ceilings().mem_mhz,
                                          [&](int v) { return clock_candidate(std::nullopt, v, r.core_mhz, v); });
        if (!stopped.empty()) return finish_fail(stopped);
        r.mem_mhz = apply_margin(r.mem_max_stable, kMemStep, obj.perf_push);
        log("mem: highest stable +" + std::to_string(r.mem_max_stable) + ", applying +" + std::to_string(r.mem_mhz));
    }

    // Soak, stepping both clocks down on failure.
    for (int attempt = 0; attempt <= kSoakRetries; ++attempt) {
        if (io.aborted && io.aborted()) return finish_fail("aborted");
        const int id = journal.begin(obj.core_oc ? std::optional<int>(r.core_mhz) : std::nullopt,
                                     obj.mem_oc ? std::optional<int>(r.mem_mhz) : std::nullopt);
        if (id < 0) return finish_fail("could not write the journal");
        if (!set_state(r.power_pct, r.core_mhz, r.mem_mhz)) { journal.complete(id, "SET FAILED"); return finish_fail(stopped); }
        log("soak: 60 s at power " + std::to_string(r.power_pct) + " %, core +" + std::to_string(r.core_mhz) +
            ", mem +" + std::to_string(r.mem_mhz));
        const auto s = probe(kSoakS, obj.max_temp_c);
        journal.complete(id, s ? verdict_name(s->verdict) : "NOT RUN");
        if (!s) return finish_fail(stopped);
        log("soak: " + describe(*s));
        if (s->verdict == Verdict::Stable) {
            r.soak = *s;
            r.ok = true;
            return r;
        }
        if (obj.core_oc) r.core_mhz = std::max(0, r.core_mhz - kCoreStep);
        if (obj.mem_oc) r.mem_mhz = std::max(0, r.mem_mhz - kMemStep);
    }
    return finish_fail("soak failed " + std::to_string(kSoakRetries + 1) + " times");
}
```

- [ ] **Step 4: Run tests**

Run: `cmake --build build --config Debug; .\build\Debug\core_tests.exe`
Expected: `Status: SUCCESS!`. If a test's expected number differs, recompute it from the FakeCard model before touching code — the model, not the implementation, defines the expectation.

- [ ] **Step 5: Commit and push**

```bash
git add src/core/types.hpp src/core/search.hpp src/core/search.cpp tests/test_search.cpp
git commit -m "feat(core): optimize() -- power, core, memory, soak with journal ceilings"
git push origin main
```

---

### Task 4: NVML power limit and stock reset

**Files:**
- Modify: `src/hw/nvml.hpp`, `src/hw/nvml.cpp`, `src/hw/gpu_control.cpp`, `src/hw/gpu_control.hpp` (comment only), `src/app/main.cpp` (`reset()`)

**Interfaces:**
- Consumes: `GpuControl::power_limit_range_pct` (Task 3).
- Produces:
  ```cpp
  // Nvml
  std::optional<std::pair<int, int>> PowerLimitRangePct(unsigned index);  // {min, max} % of default
  bool SetPowerLimitPct(unsigned index, int pct);                          // verified by read-back
  ```

No unit test (hardware). Deliverable: builds warning-free, suite green, and `gao --reset` still works (step 4).

- [ ] **Step 1: Add the NVML calls**

In `src/hw/nvml.hpp` add `#include <optional>` and `#include <utility>`, and public:
```cpp
    // Power limit as percent of the driver default. Empty when NVML cannot
    // report the constraints (older cards, or a failed call; see Error()).
    std::optional<std::pair<int, int>> PowerLimitRangePct(unsigned index);
    // Sets the limit to pct of default and verifies by reading it back
    // (within 1 % of default). Needs administrator rights.
    bool SetPowerLimitPct(unsigned index, int pct);
```

In `src/hw/nvml.cpp`, beside the other typedefs/pointers:
```cpp
typedef nvmlReturn_t (*fn_pl_default)(nvmlDevice_t, unsigned*);
typedef nvmlReturn_t (*fn_pl_constraints)(nvmlDevice_t, unsigned*, unsigned*);
typedef nvmlReturn_t (*fn_pl_set)(nvmlDevice_t, unsigned);
static fn_pl_default     p_pl_default = nullptr;
static fn_pl_constraints p_pl_constraints = nullptr;
static fn_pl_set         p_pl_set = nullptr;
```
In `Init()` after `p_fan`:
```cpp
    p_pl_default     = (fn_pl_default)GetProcAddress(h, "nvmlDeviceGetPowerManagementDefaultLimit");
    p_pl_constraints = (fn_pl_constraints)GetProcAddress(h, "nvmlDeviceGetPowerManagementLimitConstraints");
    p_pl_set         = (fn_pl_set)GetProcAddress(h, "nvmlDeviceSetPowerManagementLimit");
```
Before the destructor:
```cpp
std::optional<std::pair<int, int>> Nvml::PowerLimitRangePct(unsigned index) {
    nvmlDevice_t dev = nullptr;
    unsigned def = 0, lo = 0, hi = 0;
    if (!inited_ || p_byIndex(index, &dev) != NVML_SUCCESS) { error_ = "NVML device not available"; return std::nullopt; }
    if (!p_pl_default || p_pl_default(dev, &def) != NVML_SUCCESS || def == 0) {
        error_ = "nvmlDeviceGetPowerManagementDefaultLimit failed"; return std::nullopt;
    }
    if (!p_pl_constraints || p_pl_constraints(dev, &lo, &hi) != NVML_SUCCESS) {
        error_ = "nvmlDeviceGetPowerManagementLimitConstraints failed"; return std::nullopt;
    }
    // Round inward so every percent in the range is actually settable.
    const int min_pct = static_cast<int>((static_cast<unsigned long long>(lo) * 100 + def - 1) / def);
    const int max_pct = static_cast<int>(static_cast<unsigned long long>(hi) * 100 / def);
    return std::make_pair(min_pct, max_pct);
}

bool Nvml::SetPowerLimitPct(unsigned index, int pct) {
    nvmlDevice_t dev = nullptr;
    unsigned def = 0, now = 0;
    if (!inited_ || p_byIndex(index, &dev) != NVML_SUCCESS) { error_ = "NVML device not available"; return false; }
    if (!p_pl_default || p_pl_default(dev, &def) != NVML_SUCCESS || def == 0) {
        error_ = "nvmlDeviceGetPowerManagementDefaultLimit failed"; return false;
    }
    const unsigned target = static_cast<unsigned>(static_cast<unsigned long long>(def) * pct / 100);
    if (!p_pl_set || p_pl_set(dev, target) != NVML_SUCCESS) {
        error_ = "nvmlDeviceSetPowerManagementLimit failed (elevated?)"; return false;
    }
    // Never trust the return code: read the limit back.
    if (!p_powerlimit || p_powerlimit(dev, &now) != NVML_SUCCESS) {
        error_ = "nvmlDeviceGetPowerManagementLimit failed after set"; return false;
    }
    const long long diff = static_cast<long long>(now) - static_cast<long long>(target);
    if ((diff < 0 ? -diff : diff) > def / 100) {
        error_ = "power limit read back " + std::to_string(now / 1000) + " W, requested " + std::to_string(target / 1000) + " W";
        return false;
    }
    return true;
}
```

- [ ] **Step 2: Wire it into GpuControl**

Replace the body of `make_gpu_control` in `src/hw/gpu_control.cpp`:
```cpp
GpuControl make_gpu_control(Nvml& nvml, Nvapi& nvapi, unsigned gpu) {
    GpuControl c;
    c.read = [&nvml, gpu] { return nvml.Read(gpu); };
    c.set_core_offset = [&nvapi, gpu](int mhz) { return nvapi.SetCoreOffsetMhz(gpu, mhz); };
    c.set_mem_offset = [&nvapi, gpu](int mhz) { return nvapi.SetMemOffsetMhz(gpu, mhz); };
    const bool power = nvml.PowerLimitRangePct(gpu).has_value();
    if (power) {
        c.set_power_limit = [&nvml, gpu](int pct) { return nvml.SetPowerLimitPct(gpu, pct); };
        c.power_limit_range_pct = [&nvml, gpu] { return *nvml.PowerLimitRangePct(gpu); };
    }
    // Stock = offsets 0 and, where the card allows it, the default power limit.
    c.reset_to_stock = [&nvapi, &nvml, gpu, power] {
        const bool offsets = nvapi.ResetOffsets(gpu);
        return (power ? nvml.SetPowerLimitPct(gpu, 100) : true) && offsets;
    };
    if (nvapi.FanControlAvailable()) {
        c.set_fan_pct = [&nvapi, gpu](int pct) { return nvapi.SetFanPct(gpu, pct); };
    }
    return c;
}
```
(The old comment "set_power_limit stays empty ..." is removed; the header comment "Callbacks the hardware cannot support are left empty on purpose" stays.)

- [ ] **Step 3: `gao --reset` restores power too**

In `src/app/main.cpp`, change `reset()` to:
```cpp
static int reset() {
    gao::Nvapi nvapi;
    if (!nvapi.Init()) { std::printf("NVAPI init failed: %s\n", nvapi.Error().c_str()); return 1; }
    bool ok = nvapi.ResetOffsets(kGpu);
    const auto readback = nvapi.ReadOffsetsMhz(kGpu);
    std::printf("reset: requested core 0 MHz, mem 0 MHz\n");
    if (readback) std::printf("read back core %d MHz, mem %d MHz\n", readback->first, readback->second);
    else std::printf("read back unavailable (%s)\n", nvapi.Error().c_str());
    if (!ok) std::printf("%s\n", nvapi.Error().c_str());
    // Power limit back to the driver default, where the card supports it.
    gao::Nvml nvml;
    if (nvml.Init() && nvml.PowerLimitRangePct(kGpu)) {
        const bool power_ok = nvml.SetPowerLimitPct(kGpu, 100);
        std::printf("power limit: default %s\n", power_ok ? "restored" : nvml.Error().c_str());
        ok = ok && power_ok;
    }
    std::printf("%s\n", ok ? "OK" : "MISMATCH");
    return ok ? 0 : 1;
}
```

- [ ] **Step 4: Build, test, smoke**

Run: `cmake --build build --config Release; ctest --test-dir build -C Release --output-on-failure`
Expected: no warnings, `100% tests passed`.
Then from an elevated shell: `.\build\Release\gao.exe --reset`
Expected: `read back core 0 MHz, mem 0 MHz`, `power limit: default restored`, `OK`.

- [ ] **Step 5: Commit and push**

```bash
git add src/hw/nvml.hpp src/hw/nvml.cpp src/hw/gpu_control.cpp src/app/main.cpp
git commit -m "feat(hw): NVML power limit with read-back; stock includes default power"
git push origin main
```

---

### Task 5: Journal file and `gao --optimize`

**Files:**
- Create: `src/hw/journal_file.hpp`, `src/hw/journal_file.cpp`
- Modify: `CMakeLists.txt` (add to `hw`), `src/app/main.cpp`, `docs/hardware-checks.md`, `README.md`

**Interfaces:**
- Consumes: `Journal` (Task 1), `optimize`/`OptimizeIo`/`OptimizeResult` (Task 3), `make_gpu_control` (Task 4), `gao::Stress` + `run_stability` (P2), `objectives_for`/`Preset`.
- Produces:
  ```cpp
  namespace gao {
  std::filesystem::path journal_path();                                   // empty if %LOCALAPPDATA% unset
  std::vector<std::string> read_lines(const std::filesystem::path& p);   // missing file -> empty
  bool append_line_durable(const std::filesystem::path& p, const std::string& line);
  }
  ```
  CLI: `gao --optimize [best|quiet|cool|max]`, exit 0 on success, 1 otherwise.

- [ ] **Step 1: Journal file**

`src/hw/journal_file.hpp`:
```cpp
#pragma once
#include <filesystem>
#include <string>
#include <vector>

namespace gao {

// %LOCALAPPDATA%\GpuAutoOptimizer\journal.jsonl, or empty when the variable
// is not set.
std::filesystem::path journal_path();
std::vector<std::string> read_lines(const std::filesystem::path& p);
// Appends line + '\n' and forces it to disk (FlushFileBuffers) before
// returning, so the line survives a freeze that follows immediately after.
bool append_line_durable(const std::filesystem::path& p, const std::string& line);

}
```

`src/hw/journal_file.cpp`:
```cpp
#include "hw/journal_file.hpp"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdlib>
#include <fstream>

namespace gao {

std::filesystem::path journal_path() {
    const wchar_t* base = _wgetenv(L"LOCALAPPDATA");
    if (!base || !*base) return {};
    return std::filesystem::path(base) / L"GpuAutoOptimizer" / L"journal.jsonl";
}

std::vector<std::string> read_lines(const std::filesystem::path& p) {
    std::vector<std::string> lines;
    std::ifstream in(p, std::ios::binary);
    for (std::string line; std::getline(in, line);) lines.push_back(line);
    return lines;
}

bool append_line_durable(const std::filesystem::path& p, const std::string& line) {
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    const HANDLE h = CreateFileW(p.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    const std::string data = line + "\n";
    DWORD written = 0;
    const bool ok = WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) &&
                    written == data.size() && FlushFileBuffers(h);
    CloseHandle(h);
    return ok;
}

}
```
Add `src/hw/journal_file.hpp src/hw/journal_file.cpp` to the `hw` library in `CMakeLists.txt`.

- [ ] **Step 2: `--optimize` command**

In `src/app/main.cpp` add at the top (before the other includes):
```cpp
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
```
and add includes:
```cpp
#include "core/journal.hpp"
#include "core/objectives.hpp"
#include "core/search.hpp"
#include "hw/gpu_control.hpp"
#include "hw/journal_file.hpp"
#include <atomic>
#include <string>
```

Add after `stress()`:
```cpp
static std::atomic<bool> g_abort{false};

// Ctrl+C / Ctrl+Break do not kill the process during --optimize: they ask
// the search to stop, and the search restores stock before returning.
static BOOL WINAPI OnConsoleCtrl(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) {
        g_abort = true;
        std::printf("\nabort requested -- finishing the current probe, then restoring stock\n");
        return TRUE;
    }
    return FALSE;
}

static bool IsElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    const bool ok = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size);
    CloseHandle(token);
    return ok && elevation.TokenIsElevated;
}

static int optimize(gao::Preset preset) {
    if (!IsElevated()) {
        std::printf("--optimize changes clocks and power limits and needs an elevated (administrator) shell\n");
        return 1;
    }
    gao::Nvml nvml;
    if (!nvml.Init()) { std::printf("NVML init failed: %s\n", nvml.Error().c_str()); return 1; }
    gao::Nvapi nvapi;
    if (!nvapi.Init()) { std::printf("NVAPI init failed: %s\n", nvapi.Error().c_str()); return 1; }
    gao::Stress load;
    if (!load.Init()) { std::printf("stress init failed: %s\n", load.Error().c_str()); return 1; }
    const gao::GpuControl gpu = gao::make_gpu_control(nvml, nvapi, kGpu);

    const auto path = gao::journal_path();
    if (path.empty()) { std::printf("LOCALAPPDATA is not set; cannot keep the crash journal\n"); return 1; }
    gao::Journal journal(gao::read_lines(path), [&path](const std::string& l) { return gao::append_line_durable(path, l); });
    for (const auto& f : journal.freezes())
        std::printf("warning: a previous run froze the machine at %s; staying below it from now on\n", f.c_str());
    if (!gpu.set_power_limit) std::printf("power limit: not adjustable on this card, skipped\n");
    if (!gpu.set_fan_pct) std::printf("fan: not controllable on this card, skipped\n");

    SetConsoleCtrlHandler(OnConsoleCtrl, TRUE);
    gao::OptimizeIo io;
    io.probe = [&](double seconds, int max_temp) {
        return gao::run_stability([&] { return load.Batch(); }, gpu.read, seconds, max_temp);
    };
    io.aborted = [] { return g_abort.load(); };
    io.log = [](const std::string& m) { std::printf("  %s\n", m.c_str()); };
    const gao::OptimizeResult r = gao::optimize(gpu, gao::objectives_for(preset), journal, io);
    SetConsoleCtrlHandler(OnConsoleCtrl, FALSE);

    if (!r.ok) { std::printf("RESULT: not applied -- %s (card at stock)\n", r.reason.c_str()); return 1; }
    std::printf("RESULT: power %d %%, core +%d MHz (max stable +%d), mem +%d MHz (max stable +%d)\n",
                r.power_pct, r.core_mhz, r.core_max_stable, r.mem_mhz, r.mem_max_stable);
    std::printf("  before: score=%.0f it/s  core=%d MHz  mem=%d MHz  peak=%d C  power=%d W\n",
                r.baseline.score, r.baseline.avg_core_mhz, r.baseline.avg_mem_mhz, r.baseline.peak_temp_c, r.baseline.avg_power_w);
    std::printf("  after:  score=%.0f it/s  core=%d MHz  mem=%d MHz  peak=%d C  power=%d W\n",
                r.soak.score, r.soak.avg_core_mhz, r.soak.avg_mem_mhz, r.soak.peak_temp_c, r.soak.avg_power_w);
    std::printf("Applied until reboot. `gao --reset` returns to stock.\n");
    return 0;
}
```

In `main()`, before the usage line:
```cpp
    if (argc > 1 && std::strcmp(argv[1], "--optimize") == 0) {
        gao::Preset preset = gao::Preset::BestOfMyGpu;
        if (argc > 2) {
            const std::string p = argv[2];
            if (p == "best") preset = gao::Preset::BestOfMyGpu;
            else if (p == "quiet") preset = gao::Preset::Quiet;
            else if (p == "cool") preset = gao::Preset::CoolAndEfficient;
            else if (p == "max") preset = gao::Preset::MaxPerformance;
            else { std::printf("--optimize expects best, quiet, cool or max, got '%s'\n", argv[2]); return 1; }
        }
        return optimize(preset);
    }
```
Update the usage line to:
```cpp
    std::printf("usage: gao [--version | --probe | --set-core <mhz> | --set-mem <mhz> | --reset | --set-fan <pct>\n"
                "            | --stress <sec> [--max-temp <c>] | --optimize [best|quiet|cool|max]]\n");
```

- [ ] **Step 3: Build and test**

Run: `cmake --build build --config Release; ctest --test-dir build -C Release --output-on-failure`
Expected: no warnings, `100% tests passed`.
Run (not elevated): `.\build\Release\gao.exe --optimize` → prints the elevation message, exit 1.
Run: `.\build\Release\gao.exe --optimize turbo` → `--optimize expects best, quiet, cool or max, got 'turbo'`, exit 1.

- [ ] **Step 4: Docs**

Append to the table in `docs/hardware-checks.md`:
```markdown
| 13 | Optimize end to end | `.\build\Release\gao.exe --optimize best`, then `.\build\Release\gao.exe --stress 60` | yes / no | `RESULT:` line with the applied values, soak `STABLE`; the follow-up stress run `STABLE` | |
| 14 | A recorded freeze becomes a ceiling | Append `{"id":999,"core":150,"state":"begin"}` to `%LOCALAPPDATA%\GpuAutoOptimizer\journal.jsonl`, run `--optimize best` | yes | Warning "froze the machine at core +150"; no logged core candidate ≥ +150. Remove the line afterwards. | |
| 15 | Ctrl+C restores stock | Press Ctrl+C during the core search of `--optimize best` | yes | "abort requested", then `RESULT: not applied -- aborted (card at stock)`; `--reset` read-back 0/0 | |
| 16 | Power limit applies | `--optimize quiet` log shows `power <N> %` lines; `--probe` afterwards | yes | `--probe`'s `/<limit> W` equals N % of the default limit (±1 %) | |
```
Add to the note paragraph above the table: "Checks 13–16 run `--optimize`, which writes clocks and the power limit: elevated."

In `README.md`: in the IMPORTANT block replace "**There is no tuning engine, no search and no GUI yet**" with "It can also tune: `--optimize` searches power limit, core and memory offsets against that verdict. **There is no GUI yet, and results are lost on reboot**"; in Quick start add `.\build\Release\gao.exe --optimize best   # elevated` with one sentence: "`--optimize` runs baseline → power → core → memory → 60 s soak (~5 minutes), leaves the result applied until reboot, and keeps a crash journal in `%LOCALAPPDATA%\GpuAutoOptimizer` so a setting that froze the machine is never tried again. Ctrl+C restores stock."; add `--optimize` to the WARNING block's list of commands that write to the GPU; in Project structure add `journal.*` and `search.*` under `src/core/` and `journal_file.*` under `src/hw/`; in Status say phases 0–3 are done.

- [ ] **Step 5: Commit and push**

```bash
git add src/hw/journal_file.hpp src/hw/journal_file.cpp CMakeLists.txt src/app/main.cpp docs/hardware-checks.md README.md
git commit -m "feat(app): gao --optimize with crash journal and Ctrl+C restore"
git push origin main
```

---

### Task 6: Hardware verification (with the user, RTX 4070)

**Files:**
- Modify: `docs/hardware-checks.md` (Result column)

**Interfaces:**
- Consumes: `gao --optimize`, `--reset`, `--probe`, `--stress`.
- Produces: filled rows 13–16.

All commands elevated (run via a UAC-launched script like the P1/P2 checks, logging to the scratchpad).

- [ ] **Step 1: Check 16 then 13** — `--optimize quiet` (note power lines, then `--probe` limit), `--reset`, then `--optimize best` and `--stress 60`. Record the RESULT, before/after lines and duration.
- [ ] **Step 2: Check 14** — append the fake freeze line, run `--optimize best`, confirm the warning and that no logged core candidate is ≥ +150; remove the line (rewrite the file without it).
- [ ] **Step 3: Check 15** — this needs a human keypress: ask the user to run `--optimize best` in an elevated terminal and press Ctrl+C when the log shows `core +`; then run `--reset` and `--probe`.
- [ ] **Step 4: Finish** — `--reset`; fill the Result column with date and numbers; commit `docs: record P3 hardware checks on the RTX 4070`; push.
