#include "doctest/doctest.h"
#include "core/objectives.hpp"
#include <string>

using namespace gao;

TEST_CASE("the default preset chases performance within a quiet thermal envelope") {
    const Objectives o = objectives_for(Preset::BestOfMyGpu);
    CHECK(o.max_temp_c == 75);
    CHECK(o.perf_push == doctest::Approx(0.7f));
    CHECK(o.core_oc);
    CHECK(o.mem_oc);
    CHECK(o.power);
}

TEST_CASE("quiet pushes clocks less hard than the default") {
    const Objectives quiet = objectives_for(Preset::Quiet);
    const Objectives best = objectives_for(Preset::BestOfMyGpu);
    CHECK(quiet.perf_push < best.perf_push);
    CHECK(quiet.max_temp_c > best.max_temp_c);
}

TEST_CASE("cool and efficient does not overclock") {
    const Objectives o = objectives_for(Preset::CoolAndEfficient);
    CHECK_FALSE(o.core_oc);
    CHECK_FALSE(o.mem_oc);
    CHECK(o.power);
}

TEST_CASE("max performance pushes every ceiling to its highest value") {
    const Objectives o = objectives_for(Preset::MaxPerformance);
    CHECK(o.max_temp_c == 83);
    CHECK(o.perf_push == doctest::Approx(1.0f));
    CHECK(o.core_oc);
    CHECK(o.mem_oc);
    CHECK(o.power);
}

TEST_CASE("preset names round-trip and unknown names are rejected") {
    for (Preset p : {Preset::BestOfMyGpu, Preset::Quiet, Preset::CoolAndEfficient, Preset::MaxPerformance, Preset::Undervolt})
        CHECK(preset_from_name(preset_name(p)) == p);
    CHECK(std::string(preset_name(Preset::BestOfMyGpu)) == "best");
    CHECK(std::string(preset_name(Preset::CoolAndEfficient)) == "cool");
    CHECK_FALSE(preset_from_name("turbo").has_value());
    CHECK_FALSE(preset_from_name("").has_value());
}

TEST_CASE("the undervolt preset keeps the clocks and the power limit at stock") {
    const Objectives o = objectives_for(Preset::Undervolt);
    CHECK_FALSE(o.core_oc);
    CHECK_FALSE(o.mem_oc);
    CHECK_FALSE(o.power);
    // The raise of the curve keeps the margin of the default preset.
    CHECK(o.perf_push == doctest::Approx(objectives_for(Preset::BestOfMyGpu).perf_push));
    CHECK(o.max_temp_c == objectives_for(Preset::BestOfMyGpu).max_temp_c);
    CHECK(std::string(preset_name(Preset::Undervolt)) == "undervolt");
}
