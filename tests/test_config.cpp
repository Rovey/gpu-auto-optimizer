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
