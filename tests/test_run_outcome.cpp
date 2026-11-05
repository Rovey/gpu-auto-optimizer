#include "doctest/doctest.h"

#include "app/common.hpp"
#include "core/boot.hpp"
#include "core/step_down.hpp"
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

TEST_CASE("a stepped retest names its step") {
    gao::app::RetestOutcome retest;
    retest.ran = true;
    retest.ok = true;
    retest.driver = "617.42";
    retest.driver_was = "617.14";
    retest.result.test = measured(5742, 190);
    retest.first_step = 1;
    retest.final_step = 2;
    const auto lines = gao::app::result_lines(retest);
    REQUIRE(lines.size() == 2);
    CHECK(lines[1].rfind("RESULT: the tune held at -10 % (step 2) after -5 % (step 1) failed", 0) == 0);
}

TEST_CASE("a retest that stepped from the tune the run found names it") {
    gao::app::RetestOutcome retest;
    retest.ran = true;
    retest.ok = true;
    retest.driver = "617.42";
    retest.driver_was = "617.14";
    retest.result.test = measured(5742, 190);
    retest.first_step = 0;
    retest.final_step = 1;
    const auto lines = gao::app::result_lines(retest);
    REQUIRE(lines.size() == 2);
    CHECK(lines[1].rfind("RESULT: the tune held at -5 % (step 1) after the tune the run found failed, on driver ", 0) == 0);
}

TEST_CASE("a retest that stepped more than once names the range that failed") {
    gao::app::RetestOutcome retest;
    retest.ran = true;
    retest.ok = true;
    retest.driver = "617.42";
    retest.driver_was = "617.14";
    retest.result.test = measured(5742, 190);
    retest.first_step = 1;
    retest.final_step = 3;
    auto lines = gao::app::result_lines(retest);
    REQUIRE(lines.size() == 2);
    CHECK(lines[1].rfind("RESULT: the tune held at -15 % (step 3) after -5 % (step 1) to -10 % (step 2) failed, on driver 617.42 "
                         "(it was tested on 617.14), and is applied: ",
                         0) == 0);
    retest.first_step = 0;
    lines = gao::app::result_lines(retest);
    REQUIRE(lines.size() == 2);
    CHECK(lines[1].rfind("RESULT: the tune held at -15 % (step 3) after the tune the run found to -10 % (step 2) failed, ", 0) == 0);
}

TEST_CASE("a retest on the driver the tune was tested on does not name it twice") {
    gao::app::RetestOutcome retest;
    retest.ran = true;
    retest.ok = true;
    retest.driver = "621.14";
    retest.driver_was = "621.14";
    retest.result.test = measured(5870, 164);
    auto lines = gao::app::result_lines(retest);
    REQUIRE(lines.size() == 2);
    CHECK(lines[1].rfind("RESULT: the tune holds on driver 621.14 and is applied: ", 0) == 0);
    CHECK(gao::app::retest_boot_line(retest).rfind("tested on driver 621.14: applied ", 0) == 0);
    retest.first_step = 1;
    retest.final_step = 3;
    lines = gao::app::result_lines(retest);
    REQUIRE(lines.size() == 2);
    CHECK(lines[1].rfind("RESULT: the tune held at -15 % (step 3) after -5 % (step 1) to -10 % (step 2) failed, on driver 621.14, and is "
                         "applied: ",
                         0) == 0);
    // After a driver update both are named.
    retest.driver = "621.30";
    CHECK(gao::app::retest_boot_line(retest).rfind("tested again on driver 621.30 (it was tested on 621.14): applied ", 0) == 0);
}

TEST_CASE("the logon refusal on a PC with two NVIDIA GPUs says why") {
    const std::string text = gao::app::decision_text(gao::BootDecision::SeveralGpus, gao::Config{}, "617.14");
    CHECK(text == gao::kSeveralGpusRefusal);
    CHECK(text.find("more than one NVIDIA GPU") != std::string::npos);
    CHECK(text.find("Nothing was changed.") != std::string::npos);
    // It ends with what someone with two cards can do about it.
    const std::string ask = "If you have two NVIDIA cards and want to help make this work, open an issue at "
                            "https://github.com/Rovey/gpu-auto-optimizer/issues";
    CHECK(text.size() >= ask.size());
    CHECK(text.compare(text.size() - ask.size(), ask.size(), ask) == 0);
}

TEST_CASE("the logon refusal after too many crashes offers the step down first") {
    gao::Config c;
    c.boot_strikes = gao::kMaxBootStrikes;
    CHECK(gao::app::decision_text(gao::BootDecision::TooManyStrikes, c, "617.14") ==
          "disabled after " + std::to_string(gao::kMaxBootStrikes) +
              " crashes; turn boot-apply on again to retry; or lower the tune 5 % and turn boot-apply on again");
}

namespace {
// The found tune of a saved profile that was lowered to `step`, with `failed` on record.
gao::Config stepped_config(int step, int failed) {
    gao::Config c;
    gao::Profile p;
    p.power_pct = 110;
    p.core_mhz = 150;
    p.mem_mhz = 1000;
    p.driver = "617.14";
    p.gpu = "GPU-1";
    c.profile = gao::at_step(p, step, gao::kDefaultGridKhz);
    c.profile->step_down->failed_step = failed;
    return c;
}
}

TEST_CASE("the logon refusal of a tune that failed its test names the step") {
    gao::Config c = stepped_config(2, 2);
    CHECK(gao::app::decision_text(gao::BootDecision::FailedTest, c, "617.14") ==
          "the saved tune failed its test at -10 % (step 2); optimize again or lower it 5 %");
    c = stepped_config(gao::kMaxStep, gao::kMaxStep);
    CHECK(gao::app::decision_text(gao::BootDecision::FailedTest, c, "617.14") ==
          "the saved tune failed its test at -50 % (step 10); optimize again");
}

TEST_CASE("a stepped retest that found no step that holds names the steps that failed") {
    gao::app::RetestOutcome retest;
    retest.ran = true;
    retest.driver = retest.driver_was = "617.14";
    retest.result.test = measured(5742, 190);
    retest.result.test.verdict = gao::Verdict::WrongResult;
    retest.error = "the tune did not hold on driver 617.14 (WRONG RESULT); optimize again -- card at stock";
    retest.first_step = 1;
    retest.final_step = 10;
    auto lines = gao::app::result_lines(retest);
    REQUIRE(lines.size() == 2);
    CHECK(lines[1] == "RESULT: not applied -- -5 % (step 1) to -50 % (step 10) failed: " + retest.error);
    retest.first_step = 0;
    retest.final_step = 1;
    lines = gao::app::result_lines(retest);
    CHECK(lines[1] == "RESULT: not applied -- the tune the run found to -5 % (step 1) failed: " + retest.error);
    // One step that failed on its own is named too, at step 0 not.
    retest.first_step = retest.final_step = 2;
    CHECK(gao::app::result_lines(retest)[1] == "RESULT: not applied -- -10 % (step 2) failed: " + retest.error);
    retest.first_step = retest.final_step = 0;
    CHECK(gao::app::result_lines(retest)[1] == "RESULT: not applied -- " + retest.error);
    // Stopped: the step under test did not fail.
    retest.first_step = 1;
    retest.final_step = 2;
    retest.result.test.verdict = gao::Verdict::Aborted;
    retest.error = "aborted -- the tune under test is applied again";
    CHECK(gao::app::result_lines(retest)[1] == "RESULT: not applied -- " + retest.error);
}

TEST_CASE("a retest that ran and failed tells a running tray to stop keeping the tune") {
    gao::app::RetestOutcome retest;
    retest.ran = true;
    retest.result.test = measured(5742, 190);
    CHECK_FALSE(gao::app::retest_failed(retest));   // it held, and only the save went wrong
    retest.result.test.verdict = gao::Verdict::WrongResult;
    CHECK(gao::app::retest_failed(retest));
    retest.ok = true;
    CHECK_FALSE(gao::app::retest_failed(retest));
    retest.ok = false;
    retest.result.test.verdict = gao::Verdict::Aborted;
    CHECK_FALSE(gao::app::retest_failed(retest));   // stopped: the tune it found is put back
    retest.result.test = gao::StabilityResult{};       // refused before the test: nothing changed
    CHECK_FALSE(gao::app::retest_failed(retest));
}

TEST_CASE("a tune that failed its test can be lowered, not raised") {
    std::string why;
    const gao::Config failed = stepped_config(2, 2);
    const auto down = gao::app::step_base(failed, -1, "617.14", "GPU-1", 1, &why);
    REQUIRE(down);
    CHECK(down->step_down->step == 2);
    CHECK_FALSE(gao::app::step_base(failed, 1, "617.14", "GPU-1", 1, &why));
    CHECK(why == gao::app::decision_text(gao::BootDecision::FailedTest, failed, "617.14"));
    // Every other refusal holds both ways.
    CHECK_FALSE(gao::app::step_base(failed, -1, "617.42", "GPU-1", 1, &why));
    CHECK_FALSE(gao::app::step_base(failed, -1, "617.14", "GPU-2", 1, &why));
    // Strikes only gate the logon apply.
    gao::Config struck = stepped_config(2, 1);
    struck.boot_strikes = gao::kMaxBootStrikes;
    CHECK(gao::app::step_base(struck, 1, "617.14", "GPU-1", 1, &why));
}

TEST_CASE("a step that was refused before anything was written says the card was not changed") {
    CHECK(gao::app::step_not_applied("profile values out of range; nothing applied", true) ==
          "not applied, the card was not changed: profile values out of range; nothing applied");
    CHECK(gao::app::step_not_applied("setting core +150 MHz failed -- card at stock", true) ==
          "not applied: setting core +150 MHz failed -- card at stock");
    CHECK(gao::app::step_not_applied("setting core +150 MHz failed -- reset to stock FAILED, run `gao --reset`", false) ==
          "not applied: setting core +150 MHz failed -- reset to stock FAILED, run `gao --reset`");
}
