#include "doctest/doctest.h"
#include "core/config.hpp"
#include "core/boot.hpp"

using namespace gao;

namespace {
Profile sample() {
    Profile p;
    p.preset = Preset::Quiet;
    p.power_pct = 80;
    p.core_mhz = 90;
    p.mem_mhz = 600;
    p.driver = "610.74";
    p.gpu = "GPU-8a1b";
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
    CHECK(back.profile->gpu == "GPU-8a1b");
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
    CHECK_FALSE(from_json(R"({"profile":{"preset":"best","power_pct":"105","core_mhz":0,"mem_mhz":0,"driver":"x","gpu":"g","saved_at":"y"}})").profile);
    CHECK_FALSE(from_json(R"({"profile":{"preset":"best","power_pct":105,"core_mhz":0,"driver":"x","gpu":"g","saved_at":"y"}})").profile);
    CHECK_FALSE(from_json(R"({"profile":{"preset":"turbo","power_pct":105,"core_mhz":0,"mem_mhz":0,"driver":"x","gpu":"g","saved_at":"y"}})").profile);
    CHECK(from_json(R"({"profile":{"preset":"best","power_pct":105,"core_mhz":0,"mem_mhz":0,"driver":"x","gpu":"g","saved_at":"y"}})").profile);
}

TEST_CASE("strike count is sanitized") {
    CHECK(from_json(R"({"boot_strikes":-4})").boot_strikes == 3);
    CHECK(from_json(R"({"boot_strikes":"three"})").boot_strikes == 3);
    CHECK(from_json(R"({"boot_strikes":3})").boot_strikes == 3);
}

TEST_CASE("an old-format config loads without error") {
    const Config c = from_json(R"({"preset":0,"objectives":{"max_temp_c":75},"core_offset_mhz":150,
                                  "mem_offset_mhz":800,"power_limit_pct":90,"blacklisted_core_offsets":[180]})");
    CHECK_FALSE(c.profile.has_value());
    CHECK(c.boot_strikes == 0);
}

TEST_CASE("a profile saved before GPU identity existed is no profile") {
    CHECK_FALSE(from_json(R"({"profile":{"preset":"best","power_pct":105,"core_mhz":135,"mem_mhz":1050,"driver":"610.74","saved_at":"y"}})").profile);
}

TEST_CASE("a strike count that is not a sane integer fails closed") {
    // Anything but a whole number in 0..1000 must disable boot-apply rather
    // than silently re-enable it.
    for (const char* bad : {R"({"boot_strikes":3.0})", R"({"boot_strikes":-1})", R"({"boot_strikes":4294967296})",
                            R"({"boot_strikes":"x"})", R"({"boot_strikes":1001})", R"({"boot_strikes":null})"}) {
        CAPTURE(bad);
        CHECK(from_json(bad).boot_strikes == kMaxBootStrikes);
    }
    CHECK(from_json(R"({"boot_strikes":0})").boot_strikes == 0);
    CHECK(from_json(R"({"boot_strikes":2})").boot_strikes == 2);
    CHECK(from_json(R"({})").boot_strikes == 0);
}

#include "core/fan_curve.hpp"

TEST_CASE("fan settings survive a round-trip") {
    Config c;
    c.profile = sample();
    c.profile->fan_curve = FanCurve{50, {{50, 30}, {80, 100}}};
    c.fan_curve = FanCurve{std::nullopt, {{40, 40}, {70, 90}}};
    c.fan_control = true;
    const Config back = from_json(to_json(c));
    REQUIRE(back.profile.has_value());
    CHECK(back.profile->fan_curve == c.profile->fan_curve);
    CHECK(back.fan_curve == c.fan_curve);
    CHECK(back.fan_control);
}

TEST_CASE("a config from before fan control keeps its profile and has fan control off") {
    const Config back = from_json(R"({"boot_strikes":0,"profile":{"preset":"quiet","power_pct":80,"core_mhz":90,
        "mem_mhz":600,"driver":"610.74","gpu":"GPU-8a1b","saved_at":"2026-09-26 22:41"}})");
    REQUIRE(back.profile.has_value());
    CHECK_FALSE(back.profile->fan_curve.has_value());
    CHECK_FALSE(back.fan_curve.has_value());
    CHECK_FALSE(back.fan_control);
}

TEST_CASE("an invalid fan curve reads as absent without losing the profile") {
    Config c;
    c.profile = sample();
    c.profile->fan_curve = FanCurve{std::nullopt, {{60, 60}, {40, 40}}};   // descending: invalid
    c.fan_curve = FanCurve{std::nullopt, {{40, 40}}};                       // one point: invalid
    const Config back = from_json(to_json(c));
    REQUIRE(back.profile.has_value());
    CHECK_FALSE(back.profile->fan_curve.has_value());
    CHECK_FALSE(back.fan_curve.has_value());
    const Config junk = from_json(R"({"fan_curve":{"points":"x"},"fan_control":"yes"})");
    CHECK_FALSE(junk.fan_curve.has_value());
    CHECK_FALSE(junk.fan_control);
}

TEST_CASE("the active fan curve is the edited one, then the tested one, then the profile default") {
    Config c;
    CHECK_FALSE(active_fan_curve(c).has_value());
    c.profile = sample();   // Quiet, no tested curve
    CHECK(active_fan_curve(c) == default_curve(Preset::Quiet));
    c.profile->fan_curve = FanCurve{50, {{50, 40}, {80, 100}}};
    CHECK(active_fan_curve(c) == c.profile->fan_curve);
    c.fan_curve = FanCurve{std::nullopt, {{40, 40}, {70, 90}}};
    CHECK(active_fan_curve(c) == c.fan_curve);
}

TEST_CASE("a learned fan minimum belongs to the card it was learned on") {
    Config c;
    c.fan_min_pct = 50;
    c.fan_min_gpu = "GPU-8a1b";
    const Config back = from_json(to_json(c));
    CHECK(back.fan_min_pct == 50);
    CHECK(back.fan_min_gpu == "GPU-8a1b");
    CHECK(fan_min_for(back, "GPU-8a1b", 30) == 50);
    CHECK(fan_min_for(back, "GPU-other", 30) == 30);   // another card: NVML's minimum
    CHECK(fan_min_for(back, "GPU-8a1b", 55) == 55);    // never below NVML's own
    CHECK(from_json(R"({"fan_min_pct":500,"fan_min_gpu":"x"})").fan_min_pct == 0);   // out of range: ignored
}

TEST_CASE("the update check is on unless the file says otherwise") {
    CHECK(Config{}.update_check);
    CHECK(from_json("{}").update_check);                         // a file from before the setting existed
    CHECK(from_json(R"({"update_check":"no"})").update_check);   // junk keeps the default
    Config c;
    c.update_check = false;
    CHECK_FALSE(from_json(to_json(c)).update_check);
}

TEST_CASE("an undervolt profile survives a round-trip") {
    Config c;
    c.profile = sample();
    c.profile->preset = Preset::Undervolt;
    c.profile->power_pct = 100;
    c.profile->core_mhz = 0;
    c.profile->mem_mhz = 0;
    c.profile->undervolt = UndervoltTune{960000, 2745000, 210000};
    const Config back = from_json(to_json(c));
    REQUIRE(back.profile.has_value());
    CHECK(back.profile->preset == Preset::Undervolt);
    REQUIRE(back.profile->undervolt.has_value());
    CHECK(back.profile->undervolt->volt_uv == 960000);
    CHECK(back.profile->undervolt->freq_khz == 2745000);
    CHECK(back.profile->undervolt->raise_khz == 210000);
    CHECK(to_json(c).find("\"undervolt\"") != std::string::npos);
    // A profile without one writes no such key, so a file from an older build reads the same.
    c.profile = sample();
    CHECK(to_json(c).find("undervolt") == std::string::npos);
    CHECK_FALSE(from_json(to_json(c)).profile->undervolt.has_value());
}

TEST_CASE("an undervolt that cannot be read whole is no profile, never a profile without it") {
    const std::string head = R"({"profile":{"preset":"undervolt","power_pct":100,"core_mhz":0,"mem_mhz":0,"driver":"x","gpu":"g","saved_at":"y")";
    CHECK(from_json(head + R"(,"undervolt":{"volt_uv":960000,"freq_khz":2745000,"raise_khz":210000}}})").profile);
    CHECK_FALSE(from_json(head + R"(,"undervolt":{"volt_uv":960000,"freq_khz":2745000}}})").profile);                     // a field missing
    CHECK_FALSE(from_json(head + R"(,"undervolt":{"volt_uv":"960000","freq_khz":2745000,"raise_khz":210000}}})").profile);  // a wrong type
    CHECK_FALSE(from_json(head + R"(,"undervolt":17}})").profile);
    // The undervolt preset without its curve would apply as plain stock and look applied.
    CHECK_FALSE(from_json(head + "}}").profile);
}

#include "core/objectives.hpp"
#include "core/search.hpp"
#include <regex>
TEST_CASE("a profile's temperature ceiling survives a round-trip, and is absent when there is none") {
    Config c;
    c.profile = sample();
    CHECK(to_json(c).find("temp_ceiling_c") == std::string::npos);   // as every profile was written before
    CHECK_FALSE(from_json(to_json(c)).profile->temp_ceiling_c);
    c.profile->temp_ceiling_c = 85;
    const Config back = from_json(to_json(c));
    REQUIRE(back.profile);
    CHECK(back.profile->temp_ceiling_c == 85);
}

TEST_CASE("a profile's ceiling is its own limit unless the card reached more at stock") {
    Profile p = sample();
    p.preset = Preset::Undervolt;
    p.undervolt = UndervoltTune{781000, 1710000, 225000};
    CHECK(profile_ceiling(p) == objectives_for(Preset::Undervolt).max_temp_c);
    p.temp_ceiling_c = 85;
    CHECK(profile_ceiling(p) == 85);
    // Never below the profile's own limit, and never above what nothing goes beyond.
    p.temp_ceiling_c = 60;
    CHECK(profile_ceiling(p) == objectives_for(Preset::Undervolt).max_temp_c);
    p.temp_ceiling_c = 120;
    CHECK(profile_ceiling(p) == 85);
}

TEST_CASE("a ceiling that is no number is ignored, and the profile stays") {
    Config c;
    c.profile = sample();
    c.profile->temp_ceiling_c = 85;
    const std::string text = std::regex_replace(to_json(c), std::regex("\"temp_ceiling_c\":\\s*85"), "\"temp_ceiling_c\": \"hot\"");
    REQUIRE(text.find("hot") != std::string::npos);
    const Config back = from_json(text);
    REQUIRE(back.profile);
    CHECK_FALSE(back.profile->temp_ceiling_c);
    CHECK(profile_ceiling(*back.profile) == objectives_for(back.profile->preset).max_temp_c);
}

TEST_CASE("a curve that was saved with its ceiling is not moved a second time") {
    // What the undervolt run does on a card that reaches 86 C at stock: the
    // first run moves Normal up by 10 C and saves it with the ceiling of 85 C.
    // The run after it finds that curve in use, drawn against 85 already.
    Profile p = sample();
    p.preset = Preset::Undervolt;
    p.undervolt = UndervoltTune{781000, 1710000, 225000};
    const int limit = objectives_for(p.preset).max_temp_c, ceiling = temp_ceiling(limit, 86);
    const FanCurve first = shifted_curve(default_curve(Preset::Undervolt), ceiling - profile_ceiling(p));
    CHECK(first == FanCurve{50, {{50, 35}, {70, 45}, {80, 65}, {85, 100}}});
    p.fan_curve = first;
    p.temp_ceiling_c = ceiling;
    CHECK(shifted_curve(*p.fan_curve, ceiling - profile_ceiling(p)) == first);
    // On a card that stays under the profile's limit nothing moves, and nothing is saved with it.
    Profile cool = sample();
    cool.preset = Preset::Undervolt;
    CHECK(temp_ceiling(limit, 67) == limit);
    CHECK(shifted_curve(default_curve(Preset::Undervolt), temp_ceiling(limit, 67) - profile_ceiling(cool)) == default_curve(Preset::Undervolt));
}

