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
    CHECK(back.objectives.max_temp_c == 80);
    CHECK(back.objectives.max_fan_pct == 40);
    CHECK(back.objectives.perf_push == 0.4f);
    CHECK(back.objectives.core_oc == true);
    CHECK(back.objectives.mem_oc == true);
    CHECK(back.objectives.power == true);
    CHECK(back.objectives.undervolt == false);
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

TEST_CASE("a config with wrong field types falls back to defaults instead of throwing") {
    const Config c = from_json(R"({"preset": "oops", "core_offset_mhz": [1,2]})");
    CHECK(c.preset == Preset::BestOfMyGpu);
    CHECK(c.core_offset_mhz == 0);
    CHECK(c.blacklisted_core_offsets.empty());
}
