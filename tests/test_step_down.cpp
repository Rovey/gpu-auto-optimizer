#include "doctest/doctest.h"
#include "core/step_down.hpp"

using namespace gao;

namespace {
Profile owners() {
    Profile p;
    p.preset = Preset::Undervolt;
    p.power_pct = 90;
    p.core_mhz = 270;
    p.mem_mhz = 1850;
    p.driver = "610.74";
    p.gpu = "GPU-8a1b";
    p.saved_at = "2026-11-05 20:00";
    p.fan_curve = FanCurve{};
    p.undervolt = UndervoltTune{960000, 2932000, 322000};
    return p;
}
}

TEST_CASE("the owner's tune, steps 1 and 2") {
    const Profile p = owners();
    const Profile a = at_step(p, 1, 7500);
    CHECK(a.core_mhz == 255);
    CHECK(a.mem_mhz == 1750);
    REQUIRE(a.undervolt.has_value());
    CHECK(a.undervolt->freq_khz == 2910000);
    CHECK(a.undervolt->raise_khz == 300000);
    CHECK(a.undervolt->volt_uv == 960000);
    REQUIRE(a.step_down.has_value());
    CHECK(a.step_down->step == 1);
    CHECK_FALSE(a.step_down->tested);
    CHECK(a.step_down->found == FoundTune{270, 1850, UndervoltTune{960000, 2932000, 322000}});
    const Profile b = at_step(p, 2, 7500);
    CHECK(b.core_mhz == 240);
    CHECK(b.mem_mhz == 1650);
    CHECK(b.undervolt->freq_khz == 2895000);
    CHECK(b.undervolt->raise_khz == 285000);
}

TEST_CASE("step 0 is the found tune again") {
    const Profile p = owners();
    const Profile r = at_step(at_step(p, 2, 7500), 0, 7500);
    CHECK(r.core_mhz == p.core_mhz);
    CHECK(r.mem_mhz == p.mem_mhz);
    CHECK(r.undervolt == p.undervolt);
    CHECK_FALSE(r.step_down.has_value());
    CHECK(r.saved_at == p.saved_at);
}

TEST_CASE("steps do not compound") {
    const Profile p = owners();
    const Profile a = at_step(at_step(p, 1, 7500), 3, 7500);
    const Profile b = at_step(p, 3, 7500);
    CHECK(a.core_mhz == b.core_mhz);
    CHECK(a.mem_mhz == b.mem_mhz);
    CHECK(a.undervolt == b.undervolt);
    CHECK(a.step_down == b.step_down);
}

TEST_CASE("a negative offset moves towards zero") {
    Profile p = owners();
    p.core_mhz = -100;
    CHECK(at_step(p, 1, 7500).core_mhz == -90);
}

TEST_CASE("power limit, fan curve and voltage never change") {
    const Profile p = owners();
    const Profile r = at_step(p, 5, 7500);
    CHECK(r.power_pct == p.power_pct);
    CHECK(r.fan_curve == p.fan_curve);
    CHECK(r.undervolt->volt_uv == p.undervolt->volt_uv);
    CHECK(r.preset == p.preset);
    CHECK(r.driver == p.driver);
    CHECK(r.gpu == p.gpu);
}

TEST_CASE("the default grid") {
    CHECK(at_step(owners(), 1, kDefaultGridKhz).undervolt->freq_khz == 2910000);
}

TEST_CASE("a step that changes nothing is not offered") {
    Profile p;
    p.preset = Preset::Undervolt;
    p.core_mhz = 0;
    p.mem_mhz = 0;
    p.undervolt = UndervoltTune{960000, 2932000, 0};   // off the grid, but without a drop it stays
    CHECK_FALSE(can_step_down(p, 7500));
    CHECK(at_step(p, 1, 7500).undervolt->freq_khz == 2932000);
    CHECK(can_step_down(owners(), 7500));
}

TEST_CASE("no step below the limit") {
    CHECK_FALSE(can_step_down(at_step(owners(), kMaxStep, 7500), 7500));
}

TEST_CASE("step up stops below a failed step") {
    Profile p = at_step(owners(), 3, 7500);
    p.step_down->failed_step = 2;
    CHECK_FALSE(can_step_up(p));
    Profile q = at_step(owners(), 4, 7500);
    q.step_down->failed_step = 2;
    CHECK(can_step_up(q));
    CHECK_FALSE(can_step_up(owners()));
    Profile r = at_step(owners(), 1, 7500);
    r.step_down->failed_step = -3;
    CHECK(can_step_up(r));
}

TEST_CASE("step_text") {
    CHECK(step_text(2) == "-10 % (step 2)");
    CHECK(step_text(0).empty());
}

TEST_CASE("a lowered flat top never goes below the built-in curve") {
    Profile p;
    p.preset = Preset::Undervolt;
    p.undervolt = UndervoltTune{960000, 2932000, 5000};   // the built-in point is at 2927000
    // Step 1 drops 250 kHz; down to the 15 MHz grid that is 2925000, below the built-in point.
    const Profile one = at_step(p, 1, 15000);
    REQUIRE(one.undervolt);
    CHECK(one.undervolt->freq_khz == 2927000);
    CHECK(one.undervolt->raise_khz == 0);
    CHECK(can_step_down(p, 15000));
    // Every further step is clamped to the same point: it changes nothing.
    CHECK_FALSE(can_step_down(one, 15000));
    CHECK(at_step(p, kMaxStep, 15000).undervolt->raise_khz == 0);
}

TEST_CASE("a tune fails its test at or below its failed step") {
    CHECK_FALSE(failed_its_test(owners()));
    Profile p = at_step(owners(), 2, 7500);
    CHECK_FALSE(failed_its_test(p));
    p.step_down->failed_step = 1;
    CHECK_FALSE(failed_its_test(p));
    p.step_down->failed_step = 2;
    CHECK(failed_its_test(p));
    p.step_down->failed_step = 3;
    CHECK(failed_its_test(p));
}

TEST_CASE("a stopped run puts back the tune it found, unless the saved tune changed under it") {
    const Profile found = owners();
    // Nothing was on the card: nothing to put back.
    CHECK_FALSE(put_back_target(std::nullopt, found));
    // The saved tune is the one the run found on the card: that goes back.
    CHECK(put_back_target(found, found) == found);
    // A stepped test that was stopped: the step under test is saved now, never the step that failed.
    Profile under_test = at_step(found, 1, 7500);
    under_test.step_down->failed_step = 0;
    CHECK(put_back_target(found, under_test) == under_test);
    // A saved tune that failed its test, or none: nothing goes back.
    Profile failed = at_step(found, 2, 7500);
    failed.step_down->failed_step = 2;
    CHECK_FALSE(put_back_target(at_step(found, 1, 7500), failed));
    CHECK_FALSE(put_back_target(failed, failed));
    CHECK_FALSE(put_back_target(found, std::nullopt));
}
