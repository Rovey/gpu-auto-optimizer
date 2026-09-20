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
