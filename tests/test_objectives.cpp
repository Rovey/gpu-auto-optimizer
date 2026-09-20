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
