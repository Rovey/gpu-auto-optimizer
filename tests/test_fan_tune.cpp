#include "core/fan_tune.hpp"
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using namespace gao;

namespace {

// A card under a steady load with fans that can be set. At a fan speed it
// settles on a temperature (the slower the fans, the warmer), and it gets
// there half of the way with every 15 s look.
struct FanCard {
    double temp = 60;
    int fan = -1;                 // -1: the driver has the fans
    bool can_set = true;
    bool write_ok = true;
    int writes = 0, autos = 0, probes = 0;
    int abort_at_probe = -1;
    bool aborted_now = false;
    int wrong_above_c = 200;      // the load computes wrong values above this temperature
    int nvml_min = 30;
    std::vector<int> speeds;      // every speed that was set, in order
    std::vector<std::string> log, measuring;

    // 64 % -> 63 C, 50 % -> 70 C, 45 % -> 72.5 C, 100 % -> 45 C.
    double settles_at(int pct) const { return 95.0 - 0.5 * pct; }

    GpuControl gpu() {
        GpuControl g;
        g.fan_min_pct = nvml_min;
        if (!can_set) return g;
        g.set_fan_pct = [this](int pct) {
            if (!write_ok) return false;
            ++writes;
            fan = pct;
            speeds.push_back(pct);
            return true;
        };
        g.set_fan_auto = [this] { ++autos; fan = -1; return true; };
        return g;
    }
    FanTuneIo io() {
        FanTuneIo i;
        i.probe = [this](double seconds, int max_temp, double) {
            ++probes;
            StabilityResult s;
            s.seconds = seconds;
            if (probes == abort_at_probe) { aborted_now = true; s.verdict = Verdict::Aborted; return s; }
            temp += (settles_at(fan < 0 ? 60 : fan) - temp) * 0.5;
            s.peak_temp_c = s.end_temp_c = static_cast<int>(std::lround(temp));
            if (s.end_temp_c > max_temp) s.verdict = Verdict::TooHot;
            else if (s.end_temp_c > wrong_above_c) s.verdict = Verdict::WrongResult;
            return s;
        };
        i.aborted = [this] { return aborted_now; };
        i.log = [this](const std::string& m) { log.push_back(m); };
        i.measuring = [this](const std::string& what, double) { measuring.push_back(what); };
        return i;
    }
};

}

TEST_CASE("tune_fan: the lowest speed, in steps of five, that keeps the card at the target") {
    FanCard card;
    const auto r = tune_fan(card.gpu(), card.io(), 75, 64, 30);
    REQUIRE(r.ok);
    CHECK(r.target_c == 70);      // five under the limit
    CHECK(r.hold_pct == 50);      // 50 % settles on 70 C, 45 % on 72.5
    CHECK(r.hold_temp_c <= 70);
    CHECK(card.fan == 50);        // the fans are left on the answer
    CHECK(card.autos == 0);
    // It came down a step at a time from the start, rounded up to the grid, and tried one step too far.
    CHECK(card.speeds == std::vector<int>{65, 60, 55, 50, 45, 50});
    REQUIRE(r.steps.size() == 5);
    CHECK(r.steps.back().pct == 45);
    CHECK_FALSE(r.steps.back().held);
    // The curve goes through the measured point and is one the fan driver accepts.
    CHECK(valid(r.curve));
    CHECK(curve_pct(r.curve, 70) == 50);
    CHECK(curve_pct(r.curve, 75) == 100);
    CHECK(curve_pct(r.curve, 60) < 50);
    CHECK(curve_pct(r.curve, 60) >= 30);
}

TEST_CASE("tune_fan: a speed is judged when the temperature has settled, not before") {
    FanCard card;
    card.temp = 50;   // a cool card: at 65 % it has to warm up to 62.5 C first
    const auto r = tune_fan(card.gpu(), card.io(), 75, 65, 30);
    REQUIRE(r.ok);
    CHECK(r.hold_pct == 50);
    // Every look is 15 s; no speed got fewer than four unless it was clearly too warm.
    CHECK(card.probes >= 4 * 4);
    CHECK(std::find(card.measuring.begin(), card.measuring.end(), "Fan speed 65 %: waiting for the temperature to settle") != card.measuring.end());
    CHECK(card.measuring.back().empty());
}

TEST_CASE("tune_fan: a start that is too quiet goes up to the first speed that holds") {
    FanCard card;
    card.temp = 74;
    const auto r = tune_fan(card.gpu(), card.io(), 75, 38, 30);
    REQUIRE(r.ok);
    CHECK(card.speeds.front() == 40);
    CHECK(r.hold_pct == 50);
    CHECK(card.fan == 50);
    CHECK(std::is_sorted(card.speeds.begin(), card.speeds.end()));   // never down again
}

TEST_CASE("tune_fan: never below the lowest speed the fans hold") {
    FanCard card;
    const auto r = tune_fan(card.gpu(), card.io(), 75, 64, 55);   // learned: the fans stall under 55 %
    REQUIRE(r.ok);
    CHECK(r.hold_pct == 55);
    CHECK(*std::min_element(card.speeds.begin(), card.speeds.end()) == 55);
    CHECK(valid(r.curve));
    CHECK(curve_pct(r.curve, 40) >= 55);
    // The card's own minimum counts as well.
    FanCard own;
    own.nvml_min = 60;
    const auto o = tune_fan(own.gpu(), own.io(), 75, 64, 0);
    REQUIRE(o.ok);
    CHECK(o.hold_pct == 60);
}

TEST_CASE("tune_fan: every way out that is not a success hands the fans back to the driver") {
    {
        FanCard card;   // a stop request
        card.abort_at_probe = 4;
        const auto r = tune_fan(card.gpu(), card.io(), 75, 64, 30);
        CHECK_FALSE(r.ok);
        CHECK(r.reason == "aborted");
        CHECK(card.fan == -1);
    }
    {
        FanCard card;   // a write that does not verify
        card.write_ok = false;
        const auto r = tune_fan(card.gpu(), card.io(), 75, 64, 30);
        CHECK_FALSE(r.ok);
        CHECK(r.reason == "a fan speed did not verify");
        CHECK(card.autos == 1);
    }
    {
        FanCard card;   // the tune under the fans does not hold at this temperature
        card.wrong_above_c = 66;
        const auto r = tune_fan(card.gpu(), card.io(), 75, 64, 30);
        CHECK_FALSE(r.ok);
        CHECK(r.reason == "the load failed while the fans were tuned (WRONG RESULT)");
        CHECK(card.fan == -1);
    }
    {
        FanCard card;   // even full speed does not hold the target
        card.temp = 80;
        const auto r = tune_fan(card.gpu(), card.io(), 49, 100, 30);   // target 44 C; full speed settles on 45
        CHECK_FALSE(r.ok);
        CHECK(r.reason.find("cannot keep the card at 44 C") != std::string::npos);
        CHECK(card.fan == -1);
    }
}

TEST_CASE("tune_fan: a card whose fans cannot be set is left alone") {
    FanCard card;
    card.can_set = false;
    const auto r = tune_fan(card.gpu(), card.io(), 75, 64, 30);
    CHECK_FALSE(r.ok);
    CHECK(r.reason == "the fans of this card cannot be set");
    CHECK(card.probes == 0);
}

TEST_CASE("tune_fan: a speed that runs the card over its limit counts as too warm, not as a failure") {
    FanCard card;
    card.temp = 74;
    // Target 70, limit 71: 45 % heads for 72.5 C and trips the limit on the way.
    const auto r = tune_fan(card.gpu(), card.io(), 71, 55, 30);
    REQUIRE(r.ok);
    CHECK(r.target_c == 66);
    CHECK(r.hold_pct == 60);   // 60 % settles on 65 C, 55 % on 67.5
}

TEST_CASE("tuned_curve is valid for every measured point") {
    for (int hold = 0; hold <= 100; hold += 5)
        for (int target : {45, 60, 70, 78})
            for (int min : {0, 30, 55}) {
                const FanCurve c = tuned_curve(target, target + 5, hold, min);
                CHECK(valid(c));
                CHECK(curve_pct(c, target) == hold);
                CHECK(curve_pct(c, target + 5) == 100);
            }
}
