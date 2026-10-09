#include "doctest/doctest.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "app/guarded_gpu.hpp"
#include "hw/nvml.hpp"
#include <memory>

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
    RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
    --in_flight;
}

int shut_down_one() {
    auto nvml = std::make_unique<gao::Nvml>();
    REQUIRE(nvml->Init(fake_init, fake_shutdown));
    nvml.reset();
    return shutdowns;
}

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
    CHECK_FALSE(guarded_driver([] { faulting_call(); }));
    CHECK(in_flight == 1);   // the call never left the library
    CHECK(Nvml::Abandoned());
    nvml.reset();
    CHECK(shutdowns == 2);   // not shut down: it would have waited for that call for ever
    // Nor is any later instance in this process: the library's state is one per process.
    CHECK(shut_down_one() == 2);
    CHECK(hung_shutdowns == 0);
}
