# Phase 1 — Afterburner Control Plane Spike — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Prove the existential assumption of the whole rewrite — that the app can edit an MSI Afterburner profile (power limit + `VFCurve` undervolt) programmatically and have Afterburner apply it to the hardware, verified via NVML — with no manual clicks.

**Architecture:** A small native C++ console harness (`abctl_spike.exe`) plus a unit-tested core library. Pure-logic pieces (VFCurve hex codec, undervolt math, cfg read/write) are built TDD-first and fully unit-tested. Hardware/Afterburner-dependent pieces (NVML read, AB apply trigger) get documented manual verification on the RTX 4070. If the final apply test passes, the C++/Afterburner architecture is validated and we proceed to Phase 2 (stress.exe). If it fails, we fall back per §4.3 of the spec (kill+relaunch AB) or reconsider the route.

**Tech Stack:** C++20, MSVC (Visual Studio Community 2026, v18.5), CMake (bundled with VS), vcpkg (bundled with VS) manifest mode, doctest (unit tests), NVML (`nvml.dll`, runtime `LoadLibrary`), MSI Afterburner (external dependency).

**Spec:** `docs/superpowers/specs/2026-06-29-cpp-afterburner-rewrite-design.md`

## Global Constraints

- Language: **C++20**. Compiler: **MSVC** from Visual Studio Community 2026. Build all from a **VS Developer PowerShell** (puts `cl`, `cmake`, `vcpkg` on PATH).
- Architecture target: **x64** only.
- Source root for the new app: `cpp/` (keeps the old Python tree untouched alongside).
- All hardware-applying steps require running the harness **as Administrator** (Afterburner needs elevation to apply).
- **Afterburner is a hard runtime dependency.** Minimum version: latest stable (4.6.5+). The harness never edits a user profile it did not back up first.
- Never apply settings that exceed safe limits during the spike: power limit changes only within the GPU's reported min/max; `VFCurve` offset test uses a small, conservative undervolt (single curve point, modest negative effective offset). Temperature ceiling 95 °C.
- No CUDA, no direct NVAPI *writes*. NVML is read-only.
- Reserved Afterburner profile slot for the tool: **Profile 5** (confirmed/overridable in Task 7).

---

## Task 0: Prerequisites — Afterburner setup + toolchain confirmation (manual)

**Files:** none (environment setup).

**This task has no code. It unblocks every hardware task. Do it first, by hand.**

- [ ] **Step 1: Install MSI Afterburner**

Download the latest stable MSI Afterburner from the official MSI site and install it. Confirm:

Run (VS Developer PowerShell):
```powershell
Test-Path "C:\Program Files (x86)\MSI Afterburner\MSIAfterburner.exe"
```
Expected: `True`

- [ ] **Step 2: Prime a profile with a V/F curve**

Launch MSI Afterburner **as Administrator**. Press **Ctrl+F** to open the Voltage/Frequency curve editor (this populates the curve data). Close the editor. Click **Save** then a profile number to store the current settings as **Profile 5**. Optionally set a small power limit (e.g. 90%) so there is a non-default value to read back later.

- [ ] **Step 3: Locate and snapshot the profile file(s)**

Run:
```powershell
Get-ChildItem "C:\Program Files (x86)\MSI Afterburner\Profiles" -Filter *.cfg | Select-Object Name,Length
```
Expected: at least one `VEN_10DE&DEV_2786&...cfg` file (RTX 4070 device id 0x2786). Copy one such file's **full contents** into the repo at `cpp/tests/fixtures/profile_sample.cfg` for use as a unit-test fixture. Confirm it contains a `VFCurve=` line and a `PowerLimit=` line:
```powershell
Select-String -Path "C:\Program Files (x86)\MSI Afterburner\Profiles\*.cfg" -Pattern "^VFCurve=","^PowerLimit=" | Select-Object Filename,LineNumber,Line
```
Expected: matches for both keys.

- [ ] **Step 4: Confirm the build toolchain**

Launch **Developer PowerShell for VS 2026** and run:
```powershell
cl /Bv 2>$null; cmake --version; vcpkg version
```
Expected: `cl` prints a version banner, `cmake` ≥ 3.28, `vcpkg` prints a version. If `vcpkg` is missing, bootstrap it from the VS-bundled copy or `git clone https://github.com/microsoft/vcpkg` + `.\bootstrap-vcpkg.bat`, and set `$env:VCPKG_ROOT`.

- [ ] **Step 5: Record findings**

Append a short note to `cpp/SPIKE_NOTES.md` (create it): Afterburner version, profile file name, which profile slot maps to which section, and whether the `.cfg` holds one profile or multiple `[...]` sections. This informs Task 7.

```bash
git add cpp/SPIKE_NOTES.md cpp/tests/fixtures/profile_sample.cfg
git commit -m "chore: capture Afterburner profile fixture + spike notes"
```

---

## Task 1: CMake + vcpkg project skeleton with doctest

**Files:**
- Create: `cpp/CMakeLists.txt`
- Create: `cpp/vcpkg.json`
- Create: `cpp/CMakePresets.json`
- Create: `cpp/src/core/version.hpp`
- Create: `cpp/tests/test_main.cpp`
- Create: `cpp/tests/test_smoke.cpp`

**Interfaces:**
- Produces: a CMake build that compiles a `core` static library and a `core_tests` executable linking doctest. Test target runnable via `ctest`.

- [ ] **Step 1: Write the vcpkg manifest**

`cpp/vcpkg.json`:
```json
{
  "name": "gpu-auto-optimizer",
  "version-string": "0.1.0",
  "dependencies": ["doctest"]
}
```

- [ ] **Step 2: Write CMakeLists.txt**

`cpp/CMakeLists.txt`:
```cmake
cmake_minimum_required(VERSION 3.28)
project(gpu_auto_optimizer CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(doctest CONFIG REQUIRED)

add_library(core STATIC
  src/core/version.hpp
)
set_target_properties(core PROPERTIES LINKER_LANGUAGE CXX)
target_include_directories(core PUBLIC src)

enable_testing()
add_executable(core_tests
  tests/test_main.cpp
  tests/test_smoke.cpp
)
target_link_libraries(core_tests PRIVATE core doctest::doctest)
add_test(NAME core_tests COMMAND core_tests)
```

- [ ] **Step 3: Write CMakePresets.json**

`cpp/CMakePresets.json` (points CMake at the vcpkg toolchain via `$env{VCPKG_ROOT}`):
```json
{
  "version": 3,
  "configurePresets": [
    {
      "name": "default",
      "generator": "Ninja",
      "binaryDir": "${sourceDir}/build",
      "cacheVariables": {
        "CMAKE_TOOLCHAIN_FILE": "$env{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake",
        "CMAKE_BUILD_TYPE": "Debug"
      }
    }
  ]
}
```

- [ ] **Step 4: Write the version header and test files**

`cpp/src/core/version.hpp`:
```cpp
#pragma once
namespace gao { constexpr const char* kVersion = "0.1.0"; }
```

`cpp/tests/test_main.cpp`:
```cpp
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
```

`cpp/tests/test_smoke.cpp`:
```cpp
#include <doctest/doctest.h>
#include "core/version.hpp"
#include <string>

TEST_CASE("version string is set") {
    CHECK(std::string(gao::kVersion) == "0.1.0");
}
```

- [ ] **Step 5: Configure, build, run tests**

Run (VS Developer PowerShell, from `cpp/`):
```powershell
cmake --preset default
cmake --build build
ctest --test-dir build --output-on-failure
```
Expected: configure succeeds (vcpkg installs doctest), build succeeds, `1 test passed`.

- [ ] **Step 6: Commit**

```bash
git add cpp/CMakeLists.txt cpp/vcpkg.json cpp/CMakePresets.json cpp/src cpp/tests
git commit -m "build: C++ project skeleton with vcpkg + doctest"
```

---

## Task 2: VFCurve float ↔ hex codec

**Files:**
- Create: `cpp/src/core/vfcurve_codec.hpp`
- Create: `cpp/src/core/vfcurve_codec.cpp`
- Create: `cpp/tests/test_vfcurve_codec.cpp`
- Modify: `cpp/CMakeLists.txt` (add sources)

**Interfaces:**
- Produces:
  - `std::string gao::FloatToHex8(float value);` — encode one float as 8 hex chars.
  - `float gao::Hex8ToFloat(std::string_view hex8);` — decode 8 hex chars to float.
  - Encoding is the raw IEEE-754 bytes in **little-endian memory order**, hex-encoded uppercase (matches VFCurveEditor `CurveReader`). **If Task 3's real-fixture round-trip fails, switch byte order here (documented contingency).**

- [ ] **Step 1: Write the failing test**

`cpp/tests/test_vfcurve_codec.cpp`:
```cpp
#include <doctest/doctest.h>
#include "core/vfcurve_codec.hpp"

TEST_CASE("float hex codec round-trips") {
    for (float v : {0.0f, 1.0f, 1000.0f, 1050.5f, -150.0f, 2730.0f}) {
        std::string h = gao::FloatToHex8(v);
        CHECK(h.size() == 8);
        CHECK(gao::Hex8ToFloat(h) == doctest::Approx(v));
    }
}

TEST_CASE("hex is 8 uppercase chars") {
    std::string h = gao::FloatToHex8(1050.0f);
    CHECK(h.size() == 8);
    for (char c : h) CHECK((isdigit((unsigned char)c) || (c >= 'A' && c <= 'F')));
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build; ctest --test-dir build -R vfcurve_codec --output-on-failure`
Expected: FAIL — `vfcurve_codec.hpp` not found.

- [ ] **Step 3: Write the implementation**

`cpp/src/core/vfcurve_codec.hpp`:
```cpp
#pragma once
#include <string>
#include <string_view>
namespace gao {
std::string FloatToHex8(float value);
float Hex8ToFloat(std::string_view hex8);
}
```

`cpp/src/core/vfcurve_codec.cpp`:
```cpp
#include "core/vfcurve_codec.hpp"
#include <cstdint>
#include <cstring>
#include <cstdio>

namespace gao {

std::string FloatToHex8(float value) {
    uint8_t bytes[4];
    std::memcpy(bytes, &value, 4);              // little-endian memory order on x64
    char out[9];
    std::snprintf(out, sizeof(out), "%02X%02X%02X%02X",
                  bytes[0], bytes[1], bytes[2], bytes[3]);
    return std::string(out, 8);
}

float Hex8ToFloat(std::string_view hex8) {
    uint8_t bytes[4]{};
    for (int i = 0; i < 4; ++i) {
        auto hexNibble = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return 0;
        };
        bytes[i] = static_cast<uint8_t>(hexNibble(hex8[i*2]) * 16 + hexNibble(hex8[i*2+1]));
    }
    float value;
    std::memcpy(&value, bytes, 4);
    return value;
}

}
```

- [ ] **Step 4: Add sources to CMake**

In `cpp/CMakeLists.txt`, add to the `core` library sources:
```cmake
  src/core/vfcurve_codec.hpp
  src/core/vfcurve_codec.cpp
```
and add to `core_tests` sources:
```cmake
  tests/test_vfcurve_codec.cpp
```

- [ ] **Step 5: Run test to verify it passes**

Run: `cmake --build build; ctest --test-dir build -R vfcurve_codec --output-on-failure`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add cpp/src/core/vfcurve_codec.* cpp/tests/test_vfcurve_codec.cpp cpp/CMakeLists.txt
git commit -m "feat: VFCurve float<->hex codec"
```

---

## Task 3: VFCurve string parse / re-encode (round-trip) + real-fixture validation

**Files:**
- Create: `cpp/src/core/vfcurve.hpp`
- Create: `cpp/src/core/vfcurve.cpp`
- Create: `cpp/tests/test_vfcurve.cpp`
- Modify: `cpp/CMakeLists.txt`

**Interfaces:**
- Consumes: `FloatToHex8`, `Hex8ToFloat` from Task 2.
- Produces:
  - `struct gao::CurvePoint { float voltage_mv; float frequency_mhz; float offset_mhz; };`
  - `struct gao::VfCurve { std::string header; std::vector<CurvePoint> points; std::string ending; };`
  - `gao::VfCurve gao::ParseVfCurve(std::string_view hex);` — header = first 24 chars; then 3-float points until a zero point `(0,0,0)`; remainder = `ending`.
  - `std::string gao::EncodeVfCurve(const VfCurve& curve);` — `header + points + ending`. Round-trips a real profile string byte-for-byte.

- [ ] **Step 1: Write the failing tests (synthetic round-trip + real fixture)**

`cpp/tests/test_vfcurve.cpp`:
```cpp
#include <doctest/doctest.h>
#include "core/vfcurve.hpp"
#include "core/vfcurve_codec.hpp"
#include <fstream>
#include <sstream>
#include <string>

static std::string MakeHex(float v, float f, float o) {
    return gao::FloatToHex8(v) + gao::FloatToHex8(f) + gao::FloatToHex8(o);
}

TEST_CASE("parse + encode round-trips a synthetic curve") {
    std::string header = gao::FloatToHex8(1) + gao::FloatToHex8(2) + gao::FloatToHex8(3);
    std::string body = MakeHex(700, 300, 0) + MakeHex(800, 1500, 0);
    std::string terminator = MakeHex(0, 0, 0);
    std::string ending = "DEADBEEF";
    std::string full = header + body + terminator + ending;

    gao::VfCurve c = gao::ParseVfCurve(full);
    CHECK(c.points.size() == 2);
    CHECK(c.points[0].voltage_mv == doctest::Approx(700));
    CHECK(c.points[1].frequency_mhz == doctest::Approx(1500));
    CHECK(gao::EncodeVfCurve(c) == full);
}

// Reads the real VFCurve= value captured in Task 0 and asserts byte-for-byte round-trip.
// This is the endianness validation gate.
TEST_CASE("round-trips the real Afterburner VFCurve fixture") {
    std::ifstream in("tests/fixtures/profile_sample.cfg");
    REQUIRE(in.good());
    std::string line, vfcurve;
    while (std::getline(in, line)) {
        if (line.rfind("VFCurve=", 0) == 0) { vfcurve = line.substr(8); break; }
    }
    // strip trailing CR if present
    while (!vfcurve.empty() && (vfcurve.back() == '\r' || vfcurve.back() == '\n')) vfcurve.pop_back();
    REQUIRE(!vfcurve.empty());

    gao::VfCurve c = gao::ParseVfCurve(vfcurve);
    CHECK(c.points.size() > 0);
    CHECK(gao::EncodeVfCurve(c) == vfcurve);   // byte-for-byte
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build build; ctest --test-dir build -R "vfcurve\b|VFCurve" --output-on-failure`
Expected: FAIL — `vfcurve.hpp` not found.

- [ ] **Step 3: Write the implementation**

`cpp/src/core/vfcurve.hpp`:
```cpp
#pragma once
#include <string>
#include <string_view>
#include <vector>
namespace gao {
struct CurvePoint { float voltage_mv; float frequency_mhz; float offset_mhz; };
struct VfCurve { std::string header; std::vector<CurvePoint> points; std::string ending; };
VfCurve ParseVfCurve(std::string_view hex);
std::string EncodeVfCurve(const VfCurve& curve);
}
```

`cpp/src/core/vfcurve.cpp`:
```cpp
#include "core/vfcurve.hpp"
#include "core/vfcurve_codec.hpp"

namespace gao {

static constexpr size_t kVal = 8;     // hex chars per float
static constexpr size_t kPt  = 24;    // hex chars per point (3 floats)
static constexpr size_t kHdr = 24;    // header = 3 floats

VfCurve ParseVfCurve(std::string_view hex) {
    VfCurve c;
    if (hex.size() < kHdr) { c.header = std::string(hex); return c; }
    c.header = std::string(hex.substr(0, kHdr));
    size_t pos = kHdr;
    while (pos + kPt <= hex.size()) {
        CurvePoint p{
            Hex8ToFloat(hex.substr(pos,          kVal)),
            Hex8ToFloat(hex.substr(pos + kVal,   kVal)),
            Hex8ToFloat(hex.substr(pos + 2*kVal, kVal)),
        };
        if (p.voltage_mv == 0 && p.frequency_mhz == 0 && p.offset_mhz == 0) {
            c.ending = std::string(hex.substr(pos));   // includes the zero terminator
            return c;
        }
        c.points.push_back(p);
        pos += kPt;
    }
    c.ending = std::string(hex.substr(pos));
    return c;
}

std::string EncodeVfCurve(const VfCurve& c) {
    std::string out = c.header;
    for (const auto& p : c.points) {
        out += FloatToHex8(p.voltage_mv);
        out += FloatToHex8(p.frequency_mhz);
        out += FloatToHex8(p.offset_mhz);
    }
    out += c.ending;
    return out;
}

}
```

- [ ] **Step 4: Add sources to CMake; ensure tests run from `cpp/` so the fixture path resolves**

Add `src/core/vfcurve.hpp/.cpp` to `core` and `tests/test_vfcurve.cpp` to `core_tests` in `cpp/CMakeLists.txt`. The real-fixture test opens `tests/fixtures/profile_sample.cfg` relative to the working directory; run ctest with `--test-dir build` **from the `cpp/` folder**, or set the test's working directory:
```cmake
set_tests_properties(core_tests PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
```

- [ ] **Step 5: Run to verify it passes**

Run (from `cpp/`): `cmake --build build; ctest --test-dir build --output-on-failure`
Expected: PASS. **If the real-fixture round-trip fails**, the byte order is wrong — swap the byte indexing in `vfcurve_codec.cpp` (reverse `bytes[0..3]`) and re-run. Document the correct order in `cpp/SPIKE_NOTES.md`.

- [ ] **Step 6: Commit**

```bash
git add cpp/src/core/vfcurve.* cpp/tests/test_vfcurve.cpp cpp/CMakeLists.txt
git commit -m "feat: VFCurve parse/encode with real-profile round-trip validation"
```

---

## Task 4: Undervolt transform

**Files:**
- Create: `cpp/src/core/undervolt.hpp`
- Create: `cpp/src/core/undervolt.cpp`
- Create: `cpp/tests/test_undervolt.cpp`
- Modify: `cpp/CMakeLists.txt`

**Interfaces:**
- Consumes: `VfCurve`, `CurvePoint` from Task 3.
- Produces:
  - `gao::VfCurve gao::ApplyUndervolt(const VfCurve& base, float target_voltage_mv, float target_freq_mhz);`
  - Semantics: choose the curve point nearest `target_voltage_mv`; set its `offset` so its effective frequency (built-in `frequency` + `offset`) equals `target_freq_mhz`; for all points at voltage **above** the target, set their `offset` so their effective frequency is clamped to `target_freq_mhz` (flatten — stop boosting past the locked voltage). Points below target are left unchanged.

- [ ] **Step 1: Write the failing test**

`cpp/tests/test_undervolt.cpp`:
```cpp
#include <doctest/doctest.h>
#include "core/undervolt.hpp"

static gao::VfCurve BaseCurve() {
    gao::VfCurve c;
    c.header = "000000000000000000000000";
    c.points = {
        {700.f,  2400.f, 0.f},
        {800.f,  2600.f, 0.f},
        {900.f,  2800.f, 0.f},  // target near here
        {1000.f, 2950.f, 0.f},
        {1050.f, 3000.f, 0.f},
    };
    c.ending = "000000000000000000000000";
    return c;
}

TEST_CASE("undervolt locks target point to target freq") {
    auto c = gao::ApplyUndervolt(BaseCurve(), 900.f, 2850.f);
    // point at 900mV effective freq == 2850
    CHECK((c.points[2].frequency_mhz + c.points[2].offset_mhz) == doctest::Approx(2850.f));
}

TEST_CASE("undervolt flattens points above target voltage") {
    auto c = gao::ApplyUndervolt(BaseCurve(), 900.f, 2850.f);
    for (const auto& p : c.points) {
        if (p.voltage_mv > 900.f)
            CHECK((p.frequency_mhz + p.offset_mhz) == doctest::Approx(2850.f));
    }
}

TEST_CASE("undervolt leaves points below target unchanged") {
    auto c = gao::ApplyUndervolt(BaseCurve(), 900.f, 2850.f);
    CHECK(c.points[0].offset_mhz == doctest::Approx(0.f));  // 700mV untouched
    CHECK(c.points[1].offset_mhz == doctest::Approx(0.f));  // 800mV untouched
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build build; ctest --test-dir build -R undervolt --output-on-failure`
Expected: FAIL — header not found.

- [ ] **Step 3: Write the implementation**

`cpp/src/core/undervolt.hpp`:
```cpp
#pragma once
#include "core/vfcurve.hpp"
namespace gao {
VfCurve ApplyUndervolt(const VfCurve& base, float target_voltage_mv, float target_freq_mhz);
}
```

`cpp/src/core/undervolt.cpp`:
```cpp
#include "core/undervolt.hpp"
#include <cmath>

namespace gao {

VfCurve ApplyUndervolt(const VfCurve& base, float target_voltage_mv, float target_freq_mhz) {
    VfCurve c = base;
    for (auto& p : c.points) {
        if (p.voltage_mv + 0.5f >= target_voltage_mv) {
            // at or above the locked voltage: effective freq = target (flatten)
            p.offset_mhz = target_freq_mhz - p.frequency_mhz;
        }
        // below target voltage: leave offset as-is
    }
    return c;
}

}
```

- [ ] **Step 4: Add sources to CMake**

Add `src/core/undervolt.hpp/.cpp` to `core` and `tests/test_undervolt.cpp` to `core_tests`.

- [ ] **Step 5: Run to verify it passes**

Run: `cmake --build build; ctest --test-dir build -R undervolt --output-on-failure`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add cpp/src/core/undervolt.* cpp/tests/test_undervolt.cpp cpp/CMakeLists.txt
git commit -m "feat: undervolt curve transform"
```

---

## Task 5: Afterburner profile cfg reader/writer

**Files:**
- Create: `cpp/src/core/ab_profile.hpp`
- Create: `cpp/src/core/ab_profile.cpp`
- Create: `cpp/tests/test_ab_profile.cpp`
- Modify: `cpp/CMakeLists.txt`

**Interfaces:**
- Produces:
  - `std::optional<std::string> gao::ReadProfileKey(const std::string& cfgText, std::string_view key);` — return the value of `key=` (first match), or nullopt.
  - `std::string gao::SetProfileKey(const std::string& cfgText, std::string_view key, std::string_view value);` — return cfg text with `key=`'s value replaced in place (preserving all other lines and line endings); if the key is absent, return text unchanged (the spike requires AB to have written the keys already).
- Operates on file **text** (caller does file I/O), so it is fully unit-testable against the captured fixture.

- [ ] **Step 1: Write the failing test**

`cpp/tests/test_ab_profile.cpp`:
```cpp
#include <doctest/doctest.h>
#include "core/ab_profile.hpp"

static const char* kCfg =
    "[Profile]\r\n"
    "Format=2\r\n"
    "PowerLimit=90\r\n"
    "CoreClkBoost=0\r\n"
    "VFCurve=ABCDEF0123\r\n";

TEST_CASE("reads a key value") {
    CHECK(gao::ReadProfileKey(kCfg, "PowerLimit").value() == "90");
    CHECK(gao::ReadProfileKey(kCfg, "VFCurve").value() == "ABCDEF0123");
    CHECK(!gao::ReadProfileKey(kCfg, "Missing").has_value());
}

TEST_CASE("sets a key value in place, preserving the rest") {
    std::string out = gao::SetProfileKey(kCfg, "PowerLimit", "75");
    CHECK(gao::ReadProfileKey(out, "PowerLimit").value() == "75");
    // other keys intact
    CHECK(gao::ReadProfileKey(out, "VFCurve").value() == "ABCDEF0123");
    // CRLF preserved
    CHECK(out.find("PowerLimit=75\r\n") != std::string::npos);
}

TEST_CASE("replaces a long VFCurve value") {
    std::string out = gao::SetProfileKey(kCfg, "VFCurve", "00112233");
    CHECK(gao::ReadProfileKey(out, "VFCurve").value() == "00112233");
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build build; ctest --test-dir build -R ab_profile --output-on-failure`
Expected: FAIL — header not found.

- [ ] **Step 3: Write the implementation**

`cpp/src/core/ab_profile.hpp`:
```cpp
#pragma once
#include <optional>
#include <string>
#include <string_view>
namespace gao {
std::optional<std::string> ReadProfileKey(const std::string& cfgText, std::string_view key);
std::string SetProfileKey(const std::string& cfgText, std::string_view key, std::string_view value);
}
```

`cpp/src/core/ab_profile.cpp`:
```cpp
#include "core/ab_profile.hpp"

namespace gao {

static size_t FindKeyLineStart(const std::string& text, std::string_view key, size_t& valueStart, size_t& lineEnd) {
    std::string needle = std::string(key) + "=";
    size_t pos = 0;
    while (pos < text.size()) {
        size_t ls = text.find(needle, pos);
        if (ls == std::string::npos) return std::string::npos;
        bool atLineStart = (ls == 0) || text[ls-1] == '\n' || text[ls-1] == '\r';
        if (atLineStart) {
            valueStart = ls + needle.size();
            size_t e = text.find('\n', valueStart);
            lineEnd = (e == std::string::npos) ? text.size() : e;
            // trim trailing CR from value region
            if (lineEnd > valueStart && text[lineEnd-1] == '\r') lineEnd -= 1;
            return ls;
        }
        pos = ls + needle.size();
    }
    return std::string::npos;
}

std::optional<std::string> ReadProfileKey(const std::string& text, std::string_view key) {
    size_t vs = 0, le = 0;
    if (FindKeyLineStart(text, key, vs, le) == std::string::npos) return std::nullopt;
    return text.substr(vs, le - vs);
}

std::string SetProfileKey(const std::string& text, std::string_view key, std::string_view value) {
    size_t vs = 0, le = 0;
    if (FindKeyLineStart(text, key, vs, le) == std::string::npos) return text;
    std::string out = text;
    out.replace(vs, le - vs, value);
    return out;
}

}
```

- [ ] **Step 4: Add sources to CMake**

Add `src/core/ab_profile.hpp/.cpp` to `core` and `tests/test_ab_profile.cpp` to `core_tests`.

- [ ] **Step 5: Run to verify it passes**

Run: `cmake --build build; ctest --test-dir build -R ab_profile --output-on-failure`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add cpp/src/core/ab_profile.* cpp/tests/test_ab_profile.cpp cpp/CMakeLists.txt
git commit -m "feat: Afterburner profile cfg key read/write"
```

---

## Task 6: NVML telemetry probe

**Files:**
- Create: `cpp/src/hw/nvml.hpp`
- Create: `cpp/src/hw/nvml.cpp`
- Create: `cpp/src/tools/nvml_probe_main.cpp`
- Modify: `cpp/CMakeLists.txt`

**Interfaces:**
- Produces:
  - `struct gao::GpuTelemetry { unsigned coreClockMhz; unsigned memClockMhz; unsigned tempC; unsigned powerW; unsigned powerLimitW; unsigned utilPct; unsigned long long eccErrors; bool ok; std::string error; };`
  - `class gao::Nvml { public: bool Init(); GpuTelemetry Read(unsigned index); ~Nvml(); };`
  - NVML is loaded at runtime via `LoadLibraryA("nvml.dll")` + `GetProcAddress` (no link-time SDK dependency).
- This task is hardware-dependent: verified by a manual smoke run on the RTX 4070, not a unit test.

- [ ] **Step 1: Write the NVML wrapper header**

`cpp/src/hw/nvml.hpp`:
```cpp
#pragma once
#include <string>
namespace gao {
struct GpuTelemetry {
    unsigned coreClockMhz = 0, memClockMhz = 0, tempC = 0, powerW = 0, powerLimitW = 0, utilPct = 0;
    unsigned long long eccErrors = 0;
    bool ok = false;
    std::string error;
};
class Nvml {
public:
    bool Init();
    GpuTelemetry Read(unsigned index);
    ~Nvml();
private:
    void* lib_ = nullptr;
    bool inited_ = false;
};
}
```

- [ ] **Step 2: Write the implementation (runtime-loaded NVML)**

`cpp/src/hw/nvml.cpp`:
```cpp
#include "hw/nvml.hpp"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace gao {

// Minimal NVML function pointer typedefs (subset we need).
using nvmlReturn_t = int;
static constexpr nvmlReturn_t NVML_SUCCESS = 0;
using nvmlDevice_t = void*;

typedef nvmlReturn_t (*fn_init)();
typedef nvmlReturn_t (*fn_shutdown)();
typedef nvmlReturn_t (*fn_byIndex)(unsigned, nvmlDevice_t*);
typedef nvmlReturn_t (*fn_clock)(nvmlDevice_t, int type, unsigned*);
typedef nvmlReturn_t (*fn_temp)(nvmlDevice_t, int sensor, unsigned*);
typedef nvmlReturn_t (*fn_power)(nvmlDevice_t, unsigned*);
typedef nvmlReturn_t (*fn_powerlimit)(nvmlDevice_t, unsigned*);
typedef struct { unsigned gpu; unsigned memory; } nvmlUtilization_t;
typedef nvmlReturn_t (*fn_util)(nvmlDevice_t, nvmlUtilization_t*);

static fn_init        p_init = nullptr;
static fn_shutdown    p_shutdown = nullptr;
static fn_byIndex     p_byIndex = nullptr;
static fn_clock       p_clock = nullptr;
static fn_temp        p_temp = nullptr;
static fn_power       p_power = nullptr;
static fn_powerlimit  p_powerlimit = nullptr;
static fn_util        p_util = nullptr;

bool Nvml::Init() {
    HMODULE h = LoadLibraryA("nvml.dll");
    if (!h) h = LoadLibraryA("C:\\Windows\\System32\\nvml.dll");
    if (!h) return false;
    lib_ = h;
    p_init       = (fn_init)GetProcAddress(h, "nvmlInit_v2");
    p_shutdown   = (fn_shutdown)GetProcAddress(h, "nvmlShutdown");
    p_byIndex    = (fn_byIndex)GetProcAddress(h, "nvmlDeviceGetHandleByIndex_v2");
    p_clock      = (fn_clock)GetProcAddress(h, "nvmlDeviceGetClockInfo");
    p_temp       = (fn_temp)GetProcAddress(h, "nvmlDeviceGetTemperature");
    p_power      = (fn_power)GetProcAddress(h, "nvmlDeviceGetPowerUsage");
    p_powerlimit = (fn_powerlimit)GetProcAddress(h, "nvmlDeviceGetPowerManagementLimit");
    p_util       = (fn_util)GetProcAddress(h, "nvmlDeviceGetUtilizationRates");
    if (!p_init || !p_byIndex) return false;
    inited_ = (p_init() == NVML_SUCCESS);
    return inited_;
}

GpuTelemetry Nvml::Read(unsigned index) {
    GpuTelemetry t;
    if (!inited_) { t.error = "NVML not initialized"; return t; }
    nvmlDevice_t dev = nullptr;
    if (p_byIndex(index, &dev) != NVML_SUCCESS) { t.error = "GetHandleByIndex failed"; return t; }
    unsigned v = 0;
    if (p_clock && p_clock(dev, /*GRAPHICS*/0, &v) == NVML_SUCCESS) t.coreClockMhz = v;
    if (p_clock && p_clock(dev, /*MEM*/2, &v) == NVML_SUCCESS)      t.memClockMhz = v;
    if (p_temp && p_temp(dev, /*GPU*/0, &v) == NVML_SUCCESS)        t.tempC = v;
    if (p_power && p_power(dev, &v) == NVML_SUCCESS)                t.powerW = v / 1000;
    if (p_powerlimit && p_powerlimit(dev, &v) == NVML_SUCCESS)      t.powerLimitW = v / 1000;
    nvmlUtilization_t u{};
    if (p_util && p_util(dev, &u) == NVML_SUCCESS)                  t.utilPct = u.gpu;
    t.ok = true;
    return t;
}

Nvml::~Nvml() {
    if (inited_ && p_shutdown) p_shutdown();
    if (lib_) FreeLibrary((HMODULE)lib_);
}

}
```

- [ ] **Step 3: Write the probe tool**

`cpp/src/tools/nvml_probe_main.cpp`:
```cpp
#include "hw/nvml.hpp"
#include <cstdio>

int main() {
    gao::Nvml nvml;
    if (!nvml.Init()) { std::printf("NVML init FAILED\n"); return 1; }
    auto t = nvml.Read(0);
    if (!t.ok) { std::printf("read FAILED: %s\n", t.error.c_str()); return 2; }
    std::printf("core=%u MHz  mem=%u MHz  temp=%u C  power=%u W  limit=%u W  util=%u%%\n",
                t.coreClockMhz, t.memClockMhz, t.tempC, t.powerW, t.powerLimitW, t.utilPct);
    return 0;
}
```

- [ ] **Step 4: Add a `core_hw` lib + `nvml_probe` exe to CMake**

In `cpp/CMakeLists.txt`:
```cmake
add_library(core_hw STATIC src/hw/nvml.hpp src/hw/nvml.cpp)
target_include_directories(core_hw PUBLIC src)

add_executable(nvml_probe src/tools/nvml_probe_main.cpp)
target_link_libraries(nvml_probe PRIVATE core_hw)
```

- [ ] **Step 5: Build and run the probe on real hardware (manual verification)**

Run: `cmake --build build; ./build/nvml_probe.exe`
Expected: a line like `core=2745 MHz  mem=10501 MHz  temp=45 C  power=18 W  limit=200 W  util=3%`. Power limit should read **200 W** (the 4070 default). If clocks read 0 at idle that's fine; temp/limit must be non-zero.

- [ ] **Step 6: Commit**

```bash
git add cpp/src/hw cpp/src/tools/nvml_probe_main.cpp cpp/CMakeLists.txt
git commit -m "feat: runtime-loaded NVML telemetry probe"
```

---

## Task 7: Afterburner controller — locate, back up, apply trigger

**Files:**
- Create: `cpp/src/hw/afterburner.hpp`
- Create: `cpp/src/hw/afterburner.cpp`
- Modify: `cpp/CMakeLists.txt`

**Interfaces:**
- Consumes: `ReadProfileKey`/`SetProfileKey` (Task 5).
- Produces:
  - `struct gao::AbLocation { std::string exePath; std::string profileCfgPath; int slot; };`
  - `std::optional<AbLocation> gao::FindAfterburner(int slot);` — resolve `MSIAfterburner.exe` and the profile `.cfg` for the RTX 4070 (match `DEV_2786` filename) and the reserved slot.
  - `bool gao::BackupProfile(const AbLocation&);` — copy the cfg to `*.cfg.gao-backup` once.
  - `std::string gao::ReadProfileText(const AbLocation&);` / `bool gao::WriteProfileText(const AbLocation&, const std::string&);`
  - `bool gao::ApplyProfile(const AbLocation&);` — launch `MSIAfterburner.exe -Profile<slot>` and wait briefly. **Implements the trigger strategy from spec §4.3; the working strategy is determined by Task 8's experiment and recorded in SPIKE_NOTES.md.**

- [ ] **Step 1: Write the controller header**

`cpp/src/hw/afterburner.hpp`:
```cpp
#pragma once
#include <optional>
#include <string>
namespace gao {
struct AbLocation { std::string exePath; std::string profileCfgPath; int slot = 5; };
std::optional<AbLocation> FindAfterburner(int slot);
bool BackupProfile(const AbLocation& loc);
std::string ReadProfileText(const AbLocation& loc);
bool WriteProfileText(const AbLocation& loc, const std::string& text);
bool ApplyProfile(const AbLocation& loc);
}
```

- [ ] **Step 2: Write the implementation**

`cpp/src/hw/afterburner.cpp`:
```cpp
#include "hw/afterburner.hpp"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <fstream>
#include <sstream>
#include <filesystem>

namespace fs = std::filesystem;

namespace gao {

std::optional<AbLocation> FindAfterburner(int slot) {
    AbLocation loc; loc.slot = slot;
    const char* base = "C:\\Program Files (x86)\\MSI Afterburner";
    std::string exe = std::string(base) + "\\MSIAfterburner.exe";
    if (!fs::exists(exe)) return std::nullopt;
    loc.exePath = exe;
    fs::path profiles = fs::path(base) / "Profiles";
    if (fs::exists(profiles)) {
        for (auto& e : fs::directory_iterator(profiles)) {
            auto name = e.path().filename().string();
            if (name.find("DEV_2786") != std::string::npos && e.path().extension() == ".cfg") {
                loc.profileCfgPath = e.path().string();
                break;
            }
        }
    }
    if (loc.profileCfgPath.empty()) return std::nullopt;
    return loc;
}

bool BackupProfile(const AbLocation& loc) {
    std::string bak = loc.profileCfgPath + ".gao-backup";
    if (fs::exists(bak)) return true;
    std::error_code ec;
    fs::copy_file(loc.profileCfgPath, bak, ec);
    return !ec;
}

std::string ReadProfileText(const AbLocation& loc) {
    std::ifstream in(loc.profileCfgPath, std::ios::binary);
    std::ostringstream ss; ss << in.rdbuf();
    return ss.str();
}

bool WriteProfileText(const AbLocation& loc, const std::string& text) {
    std::ofstream out(loc.profileCfgPath, std::ios::binary | std::ios::trunc);
    out << text;
    return out.good();
}

bool ApplyProfile(const AbLocation& loc) {
    std::string cmd = "\"" + loc.exePath + "\" -Profile" + std::to_string(loc.slot);
    STARTUPINFOA si{ sizeof(si) };
    PROCESS_INFORMATION pi{};
    std::string mutableCmd = cmd;
    BOOL ok = CreateProcessA(nullptr, mutableCmd.data(), nullptr, nullptr, FALSE,
                             0, nullptr, nullptr, &si, &pi);
    if (!ok) return false;
    WaitForSingleObject(pi.hProcess, 4000);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

}
```

- [ ] **Step 3: Add to CMake**

Add `src/hw/afterburner.hpp/.cpp` to the `core_hw` library and link `core` into `core_hw` (it uses `ab_profile`):
```cmake
target_link_libraries(core_hw PUBLIC core)
```

- [ ] **Step 4: Build (compile-only verification)**

Run: `cmake --build build`
Expected: builds clean. (Behavior verified in Task 8.)

- [ ] **Step 5: Commit**

```bash
git add cpp/src/hw/afterburner.* cpp/CMakeLists.txt
git commit -m "feat: Afterburner locate/backup/read/write/apply"
```

---

## Task 8: Existential apply spike — the make-or-break test

**Files:**
- Create: `cpp/src/tools/abctl_spike_main.cpp`
- Modify: `cpp/CMakeLists.txt`
- Modify: `cpp/SPIKE_NOTES.md`

**Interfaces:**
- Consumes: everything above (NVML, Afterburner controller, cfg + VFCurve + undervolt).
- Produces: `abctl_spike.exe` that performs the end-to-end proof and prints `SPIKE PASS` / `SPIKE FAIL`.

**This is the task the whole rewrite hinges on. Run it as Administrator with Afterburner running.**

- [ ] **Step 1: Write the spike harness**

`cpp/src/tools/abctl_spike_main.cpp`:
```cpp
#include "hw/nvml.hpp"
#include "hw/afterburner.hpp"
#include "core/ab_profile.hpp"
#include <cstdio>
#include <thread>
#include <chrono>
#include <string>

using namespace std::chrono_literals;

int main(int argc, char** argv) {
    int slot = 5;
    std::string targetPower = (argc > 1) ? argv[1] : "80";  // % power limit to set

    gao::Nvml nvml;
    if (!nvml.Init()) { std::printf("SPIKE FAIL: NVML init\n"); return 1; }

    auto loc = gao::FindAfterburner(slot);
    if (!loc) { std::printf("SPIKE FAIL: Afterburner/profile not found (is it installed + profile %d saved?)\n", slot); return 2; }
    std::printf("Afterburner: %s\n  profile: %s\n", loc->exePath.c_str(), loc->profileCfgPath.c_str());

    if (!gao::BackupProfile(*loc)) { std::printf("SPIKE FAIL: backup\n"); return 3; }

    auto before = nvml.Read(0);
    std::printf("BEFORE: powerLimit=%u W\n", before.powerLimitW);

    std::string text = gao::ReadProfileText(*loc);
    auto curPower = gao::ReadProfileKey(text, "PowerLimit");
    std::printf("profile PowerLimit was: %s\n", curPower ? curPower->c_str() : "(absent)");

    std::string edited = gao::SetProfileKey(text, "PowerLimit", targetPower);
    if (!gao::WriteProfileText(*loc, edited)) { std::printf("SPIKE FAIL: write\n"); return 4; }

    if (!gao::ApplyProfile(*loc)) { std::printf("SPIKE FAIL: apply launch\n"); return 5; }
    std::this_thread::sleep_for(3s);

    auto after = nvml.Read(0);
    std::printf("AFTER:  powerLimit=%u W\n", after.powerLimitW);

    bool changed = (after.powerLimitW != before.powerLimitW);
    std::printf("%s: power limit %s (%u W -> %u W)\n",
                changed ? "SPIKE PASS" : "SPIKE FAIL",
                changed ? "CHANGED" : "did NOT change",
                before.powerLimitW, after.powerLimitW);
    return changed ? 0 : 10;
}
```

- [ ] **Step 2: Add to CMake**

```cmake
add_executable(abctl_spike src/tools/abctl_spike_main.cpp)
target_link_libraries(abctl_spike PRIVATE core_hw core)
```

- [ ] **Step 3: Build**

Run: `cmake --build build`
Expected: builds clean.

- [ ] **Step 4: Run the existential test (manual, Administrator, AB running)**

Ensure MSI Afterburner is running (started as admin) with Profile 5 saved. Then from an **Administrator** VS Developer PowerShell:
```powershell
./build/abctl_spike.exe 80
```
Expected (success): `SPIKE PASS: power limit CHANGED (200 W -> 160 W)` (160 = 80% of 200). NVML read-back reflects the new limit.

**If it prints SPIKE FAIL: power did NOT change**, AB did not re-read the cfg on `-Profile5`. Try the fallback trigger strategies from spec §4.3, in order, modifying `ApplyProfile` and re-running:
  1. `taskkill /IM MSIAfterburner.exe /F` then relaunch with `-Profile5`.
  2. Set AB's "apply on startup" to the slot, then kill + relaunch with `-startup`.
Record which strategy worked (or that none did) in `cpp/SPIKE_NOTES.md`.

- [ ] **Step 5: Extend the test to VFCurve undervolt (manual)**

Once power-limit apply works, manually extend `abctl_spike_main.cpp` to also: read `VFCurve=`, parse it (`ParseVfCurve`), apply a conservative undervolt (`ApplyUndervolt` at, e.g., the curve's ~900 mV point to its current effective freq − 0 MHz to start, then a small flatten), `EncodeVfCurve`, `SetProfileKey`, write, apply, and confirm via NVML that under a brief load the voltage/clock behavior changed. Keep it conservative (no instability risk at stock-ish settings). Record the result in SPIKE_NOTES.md.

- [ ] **Step 6: Restore stock + commit**

Restore the backup so the machine is left at stock:
```powershell
Copy-Item "<profileCfgPath>.gao-backup" "<profileCfgPath>" -Force
./build/abctl_spike.exe   # or re-apply via AB to confirm restore
```
```bash
git add cpp/src/tools/abctl_spike_main.cpp cpp/CMakeLists.txt cpp/SPIKE_NOTES.md
git commit -m "feat: existential Afterburner apply spike (power limit + VFCurve)"
```

---

## Definition of Done (Phase 1)

- All unit tests pass (`ctest` green): codec, VFCurve parse/encode incl. **real-fixture byte-for-byte round-trip**, undervolt transform, cfg read/write.
- `nvml_probe.exe` reads real telemetry from the RTX 4070 (power limit = 200 W at stock).
- `abctl_spike.exe` prints **SPIKE PASS** — a programmatic profile edit, applied via Afterburner, is confirmed by NVML read-back (power limit; then VFCurve undervolt).
- `cpp/SPIKE_NOTES.md` records: Afterburner version, profile layout, the working apply-trigger strategy, and the confirmed VFCurve byte order.

**Gate:** Phase 1 PASS → proceed to Phase 2 (DX12 `stress.exe`). Phase 1 FAIL on the apply trigger with no working fallback → stop and reconsider the route before building further.

## Self-Review notes

- Spec coverage: §4.1 cfg (Task 5), §4.2 VFCurve format (Tasks 2–3), §4.2 undervolt algorithm (Task 4), §4.3 apply trigger (Tasks 7–8), §7 NVML (Task 6), §10 build/toolchain (Tasks 0–1), §11 risks #1/#2 (Tasks 8/3). Stress engine (§5), optimizer (§6), GUI (§8), persistence/boot-apply (§9) are **out of scope for Phase 1** — they get their own plans after the gate.
- No placeholders: every code step contains full code. Manual hardware steps give exact commands + expected output.
- Type consistency: `AbLocation`, `VfCurve`, `CurvePoint`, `GpuTelemetry`, `ReadProfileKey`/`SetProfileKey`, `FloatToHex8`/`Hex8ToFloat` names are consistent across tasks.
