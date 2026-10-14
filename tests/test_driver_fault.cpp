#include "doctest/doctest.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "app/guarded_gpu.hpp"
#include "hw/nvapi.hpp"
#include "hw/nvml.hpp"
#include <memory>
#include <string>
#include <vector>

namespace {

// Stands in for nvml.dll, with the one thing of it that matters here: like the
// real library it counts the calls that are inside it, and its shutdown waits
// until there are none.
int in_flight = 0;
int shutdowns = 0;
int hung_shutdowns = 0;

int fake_init() { return 0; }
int fake_shutdown() {
    ++shutdowns;
    if (in_flight != 0) ++hung_shutdowns;   // the real one never returns from here
    return 0;
}

// A call that enters the library and leaves it through an access violation,
// as a call into nvml.dll can during a driver reset.
void faulting_call() {
    ++in_flight;
    const ULONG_PTR read_of[2] = {0, 0x18};
    RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 2, read_of);
    --in_flight;
}

std::vector<std::string> fault_lines;

int shut_down_one() {
    auto nvml = std::make_unique<gao::Nvml>();
    REQUIRE(nvml->Init(fake_init, fake_shutdown));
    nvml.reset();
    return shutdowns;
}

}

#include "hw/nvapi.hpp"
#include <cstring>
#include <vector>

namespace {
// The thermal policy information as the reference RTX 4070 answers it (driver
// 617.42): 88 bytes, one policy, its target settable from 65 to 88 C with 84 C
// as the default, each as a number of 1/256 C.
std::vector<unsigned char> thermal_info(int count, int min_c, int default_c, int max_c) {
    const int words[] = {88 | (1 << 16), 0x100 | count, 1, 0, min_c * 256, default_c * 256, max_c * 256};
    std::vector<unsigned char> buf(88, 0);
    std::memcpy(buf.data(), words, sizeof(words));
    return buf;
}
}

TEST_CASE("the default of the card's temperature target is read from its thermal policy") {
    const auto info = thermal_info(1, 65, 84, 88);
    CHECK(gao::parse_thermal_policy_default(info.data(), info.size()) == 84);
    const auto ti = thermal_info(1, 65, 83, 91);   // an RTX 3080 Ti, as MSI Afterburner shows its range
    CHECK(gao::parse_thermal_policy_default(ti.data(), ti.size()) == 83);
}

TEST_CASE("a thermal policy that makes no sense gives no default") {
    auto none = thermal_info(0, 65, 84, 88);   // the card has no such policy
    CHECK_FALSE(gao::parse_thermal_policy_default(none.data(), none.size()));
    auto zero = thermal_info(1, 0, 0, 0);
    CHECK_FALSE(gao::parse_thermal_policy_default(zero.data(), zero.size()));
    auto order = thermal_info(1, 65, 95, 88);   // a default outside its own range
    CHECK_FALSE(gao::parse_thermal_policy_default(order.data(), order.size()));
    auto hot = thermal_info(1, 65, 200, 250);
    CHECK_FALSE(gao::parse_thermal_policy_default(hot.data(), hot.size()));
    const auto info = thermal_info(1, 65, 84, 88);
    CHECK_FALSE(gao::parse_thermal_policy_default(info.data(), 24));   // a buffer too short to hold it
    CHECK_FALSE(gao::parse_thermal_policy_default(nullptr, 0));
}

// The card's own temperature target, as nvmlDeviceGetTemperatureThreshold gives it.
namespace target_fake {
int target = 84;       // what the driver holds for NVML_TEMPERATURE_THRESHOLD_ACOUSTIC_CURR
int rc = 0;            // what the call returns
int asked_for = -1;    // the threshold type of the last call
int init() { return 0; }
int shutdown() { return 0; }
int by_index(unsigned, void** dev) { *dev = &target; return 0; }
int threshold(void*, int type, unsigned* value) {
    asked_for = type;
    *value = static_cast<unsigned>(target);
    return rc;
}
gao::Nvml::Fake fake() {
    gao::Nvml::Fake f;
    f.init = init;
    f.shutdown = shutdown;
    f.by_index = by_index;
    f.temp_threshold = threshold;
    return f;
}
}

TEST_CASE("the card's temperature target is read where the card reports one") {
    using namespace target_fake;
    gao::Nvml nvml;
    REQUIRE(nvml.Init(fake()));
    target = 84;   // the reference RTX 4070
    rc = 0;
    CHECK(nvml.TempTarget(0) == 84);
    CHECK(asked_for == 5);   // the target in force now, not the lowest or highest it can be set to
    target = 77;   // a limit the owner set with another tool
    CHECK(nvml.TempTarget(0) == 77);
}

TEST_CASE("a card that reports no temperature target gives none") {
    using namespace target_fake;
    gao::Nvml nvml;
    REQUIRE(nvml.Init(fake()));
    target = 84;
    rc = 3;        // NVML_ERROR_NOT_SUPPORTED
    CHECK_FALSE(nvml.TempTarget(0));
    rc = 999;      // any other failure
    CHECK_FALSE(nvml.TempTarget(0));
    // A number that cannot be a temperature limit is no target either.
    rc = 0;
    for (const int nonsense : {0, 20, 39, 111, 250}) {
        target = nonsense;
        CAPTURE(nonsense);
        CHECK_FALSE(nvml.TempTarget(0));
    }
    // And a library without the function at all.
    gao::Nvml::Fake bare = fake();
    bare.temp_threshold = nullptr;
    gao::Nvml old;
    REQUIRE(old.Init(bare));
    CHECK_FALSE(old.TempTarget(0));
}

// On the real card, where there is one: a run sees the card through GuardedGpu,
// which builds its control anew, and the target has to come along. Without an
// NVIDIA card (CI) there is nothing to compare and the test passes as it is.
TEST_CASE("a run sees the card's temperature target") {
    gao::app::GuardedGpu hw(0);
    std::string why;
    if (!hw.Init(&why)) return;
    gao::Nvml nvml;
    gao::Nvapi nvapi;
    REQUIRE(nvml.Init());
    REQUIRE(nvapi.Init());
    CHECK(hw.control().temp_target_c == nvml.TempTarget(0).value_or(0));
    CHECK(hw.control().temp_target_default_c == nvapi.TempTargetDefaultC(0).value_or(0));
}

// nvml.dll as driver 617.42 behaves across a driver reset: the readings come
// from memory the driver shares with the process and takes away at a reset,
// and only a session that was started after it has that memory again.
namespace reset_fake {
int inits = 0;
int stops = 0;
int sessions = 0;        // nvmlInit is counted by the library: only the first one starts a session
bool memory_gone = false;
int faulting_reads = 0;  // each of these is an access violation in the real library

int init() { ++inits; if (sessions++ == 0) memory_gone = false; return 0; }
int shutdown() { ++stops; --sessions; return 0; }
int by_index(unsigned, void** dev) { *dev = &inits; return 0; }
int power_limit(void*, unsigned* mw) { *mw = 200000; return memory_gone ? 999 : 0; }
int fan(void*, unsigned* pct) { *pct = 40; return memory_gone ? 999 : 0; }
int reading(void*, int, unsigned* value) {
    if (memory_gone) ++faulting_reads;
    *value = 60;
    return 0;
}
}

// Before the test below: that one marks the library as abandoned for the rest of the process.
TEST_CASE("after a driver reset nothing reads NVML's shared memory until the session is started anew") {
    using namespace reset_fake;
    gao::Nvml::Fake fake{init, shutdown, by_index, power_limit, fan, reading, reading, 50};
    auto window = std::make_unique<gao::Nvml>();   // the app has two: the window's and the run's
    auto run = std::make_unique<gao::Nvml>();
    REQUIRE(window->Init(fake));
    REQUIRE(run->Init(fake));
    CHECK(inits == 1);   // one session for the process, or a shutdown by one of them would not stop it
    CHECK(run->Read(0).ok);

    memory_gone = true;   // the driver reset
    CHECK_FALSE(run->Read(0).ok);
    CHECK_FALSE(window->Read(0).ok);
    CHECK(faulting_reads == 0);
    CHECK(stops == 0);   // not at once: the driver needs a moment

    Sleep(80);
    CHECK(window->Read(0).ok);   // whoever reads next starts the new session, for both
    CHECK(stops == 1);
    CHECK(inits == 2);
    CHECK(run->Read(0).ok);
    CHECK(faulting_reads == 0);

    window.reset();
    CHECK(stops == 1);   // the run still uses the session
    run.reset();
    CHECK(stops == 2);
}

// One test case: the mark is for the rest of the process.
TEST_CASE("NVML is shut down until a driver call has faulted, and never after") {
    using gao::Nvml;
    using gao::app::guarded;
    using gao::app::guarded_driver;

    CHECK(guarded_driver([] {}));
    CHECK(shut_down_one() == 1);
    // A fault in anything else that runs guarded (the window's renderer) says nothing about NVML.
    CHECK_FALSE(guarded([] { RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr); }));
    CHECK_FALSE(Nvml::Abandoned());
    CHECK(shut_down_one() == 2);

    auto nvml = std::make_unique<Nvml>();
    REQUIRE(nvml->Init(fake_init, fake_shutdown));
    gao::app::set_fault_log([](const std::string& line) { fault_lines.push_back(line); });
    CHECK_FALSE(guarded_driver([] { faulting_call(); }));
    gao::app::set_fault_log(nullptr);
    // The fault is written down with where it happened: this program is among the callers.
    REQUIRE(fault_lines.size() == 1);
    CHECK(fault_lines[0].find("a call into a driver library faulted: read of 0x18 at ") == 0);
    CHECK(fault_lines[0].find("app_tests.exe+0x") != std::string::npos);
    CHECK(in_flight == 1);   // the call never left the library
    CHECK(Nvml::Abandoned());
    nvml.reset();
    CHECK(shutdowns == 2);   // not shut down: it would have waited for that call for ever
    // Nor is any later instance in this process: the library's state is one per process.
    CHECK(shut_down_one() == 2);
    CHECK(hung_shutdowns == 0);
}
