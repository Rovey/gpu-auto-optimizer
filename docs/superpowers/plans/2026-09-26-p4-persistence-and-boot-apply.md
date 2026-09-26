# GPU Auto Optimizer — Phase 4 (Persistence and Boot-Apply) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `--optimize` saves its result; `gao --apply` re-applies it; `gao --boot on` makes Windows re-apply it at logon with 3-strike and driver-version protection; `gao --status` shows the state.

**Architecture:** `core/config` is rewritten to hold one `Profile` and a strike counter; new `core/boot` holds the two decisions everything else calls (`decide_boot`, `apply_profile`), unit-tested with a fake `GpuControl`. `hw/journal_file` becomes `hw/app_files` (paths, atomic config write, durable appends); `hw/nvml` gains the driver version; `hw/boot_task` drives `schtasks.exe`. `app/main.cpp` adds the four commands.

**Tech Stack:** C++20, MSVC, CMake ≥ 3.28, doctest, nlohmann/json (vendored), NVML, Win32 (`MoveFileExW`, `CreateProcessW`, `FreeConsole`), `schtasks.exe`.

**Spec:** `docs/superpowers/specs/2026-09-26-p4-persistence-and-boot-apply-design.md` (parents: P3 spec, `2026-09-20-cpp-rewrite-design.md`)

## Global Constraints

- C++20, MSVC, x64. English everywhere. `src/core/` has no Windows/NVML/NVAPI/D3D headers.
- Every hardware write verified by read-back; any failed apply ends at stock.
- Files live in `%LOCALAPPDATA%\GpuAutoOptimizer\`: `gao.json`, `journal.jsonl`, `boot.log`.
- `gao.json` is written atomically and durably (temp → `FlushFileBuffers` → `MoveFileExW` with `REPLACE_EXISTING | WRITE_THROUGH`).
- Strikes: max 3 (`kMaxBootStrikes`); boot-apply records the strike durably **before** touching hardware and clears it after 2 minutes.
- Task name `GpuAutoOptimizer`, trigger ONLOGON, run level HIGHEST, action `"<gao.exe>" --boot-apply`.
- `--optimize` never enables boot-apply on its own.
- `cmake`/`ctest` are not on PATH: `C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\`. Commands below say `cmake`/`ctest`.
- Push after every commit (standing permission).

## Review Focus

1. **An unknown driver version** (NVML failed, empty string) — must never count as "matches", even when the profile was also saved with an empty driver. (Task 2 test: "an unknown driver never matches".)
2. **Nonsense strike counts in a hand-edited `gao.json`** (negative, string) — negative clamps to 0, wrong type reads as 0; never a crash. (Task 1 test: "strike count is sanitized".)
3. **`--optimize` saving a new profile while a boot-apply is in its 2-minute wait** — the strike reset must reload the file and only clear the counter, not write back the old profile. (Task 4 step: `boot_apply` reloads before clearing; manual check in Task 5.)
4. **The executable path contains spaces** (this repo's does) — the scheduled task's action must quote it. (Task 3 code quotes; Task 5 check 19 exercises it for real.)
5. **A profile that needs power control on a card that lost it** (driver change, other GPU) — refuse, never apply half. (Task 2 test: "power other than 100 % without power control fails and resets".)

---

## File Structure

| File | Responsibility |
|---|---|
| `src/core/objectives.hpp/.cpp` | + `preset_name`, `preset_from_name` |
| `src/core/config.hpp/.cpp` | `Profile`, `Config`, JSON round-trip (rewritten) |
| `src/core/boot.hpp/.cpp` | `BootDecision`, `kMaxBootStrikes`, `decide_boot`, `apply_profile` |
| `src/hw/app_files.hpp/.cpp` | renamed from `journal_file.*`; + `app_dir`, `config_path`, `boot_log_path`, `read_file`, `write_file_atomic` |
| `src/hw/nvml.hpp/.cpp` | + `DriverVersion()` |
| `src/hw/boot_task.hpp/.cpp` | `boot_task_create/remove/exists` via `schtasks.exe` |
| `src/app/main.cpp` | `--apply`, `--boot on|off`, `--status`, `--boot-apply`; `--optimize` saves |
| `tests/test_config.cpp` (rewritten), `tests/test_boot.cpp`, `tests/test_objectives.cpp` (+ names) | unit tests |
| `docs/hardware-checks.md`, `README.md` | rows 17–22, new commands |

---

### Task 1: Preset names and the new config

**Files:**
- Modify: `src/core/objectives.hpp`, `src/core/objectives.cpp`, `tests/test_objectives.cpp`
- Rewrite: `src/core/config.hpp`, `src/core/config.cpp`, `tests/test_config.cpp`

**Interfaces:**
- Consumes: `Preset`.
- Produces:
  ```cpp
  const char* preset_name(Preset p);                               // "best" "quiet" "cool" "max"
  std::optional<Preset> preset_from_name(const std::string& name);
  struct Profile { Preset preset = Preset::BestOfMyGpu; int power_pct = 100, core_mhz = 0, mem_mhz = 0;
                   std::string driver, saved_at; };
  struct Config { std::optional<Profile> profile; int boot_strikes = 0; };
  std::string to_json(const Config& c);
  Config from_json(const std::string& text);   // never throws
  ```

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_objectives.cpp`:
```cpp
TEST_CASE("preset names round-trip and unknown names are rejected") {
    for (Preset p : {Preset::BestOfMyGpu, Preset::Quiet, Preset::CoolAndEfficient, Preset::MaxPerformance})
        CHECK(preset_from_name(preset_name(p)) == p);
    CHECK(std::string(preset_name(Preset::BestOfMyGpu)) == "best");
    CHECK(std::string(preset_name(Preset::CoolAndEfficient)) == "cool");
    CHECK_FALSE(preset_from_name("turbo").has_value());
    CHECK_FALSE(preset_from_name("").has_value());
}
```
(add `#include <string>` at the top if missing).

Replace `tests/test_config.cpp` entirely:
```cpp
#include "doctest/doctest.h"
#include "core/config.hpp"

using namespace gao;

namespace {
Profile sample() {
    Profile p;
    p.preset = Preset::Quiet;
    p.power_pct = 80;
    p.core_mhz = 90;
    p.mem_mhz = 600;
    p.driver = "610.74";
    p.saved_at = "2026-09-26 22:41";
    return p;
}
}

TEST_CASE("a config with a profile survives a round-trip") {
    Config c;
    c.profile = sample();
    c.boot_strikes = 2;
    const Config back = from_json(to_json(c));
    REQUIRE(back.profile.has_value());
    CHECK(back.profile->preset == Preset::Quiet);
    CHECK(back.profile->power_pct == 80);
    CHECK(back.profile->core_mhz == 90);
    CHECK(back.profile->mem_mhz == 600);
    CHECK(back.profile->driver == "610.74");
    CHECK(back.profile->saved_at == "2026-09-26 22:41");
    CHECK(back.boot_strikes == 2);
    CHECK(to_json(c).find("\"quiet\"") != std::string::npos);   // readable preset
}

TEST_CASE("a config without a profile round-trips as no profile") {
    Config c;
    c.boot_strikes = 1;
    const Config back = from_json(to_json(c));
    CHECK_FALSE(back.profile.has_value());
    CHECK(back.boot_strikes == 1);
}

TEST_CASE("unreadable JSON yields defaults instead of throwing") {
    const Config c = from_json("{ this is not json");
    CHECK_FALSE(c.profile.has_value());
    CHECK(c.boot_strikes == 0);
    CHECK_FALSE(from_json("").profile.has_value());
    CHECK_FALSE(from_json("[1,2]").profile.has_value());
}

TEST_CASE("a profile with a wrong type, a missing field or an unknown preset is no profile") {
    CHECK_FALSE(from_json(R"({"profile":{"preset":"best","power_pct":"105","core_mhz":0,"mem_mhz":0,"driver":"x","saved_at":"y"}})").profile);
    CHECK_FALSE(from_json(R"({"profile":{"preset":"best","power_pct":105,"core_mhz":0,"driver":"x","saved_at":"y"}})").profile);
    CHECK_FALSE(from_json(R"({"profile":{"preset":"turbo","power_pct":105,"core_mhz":0,"mem_mhz":0,"driver":"x","saved_at":"y"}})").profile);
    CHECK(from_json(R"({"profile":{"preset":"best","power_pct":105,"core_mhz":0,"mem_mhz":0,"driver":"x","saved_at":"y"}})").profile);
}

TEST_CASE("strike count is sanitized") {
    CHECK(from_json(R"({"boot_strikes":-4})").boot_strikes == 0);
    CHECK(from_json(R"({"boot_strikes":"three"})").boot_strikes == 0);
    CHECK(from_json(R"({"boot_strikes":3})").boot_strikes == 3);
}

TEST_CASE("an old-format config loads without error") {
    const Config c = from_json(R"({"preset":0,"objectives":{"max_temp_c":75},"core_offset_mhz":150,
                                  "mem_offset_mhz":800,"power_limit_pct":90,"blacklisted_core_offsets":[180]})");
    CHECK_FALSE(c.profile.has_value());
    CHECK(c.boot_strikes == 0);
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build build --config Debug`
Expected: FAIL — `preset_name` / `Profile` / `boot_strikes` undeclared.

- [ ] **Step 3: Implement**

In `src/core/objectives.hpp` add `#include <optional>` and `#include <string>`, and after `objectives_for`:
```cpp
// Short names used on the command line and in gao.json.
const char* preset_name(Preset preset);
std::optional<Preset> preset_from_name(const std::string& name);
```
In `src/core/objectives.cpp` append inside the namespace:
```cpp
const char* preset_name(const Preset preset) {
    switch (preset) {
        case Preset::Quiet:            return "quiet";
        case Preset::CoolAndEfficient: return "cool";
        case Preset::MaxPerformance:   return "max";
        case Preset::BestOfMyGpu:
        default:                       return "best";
    }
}

std::optional<Preset> preset_from_name(const std::string& name) {
    for (Preset p : {Preset::BestOfMyGpu, Preset::Quiet, Preset::CoolAndEfficient, Preset::MaxPerformance})
        if (name == preset_name(p)) return p;
    return std::nullopt;
}
```

Replace `src/core/config.hpp`:
```cpp
#pragma once
#include "core/objectives.hpp"
#include <optional>
#include <string>

namespace gao {

// What --optimize found, as --apply and --boot-apply re-apply it.
struct Profile {
    Preset preset = Preset::BestOfMyGpu;
    int power_pct = 100;
    int core_mhz = 0;
    int mem_mhz = 0;
    std::string driver;     // driver version the profile was tested on
    std::string saved_at;   // local time, "YYYY-MM-DD HH:MM"
};

// Everything that has to survive a reboot. Freeze ceilings are not here:
// they live in the journal.
struct Config {
    std::optional<Profile> profile;
    int boot_strikes = 0;   // logons that applied the profile and have not yet run 2 minutes
};

std::string to_json(const Config& c);
// Never throws. Bad input yields defaults; a profile with any field missing
// or mistyped is treated as no profile rather than half a profile.
Config from_json(const std::string& text);

}
```

Replace `src/core/config.cpp`:
```cpp
#include "core/config.hpp"
#include "nlohmann/json.hpp"
#include <algorithm>

namespace gao {

std::string to_json(const Config& c) {
    nlohmann::json j = {{"boot_strikes", c.boot_strikes}};
    if (c.profile) {
        const Profile& p = *c.profile;
        j["profile"] = {
            {"preset", preset_name(p.preset)},
            {"power_pct", p.power_pct},
            {"core_mhz", p.core_mhz},
            {"mem_mhz", p.mem_mhz},
            {"driver", p.driver},
            {"saved_at", p.saved_at},
        };
    }
    return j.dump(2);
}

Config from_json(const std::string& text) {
    Config c;
    const nlohmann::json j = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) return c;
    if (const auto it = j.find("boot_strikes"); it != j.end() && it->is_number_integer())
        c.boot_strikes = std::max(0, it->get<int>());

    const auto pj = j.find("profile");
    if (pj == j.end() || !pj->is_object()) return c;
    auto num = [&](const char* key) -> std::optional<int> {
        const auto it = pj->find(key);
        if (it == pj->end() || !it->is_number_integer()) return std::nullopt;
        return it->get<int>();
    };
    auto str = [&](const char* key) -> std::optional<std::string> {
        const auto it = pj->find(key);
        if (it == pj->end() || !it->is_string()) return std::nullopt;
        return it->get<std::string>();
    };
    const auto name = str("preset");
    const auto preset = name ? preset_from_name(*name) : std::nullopt;
    const auto power = num("power_pct"), core = num("core_mhz"), mem = num("mem_mhz");
    const auto driver = str("driver"), saved_at = str("saved_at");
    if (!preset || !power || !core || !mem || !driver || !saved_at) return c;
    c.profile = Profile{*preset, *power, *core, *mem, *driver, *saved_at};
    return c;
}

}
```

- [ ] **Step 4: Run tests**

Run: `cmake --build build --config Debug; .\build\Debug\core_tests.exe`
Expected: `Status: SUCCESS!`.

- [ ] **Step 5: Commit and push**

```bash
git add src/core/objectives.hpp src/core/objectives.cpp src/core/config.hpp src/core/config.cpp tests/test_objectives.cpp tests/test_config.cpp
git commit -m "feat(core): config holds one saved profile and the boot strike count"
git push
```

---

### Task 2: Boot decision and profile apply

**Files:**
- Create: `src/core/boot.hpp`, `src/core/boot.cpp`, `tests/test_boot.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `Config`, `Profile` (Task 1), `GpuControl` (`core/types.hpp`).
- Produces:
  ```cpp
  inline constexpr int kMaxBootStrikes = 3;
  enum class BootDecision { Apply, NoProfile, TooManyStrikes, DriverChanged };
  BootDecision decide_boot(const Config& c, const std::string& driver);
  bool apply_profile(const GpuControl& gpu, const Profile& p, std::string* why);
  ```

- [ ] **Step 1: Write the failing tests**

`tests/test_boot.cpp`:
```cpp
#include "doctest/doctest.h"
#include "core/boot.hpp"
#include <string>

using namespace gao;

namespace {
Config with_profile(const std::string& driver, int strikes = 0) {
    Config c;
    Profile p;
    p.power_pct = 105;
    p.core_mhz = 135;
    p.mem_mhz = 1050;
    p.driver = driver;
    c.profile = p;
    c.boot_strikes = strikes;
    return c;
}

struct Card {
    int power = 100, core = 0, mem = 0;
    bool fail_mem = false;
    GpuControl gpu(bool with_power = true) {
        GpuControl g;
        g.set_core_offset = [this](int v) { core = v; return true; };
        g.set_mem_offset = [this](int v) { if (fail_mem) return false; mem = v; return true; };
        g.reset_to_stock = [this] { power = 100; core = 0; mem = 0; return true; };
        if (with_power) g.set_power_limit = [this](int p) { power = p; return true; };
        return g;
    }
};
}

TEST_CASE("decide_boot: each outcome") {
    CHECK(decide_boot(Config{}, "610.74") == BootDecision::NoProfile);
    CHECK(decide_boot(with_profile("610.74", 3), "610.74") == BootDecision::TooManyStrikes);
    CHECK(decide_boot(with_profile("610.74"), "615.20") == BootDecision::DriverChanged);
    CHECK(decide_boot(with_profile("610.74", 2), "610.74") == BootDecision::Apply);
}

TEST_CASE("decide_boot: strikes are checked before the driver") {
    CHECK(decide_boot(with_profile("610.74", 5), "615.20") == BootDecision::TooManyStrikes);
}

TEST_CASE("an unknown driver never matches") {
    CHECK(decide_boot(with_profile("610.74"), "") == BootDecision::DriverChanged);
    CHECK(decide_boot(with_profile(""), "") == BootDecision::DriverChanged);
}

TEST_CASE("apply_profile sets power, core and memory") {
    Card card;
    std::string why;
    CHECK(apply_profile(card.gpu(), *with_profile("x").profile, &why));
    CHECK(card.power == 105);
    CHECK(card.core == 135);
    CHECK(card.mem == 1050);
}

TEST_CASE("a failing setter resets to stock and names the setter") {
    Card card;
    card.fail_mem = true;
    std::string why;
    CHECK_FALSE(apply_profile(card.gpu(), *with_profile("x").profile, &why));
    CHECK(why.find("mem") != std::string::npos);
    CHECK(card.power == 100);
    CHECK(card.core == 0);
}

TEST_CASE("power other than 100 % without power control fails and resets") {
    Card card;
    std::string why;
    CHECK_FALSE(apply_profile(card.gpu(/*with_power=*/false), *with_profile("x").profile, &why));
    CHECK(why.find("power") != std::string::npos);
    CHECK(card.core == 0);

    Profile p = *with_profile("x").profile;
    p.power_pct = 100;
    CHECK(apply_profile(card.gpu(false), p, &why));
    CHECK(card.core == 135);
}
```
Add `src/core/boot.hpp src/core/boot.cpp` to `core` and `tests/test_boot.cpp` to `core_tests` in `CMakeLists.txt`; create `boot.hpp` with only `#pragma once` and an empty `boot.cpp`.

- [ ] **Step 2: Run to verify it fails**

Run: `cmake -S . -B build -A x64; cmake --build build --config Debug`
Expected: FAIL — `decide_boot` / `BootDecision` undeclared.

- [ ] **Step 3: Implement**

`src/core/boot.hpp`:
```cpp
#pragma once
#include "core/config.hpp"
#include "core/types.hpp"
#include <string>

namespace gao {

// Logons that crashed within 2 minutes of applying before boot-apply gives up.
inline constexpr int kMaxBootStrikes = 3;

enum class BootDecision { Apply, NoProfile, TooManyStrikes, DriverChanged };

// Order: no profile, then strikes, then driver. An empty (unknown) driver
// version never matches, so a failed NVML query cannot apply offsets that
// were tested on some other driver.
BootDecision decide_boot(const Config& c, const std::string& driver);

// Sets power, core and memory from the profile; each GpuControl setter
// verifies by read-back. A setter the card lacks is fine only when the
// profile asks for stock on that dimension. Any failure resets to stock and
// returns false with the reason in *why.
bool apply_profile(const GpuControl& gpu, const Profile& p, std::string* why);

}
```

`src/core/boot.cpp`:
```cpp
#include "core/boot.hpp"

namespace gao {

BootDecision decide_boot(const Config& c, const std::string& driver) {
    if (!c.profile) return BootDecision::NoProfile;
    if (c.boot_strikes >= kMaxBootStrikes) return BootDecision::TooManyStrikes;
    if (driver.empty() || driver != c.profile->driver) return BootDecision::DriverChanged;
    return BootDecision::Apply;
}

bool apply_profile(const GpuControl& gpu, const Profile& p, std::string* why) {
    auto fail = [&](const std::string& reason) {
        if (gpu.reset_to_stock) gpu.reset_to_stock();
        if (why) *why = reason;
        return false;
    };
    // A missing setter is only acceptable when the profile wants stock there.
    auto set = [](const std::function<bool(int)>& setter, int value, int stock) {
        return setter ? setter(value) : value == stock;
    };
    if (!set(gpu.set_power_limit, p.power_pct, 100))
        return fail("setting power " + std::to_string(p.power_pct) + " % failed" +
                    (gpu.set_power_limit ? "" : " (no power control on this card)"));
    if (!set(gpu.set_core_offset, p.core_mhz, 0)) return fail("setting core +" + std::to_string(p.core_mhz) + " MHz failed");
    if (!set(gpu.set_mem_offset, p.mem_mhz, 0)) return fail("setting mem +" + std::to_string(p.mem_mhz) + " MHz failed");
    return true;
}

}
```

- [ ] **Step 4: Run tests**

Run: `cmake --build build --config Debug; .\build\Debug\core_tests.exe`
Expected: `Status: SUCCESS!`.

- [ ] **Step 5: Commit and push**

```bash
git add src/core/boot.hpp src/core/boot.cpp tests/test_boot.cpp CMakeLists.txt
git commit -m "feat(core): boot decision (strikes, driver) and verified profile apply"
git push
```

---

### Task 3: Files, driver version, scheduled task

**Files:**
- Rename: `src/hw/journal_file.hpp/.cpp` → `src/hw/app_files.hpp/.cpp` (`git mv`)
- Create: `src/hw/boot_task.hpp`, `src/hw/boot_task.cpp`
- Modify: `src/hw/nvml.hpp`, `src/hw/nvml.cpp`, `src/core/journal.hpp` (comment), `src/app/main.cpp` (include only), `CMakeLists.txt`

**Interfaces:**
- Produces:
  ```cpp
  // app_files.hpp
  std::filesystem::path app_dir();          // %LOCALAPPDATA%\GpuAutoOptimizer, empty if unset
  std::filesystem::path journal_path();     // app_dir()/journal.jsonl (empty if app_dir is)
  std::filesystem::path config_path();      // app_dir()/gao.json
  std::filesystem::path boot_log_path();    // app_dir()/boot.log
  std::vector<std::string> read_lines(const std::filesystem::path&);
  std::optional<std::string> read_file(const std::filesystem::path&);   // nullopt: missing/unreadable
  bool append_line_durable(const std::filesystem::path&, const std::string& line);
  bool write_file_atomic(const std::filesystem::path&, const std::string& text);
  // nvml.hpp
  std::string Nvml::DriverVersion();        // empty on failure (see Error())
  // boot_task.hpp
  int boot_task_create(const std::filesystem::path& exe);   // schtasks exit code; -1 = could not start
  int boot_task_remove();
  bool boot_task_exists();
  ```

No unit tests (hardware/OS). Deliverable: warning-free build, suite green.

- [ ] **Step 1: Rename and extend the file helpers**

Run: `git mv src/hw/journal_file.hpp src/hw/app_files.hpp; git mv src/hw/journal_file.cpp src/hw/app_files.cpp`

Replace `src/hw/app_files.hpp`:
```cpp
#pragma once
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace gao {

// Everything the app keeps lives in %LOCALAPPDATA%\GpuAutoOptimizer. Each
// path is empty when %LOCALAPPDATA% is not set.
std::filesystem::path app_dir();
std::filesystem::path journal_path();
std::filesystem::path config_path();
std::filesystem::path boot_log_path();

std::vector<std::string> read_lines(const std::filesystem::path& p);
std::optional<std::string> read_file(const std::filesystem::path& p);
// Appends line + '\n' and forces it to disk before returning, so the line
// survives a freeze that follows immediately after.
bool append_line_durable(const std::filesystem::path& p, const std::string& line);
// Replaces the file atomically: a crash mid-write leaves the old or the new
// content, never a mix (the boot strike counter depends on it).
bool write_file_atomic(const std::filesystem::path& p, const std::string& text);

}
```

Replace `src/hw/app_files.cpp`:
```cpp
#include "hw/app_files.hpp"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace gao {

std::filesystem::path app_dir() {
    const wchar_t* base = _wgetenv(L"LOCALAPPDATA");
    if (!base || !*base) return {};
    return std::filesystem::path(base) / L"GpuAutoOptimizer";
}

static std::filesystem::path in_app_dir(const wchar_t* name) {
    const auto dir = app_dir();
    return dir.empty() ? dir : dir / name;
}
std::filesystem::path journal_path() { return in_app_dir(L"journal.jsonl"); }
std::filesystem::path config_path() { return in_app_dir(L"gao.json"); }
std::filesystem::path boot_log_path() { return in_app_dir(L"boot.log"); }

std::vector<std::string> read_lines(const std::filesystem::path& p) {
    std::vector<std::string> lines;
    std::ifstream in(p, std::ios::binary);
    for (std::string line; std::getline(in, line);) lines.push_back(line);
    return lines;
}

std::optional<std::string> read_file(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

static bool write_all(HANDLE h, const std::string& data) {
    DWORD written = 0;
    return WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) &&
           written == data.size() && FlushFileBuffers(h);
}

bool append_line_durable(const std::filesystem::path& p, const std::string& line) {
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    const HANDLE h = CreateFileW(p.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    const bool ok = write_all(h, line + "\n");
    CloseHandle(h);
    return ok;
}

bool write_file_atomic(const std::filesystem::path& p, const std::string& text) {
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    std::filesystem::path tmp = p;
    tmp += L".tmp";
    const HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    const bool ok = write_all(h, text);
    CloseHandle(h);
    return ok && MoveFileExW(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

}
```

In `CMakeLists.txt` replace `src/hw/journal_file.hpp src/hw/journal_file.cpp` with `src/hw/app_files.hpp src/hw/app_files.cpp` and add a line `src/hw/boot_task.hpp src/hw/boot_task.cpp`. In `src/app/main.cpp` replace `#include "hw/journal_file.hpp"` with `#include "hw/app_files.hpp"`. In `src/core/journal.hpp` change the comment `hw/journal_file` to `hw/app_files`.

- [ ] **Step 2: Driver version**

In `src/hw/nvml.hpp`, public:
```cpp
    // e.g. "610.74"; empty when NVML cannot report it (see Error()).
    std::string DriverVersion();
```
In `src/hw/nvml.cpp`, beside the other typedefs/pointers:
```cpp
typedef nvmlReturn_t (*fn_driver)(char*, unsigned);
static fn_driver p_driver = nullptr;
```
in `Init()` after the power-limit lookups:
```cpp
    p_driver = (fn_driver)GetProcAddress(h, "nvmlSystemGetDriverVersion");
```
and before the destructor:
```cpp
std::string Nvml::DriverVersion() {
    char buf[96] = {};
    if (!inited_ || !p_driver || p_driver(buf, sizeof(buf)) != NVML_SUCCESS) {
        error_ = "nvmlSystemGetDriverVersion failed";
        return {};
    }
    return buf;
}
```

- [ ] **Step 3: Scheduled task**

`src/hw/boot_task.hpp`:
```cpp
#pragma once
#include <filesystem>

namespace gao {

// The logon task that runs `gao.exe --boot-apply`, driven through
// schtasks.exe. Each call returns schtasks' exit code (0 = success), or -1
// when schtasks could not be started. Creating needs an elevated caller.
// ponytail: schtasks defaults to "start only on AC power"; fine for desktops,
// switch to the Task Scheduler COM API if laptops need boot-apply on battery.
int boot_task_create(const std::filesystem::path& exe);
int boot_task_remove();
bool boot_task_exists();

}
```

`src/hw/boot_task.cpp`:
```cpp
#include "hw/boot_task.hpp"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <string>

namespace gao {

static int run_schtasks(std::wstring args) {
    std::wstring cmd = L"schtasks.exe " + args;
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    // CREATE_NO_WINDOW: schtasks' own output is not ours to print.
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return -1;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return static_cast<int>(code);
}

int boot_task_create(const std::filesystem::path& exe) {
    // /TR "\"C:\path with spaces\gao.exe\" --boot-apply": the exe is quoted
    // inside the quoted action, so a path with spaces survives.
    return run_schtasks(L"/Create /F /TN GpuAutoOptimizer /SC ONLOGON /RL HIGHEST /TR \"\\\"" +
                        exe.wstring() + L"\\\" --boot-apply\"");
}

int boot_task_remove() { return run_schtasks(L"/Delete /F /TN GpuAutoOptimizer"); }

bool boot_task_exists() { return run_schtasks(L"/Query /TN GpuAutoOptimizer") == 0; }

}
```

- [ ] **Step 4: Build and test**

Run: `cmake -S . -B build -A x64; cmake --build build --config Release; ctest --test-dir build -C Release --output-on-failure`
Expected: no warnings, `100% tests passed`. `.\build\Release\gao.exe --probe` still works (unchanged path).

- [ ] **Step 5: Commit and push**

```bash
git add -A src/hw src/core/journal.hpp src/app/main.cpp CMakeLists.txt
git commit -m "feat(hw): app files with atomic config writes, driver version, logon task"
git push
```

---

### Task 4: Commands

**Files:**
- Modify: `src/app/main.cpp`, `docs/hardware-checks.md`, `README.md`

**Interfaces:**
- Consumes: everything above; existing `IsElevated`, `optimize`, `kGpu`, `make_gpu_control`.
- Produces: `gao --apply`, `gao --boot on|off`, `gao --status`, `gao --boot-apply`; `--optimize` saves the profile.

- [ ] **Step 1: Helpers**

In `src/app/main.cpp` add includes `"core/boot.hpp"`, `"core/config.hpp"`, `"hw/boot_task.hpp"`, `<ctime>`. After `IsElevated()` add:
```cpp
static std::string now_text() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm);
    return buf;
}

static gao::Config load_config() {
    const auto text = gao::read_file(gao::config_path());
    return text ? gao::from_json(*text) : gao::Config{};
}

static bool save_config(const gao::Config& c) {
    return !gao::config_path().empty() && gao::write_file_atomic(gao::config_path(), gao::to_json(c));
}

static void boot_log(const std::string& msg) {
    gao::append_line_durable(gao::boot_log_path(), now_text() + "  " + msg);
}

static std::string profile_text(const gao::Profile& p) {
    return std::string(gao::preset_name(p.preset)) + ": power " + std::to_string(p.power_pct) + " %, core +" +
           std::to_string(p.core_mhz) + " MHz, mem +" + std::to_string(p.mem_mhz) + " MHz (driver " + p.driver +
           ", saved " + p.saved_at + ")";
}

static std::string decision_text(gao::BootDecision d, const gao::Config& c, const std::string& driver) {
    switch (d) {
        case gao::BootDecision::NoProfile: return "no saved profile; run `gao --optimize` first";
        case gao::BootDecision::TooManyStrikes:
            return "disabled after " + std::to_string(gao::kMaxBootStrikes) + " crashes; run `gao --boot on` to retry";
        case gao::BootDecision::DriverChanged:
            return "driver changed (" + c.profile->driver + " -> " + (driver.empty() ? "unknown" : driver) +
                   "); run `gao --optimize` again";
        case gao::BootDecision::Apply: return "apply";
    }
    return "unknown";
}
```

- [ ] **Step 2: `--optimize` saves**

In `optimize()`, just before `std::printf("Applied until reboot. ...")`, insert:
```cpp
    gao::Config cfg = load_config();
    cfg.profile = gao::Profile{preset, r.power_pct, r.core_mhz, r.mem_mhz, nvml.DriverVersion(), now_text()};
    if (save_config(cfg)) std::printf("Saved: `gao --apply` re-applies it, `gao --boot on` applies it at every logon.\n");
    else std::printf("warning: could not save the profile to %s\n", gao::config_path().string().c_str());
```

- [ ] **Step 3: The four commands**

Add before `main()`:
```cpp
static int apply() {
    if (!IsElevated()) { std::printf("--apply needs an elevated (administrator) shell\n"); return 1; }
    gao::Nvml nvml;
    if (!nvml.Init()) { std::printf("NVML init failed: %s\n", nvml.Error().c_str()); return 1; }
    gao::Nvapi nvapi;
    if (!nvapi.Init()) { std::printf("NVAPI init failed: %s\n", nvapi.Error().c_str()); return 1; }
    gao::Config cfg = load_config();
    cfg.boot_strikes = 0;   // strikes only gate boot-apply
    const std::string driver = nvml.DriverVersion();
    const auto d = gao::decide_boot(cfg, driver);
    if (d != gao::BootDecision::Apply) { std::printf("not applied: %s\n", decision_text(d, cfg, driver).c_str()); return 1; }
    const gao::GpuControl gpu = gao::make_gpu_control(nvml, nvapi, kGpu);
    std::string why;
    if (!gao::apply_profile(gpu, *cfg.profile, &why)) { std::printf("not applied: %s (card at stock)\n", why.c_str()); return 1; }
    std::printf("applied %s\nOK\n", profile_text(*cfg.profile).c_str());
    return 0;
}

static int boot(bool on) {
    if (!IsElevated()) { std::printf("--boot needs an elevated (administrator) shell\n"); return 1; }
    if (!on) {
        const int code = gao::boot_task_remove();
        std::printf(code == 0 ? "boot-apply off: task removed\n" : "could not remove the task (schtasks exit %d)\n", code);
        return code == 0 ? 0 : 1;
    }
    wchar_t exe[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) { std::printf("could not find gao.exe's own path\n"); return 1; }
    const int code = gao::boot_task_create(exe);
    if (code != 0) { std::printf("could not create the task (schtasks exit %d)\n", code); return 1; }
    gao::Config cfg = load_config();
    cfg.boot_strikes = 0;
    if (!save_config(cfg)) { std::printf("task created, but could not reset the strike counter\n"); return 1; }
    std::printf("boot-apply on: the saved profile is applied at every logon (strikes reset)\n");
    if (!cfg.profile) std::printf("note: there is no saved profile yet; run `gao --optimize` first\n");
    return 0;
}

static int status() {
    const auto text = gao::read_file(gao::config_path());
    const gao::Config cfg = text ? gao::from_json(*text) : gao::Config{};
    if (cfg.profile) std::printf("profile:    %s\n", profile_text(*cfg.profile).c_str());
    else if (text) std::printf("profile:    none valid in %s\n", gao::config_path().string().c_str());
    else std::printf("profile:    none (run `gao --optimize`)\n");
    gao::Nvml nvml;
    const std::string driver = nvml.Init() ? nvml.DriverVersion() : std::string();
    if (cfg.profile)
        std::printf("driver:     %s (%s)\n", driver.empty() ? "unknown" : driver.c_str(),
                    !driver.empty() && driver == cfg.profile->driver ? "matches" : "CHANGED -- run `gao --optimize` again");
    std::printf("boot-apply: %s\n", gao::boot_task_exists() ? "on (logon task registered)" : "off");
    std::printf("strikes:    %d of %d\n", cfg.boot_strikes, gao::kMaxBootStrikes);
    const auto log = gao::read_lines(gao::boot_log_path());
    if (!log.empty()) std::printf("last boot:  %s\n", log.back().c_str());
    return 0;
}

// Run by the logon task. No console, no prompts: everything goes to boot.log.
static int boot_apply() {
    FreeConsole();
    gao::Nvml nvml;
    if (!nvml.Init()) { boot_log("NVML init failed: " + nvml.Error()); return 1; }
    gao::Config cfg = load_config();
    const std::string driver = nvml.DriverVersion();
    const auto d = gao::decide_boot(cfg, driver);
    if (d != gao::BootDecision::Apply) { boot_log("not applied: " + decision_text(d, cfg, driver)); return 1; }
    // The strike is on disk before the hardware is touched: a crash from here
    // on counts.
    ++cfg.boot_strikes;
    if (!save_config(cfg)) { boot_log("could not record the strike; not applying"); return 1; }
    gao::Nvapi nvapi;
    if (!nvapi.Init()) { boot_log("NVAPI init failed: " + nvapi.Error()); return 1; }
    const gao::GpuControl gpu = gao::make_gpu_control(nvml, nvapi, kGpu);
    std::string why;
    if (!gao::apply_profile(gpu, *cfg.profile, &why)) { boot_log("apply failed: " + why + " -- card at stock"); return 1; }
    boot_log("applied " + profile_text(*cfg.profile) + ", strike " + std::to_string(cfg.boot_strikes) +
             " clears in 2 minutes");
    Sleep(2 * 60 * 1000);
    // Reload: --optimize may have saved a new profile meanwhile; only the
    // counter is ours to change.
    gao::Config latest = load_config();
    latest.boot_strikes = 0;
    save_config(latest);
    return 0;
}
```

In `main()` before the `--optimize` branch:
```cpp
    if (argc > 1 && std::strcmp(argv[1], "--apply") == 0) return apply();
    if (argc > 2 && std::strcmp(argv[1], "--boot") == 0) {
        if (std::strcmp(argv[2], "on") == 0) return boot(true);
        if (std::strcmp(argv[2], "off") == 0) return boot(false);
        std::printf("--boot expects on or off, got '%s'\n", argv[2]);
        return 1;
    }
    if (argc > 1 && std::strcmp(argv[1], "--status") == 0) return status();
    if (argc > 1 && std::strcmp(argv[1], "--boot-apply") == 0) return boot_apply();
```
Usage line:
```cpp
    std::printf("usage: gao [--version | --probe | --set-core <mhz> | --set-mem <mhz> | --reset | --set-fan <pct>\n"
                "            | --stress <sec> [--max-temp <c>] | --optimize [best|quiet|cool|max]\n"
                "            | --apply | --boot on|off | --status]\n");
```

- [ ] **Step 4: Build and smoke**

Run: `cmake --build build --config Release; ctest --test-dir build -C Release --output-on-failure`
Expected: no warnings, `100% tests passed`.
Run (not elevated): `.\build\Release\gao.exe --status` → prints profile/driver/boot-apply/strikes lines, exit 0. `.\build\Release\gao.exe --apply` → elevation message, exit 1. `.\build\Release\gao.exe --boot maybe` → `--boot expects on or off`, exit 1.

- [ ] **Step 5: Docs**

Append rows to `docs/hardware-checks.md` (and a note: "Checks 17–22 cover persistence; 19 and 20 need a log-off and log-on."):
```markdown
| 17 | Optimize saves a profile | `--optimize best` (elevated), then `--status` | yes / no | `Saved:` line; `--status` shows the profile and `driver: ... (matches)` | |
| 18 | Apply re-applies it | `--reset`, then `--apply`, then `--probe` | yes | `applied best: ...`, `OK`; `--probe` limit equals the profile's power % of default | |
| 19 | Boot-apply at logon | `--boot on`, log off and on, wait 2 min, `--status` | yes | `boot.log` last line `applied ...`; `--status` shows `boot-apply: on`, `strikes: 0 of 3` | |
| 20 | Three strikes stop it | set `"boot_strikes": 3` in `gao.json`, log off and on, `--status`; then `--boot on` | yes | last boot line `not applied: disabled after 3 crashes ...`; card at stock; after `--boot on`, `strikes: 0 of 3` | |
| 21 | A driver change blocks it | set `"driver": "000.00"` in `gao.json`, run `--apply`; restore the value | yes | `not applied: driver changed (000.00 -> ...)` | |
| 22 | Boot-apply off | `--boot off`, then `schtasks /Query /TN GpuAutoOptimizer` | yes | `boot-apply off: task removed`; schtasks reports the task does not exist | |
```

`README.md`: add to Quick start after the `--optimize` line:
```powershell
.\build\Release\gao.exe --apply           # elevated: re-apply the saved result
.\build\Release\gao.exe --boot on         # elevated: re-apply at every logon
.\build\Release\gao.exe --status
```
with one paragraph: "`--optimize` saves its result to `%LOCALAPPDATA%\GpuAutoOptimizer\gao.json`. `--boot on` registers a logon task that re-applies it; if the machine crashes within 2 minutes of three logons in a row, or the NVIDIA driver version changes, boot-apply stops and `--status` / `boot.log` say why." Update the IMPORTANT block's "results are lost on reboot" to "there is no GUI yet"; add `--apply`, `--boot` to the WARNING list; Project structure: `boot.*` under core, `app_files.*` (replacing `journal_file.*`) and `boot_task.*` under hw; Status: phases 0–4 done, P5 (ImGui screens and tray) next.

- [ ] **Step 6: Commit and push**

```bash
git add src/app/main.cpp docs/hardware-checks.md README.md
git commit -m "feat(app): --apply, --boot on|off, --status and the logon boot-apply"
git push
```

---

### Task 5: Hardware verification (with the user)

**Files:** Modify `docs/hardware-checks.md` (Result column).

- [ ] **Step 1: 17, 18, 21, 22 via one elevated script** (UAC-launched, one log file per step, no tail on the files): `--optimize best` → `--status` → `--reset` → `--apply` → `--probe` → edit driver in `gao.json` → `--apply` → restore driver → `--boot on` → `schtasks /Query /TN GpuAutoOptimizer /V /FO LIST` (confirm the quoted action with the space-containing path) → leave the task on for step 2.
- [ ] **Step 2: 19** — ask the user to log off and on, wait 2 minutes, then run `--status` and paste it; read `boot.log`.
- [ ] **Step 3: 20** — set `boot_strikes` to 3 (edit `gao.json` directly), ask the user to log off and on, then `--status`; run `--boot on` elevated and `--status` again.
- [ ] **Step 4: 22 and cleanup** — `--boot off`, `schtasks /Query` (expect "does not exist"), `--reset`. Ask the user whether to leave boot-apply on afterwards; do what they say.
- [ ] **Step 5:** fill the Result column; commit `docs: record P4 hardware checks on the RTX 4070`; push.
