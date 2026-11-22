#include "doctest/doctest.h"
#include "search_fixture.hpp"
#include "core/fan_curve.hpp"

// The measurement at stock that goes before a run (the fans are the NVIDIA driver's).
static StabilityResult stock_run(Verdict verdict, double seconds, int peak_temp_c) {
    StabilityResult s;
    s.verdict = verdict;
    s.seconds = seconds;
    s.peak_temp_c = peak_temp_c;
    return s;
}

TEST_CASE("a card that gets hot at stock with the driver's fans is not refused: the run goes on") {
    CHECK(stock_refusal(stock_run(Verdict::Stable, 300, 67)).empty());
    // A 3080 Ti: 85 C under full load with the driver holding its fans at 65 %. Nothing is wrong with it.
    CHECK(stock_refusal(stock_run(Verdict::TooHot, 140, kStockTempC + 1)).empty());
    CHECK(stock_refusal(stock_run(Verdict::TooHot, kStockMinS, kStockTempC + 1)).empty());
}

TEST_CASE("a card that was hot before the measurement at stock got going is asked to cool down first") {
    const std::string why = stock_refusal(stock_run(Verdict::TooHot, 4, 88));
    CHECK(why.find("88 C") != std::string::npos);
    CHECK(why.find("cool down") != std::string::npos);
    CHECK(why.find("not stable") == std::string::npos);
}

TEST_CASE("a card that fails at stock for another reason gets no run") {
    CHECK(stock_refusal(stock_run(Verdict::WrongResult, 12, 60)) == "the card is not stable at stock (WRONG RESULT)");
    CHECK(stock_refusal(stock_run(Verdict::DeviceLost, 12, 60)) == "the card is not stable at stock (DEVICE LOST)");
    CHECK(stock_refusal(stock_run(Verdict::NoTelemetry, 12, 60)) == "the card is not stable at stock (NO TELEMETRY)");
    CHECK(stock_refusal(stock_run(Verdict::Aborted, 12, 60)) == "aborted");
}

TEST_CASE("a run does not start while MSI Afterburner or RivaTuner runs") {
    const std::string ab = other_tool_refusal({"explorer.exe", "MSIAfterburner.exe", "Discord.exe"});
    CHECK(ab.find("MSI Afterburner is running") == 0);
    CHECK(ab.find("Close it") != std::string::npos);
    const std::string rtss = other_tool_refusal({"RTSS.exe"});
    CHECK(rtss.find("RivaTuner Statistics Server (RTSS) is running") == 0);
    // Windows does not care about the case of a file name.
    CHECK_FALSE(other_tool_refusal({"msiafterburner.EXE"}).empty());
    CHECK_FALSE(other_tool_refusal({"rtss.exe"}).empty());
    // Afterburner starts RivaTuner with it: the one the user knows is named.
    CHECK(other_tool_refusal({"RTSS.exe", "MSIAfterburner.exe"}).find("MSI Afterburner") == 0);
}

TEST_CASE("a fan tool does not stop a run, and the run says that it may hold the fans") {
    CHECK(fan_tool_warning({"explorer.exe", "FanControl.exe"}) ==
          "fans: FanControl is running and may hold the fans, so this run's numbers can depend on its curve. Close it for a "
          "result on the card's own fan control");
    CHECK(fan_tool_warning({"ArgusMonitor.exe"}).find("fans: Argus Monitor is running") == 0);
    CHECK_FALSE(fan_tool_warning({"fancontrol.EXE"}).empty());   // Windows does not care about the case
    CHECK(other_tool_refusal({"FanControl.exe", "ArgusMonitor.exe"}).empty());
    CHECK(fan_tool_warning({}).empty());
    CHECK(fan_tool_warning({"explorer.exe", "MSIAfterburner.exe", "NotFanControl.exe", "FanControl.exe.bak"}).empty());
}

TEST_CASE("a run starts when neither runs, whatever else does") {
    CHECK(other_tool_refusal({}).empty());
    CHECK(other_tool_refusal({"explorer.exe", "Discord.exe", "GpuAutoOptimizer.exe", "gao.exe"}).empty());
    // Only the programs themselves: not a file that merely carries the name.
    CHECK(other_tool_refusal({"RTSSHooksLoader64.exe", "MSIAfterburner.exe.bak", "NotRTSS.exe", "Afterburner"}).empty());
}

namespace {

// A card that is warm before anything is changed: 15 C above the usual one, so
// that the profile's 75 C only holds at a power limit of 65 %. The fake card
// scores by its power limit (1000 it/s from 90 % up), so that costs speed, as
// it did on an RTX 3080 Ti: 4749 it/s after the run, 4937 at stock.
Run warm_card(double stock_score) {
    Run run;
    run.card.extra_heat = 15;
    StabilityResult stock;
    stock.verdict = Verdict::Stable;
    stock.seconds = 300;
    stock.score = stock_score;
    stock.peak_temp_c = 85;
    run.stock = stock;
    return run;
}

}

TEST_CASE("a profile that is after speed keeps nothing when the result is slower than stock") {
    Run run = warm_card(1000);
    const auto r = run.go(Preset::BestOfMyGpu);
    CAPTURE(r.reason);
    CHECK_FALSE(r.ok);
    CHECK(r.reason.find("slower than stock") != std::string::npos);
    CHECK(r.reason.find("722 it/s against 1000") != std::string::npos);
    CHECK(r.reason.find("power limit went down to 65 %") != std::string::npos);
    CHECK(r.reason.find("Undervolt") != std::string::npos);
    // Not applied: the card is at stock again.
    CHECK(r.stock_restored);
    CHECK(run.card.core == 0);
    CHECK(run.card.mem == 0);
    CHECK(run.card.power == 100);
    // The search itself ran to its end: the result was tested before it was judged.
    CHECK(run.logged("soak: STABLE"));
    check_journal_rules(run);
}

TEST_CASE("max performance keeps nothing either when the card scored more at stock") {
    // Its limit is 83 C, so this card keeps a power limit of 90 %: the fake
    // card's full 1000 it/s. The card scored more than that at stock.
    Run run = warm_card(1100);
    const auto r = run.go(Preset::MaxPerformance);
    CAPTURE(r.reason);
    CHECK_FALSE(r.ok);
    CHECK(r.reason.find("slower than stock: 1000 it/s against 1100") != std::string::npos);
    CHECK(r.stock_restored);
    CHECK(run.card.core == 0);
    CHECK(run.card.power == 100);
}

TEST_CASE("a profile that is after speed keeps a result that is faster than stock, or as fast") {
    Run faster = warm_card(700);
    const auto r = faster.go(Preset::BestOfMyGpu);
    REQUIRE(r.ok);
    CHECK(r.power_pct == 65);
    CHECK(faster.card.power == 65);   // applied
    CHECK(faster.card.core == r.core_mhz);
    // Within one percent of stock is the scatter of two five-minute runs, not a slower card.
    Run level = warm_card(728);       // 722 against 728: 0.8 % less
    CHECK(level.go(Preset::BestOfMyGpu).ok);
    Run just_slower = warm_card(735); // 1.8 % less
    CHECK_FALSE(just_slower.go(Preset::BestOfMyGpu).ok);
}

TEST_CASE("the profiles that give speed away on purpose keep their result when it is slower than stock") {
    // Quiet and Cool & efficient lower the power limit for less noise and
    // heat. All in one goes on to an undervolt and a fan tune after this step.
    for (const Preset preset : {Preset::Quiet, Preset::CoolAndEfficient, Preset::AllInOne}) {
        Run run = warm_card(1000);
        if (preset == Preset::CoolAndEfficient) run.card.extra_heat = 5;    // its limit is 65 C
        const auto r = run.go(preset);
        CAPTURE(std::string(preset_name(preset)));
        CAPTURE(r.reason);
        REQUIRE(r.ok);
        CHECK(r.soak.score < 1000);
        CHECK(run.card.power == r.power_pct);
    }
}

TEST_CASE("without a measurement at stock there is nothing to be slower than") {
    Run run = warm_card(1000);
    run.stock.reset();
    CHECK(run.go(Preset::BestOfMyGpu).ok);
    Run unscored = warm_card(0);   // a measurement that scored nothing says nothing
    CHECK(unscored.go(Preset::BestOfMyGpu).ok);
}

TEST_CASE("the temperature ceiling of a run is the profile's limit, or what the card reached at stock, and never more than 85 C") {
    CHECK(temp_ceiling(75, 67) == 75);   // the reference card: nothing changes
    CHECK(temp_ceiling(75, 75) == 75);
    CHECK(temp_ceiling(75, 80) == 80);
    CHECK(temp_ceiling(75, 86) == 85);   // the tester's RTX 3080 Ti
    CHECK(temp_ceiling(83, 86) == 85);
    CHECK(temp_ceiling(75, -1) == 75);   // no reading
    CHECK(temp_ceiling(65, 70) == 70);
}
TEST_CASE("the card's own temperature target is the ceiling's top, where the card reports one") {
    // The card throttles itself at its target: a run has no business above it.
    CHECK(temp_ceiling(75, 86, 83) == 83);   // an RTX 3080 Ti at its default
    CHECK(temp_ceiling(75, 86, 77) == 77);   // the same card with the limit its owner set
    CHECK(temp_ceiling(75, 80, 83) == 80);   // what the card reached, when that is less
    CHECK(temp_ceiling(75, 67, 84) == 75);   // the reference card: nothing changes
    // A target above 85 C does not raise the top: the measurement at stock
    // stops there, so nothing above it was ever seen on this card.
    CHECK(temp_ceiling(75, 86, 88) == 85);
    CHECK(temp_ceiling(75, 86, 91) == 85);
}

TEST_CASE("a card that reports no temperature target keeps 85 C, exactly as before") {
    for (const int limit : {65, 75, 80, 83})
        for (int peak = -1; peak <= 95; ++peak) {
            CAPTURE(limit);
            CAPTURE(peak);
            CHECK(temp_ceiling(limit, peak, 0) == temp_ceiling(limit, peak));
            CHECK(temp_ceiling(limit, peak) == std::max(limit, std::min(peak, kStockTempC)));
            CHECK(temp_ceiling(limit, peak, -1) == temp_ceiling(limit, peak));
        }
}

TEST_CASE("a target below the profile's own limit does not lower that limit") {
    // The profile's limit is what the profile was asked for; a card that
    // throttles below it does so by itself, as it did before.
    CHECK(temp_ceiling(75, 86, 70) == 75);
    CHECK(temp_ceiling(83, 86, 77) == 83);
    CHECK(temp_ceiling(75, 60, 70) == 75);
}

TEST_CASE("the fan curve moves by what the card's own target leaves, not by a fixed ten degrees") {
    const FanCurve normal = fan_preset_curve(FanPreset::Normal);
    // Target 83 C: the red line and the last point at 83.
    CHECK(shifted_curve(normal, temp_ceiling(75, 86, 83) - 75) == FanCurve{50, {{50, 35}, {68, 45}, {78, 65}, {83, 100}}});
    // Target 77 C, set by the owner: two degrees, and the fans are at 100 % where the card would begin to slow down.
    CHECK(shifted_curve(normal, temp_ceiling(75, 86, 77) - 75) == FanCurve{50, {{50, 35}, {62, 45}, {72, 65}, {77, 100}}});
    // No target reported: ten degrees, as in the step before this one.
    CHECK(shifted_curve(normal, temp_ceiling(75, 86) - 75) == FanCurve{50, {{50, 35}, {70, 45}, {80, 65}, {85, 100}}});
}

// Hardware check 112: a GTX 1080 Ti that reached 86 C at stock.
static StabilityResult hot_stock(int end_fan_pct) {
    StabilityResult s = stock_run(Verdict::TooHot, 182, 86);
    s.end_fan_pct = end_fan_pct;
    return s;
}

TEST_CASE("a card at 85 C at stock with its fans at 100 %, or with fans the app cannot set, cannot be cooled more") {
    CHECK(hot_at_stock_at_full_fans(hot_stock(100), true));
    CHECK(hot_at_stock_at_full_fans(hot_stock(78), false));    // the fans are not the app's to raise
    CHECK(hot_at_stock_at_full_fans(hot_stock(-1), false));
    CHECK_FALSE(hot_at_stock_at_full_fans(hot_stock(78), true));   // the driver's 78 %: a curve can still raise them
    CHECK_FALSE(hot_at_stock_at_full_fans(hot_stock(-1), true));   // no reading: not known to be at 100 %
    StabilityResult cool = stock_run(Verdict::Stable, 300, 80);
    cool.end_fan_pct = 100;
    CHECK_FALSE(hot_at_stock_at_full_fans(cool, true));        // loud, but it held
    CHECK_FALSE(hot_at_stock_at_full_fans(cool, false));
}

TEST_CASE("the Undervolt profile does not measure stock again when the fans were already at 100 %") {
    // Check 112: 86 C with the fans at 100 % on the run's curve; the second
    // measurement, on the curve moved up to 84 C, stopped after 30 s.
    CHECK_FALSE(measure_stock_again(84, 75, hot_stock(100), true));
    CHECK_FALSE(measure_stock_again(84, 75, hot_stock(70), false));
    // Fans that can still go up: measured again, as before.
    CHECK(measure_stock_again(84, 75, hot_stock(78), true));
    CHECK(measure_stock_again(80, 75, stock_run(Verdict::Stable, 300, 80), true));
    // A ceiling at the profile's limit never had a second measurement.
    CHECK_FALSE(measure_stock_again(75, 75, stock_run(Verdict::Stable, 300, 70), true));
    CHECK_FALSE(measure_stock_again(75, 75, hot_stock(78), true));
}

TEST_CASE("an undervolt that is not compared says the card is at stock, not that nothing changed") {
    const std::string skipped = undervolt_not_compared(hot_stock(100), true, true);
    CHECK(skipped.rfind("not compared: the measurement at stock stopped at 86 C after 182 s with the fans at 100 %, and a "
                        "second measurement would stop as well, so there is nothing to judge an undervolt against",
                        0) == 0);
    // measure_stock set the card to stock whatever was on it (check 112 read a tune's +60 MHz just before).
    CHECK(skipped.find("Nothing was changed") == std::string::npos);
    CHECK(skipped.find("The card was set to stock for the measurement and is at stock now") != std::string::npos);
    CHECK(skipped.find("the saved tune is unchanged") != std::string::npos);
    CHECK(undervolt_not_compared(hot_stock(-1), true, false).find("with fans the app cannot set") != std::string::npos);
    // Measured twice: the second measurement's numbers, as before; without a saved tune nothing is said about one.
    const std::string twice = undervolt_not_compared(stock_run(Verdict::TooHot, 30, 86), false, false);
    CHECK(twice == "not compared: the measurement at stock stopped at 86 C after 30 s, so there is nothing to judge an undervolt "
                   "against. The card was set to stock for the measurement and is at stock now");
}

TEST_CASE("best and max suggest the Undervolt profile only where it can run (decision D8)") {
    // Check 112: Best and Max ended slower than stock on a card at 86 C, then
    // pointed to Undervolt, which refused on the same card.
    auto run_with = [](int end_fan_pct, bool settable) {
        Run run = warm_card(1000);
        run.stock->verdict = Verdict::TooHot;
        run.stock->seconds = 182;
        run.stock->peak_temp_c = 86;
        run.stock->end_fan_pct = end_fan_pct;
        GpuControl gpu = run.card.gpu();
        if (settable) gpu.set_fan_pct = [](int) { return true; };
        return run.go(Preset::BestOfMyGpu, gpu);
    };
    // The driver's fans at 78 %: a curve can raise them, so Undervolt may run. It is offered, without a promise.
    const auto may = run_with(78, true);
    CAPTURE(may.reason);
    REQUIRE_FALSE(may.ok);
    CHECK(may.reason.find("slower than stock") != std::string::npos);
    CHECK(may.reason.find("the Undervolt profile may help") != std::string::npos);
    CHECK(may.reason.find("only its own run shows whether it does on this card") != std::string::npos);
    CHECK(may.reason.find("is the one that helps") == std::string::npos);
    // Already at 100 %, or fans the app cannot set: no suggestion, the fact instead.
    const auto full = run_with(100, true);
    CAPTURE(full.reason);
    REQUIRE_FALSE(full.ok);
    CHECK(full.reason.find("may help") == std::string::npos);
    CHECK(full.reason.find("The card reached 86 C at stock with its fans at 100 %: the Undervolt profile would stop at its own "
                           "measurement at stock as well") != std::string::npos);
    const auto unset = run_with(70, false);
    CHECK(unset.reason.find("may help") == std::string::npos);
    CHECK(unset.reason.find("with fans the app cannot set") != std::string::npos);
    // A card that held at stock keeps the suggestion, whatever its fans did.
    Run held = warm_card(1000);
    held.stock->end_fan_pct = 100;
    CHECK(held.go(Preset::BestOfMyGpu).reason.find("the Undervolt profile may help") != std::string::npos);
}
