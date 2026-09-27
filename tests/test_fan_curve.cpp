#include "doctest/doctest.h"
#include "core/fan_curve.hpp"
#include "core/objectives.hpp"

using namespace gao;
using Clock = std::chrono::steady_clock;

namespace {
FanCurve simple() { return FanCurve{std::nullopt, {{40, 40}, {80, 80}}}; }
}

TEST_CASE("curve_pct interpolates between points and holds at the ends") {
    const FanCurve c = simple();
    CHECK(curve_pct(c, 30) == 40);
    CHECK(curve_pct(c, 40) == 40);
    CHECK(curve_pct(c, 60) == 60);
    CHECK(curve_pct(c, 61) == 61);
    CHECK(curve_pct(c, 80) == 80);
    CHECK(curve_pct(c, 95) == 80);
    const FanCurve d{std::nullopt, {{50, 30}, {65, 40}}};
    CHECK(curve_pct(d, 57) == 35);   // 30 + 10 * 7 / 15 = 34.67, rounded
}

TEST_CASE("valid rejects curves the controller cannot follow") {
    CHECK(valid(simple()));
    CHECK_FALSE(valid(FanCurve{std::nullopt, {{40, 40}}}));                                          // one point
    CHECK_FALSE(valid(FanCurve{std::nullopt, {{40, 40}, {45, 45}, {50, 50}, {55, 55}, {60, 60}, {65, 65}, {70, 70}}}));
    CHECK_FALSE(valid(FanCurve{std::nullopt, {{40, 40}, {40, 50}}}));                                // same temperature
    CHECK_FALSE(valid(FanCurve{std::nullopt, {{50, 40}, {40, 50}}}));                                // descending temperature
    CHECK_FALSE(valid(FanCurve{std::nullopt, {{40, 60}, {50, 50}}}));                                // fan % goes down
    CHECK_FALSE(valid(FanCurve{std::nullopt, {{40, 40}, {50, 101}}}));                               // above 100 %
    CHECK_FALSE(valid(FanCurve{std::nullopt, {{-5, 40}, {50, 50}}}));                                // below 0 C
    CHECK_FALSE(valid(FanCurve{15, {{40, 40}, {50, 50}}}));                                          // stop threshold out of 20-90
    CHECK(valid(FanCurve{50, {{50, 30}, {80, 100}}}));
}

TEST_CASE("every default curve is valid and reaches 100 % at the profile's limit") {
    for (const Preset p : {Preset::BestOfMyGpu, Preset::Quiet, Preset::CoolAndEfficient, Preset::MaxPerformance}) {
        const FanCurve c = default_curve(p);
        CHECK(valid(c));
        const int limit = objectives_for(p).max_temp_c;
        FanController ctrl(c, 30, limit);
        const FanCommand at_limit = ctrl.decide(limit, Clock::now());
        CHECK_FALSE(at_limit.driver);
        CHECK(at_limit.pct == 100);
    }
    CHECK(default_curve(Preset::BestOfMyGpu).points.back().pct == 100);
    CHECK(default_curve(Preset::MaxPerformance).stop_below_c == std::nullopt);
    CHECK(default_curve(Preset::Quiet).stop_below_c == 50);
}

TEST_CASE("quieter_than compares every temperature from 30 C to the limit") {
    const FanCurve tested{50, {{50, 35}, {75, 100}}};
    CHECK_FALSE(quieter_than(tested, tested, 75));
    CHECK(quieter_than(FanCurve{50, {{50, 30}, {75, 100}}}, tested, 75));      // lower fan %
    CHECK(quieter_than(FanCurve{55, {{50, 35}, {75, 100}}}, tested, 75));      // higher fan-stop threshold
    CHECK_FALSE(quieter_than(FanCurve{45, {{50, 40}, {75, 100}}}, tested, 75));   // louder everywhere
}

TEST_CASE("the controller follows the curve and never goes below the card's minimum") {
    FanController ctrl(FanCurve{std::nullopt, {{40, 10}, {80, 80}}}, 30, 90);
    const auto t = Clock::now();
    CHECK(ctrl.decide(40, t).pct == 30);   // 10 % raised to the minimum
    CHECK_FALSE(ctrl.decide(40, t).driver);
    CHECK(ctrl.decide(80, t).pct == 80);
}

TEST_CASE("the controller hands the fans to the driver without a temperature reading") {
    FanController ctrl(simple(), 30, 90);
    const auto t = Clock::now();
    CHECK_FALSE(ctrl.decide(60, t).driver);
    CHECK(ctrl.decide(-1, t).driver);
}

TEST_CASE("at or above the profile's limit the controller demands 100 %") {
    FanController ctrl(FanCurve{50, {{50, 30}, {65, 40}}}, 30, 70);
    const auto t = Clock::now();
    CHECK(ctrl.decide(70, t).pct == 100);
    CHECK(ctrl.decide(85, t).pct == 100);
}

TEST_CASE("the fan-stop zone has 3 C of hysteresis") {
    FanController ctrl(FanCurve{50, {{50, 30}, {80, 80}}}, 30, 90);
    auto t = Clock::now();
    CHECK(ctrl.decide(40, t).driver);   // well below: driver
    CHECK(ctrl.decide(49, t).driver);   // still below the threshold: driver
    CHECK_FALSE(ctrl.decide(50, t).driver);   // at the threshold: the curve takes over
    t += std::chrono::seconds(10);
    CHECK_FALSE(ctrl.decide(48, t).driver);   // within 3 C below: stays on the curve
    CHECK(ctrl.decide(47, t).driver);          // 3 C below: back to the driver
}

TEST_CASE("the controller raises at once and lowers only after 5 s") {
    FanController ctrl(simple(), 30, 90);
    auto t = Clock::now();
    CHECK(ctrl.decide(70, t).pct == 70);
    CHECK(ctrl.decide(75, t).pct == 75);          // up: immediate
    t += std::chrono::seconds(1);
    CHECK(ctrl.decide(60, t).pct == 75);          // down: held
    t += std::chrono::seconds(4);
    CHECK(ctrl.decide(60, t).pct == 75);          // 4 s: still held
    t += std::chrono::seconds(1);
    CHECK(ctrl.decide(60, t).pct == 60);          // 5 s lower: follows
    t += std::chrono::seconds(1);
    CHECK(ctrl.decide(50, t).pct == 60);          // a new drop starts a new wait
    CHECK(ctrl.decide(70, t).pct == 70);          // and a rise cancels it
}

#include "core/types.hpp"

namespace {
// A fake card: records writes and reports what a real driver would.
struct FakeFans {
    bool manual = false;
    int target = 30;
    int writes = 0;
    bool fail_writes = false;
    GpuControl gpu() {
        GpuControl g;
        g.fan_min_pct = 30;
        g.set_fan_pct = [this](int pct) {
            ++writes;
            if (fail_writes) return false;
            manual = true;
            target = pct;
            return true;
        };
        g.set_fan_auto = [this] { manual = false; return true; };
        g.read_fan = [this] { return std::optional<FanReading>(FanReading{manual, target}); };
        return g;
    }
};
}

TEST_CASE("the driver writes the curve and hands the fans back on release") {
    FakeFans f;
    const GpuControl g = f.gpu();
    FanDriver d(g, simple(), 90);
    const auto t = Clock::now();
    CHECK(d.tick(60, t).mode == FanMode::Curve);
    CHECK(f.manual);
    CHECK(f.target == 60);
    d.release();
    CHECK_FALSE(f.manual);
    CHECK(d.state().mode == FanMode::Driver);
}

TEST_CASE("the driver only writes on a change of 2 % or more") {
    FakeFans f;
    const GpuControl g = f.gpu();
    FanDriver d(g, simple(), 90);
    const auto t = Clock::now();
    d.tick(60, t);
    d.tick(61, t);   // 61 %: 1 % more, no write
    CHECK(f.writes == 1);
    d.tick(62, t);   // 62 %: 2 % more, written
    CHECK(f.writes == 2);
    CHECK(f.target == 62);
}

TEST_CASE("a driver reset is re-applied") {
    FakeFans f;
    const GpuControl g = f.gpu();
    FanDriver d(g, simple(), 90);
    const auto t = Clock::now();
    d.tick(60, t);
    f.manual = false;   // TDR: the driver took the fans back
    CHECK(d.tick(60, t).mode == FanMode::Curve);
    CHECK(f.manual);
    CHECK(f.writes == 2);
}

TEST_CASE("another tool's manual speed makes the driver step aside") {
    FakeFans f;
    const GpuControl g = f.gpu();
    FanDriver d(g, simple(), 90);
    const auto t = Clock::now();
    d.tick(60, t);
    f.target = 45;   // Afterburner set 45 %
    CHECK(d.tick(70, t).mode == FanMode::Foreign);
    CHECK(f.target == 45);   // not overwritten
    CHECK(f.manual);         // and not handed back either: the other tool owns it
    CHECK(d.tick(80, t).mode == FanMode::Foreign);
    CHECK(f.writes == 1);
}

TEST_CASE("a failed write hands the fans back and stops") {
    FakeFans f;
    f.fail_writes = true;
    const GpuControl g = f.gpu();
    FanDriver d(g, simple(), 90);
    const auto t = Clock::now();
    CHECK(d.tick(60, t).mode == FanMode::Failed);
    CHECK_FALSE(f.manual);
    CHECK(d.tick(70, t).mode == FanMode::Failed);
    CHECK(f.writes == 1);
}

TEST_CASE("no temperature reading hands the fans to the driver") {
    FakeFans f;
    const GpuControl g = f.gpu();
    FanDriver d(g, simple(), 90);
    const auto t = Clock::now();
    d.tick(60, t);
    CHECK(d.tick(-1, t).mode == FanMode::Driver);
    CHECK_FALSE(f.manual);
}

TEST_CASE("a new curve applies on the next tick without a false 'another program'") {
    FakeFans f;
    const GpuControl g = f.gpu();
    FanDriver d(g, simple(), 90);
    const auto t = Clock::now();
    d.tick(60, t);
    d.set_curve(FanCurve{std::nullopt, {{40, 70}, {80, 100}}}, 90);
    CHECK(d.tick(60, t).mode == FanMode::Curve);
    CHECK(f.target == 85);
}

TEST_CASE("a card without fan control never gets a write") {
    GpuControl g;   // every fan callback empty
    FanDriver d(g, simple(), 90);
    CHECK(d.tick(60, Clock::now()).mode == FanMode::Driver);
}

TEST_CASE("re-setting the same curve keeps the controller's state") {
    FakeFans f;
    const GpuControl g = f.gpu();
    const FanCurve c{50, {{50, 30}, {80, 80}}};
    FanDriver d(g, c, 90);
    auto t = Clock::now();
    CHECK(d.tick(55, t).mode == FanMode::Curve);   // on the curve
    d.set_curve(c, 90);                            // the tray re-syncs every 30 s
    CHECK(d.tick(48, t).mode == FanMode::Curve);   // within the 3 C hysteresis: still on the curve
    CHECK(d.tick(70, t).pct == 63);
    d.set_curve(c, 90);
    t += std::chrono::seconds(1);
    CHECK(d.tick(60, t).pct == 63);                // the slow-down wait survives the re-sync
}

TEST_CASE("another tool's manual speed is detected while the driver has the fans") {
    FakeFans f;
    const GpuControl g = f.gpu();
    FanDriver d(g, FanCurve{50, {{50, 30}, {80, 80}}}, 90);
    const auto t = Clock::now();
    CHECK(d.tick(40, t).mode == FanMode::Driver);   // fan-stop zone
    f.manual = true;                                // Afterburner sets 45 %
    f.target = 45;
    CHECK(d.tick(60, t).mode == FanMode::Foreign);
    CHECK(f.target == 45);
    CHECK(f.writes == 0);
}
