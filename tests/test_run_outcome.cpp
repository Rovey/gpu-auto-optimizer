#include "doctest/doctest.h"

#include "app/common.hpp"
#include <string>
#include <vector>

using gao::app::AllInOneOutcome;
using gao::app::driver_resets;
using gao::app::OptimizeOutcome;
using gao::app::UndervoltOutcome;

TEST_CASE("a run's driver resets are counted over its steps") {
    OptimizeOutcome overclock;
    overclock.ran = true;
    overclock.result.driver_resets = 1;
    CHECK(driver_resets(overclock) == 1);

    UndervoltOutcome undervolt;
    undervolt.ran = true;
    undervolt.result.driver_resets = 1;
    CHECK(driver_resets(undervolt) == 1);

    AllInOneOutcome all;
    all.overclock = overclock;
    CHECK(driver_resets(all) == 1);
    all.undervolt = undervolt;
    CHECK(driver_resets(all) == 2);
}

TEST_CASE("a step that did not run counts no driver reset") {
    OptimizeOutcome overclock;
    overclock.result.driver_resets = 1;   // never valid when the step did not run
    CHECK(driver_resets(overclock) == 0);

    UndervoltOutcome undervolt;
    undervolt.result.driver_resets = 1;
    CHECK(driver_resets(undervolt) == 0);

    CHECK(driver_resets(AllInOneOutcome{}) == 0);
}

namespace {

bool has_line(const std::vector<std::string>& lines, const std::string& part) {
    for (const std::string& l : lines)
        if (l.find(part) != std::string::npos) return true;
    return false;
}

gao::StabilityResult measured(double score, int power_w) {
    gao::StabilityResult s;
    s.verdict = gao::Verdict::Stable;
    s.score = score;
    s.avg_core_mhz = 2895;
    s.avg_mem_mhz = 15850;
    s.peak_temp_c = 71;
    s.end_fan_pct = 35;
    s.avg_power_w = power_w;
    s.seconds = 300;
    return s;
}

}

// Hardware check 99: run.log ended at the final test. The end of a run is
// now a set of lines that goes through the run's log like every other line.
TEST_CASE("the end of an all-in-one run is in its result lines, with the final test") {
    AllInOneOutcome all;
    all.ran = true;
    all.ok = true;
    all.profile.power_pct = 100;
    all.profile.core_mhz = 255;
    all.profile.mem_mhz = 1850;
    all.profile.undervolt = gao::UndervoltTune{965000, 2940000, 0};
    all.fan.ok = true;
    all.fan.hold_pct = 35;
    all.fan.hold_temp_c = 69;
    all.stock = measured(5786, 199);
    all.now = measured(5970, 171);
    all.overclock.ran = true;
    all.overclock.result.driver_resets = 1;
    const auto lines = gao::app::result_lines(all);
    REQUIRE_FALSE(lines.empty());
    CHECK(lines.front() == "RESULT: power 100 %, core +255 MHz, mem +1850 MHz, 2940 MHz at 965 mV, fans 35 % at 69 C");
    CHECK(has_line(lines, "driver resets during this run: 1"));
    CHECK(has_line(lines, "stock: score=5786 it/s"));
    CHECK(has_line(lines, "now:   score=5970 it/s"));
    CHECK(has_line(lines, "that is +3.2 % score and -14.1 % power against stock"));
}

TEST_CASE("a run that ended without a result says why in its result lines") {
    AllInOneOutcome all;
    all.error = "aborted";
    CHECK(gao::app::result_lines(all).front() == "RESULT: not applied -- aborted");

    OptimizeOutcome stopped;
    stopped.error = "another program is tuning the card";
    CHECK(gao::app::result_lines(stopped).front() == "RESULT: not started -- another program is tuning the card");

    OptimizeOutcome failed;
    failed.ran = true;
    failed.result.reason = "the driver reset twice";
    failed.result.stock_restored = true;
    failed.result.driver_resets = 2;
    const auto lines = gao::app::result_lines(failed);
    CHECK(lines.front() == "RESULT: not applied -- the driver reset twice (card at stock)");
    CHECK(has_line(lines, "driver resets during this run: 2"));
}

// Hardware check 98: the retest's score went to the console only.
TEST_CASE("a retest's result lines carry the test's score") {
    gao::app::RetestOutcome retest;
    retest.ran = true;
    retest.ok = true;
    retest.driver = "617.42";
    retest.driver_was = "617.14";
    retest.result.test = measured(5742, 190);
    const auto lines = gao::app::result_lines(retest);
    REQUIRE(lines.size() == 2);
    CHECK(lines[0].find("STABLE score=5742 it/s") != std::string::npos);
    CHECK(lines[1].rfind("RESULT: the tune holds on driver 617.42 (it was tested on 617.14) and is applied: ", 0) == 0);
}
