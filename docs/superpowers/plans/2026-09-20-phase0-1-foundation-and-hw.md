# GPU Auto Optimizer — Phase 0 + 1 (Foundation and Hardware Layer) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turn the repository into a pure C++ project that builds one executable, and give it a hardware layer that can read telemetry and set core/memory offsets and fan speed — every write verified by read-back.

**Architecture:** Three layers in one process. `src/core/` is pure logic with no Windows or driver calls and is the only part unit-tested in CI. `src/hw/` is the only code that touches NVML, NVAPI or D3D11. `src/app/` is the entry point; in this plan it is a command-line probe, because the ImGui UI is Phase 5. `core` reaches hardware exclusively through a `GpuControl` struct of callbacks, which tests fill with lambdas.

**Tech Stack:** C++20, MSVC (Visual Studio 2026), CMake ≥ 3.28, doctest (vendored), nlohmann/json (vendored single header), NVML and NVAPI loaded at runtime with `LoadLibrary`.

**Spec:** `docs/superpowers/specs/2026-09-20-cpp-rewrite-design.md`

## Global Constraints

- Language: **C++20**, MSVC, **x64 only**. Build from a VS Developer PowerShell.
- **English everywhere**: code, comments, commit messages, CLI output, documentation.
- `src/core/` must not include `windows.h`, `nvml.h`, NVAPI headers, or any D3D header. If a core file needs a platform type, the design is wrong.
- No package manager. Third-party code is vendored under `third_party/` as source.
- **Every hardware write is followed by a read-back, and a mismatch is a failure.** A driver return code of "OK" is never sufficient evidence that a setting was applied.
- No dependency is added for something a few lines of standard library can do.
- Hardware-touching code paths are never unit-tested in CI; they are verified manually through `docs/hardware-checks.md` on the RTX 4070.
- Clock offsets are expressed in **MHz** at every API boundary; only the NVAPI buffer uses kHz.

---

## File Structure

| File | Responsibility |
|---|---|
| `CMakeLists.txt` | Root build: `core` (static lib), `hw` (static lib), `gao` (exe), `core_tests` (doctest exe) |
| `src/core/types.hpp` | `Telemetry`, `FanCurve`, `GpuControl` — the hardware seam |
| `src/core/objectives.hpp/.cpp` | `Preset` enum, `Objectives` struct, `objectives_for(Preset)` |
| `src/core/config.hpp/.cpp` | `Config` struct, JSON load/save, default path |
| `src/hw/nvml.hpp/.cpp` | Telemetry via NVML (exists today, extended with fan %) |
| `src/hw/nvapi.hpp/.cpp` | NVAPI loader, clock offsets, fan control, `make_gpu_control()` |
| `src/app/main.cpp` | CLI: `--version`, `--probe`, `--set-core`, `--set-mem`, `--set-fan`, `--reset` |
| `tests/*.cpp` | doctest over `core` only |
| `.github/workflows/ci.yml` | Configure, build, `ctest` on `windows-latest` |
| `docs/hardware-checks.md` | The manual checklist Phase 1 ends with |

---

### Task 1: Repository migration and build skeleton

**Files:**
- Create: `CMakeLists.txt`, `src/app/main.cpp`, `src/core/version.hpp`, `tests/test_main.cpp`, `tests/test_version.cpp`, `.github/workflows/ci.yml`, `.gitignore`
- Delete: the entire Python tree, `cpp/src/core/ab_profile.*`, `cpp/src/hw/afterburner.*`, `cpp/src/tools/abctl_spike_main.cpp`, `requirements*.txt`, `setup.bat`, `start.bat`, `installer.py`, `gpu_optimizer.py`, `verify_settings.py`, `optimizer_config.json`, `src/`, `tests/` (Python), the Python CI workflow
- Move: `cpp/src/**` → `src/**`, `cpp/tests/**` → `tests/**`, `cpp/third_party/**` → `third_party/**`

**Interfaces:**
- Consumes: nothing.
- Produces: a build where `cmake --build` yields `gao.exe` and `core_tests.exe`; `gao.exe --version` prints `gpu-auto-optimizer <version>`.

- [ ] **Step 1: Tag the Python version so it stays retrievable**

```bash
git tag -a v0.9-python -m "Final Python version before the C++ rewrite"
git push origin v0.9-python
```

- [ ] **Step 2: Remove the Python tree and the Afterburner spike**

```bash
git rm -r --quiet src tests requirements.txt requirements-dev.txt setup.bat start.bat \
  installer.py gpu_optimizer.py verify_settings.py optimizer_config.json .github/workflows/ci.yml
git rm --quiet cpp/src/core/ab_profile.cpp cpp/src/core/ab_profile.hpp \
  cpp/src/hw/afterburner.cpp cpp/src/hw/afterburner.hpp cpp/src/tools/abctl_spike_main.cpp \
  cpp/tests/test_ab_profile.cpp
```

- [ ] **Step 3: Flatten the C++ tree to the repository root**

```bash
git mv cpp/src src
git mv cpp/tests tests
git mv cpp/third_party third_party
git mv cpp/CMakePresets.json CMakePresets.json
git rm --quiet cpp/CMakeLists.txt cpp/build.ps1 cpp/.gitignore
```

- [ ] **Step 4: Write the root CMakeLists.txt**

```cmake
cmake_minimum_required(VERSION 3.28)
project(gpu_auto_optimizer CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(doctest INTERFACE)
target_include_directories(doctest INTERFACE third_party)

# Pure logic. No Windows, no driver, no D3D — this is what CI tests.
add_library(core STATIC
  src/core/version.hpp
  src/core/version.cpp
)
target_include_directories(core PUBLIC src)

# The only code allowed to touch hardware.
add_library(hw STATIC
  src/hw/nvml.hpp src/hw/nvml.cpp
)
target_include_directories(hw PUBLIC src)
target_link_libraries(hw PUBLIC core)

add_executable(gao src/app/main.cpp)
target_link_libraries(gao PRIVATE core hw)

enable_testing()
add_executable(core_tests
  tests/test_main.cpp
  tests/test_version.cpp
)
target_link_libraries(core_tests PRIVATE core doctest)
add_test(NAME core_tests COMMAND core_tests)
```

- [ ] **Step 5: Write the failing test**

`tests/test_version.cpp`:

```cpp
#include "doctest/doctest.h"
#include "core/version.hpp"

TEST_CASE("version string is the product name and a semver") {
    CHECK(gao::kProductName == std::string("gpu-auto-optimizer"));
    CHECK(gao::kVersion == std::string("0.1.0"));
}
```

Keep the existing `tests/test_main.cpp` (it defines `DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN`); if it is missing, create it with exactly that define followed by `#include "doctest/doctest.h"`.

- [ ] **Step 6: Run the test to verify it fails**

```powershell
cmake -S . -B build -A x64
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

Expected: compile error — `core/version.hpp` does not declare `kProductName`.

- [ ] **Step 7: Write the minimal implementation**

`src/core/version.hpp`:

```cpp
#pragma once
#include <string_view>

namespace gao {
inline constexpr std::string_view kProductName = "gpu-auto-optimizer";
inline constexpr std::string_view kVersion = "0.1.0";
}
```

Delete `src/core/version.cpp` if it exists and drop it from `CMakeLists.txt`; the header is the whole implementation.

- [ ] **Step 8: Write the CLI entry point**

`src/app/main.cpp`:

```cpp
#include "core/version.hpp"
#include <cstdio>
#include <cstring>

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--version") == 0) {
        std::printf("%s %s\n", gao::kProductName.data(), gao::kVersion.data());
        return 0;
    }
    std::printf("usage: gao --version\n");
    return argc > 1 ? 1 : 0;
}
```

- [ ] **Step 9: Run the test to verify it passes**

```powershell
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
.\build\Debug\gao.exe --version
```

Expected: `core_tests` passes; the executable prints `gpu-auto-optimizer 0.1.0`.

- [ ] **Step 10: Write the CI workflow**

`.github/workflows/ci.yml`:

```yaml
name: CI

on:
  push:
    branches: [main]
  pull_request:

permissions:
  contents: read

jobs:
  build-and-test:
    name: Build and test (x64)
    runs-on: windows-latest
    steps:
      - uses: actions/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1 # v7.0.1
        with:
          persist-credentials: false

      - name: Configure
        run: cmake -S . -B build -A x64

      # The full executable is built, not just the test target: the runner has no
      # GPU, but it still catches link errors in the hardware layer.
      - name: Build
        run: cmake --build build --config Release

      - name: Test
        run: ctest --test-dir build -C Release --output-on-failure
```

- [ ] **Step 11: Write .gitignore**

```gitignore
build/
out/
.vs/
*.user
```

- [ ] **Step 12: Commit**

```bash
git add -A
git commit -m "chore: replace the Python app with a C++ skeleton

main is now a single C++ project: core (pure logic), hw (hardware only) and
one executable. The Python version stays reachable through the v0.9-python
tag, and the Afterburner spike is removed with it."
git push origin main
```

Confirm the CI run is green before starting Task 2.

---

### Task 2: Presets and objectives

**Files:**
- Create: `src/core/objectives.hpp`, `src/core/objectives.cpp`, `tests/test_objectives.cpp`
- Modify: `CMakeLists.txt` (add the two core files and the test file)

**Interfaces:**
- Consumes: nothing.
- Produces: `enum class gao::Preset { BestOfMyGpu, Quiet, CoolAndEfficient, MaxPerformance }` and `gao::Objectives gao::objectives_for(Preset)` returning `struct Objectives { int max_temp_c; int max_fan_pct; float perf_push; bool core_oc, mem_oc, power, undervolt; }`.

- [ ] **Step 1: Write the failing test**

`tests/test_objectives.cpp`:

```cpp
#include "doctest/doctest.h"
#include "core/objectives.hpp"

using namespace gao;

TEST_CASE("the default preset chases performance within a quiet thermal envelope") {
    const Objectives o = objectives_for(Preset::BestOfMyGpu);
    CHECK(o.max_temp_c == 75);
    CHECK(o.max_fan_pct == 60);
    CHECK(o.perf_push == doctest::Approx(0.7f));
    CHECK(o.core_oc);
    CHECK(o.mem_oc);
    CHECK(o.power);
}

TEST_CASE("quiet trades temperature headroom for a lower fan ceiling") {
    const Objectives quiet = objectives_for(Preset::Quiet);
    const Objectives best = objectives_for(Preset::BestOfMyGpu);
    CHECK(quiet.max_fan_pct < best.max_fan_pct);
    CHECK(quiet.max_temp_c > best.max_temp_c);
}

TEST_CASE("cool and efficient does not overclock") {
    const Objectives o = objectives_for(Preset::CoolAndEfficient);
    CHECK_FALSE(o.core_oc);
    CHECK_FALSE(o.mem_oc);
    CHECK(o.power);
}

TEST_CASE("no preset ever enables undervolting") {
    for (const Preset p : {Preset::BestOfMyGpu, Preset::Quiet,
                           Preset::CoolAndEfficient, Preset::MaxPerformance}) {
        CHECK_FALSE(objectives_for(p).undervolt);
    }
}
```

- [ ] **Step 2: Run the test to verify it fails**

Add `src/core/objectives.hpp src/core/objectives.cpp` to the `core` target and `tests/test_objectives.cpp` to `core_tests` in `CMakeLists.txt`, then:

```powershell
cmake --build build --config Debug
```

Expected: compile error — `core/objectives.hpp` does not exist.

- [ ] **Step 3: Write the minimal implementation**

`src/core/objectives.hpp`:

```cpp
#pragma once

namespace gao {

enum class Preset { BestOfMyGpu, Quiet, CoolAndEfficient, MaxPerformance };

// What the user wants, not what the tuner will do. The search reads these as
// ceilings; perf_push decides how hard it chases clocks inside them.
struct Objectives {
    int   max_temp_c = 75;
    int   max_fan_pct = 60;
    float perf_push = 0.7f;   // 0..1
    bool  core_oc = true;
    bool  mem_oc = true;
    bool  power = true;
    bool  undervolt = false;  // opt-in only, never set by a preset
};

Objectives objectives_for(Preset preset);

}
```

`src/core/objectives.cpp`:

```cpp
#include "core/objectives.hpp"

namespace gao {

Objectives objectives_for(const Preset preset) {
    switch (preset) {
        case Preset::Quiet:            return {80, 40, 0.4f, true,  true,  true, false};
        case Preset::CoolAndEfficient: return {65, 70, 0.3f, false, false, true, false};
        case Preset::MaxPerformance:   return {83, 100, 1.0f, true, true,  true, false};
        case Preset::BestOfMyGpu:
        default:                       return {75, 60, 0.7f, true,  true,  true, false};
    }
}

}
```

- [ ] **Step 4: Run the test to verify it passes**

```powershell
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

Expected: all four test cases pass.

- [ ] **Step 5: Commit**

```bash
git add src/core/objectives.hpp src/core/objectives.cpp tests/test_objectives.cpp CMakeLists.txt
git commit -m "feat(core): presets as data over three objectives"
```

---

### Task 3: Config file round-trip

**Files:**
- Create: `src/core/config.hpp`, `src/core/config.cpp`, `tests/test_config.cpp`, `third_party/nlohmann/json.hpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `Objectives` from Task 2.
- Produces: `struct gao::Config { Preset preset; Objectives objectives; int core_offset_mhz; int mem_offset_mhz; int power_limit_pct; std::vector<int> blacklisted_core_offsets; }`, plus `std::string gao::to_json(const Config&)` and `Config gao::from_json(const std::string&)`.

- [ ] **Step 1: Vendor the JSON header**

Download the single-header release of nlohmann/json (v3.11 or newer) to `third_party/nlohmann/json.hpp`. Nothing else from that project is needed.

- [ ] **Step 2: Write the failing test**

`tests/test_config.cpp`:

```cpp
#include "doctest/doctest.h"
#include "core/config.hpp"

using namespace gao;

TEST_CASE("a config survives a round-trip through JSON") {
    Config c;
    c.preset = Preset::Quiet;
    c.objectives = objectives_for(Preset::Quiet);
    c.core_offset_mhz = 150;
    c.mem_offset_mhz = 800;
    c.power_limit_pct = 90;
    c.blacklisted_core_offsets = {180, 210};

    const Config back = from_json(to_json(c));

    CHECK(back.preset == Preset::Quiet);
    CHECK(back.objectives.max_fan_pct == 40);
    CHECK(back.core_offset_mhz == 150);
    CHECK(back.mem_offset_mhz == 800);
    CHECK(back.power_limit_pct == 90);
    CHECK(back.blacklisted_core_offsets == std::vector<int>{180, 210});
}

TEST_CASE("an unreadable config falls back to defaults instead of throwing") {
    const Config c = from_json("{ this is not json");
    CHECK(c.preset == Preset::BestOfMyGpu);
    CHECK(c.core_offset_mhz == 0);
    CHECK(c.blacklisted_core_offsets.empty());
}
```

- [ ] **Step 3: Run the test to verify it fails**

Add the files to `CMakeLists.txt`, then `cmake --build build --config Debug`.
Expected: compile error — `core/config.hpp` does not exist.

- [ ] **Step 4: Write the minimal implementation**

`src/core/config.hpp`:

```cpp
#pragma once
#include "core/objectives.hpp"
#include <string>
#include <vector>

namespace gao {

// Everything that has to survive a reboot. One flat struct: there is no second
// version of this format yet, so there is nothing to migrate.
struct Config {
    Preset preset = Preset::BestOfMyGpu;
    Objectives objectives = objectives_for(Preset::BestOfMyGpu);
    int core_offset_mhz = 0;
    int mem_offset_mhz = 0;
    int power_limit_pct = 100;
    std::vector<int> blacklisted_core_offsets;
};

std::string to_json(const Config& c);
Config from_json(const std::string& text);  // never throws; bad input yields defaults

}
```

`src/core/config.cpp`:

```cpp
#include "core/config.hpp"
#include "nlohmann/json.hpp"

namespace gao {

std::string to_json(const Config& c) {
    const nlohmann::json j = {
        {"preset", static_cast<int>(c.preset)},
        {"objectives", {
            {"max_temp_c", c.objectives.max_temp_c},
            {"max_fan_pct", c.objectives.max_fan_pct},
            {"perf_push", c.objectives.perf_push},
            {"core_oc", c.objectives.core_oc},
            {"mem_oc", c.objectives.mem_oc},
            {"power", c.objectives.power},
            {"undervolt", c.objectives.undervolt},
        }},
        {"core_offset_mhz", c.core_offset_mhz},
        {"mem_offset_mhz", c.mem_offset_mhz},
        {"power_limit_pct", c.power_limit_pct},
        {"blacklisted_core_offsets", c.blacklisted_core_offsets},
    };
    return j.dump(2);
}

Config from_json(const std::string& text) {
    Config c;
    const nlohmann::json j = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) return c;

    c.preset = static_cast<Preset>(j.value("preset", 0));
    c.objectives = objectives_for(c.preset);
    if (const auto it = j.find("objectives"); it != j.end() && it->is_object()) {
        c.objectives.max_temp_c = it->value("max_temp_c", c.objectives.max_temp_c);
        c.objectives.max_fan_pct = it->value("max_fan_pct", c.objectives.max_fan_pct);
        c.objectives.perf_push = it->value("perf_push", c.objectives.perf_push);
        c.objectives.core_oc = it->value("core_oc", c.objectives.core_oc);
        c.objectives.mem_oc = it->value("mem_oc", c.objectives.mem_oc);
        c.objectives.power = it->value("power", c.objectives.power);
        c.objectives.undervolt = it->value("undervolt", c.objectives.undervolt);
    }
    c.core_offset_mhz = j.value("core_offset_mhz", 0);
    c.mem_offset_mhz = j.value("mem_offset_mhz", 0);
    c.power_limit_pct = j.value("power_limit_pct", 100);
    c.blacklisted_core_offsets = j.value("blacklisted_core_offsets", std::vector<int>{});
    return c;
}

}
```

- [ ] **Step 5: Run the test to verify it passes**

```powershell
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

- [ ] **Step 6: Commit**

```bash
git add src/core/config.hpp src/core/config.cpp tests/test_config.cpp third_party/nlohmann/json.hpp CMakeLists.txt
git commit -m "feat(core): config round-trip with defaults on bad input"
```

---

### Task 4: The hardware seam and NVML telemetry

**Files:**
- Create: `src/core/types.hpp`, `tests/test_types.cpp`
- Modify: `src/hw/nvml.hpp`, `src/hw/nvml.cpp` (add fan percentage), `src/app/main.cpp` (add `--probe`), `CMakeLists.txt`

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces: `struct gao::Telemetry { int core_mhz, mem_mhz, temp_c, fan_pct, power_w, power_limit_w; bool ok; }` and `struct gao::GpuControl` with members `read`, `set_core_offset`, `set_mem_offset`, `set_power_limit`, `set_fan_pct`, `reset_to_stock` — all `std::function`. `gao::Nvml::Read()` fills `Telemetry`.

- [ ] **Step 1: Write the failing test**

`tests/test_types.cpp` — this tests the seam, not the driver: a `GpuControl` built from lambdas must be usable by pure code, which is the property the whole test strategy depends on.

```cpp
#include "doctest/doctest.h"
#include "core/types.hpp"

using namespace gao;

TEST_CASE("core code can drive a fake GpuControl") {
    int applied_core = 0;
    Telemetry fake{};
    fake.ok = true;
    fake.core_mhz = 2600;
    fake.temp_c = 61;

    GpuControl gpu;
    gpu.read = [&] { return fake; };
    gpu.set_core_offset = [&](int mhz) { applied_core = mhz; return true; };

    CHECK(gpu.read().temp_c == 61);
    CHECK(gpu.set_core_offset(150));
    CHECK(applied_core == 150);
}

TEST_CASE("an unset callback is detectable rather than crashing") {
    const GpuControl gpu;
    CHECK_FALSE(static_cast<bool>(gpu.set_fan_pct));
}
```

- [ ] **Step 2: Run the test to verify it fails**

Add `tests/test_types.cpp` to `core_tests`, then `cmake --build build --config Debug`.
Expected: compile error — `core/types.hpp` does not exist.

- [ ] **Step 3: Write the minimal implementation**

`src/core/types.hpp`:

```cpp
#pragma once
#include <functional>
#include <vector>

namespace gao {

struct Telemetry {
    int core_mhz = 0;
    int mem_mhz = 0;
    int temp_c = 0;
    int fan_pct = -1;         // -1 when the driver does not report it
    int power_w = 0;
    int power_limit_w = 0;
    bool ok = false;
};

struct FanPoint { int temp_c; int fan_pct; };
using FanCurve = std::vector<FanPoint>;   // four points, ascending by temp_c

// The only way core code reaches hardware. hw/ fills these in production,
// tests fill them with lambdas. An empty callback means "not supported here".
struct GpuControl {
    std::function<Telemetry()> read;
    std::function<bool(int)> set_core_offset;   // MHz, verified by read-back
    std::function<bool(int)> set_mem_offset;    // MHz, verified by read-back
    std::function<bool(int)> set_power_limit;   // percent of default
    std::function<bool(int)> set_fan_pct;       // percent, -1 restores automatic
    std::function<bool()> reset_to_stock;
};

}
```

- [ ] **Step 4: Run the test to verify it passes**

```powershell
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

- [ ] **Step 5: Extend the NVML wrapper to fill Telemetry**

Replace the body of `src/hw/nvml.hpp` so it returns the shared type instead of its own:

```cpp
#pragma once
#include "core/types.hpp"
#include <string>

namespace gao {

class Nvml {
public:
    bool Init();
    ~Nvml();
    int DeviceCount();
    Telemetry Read(unsigned index);
    const std::string& Error() const { return error_; }

private:
    void* lib_ = nullptr;
    bool inited_ = false;
    std::string error_;
};

}
```

In `src/hw/nvml.cpp`, keep the existing runtime-loading approach and add the fan reading, resolving `nvmlDeviceGetFanSpeed` the same way the other entry points are resolved. When that symbol is missing or returns a non-zero status, leave `fan_pct` at `-1` rather than reporting `0` — zero is a real fan speed and must not be faked.

- [ ] **Step 6: Add the --probe command**

In `src/app/main.cpp`:

```cpp
#include "core/types.hpp"
#include "core/version.hpp"
#include "hw/nvml.hpp"
#include <cstdio>
#include <cstring>

static int probe() {
    gao::Nvml nvml;
    if (!nvml.Init()) {
        std::printf("NVML init failed: %s\n", nvml.Error().c_str());
        return 1;
    }
    const int count = nvml.DeviceCount();
    std::printf("GPUs: %d\n", count);
    for (int i = 0; i < count; ++i) {
        const gao::Telemetry t = nvml.Read(static_cast<unsigned>(i));
        if (!t.ok) { std::printf("  [%d] read failed\n", i); continue; }
        std::printf("  [%d] core=%d MHz  mem=%d MHz  temp=%d C  fan=%d%%  power=%d/%d W\n",
                    i, t.core_mhz, t.mem_mhz, t.temp_c, t.fan_pct, t.power_w, t.power_limit_w);
    }
    return 0;
}

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--version") == 0) {
        std::printf("%s %s\n", gao::kProductName.data(), gao::kVersion.data());
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "--probe") == 0) return probe();
    std::printf("usage: gao [--version | --probe]\n");
    return argc > 1 ? 1 : 0;
}
```

- [ ] **Step 7: Verify on hardware**

```powershell
cmake --build build --config Release
.\build\Release\gao.exe --probe
```

Expected on the RTX 4070: one GPU, plausible clocks, a temperature between 25 and 90, a fan percentage that is not `-1`, and a power limit near 200 W. Compare the numbers against `nvidia-smi` — they must agree. Record the output in `docs/hardware-checks.md` in Task 6.

- [ ] **Step 8: Commit**

```bash
git add src/core/types.hpp src/hw/nvml.hpp src/hw/nvml.cpp src/app/main.cpp tests/test_types.cpp CMakeLists.txt
git commit -m "feat(hw): shared Telemetry type and NVML probe"
```

---

### Task 5: NVAPI clock offsets with read-back verification

**Files:**
- Create: `src/hw/nvapi.hpp`, `src/hw/nvapi.cpp`
- Modify: `src/app/main.cpp` (add `--set-core`, `--set-mem`, `--reset`), `CMakeLists.txt`

**Interfaces:**
- Consumes: `Telemetry` and `GpuControl` from Task 4.
- Produces: `class gao::Nvapi` with `bool Init()`, `bool SetCoreOffsetMhz(unsigned gpu, int mhz)`, `bool SetMemOffsetMhz(unsigned gpu, int mhz)`, `std::optional<std::pair<int,int>> ReadOffsetsMhz(unsigned gpu)`, `bool ResetOffsets(unsigned gpu)`, `const std::string& Error() const`.

The constants below are the ones the Python backend already proved on this hardware — `src/backends/nvapi.py` in the `v0.9-python` tag. Do not re-derive them.

| Constant | Value | Meaning |
|---|---|---|
| `NvAPI_Initialize` | `0x0150E828` | QueryInterface id |
| `NvAPI_Unload` | `0x0D22BDD7` | QueryInterface id |
| `NvAPI_EnumPhysicalGPUs` | `0xE5AC921F` | QueryInterface id |
| `NvAPI_GPU_GetPstates20` | `0x6FF81213` | QueryInterface id |
| `NvAPI_GPU_SetPstates20` | `0x0F4DAE6B` | QueryInterface id |
| buffer size | `7416` bytes | `NV_GPU_PERF_PSTATES20_INFO` V2 |
| version word | `7416 \| (2 << 16)` = `0x00021CF8` | written at offset 0 |
| core delta offset | `40` | int32, kHz |
| memory delta offset | `84` | int32, kHz |
| verification tolerance | `1000` kHz | read-back may differ by one step |

- [ ] **Step 1: Write the implementation**

`src/hw/nvapi.hpp`:

```cpp
#pragma once
#include <optional>
#include <string>
#include <utility>

namespace gao {

// Clock offsets through NVAPI's PState20 V2 buffer. Every setter verifies by
// reading the buffer back: NVAPI returns success for changes it does not apply.
class Nvapi {
public:
    bool Init();
    ~Nvapi();
    bool SetCoreOffsetMhz(unsigned gpu, int mhz);
    bool SetMemOffsetMhz(unsigned gpu, int mhz);
    bool ResetOffsets(unsigned gpu);
    std::optional<std::pair<int, int>> ReadOffsetsMhz(unsigned gpu);  // {core, mem}
    const std::string& Error() const { return error_; }

private:
    bool SetDeltaKhz(unsigned gpu, int offset_bytes, int khz);
    void* lib_ = nullptr;
    void* query_ = nullptr;
    std::string error_;
};

}
```

`src/hw/nvapi.cpp` follows the same runtime-loading shape as `nvml.cpp`: `LoadLibraryA("nvapi64.dll")`, `GetProcAddress(lib, "nvapi_QueryInterface")`, then resolve each id through that. `SetDeltaKhz` reads the 7416-byte buffer with `GetPstates20`, writes the version word and the int32 delta at the given offset, calls `SetPstates20`, then calls `ReadOffsetsMhz` and returns `false` unless the value is within 1000 kHz of the request. Any failure sets `error_` to a sentence naming the call that failed.

- [ ] **Step 2: Add the CLI commands**

Extend `main()` with `--set-core <mhz>`, `--set-mem <mhz>` and `--reset`, each printing the requested value, the read-back value, and `OK` or `MISMATCH`. Print the read-back even on success — the whole point is that the number is visible.

- [ ] **Step 3: Verify on hardware, starting small**

Run an elevated PowerShell:

```powershell
.\build\Release\gao.exe --set-core 25
.\build\Release\gao.exe --probe
.\build\Release\gao.exe --reset
```

Expected: `--set-core 25` reports a read-back of 25 MHz and `OK`; `--probe` shows a core clock roughly 25 MHz above its previous boost figure under the same load; `--reset` returns the read-back to 0. If the read-back stays 0 while the call returns success, that is exactly the failure this design exists to catch — stop and report it rather than continuing.

- [ ] **Step 4: Commit**

```bash
git add src/hw/nvapi.hpp src/hw/nvapi.cpp src/app/main.cpp CMakeLists.txt
git commit -m "feat(hw): NVAPI clock offsets, verified by read-back"
```

---

### Task 6: Fan control with fallback, and the hardware checklist

**Files:**
- Modify: `src/hw/nvapi.hpp`, `src/hw/nvapi.cpp` (fan control), `src/app/main.cpp` (`--set-fan`), `CMakeLists.txt`
- Create: `src/hw/gpu_control.cpp`, `docs/hardware-checks.md`

**Interfaces:**
- Consumes: `GpuControl` from Task 4, `Nvapi` from Task 5.
- Produces: `bool gao::Nvapi::SetFanPct(unsigned gpu, int pct)` (`-1` restores automatic control), `bool gao::Nvapi::FanControlAvailable() const`, and `gao::GpuControl gao::make_gpu_control(Nvml&, Nvapi&, unsigned gpu)`.

Fan control is the one API in this plan whose interface ids are not already proven in this repository. `NvAPI_GPU_SetCoolerLevels = 0x891FA0AE` is used by Open Hardware Monitor and nvapi-rs and is the fallback path. The newer client fan-cooler ids must be read from `arcnmx/nvapi-rs` (`src/gpu/cooler.rs`) at implementation time rather than guessed. A wrong id is safe: `nvapi_QueryInterface` returns null, which this code treats as "unsupported".

- [ ] **Step 1: Implement fan control with an explicit unsupported state**

Try the client fan-cooler control first, fall back to `SetCoolerLevels`, and verify by reading the fan percentage back through NVML after a two-second settle. If neither path verifies, set `fan_available_ = false` and leave `error_` explaining which call failed. Never report success for a fan change that did not move.

- [ ] **Step 2: Build the production GpuControl**

`src/hw/gpu_control.cpp`:

```cpp
#include "core/types.hpp"
#include "hw/nvapi.hpp"
#include "hw/nvml.hpp"

namespace gao {

// Wires the two drivers into the single struct core code sees. Callbacks the
// hardware cannot support are left empty on purpose, so callers can tell.
GpuControl make_gpu_control(Nvml& nvml, Nvapi& nvapi, unsigned gpu) {
    GpuControl c;
    c.read = [&nvml, gpu] { return nvml.Read(gpu); };
    c.set_core_offset = [&nvapi, gpu](int mhz) { return nvapi.SetCoreOffsetMhz(gpu, mhz); };
    c.set_mem_offset = [&nvapi, gpu](int mhz) { return nvapi.SetMemOffsetMhz(gpu, mhz); };
    c.reset_to_stock = [&nvapi, gpu] { return nvapi.ResetOffsets(gpu); };
    if (nvapi.FanControlAvailable()) {
        c.set_fan_pct = [&nvapi, gpu](int pct) { return nvapi.SetFanPct(gpu, pct); };
    }
    return c;
}

}
```

`set_power_limit` stays empty until Phase 2 wires NVML's power-limit setter; an empty callback is the honest representation of "not implemented yet".

- [ ] **Step 3: Verify on hardware**

```powershell
.\build\Release\gao.exe --set-fan 70
.\build\Release\gao.exe --probe      # fan_pct should be near 70, audibly
.\build\Release\gao.exe --set-fan -1 # back to automatic
```

- [ ] **Step 4: Write the hardware checklist**

`docs/hardware-checks.md` — the checks CI can never run, with space to record the result and the driver version:

```markdown
# Hardware checks

CI builds this project but has no GPU. These checks are the evidence that the
hardware layer works. Run them elevated, record the date and driver version.

| # | Check | Command | Expected | Result |
|---|---|---|---|---|
| 1 | Telemetry matches nvidia-smi | `gao --probe` | Clocks, temp and power agree with `nvidia-smi` | |
| 2 | Fan percentage is real | `gao --probe` | `fan=` is not `-1` | |
| 3 | Core offset applies | `gao --set-core 25` | Read-back 25, `OK` | |
| 4 | Core offset reverts | `gao --reset` | Read-back 0 | |
| 5 | Memory offset applies | `gao --set-mem 100` | Read-back 100, `OK` | |
| 6 | A refused write is reported | `gao --set-core 5000` | `MISMATCH`, not `OK` | |
| 7 | Fan control applies | `gao --set-fan 70` | Fan audibly rises, probe shows ~70 % | |
| 8 | Fan returns to automatic | `gao --set-fan -1` | Fan drops back under driver control | |
| 9 | Unsupported fan API is reported | (card without support) | "fan control unavailable", not silence | |
```

- [ ] **Step 5: Commit**

```bash
git add src/hw/nvapi.hpp src/hw/nvapi.cpp src/hw/gpu_control.cpp src/app/main.cpp docs/hardware-checks.md CMakeLists.txt
git commit -m "feat(hw): fan control with fallback and an explicit unsupported state"
git push origin main
```

---

## Definition of Done (Phase 0 + 1)

- `main` contains no Python; `v0.9-python` tag exists on the remote.
- CI is green: configure, build `gao.exe`, run `core_tests`.
- `core` has tests for objectives, config round-trip and the `GpuControl` seam, and includes no platform header.
- Checks 1-8 in `docs/hardware-checks.md` are filled in on the RTX 4070, with check 6 proving that a refused write reports `MISMATCH`.
- `make_gpu_control()` returns a struct whose fan callback is empty when the hardware cannot do it.

**Gate:** check 6 is the one that matters. If a write NVAPI accepts cannot be distinguished from one it applies, the search in Phase 3 cannot be trusted and the approach needs rethinking before more is built on it.

## Self-Review notes

- Spec coverage: §3 architecture (Tasks 1, 4), §4 control layer and the read-back rule (Tasks 5, 6), §6.1 presets (Task 2), §7 persistence format (Task 3), §9 CI and the manual checklist (Tasks 1, 6), §11 migration (Task 1). Stress load (§5), search and journal (§6.2, §6.3), boot-apply and tray (§7), UI (§8) and release (§10) are deliberately out of scope here — they belong to Phase 2 and later, which get their own plans once check 6 passes.
- No placeholders: every step names the files, the commands and the expected output. The two places where exact values are not in this document — the client fan-cooler interface ids and the interior of `nvapi.cpp` — say exactly where the values come from and what a wrong value looks like at runtime.
- Type consistency: `Telemetry`, `GpuControl`, `Objectives`, `Config`, `Preset`, `Nvml::Read`, `Nvapi::SetCoreOffsetMhz`, `make_gpu_control` are spelled identically in every task that mentions them. Offsets are MHz at every boundary; kHz appears only inside `nvapi.cpp`.
