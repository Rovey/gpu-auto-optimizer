#include "doctest/doctest.h"
#include "core/boot.hpp"
#include "core/objectives.hpp"
#include "core/retest.hpp"
#include "core/step_down.hpp"
#include <algorithm>
#include <string>
#include <utility>
#include <vector>

using namespace gao;

namespace {
// A card with a tune saved on driver 617.14, now running 617.42.
struct Card {
    int power = 100, core = 0, mem = 0;
    Verdict verdict = Verdict::Stable;   // how the five minutes end
    std::vector<Verdict> script;         // verdicts of the first probes; `verdict` after that
    std::vector<double> scores;          // scores of the first probes; 5900 after that
    double failed_after_s = 0;           // > 0: a probe that does not end STABLE covers this long
    bool reset_ok = true;
    bool rest_ok = true;                 // false: a rest is cut short by a stop request
    int recovers = 0;
    std::vector<std::string> events;     // "recover", "stock", "rest 20", "prepare" and "probe 300", in the order they happen
    std::vector<std::pair<double, int>> probes;                 // seconds, temperature limit
    std::vector<bool> stock_at_probe;                           // was the card at stock when each probe began
    std::vector<std::pair<std::string, double>> announced;
    std::vector<std::string> logged;
    int power_at_probe = 0, core_at_probe = 0, mem_at_probe = 0;

    Config saved() const {
        Config c;
        Profile p;
        p.preset = Preset::BestOfMyGpu;
        p.power_pct = 105;
        p.core_mhz = 135;
        p.mem_mhz = 650;
        p.driver = "617.14";
        p.gpu = "GPU-1";
        c.profile = p;
        c.boot_strikes = 2;
        return c;
    }
    GpuControl gpu() {
        GpuControl g;
        g.set_power_limit = [this](int v) { power = v; return true; };
        g.set_core_offset = [this](int v) { core = v; return true; };
        g.set_mem_offset = [this](int v) { mem = v; return true; };
        g.reset_to_stock = [this] {
            events.push_back("stock");
            if (!reset_ok) return false;
            power = 100; core = 0; mem = 0;
            return true;
        };
        g.recover = [this] { ++recovers; events.push_back("recover"); return true; };
        return g;
    }
    RetestIo io() {
        RetestIo io;
        io.probe = [this](double seconds, int max_temp, double) {
            probes.emplace_back(seconds, max_temp);
            stock_at_probe.push_back(stock());
            events.push_back("probe " + std::to_string(static_cast<int>(seconds)));
            power_at_probe = power; core_at_probe = core; mem_at_probe = mem;
            StabilityResult s;
            s.verdict = probes.size() <= script.size() ? script[probes.size() - 1] : verdict;
            s.seconds = s.verdict != Verdict::Stable && failed_after_s > 0 ? failed_after_s : seconds;
            s.score = probes.size() <= scores.size() ? scores[probes.size() - 1] : 5900;
            return s;
        };
        io.measuring = [this](const std::string& what, double seconds) { announced.emplace_back(what, seconds); };
        io.log = [this](const std::string& line) { logged.push_back(line); };
        io.rest = [this](double seconds) {
            events.push_back("rest " + std::to_string(static_cast<int>(seconds)));
            return rest_ok;
        };
        io.prepare_load = [this] { events.push_back("prepare"); return true; };
        return io;
    }
    // The saved tune, already lowered to `step` (the found tune is saved()).
    Config saved_at(int step) const {
        Config c = saved();
        c.profile = at_step(*c.profile, step, 7500);
        c.profile->step_down->tested = true;
        return c;
    }
    bool logged_line(const std::string& start, const std::string& end) const {
        for (const auto& l : logged)
            if (l.starts_with(start) && l.ends_with(end)) return true;
        return false;
    }
    bool stock() const { return power == 100 && core == 0 && mem == 0; }
};
}

TEST_CASE("after a driver update the saved tune is refused until it has been tested again") {
    Card card;
    const Config c = card.saved();
    CHECK(decide_boot(c, "617.42", "GPU-1") == BootDecision::DriverChanged);
}

TEST_CASE("a tune that holds five minutes on the new driver is applied and belongs to that driver from then on") {
    Card card;
    Config c = card.saved();
    const RetestResult r = retest_saved_tune(c, "617.42", "GPU-1", card.gpu(), card.io());
    REQUIRE(r.ok);
    CHECK(r.reason.empty());
    // The five minutes ran with the tune on the card, under its preset's temperature limit.
    REQUIRE(card.probes.size() == 1);
    CHECK(card.probes[0].first == 300);
    CHECK(card.probes[0].second == objectives_for(Preset::BestOfMyGpu).max_temp_c);
    CHECK(card.power_at_probe == 105);
    CHECK(card.core_at_probe == 135);
    CHECK(card.mem_at_probe == 650);
    // And it stays applied.
    CHECK(card.power == 105);
    CHECK(card.core == 135);
    CHECK(card.mem == 650);
    // The next logon applies it, with a clean slate.
    CHECK(c.profile->driver == "617.42");
    CHECK(c.boot_strikes == 0);
    CHECK(decide_boot(c, "617.42", "GPU-1") == BootDecision::Apply);
    // It says what it is doing, and that it has finished.
    REQUIRE(card.announced.size() == 2);
    CHECK(card.announced[0] == std::make_pair(std::string("Testing the saved tune"), 300.0));
    CHECK(card.announced[1] == std::make_pair(std::string(), 0.0));
}

TEST_CASE("a tune that does not hold on the new driver is not applied and stays refused") {
    Card card;
    card.verdict = Verdict::WrongResult;
    Config c = card.saved();
    const RetestResult r = retest_saved_tune(c, "617.42", "GPU-1", card.gpu(), card.io());
    CHECK_FALSE(r.ok);
    CHECK(r.reason == "the tune did not hold on driver 617.42 (WRONG RESULT)");
    CHECK(r.at_stock);
    CHECK(card.stock());
    CHECK(card.recovers == 0);   // nothing reset the driver: the connections are fine
    // The profile is untouched: still the old driver's, still refused at the next logon.
    CHECK(c.profile->driver == "617.14");
    CHECK(c.boot_strikes == 2);
    CHECK(decide_boot(c, "617.42", "GPU-1") == BootDecision::DriverChanged);
}

TEST_CASE("a driver reset in the test: the connections are made anew before stock is written") {
    Card card;
    card.verdict = Verdict::DeviceLost;
    Config c = card.saved();
    const RetestResult r = retest_saved_tune(c, "617.42", "GPU-1", card.gpu(), card.io());
    CHECK_FALSE(r.ok);
    CHECK(r.at_stock);
    // "stock" once before the tune was applied is apply_profile's own business; the last two are ours.
    REQUIRE(card.events.size() >= 2);
    CHECK(card.events[card.events.size() - 2] == "recover");
    CHECK(card.events.back() == "stock");
    CHECK(c.profile->driver == "617.14");
}

TEST_CASE("a test that was stopped ends at stock and decides nothing") {
    Card card;
    card.verdict = Verdict::Aborted;
    Config c = card.saved();
    const RetestResult r = retest_saved_tune(c, "617.42", "GPU-1", card.gpu(), card.io());
    CHECK_FALSE(r.ok);
    CHECK(r.reason == "aborted");
    CHECK(card.stock());
    CHECK(c.profile->driver == "617.14");
}

TEST_CASE("a reset to stock that fails after a failed test is not claimed as stock") {
    Card card;
    card.verdict = Verdict::TooHot;
    card.reset_ok = false;
    Config c = card.saved();
    const RetestResult r = retest_saved_tune(c, "617.42", "GPU-1", card.gpu(), card.io());
    CHECK_FALSE(r.ok);
    CHECK_FALSE(r.at_stock);
}

TEST_CASE("nothing is tested on another card, without a tune, or on a driver nobody could name") {
    Card card;
    Config c = card.saved();
    CHECK_FALSE(retest_saved_tune(c, "617.42", "GPU-2", card.gpu(), card.io()).ok);
    CHECK_FALSE(retest_saved_tune(c, "", "GPU-1", card.gpu(), card.io()).ok);
    Config none;
    CHECK_FALSE(retest_saved_tune(none, "617.42", "GPU-1", card.gpu(), card.io()).ok);
    CHECK(card.probes.empty());
    CHECK(card.events.empty());   // not even a reset: nothing was written
    CHECK(card.stock());
    CHECK(c.profile->driver == "617.14");
}

TEST_CASE("a failing tune steps down until one holds") {
    Card card;
    card.script = {Verdict::WrongResult, Verdict::WrongResult};
    Config c = card.saved();
    const SteppedRetestResult r = retest_stepping(c, "617.42", "GPU-1", card.gpu(), card.io(), 7500);
    REQUIRE(r.ok);
    CHECK(r.stepped);
    CHECK(r.first_step == 0);
    CHECK(r.final_step == 2);
    CHECK(card.probes.size() == 3);
    REQUIRE(c.profile->step_down);
    CHECK(c.profile->step_down->step == 2);
    CHECK(c.profile->step_down->failed_step == 1);
    CHECK(c.profile->step_down->tested);
    CHECK(c.profile->driver == "617.42");
    CHECK(c.boot_strikes == 0);
    CHECK(card.core == c.profile->core_mhz);   // the held step is on the card
}

TEST_CASE("the found tune that fails becomes step 1") {
    Card card;
    card.script = {Verdict::WrongResult};
    Config c = card.saved();
    const SteppedRetestResult r = retest_stepping(c, "617.42", "GPU-1", card.gpu(), card.io(), 7500);
    REQUIRE(r.ok);
    CHECK(r.final_step == 1);
    REQUIRE(c.profile->step_down);
    CHECK(c.profile->step_down->failed_step == 0);
}

TEST_CASE("no step left") {
    Card card;
    card.verdict = Verdict::WrongResult;
    Config c = card.saved();
    c.profile->core_mhz = 15;   // step 1 drops it to 0, step 2 changes nothing more
    c.profile->mem_mhz = 0;
    const SteppedRetestResult r = retest_stepping(c, "617.42", "GPU-1", card.gpu(), card.io(), 7500);
    CHECK_FALSE(r.ok);
    CHECK(r.stepped);
    CHECK(r.final_step == 1);
    CHECK(card.probes.size() == 2);
    CHECK(r.last.reason.ends_with("; optimize again"));
    REQUIRE(c.profile->step_down);
    CHECK(c.profile->step_down->step == 1);
    CHECK(c.profile->step_down->failed_step == 1);
    CHECK(c.profile->driver == "617.14");
    CHECK(decide_boot(c, "617.14", "GPU-1") == BootDecision::FailedTest);
}

TEST_CASE("a tune that cannot step down at all is left untouched when it fails") {
    Card card;
    card.verdict = Verdict::WrongResult;
    Config c = card.saved();
    c.profile->core_mhz = 0;
    c.profile->mem_mhz = 0;
    const SteppedRetestResult r = retest_stepping(c, "617.42", "GPU-1", card.gpu(), card.io(), 7500);
    CHECK_FALSE(r.ok);
    CHECK_FALSE(r.stepped);
    CHECK(r.last.reason.ends_with("; optimize again"));
    CHECK_FALSE(c.profile->step_down);
}

TEST_CASE("an abort does not step") {
    Card card;
    card.verdict = Verdict::Aborted;
    Config c = card.saved();
    const Profile before = *c.profile;
    const SteppedRetestResult r = retest_stepping(c, "617.42", "GPU-1", card.gpu(), card.io(), 7500);
    CHECK_FALSE(r.ok);
    CHECK_FALSE(r.stepped);
    CHECK(r.last.reason == "aborted");
    CHECK(c.profile->core_mhz == before.core_mhz);
    CHECK_FALSE(c.profile->step_down);
}

TEST_CASE("an abort at a lower step leaves that step, not a failed one") {
    Card card;
    card.script = {Verdict::WrongResult};
    card.verdict = Verdict::Aborted;
    Config c = card.saved();
    const SteppedRetestResult r = retest_stepping(c, "617.42", "GPU-1", card.gpu(), card.io(), 7500);
    CHECK_FALSE(r.ok);
    CHECK(r.stepped);
    CHECK(r.final_step == 1);
    REQUIRE(c.profile->step_down);
    CHECK(c.profile->step_down->step == 1);
    CHECK(c.profile->step_down->failed_step == 0);
}

TEST_CASE("a refusal before the test does not step") {
    Card card;
    Config c = card.saved();
    const SteppedRetestResult r = retest_stepping(c, "617.42", "GPU-2", card.gpu(), card.io(), 7500);
    CHECK_FALSE(r.ok);
    CHECK_FALSE(r.stepped);
    CHECK(card.probes.empty());
    CHECK_FALSE(c.profile->step_down);
}

TEST_CASE("the run screen names the step") {
    Card card;
    card.script = {Verdict::WrongResult};
    Config c = card.saved();
    retest_stepping(c, "617.42", "GPU-1", card.gpu(), card.io(), 7500);
    bool found = false;
    for (const auto& a : card.announced) found = found || a.first == "Testing -5 % (step 1)";
    CHECK(found);
}

// Hardware check 2026-11-05, RTX 5070: a forced reset ended step 1, and step 2
// was loaded at once on a card that was not back. It ended three seconds
// later, so one reset cost two steps.
TEST_CASE("a driver reset at step 1, then a stable step 2: the card rests and is checked first, and the tune ends at step 2") {
    Card card;
    card.script = {Verdict::DeviceLost};
    card.failed_after_s = 93;
    Config c = card.saved_at(1);
    const SteppedRetestResult r = retest_stepping(c, "617.42", "GPU-1", card.gpu(), card.io(), 7500);
    REQUIRE(r.ok);
    CHECK(r.first_step == 1);
    CHECK(r.final_step == 2);
    REQUIRE(c.profile->step_down);
    CHECK(c.profile->step_down->step == 2);
    CHECK(c.profile->step_down->failed_step == 1);
    CHECK(c.profile->step_down->tested);
    // Step 1, the health probe (5 s), step 2.
    REQUIRE(card.probes.size() == 3);
    CHECK(card.probes[0].first == 300);
    CHECK(card.probes[1].first == kHealthProbeS);
    CHECK(card.probes[2].first == 300);
    CHECK(card.stock_at_probe[1]);   // the health check runs at stock
    CHECK_FALSE(card.stock_at_probe[2]);
    // Reconnect and stock, then the rest and a fresh load, before anything loads the card again.
    const auto at = [&](const std::string& e) { return std::find(card.events.begin(), card.events.end(), e) - card.events.begin(); };
    CHECK(at("recover") < at("rest 20"));
    CHECK(at("rest 20") < at("prepare"));
    CHECK(at("prepare") < at("probe 5"));
    CHECK(card.recovers == 1);
}

TEST_CASE("a failed step's verdict is in the log") {
    Card card;
    card.script = {Verdict::DeviceLost};
    card.failed_after_s = 93;
    Config c = card.saved_at(1);
    retest_stepping(c, "617.42", "GPU-1", card.gpu(), card.io(), 7500);
    CHECK(card.logged_line("  DEVICE LOST score=5900 it/s", "(93 s)"));
}

TEST_CASE("a failed step that did not reset the driver is logged too, and tested next without a rest") {
    Card card;
    card.script = {Verdict::WrongResult};
    Config c = card.saved();
    retest_stepping(c, "617.42", "GPU-1", card.gpu(), card.io(), 7500);
    CHECK(card.logged_line("  WRONG RESULT score=5900 it/s", "(300 s)"));
    CHECK(std::find(card.events.begin(), card.events.end(), "rest 20") == card.events.end());
    CHECK(card.probes.size() == 2);
}

TEST_CASE("a second driver reset in the same stepped retest ends it at stock") {
    Card card;
    card.script = {Verdict::DeviceLost, Verdict::Stable, Verdict::DeviceLost};   // step 1, health, step 2
    Config c = card.saved_at(1);
    const SteppedRetestResult r = retest_stepping(c, "617.42", "GPU-1", card.gpu(), card.io(), 7500);
    CHECK_FALSE(r.ok);
    CHECK(r.stepped);
    CHECK(card.probes.size() == 3);   // step 3 is not tried
    CHECK(r.final_step == 2);
    CHECK(r.last.reason.find("the driver reset twice") != std::string::npos);
    CHECK(r.last.reason.ends_with("; optimize again"));
    CHECK(r.last.at_stock);
    CHECK(card.stock());
    REQUIRE(c.profile->step_down);
    CHECK(c.profile->step_down->step == 2);
    CHECK(c.profile->step_down->failed_step == 2);
    CHECK(c.profile->driver == "617.14");
    CHECK(decide_boot(c, "617.14", "GPU-1") == BootDecision::FailedTest);
}

TEST_CASE("a driver reset in the health check is the second one") {
    Card card;
    card.script = {Verdict::DeviceLost, Verdict::DeviceLost};   // step 1, health
    Config c = card.saved_at(1);
    const SteppedRetestResult r = retest_stepping(c, "617.42", "GPU-1", card.gpu(), card.io(), 7500);
    CHECK_FALSE(r.ok);
    CHECK(card.probes.size() == 2);
    CHECK(r.last.reason.ends_with("; optimize again"));
    CHECK(card.recovers == 2);   // the connections are made anew again before stock is written
    CHECK(card.events.back() == "stock");
    CHECK(r.last.at_stock);
    CHECK(r.final_step == 1);
    REQUIRE(c.profile->step_down);
    CHECK(c.profile->step_down->step == 1);
    CHECK(c.profile->step_down->failed_step == 1);
    CHECK(decide_boot(c, "617.14", "GPU-1") == BootDecision::FailedTest);
}

TEST_CASE("a card that is slow after the reset gets another rest, measured against its stock score") {
    Card card;
    card.script = {Verdict::DeviceLost};
    card.scores = {5900, 3000, 5800};   // step 1, a slow health probe, a good one; step 2 at 5900
    Config c = card.saved_at(1);
    c.profile->against_stock = AgainstStock{MeasuredRun{.score = 5786}, MeasuredRun{.score = 5970}};
    const SteppedRetestResult r = retest_stepping(c, "617.42", "GPU-1", card.gpu(), card.io(), 7500);
    REQUIRE(r.ok);
    CHECK(r.final_step == 2);
    REQUIRE(card.probes.size() == 4);
    CHECK(std::count(card.events.begin(), card.events.end(), "rest 20") == 2);
    CHECK(std::count(card.events.begin(), card.events.end(), "prepare") == 1);   // once per recovery
}

TEST_CASE("a card that does not get back to its stock score ends the stepped retest") {
    Card card;
    card.script = {Verdict::DeviceLost};
    card.scores = {5900, 3000, 3000, 3000};
    Config c = card.saved_at(1);
    c.profile->against_stock = AgainstStock{MeasuredRun{.score = 5786}, MeasuredRun{.score = 5970}};
    const SteppedRetestResult r = retest_stepping(c, "617.42", "GPU-1", card.gpu(), card.io(), 7500);
    CHECK_FALSE(r.ok);
    CHECK(card.probes.size() == 1 + kHealthTries);
    CHECK(r.last.reason.find("did not recover after a driver reset") != std::string::npos);
    CHECK(r.last.reason.ends_with("; optimize again"));
    CHECK(card.stock());
    REQUIRE(c.profile->step_down);
    CHECK(c.profile->step_down->failed_step == 1);
}

TEST_CASE("a stop request during the rest after a reset leaves the next step, not the failed one") {
    Card card;
    card.script = {Verdict::DeviceLost};
    card.rest_ok = false;
    Config c = card.saved_at(1);
    const SteppedRetestResult r = retest_stepping(c, "617.42", "GPU-1", card.gpu(), card.io(), 7500);
    CHECK_FALSE(r.ok);
    CHECK(r.stepped);
    CHECK(r.last.reason == "aborted");
    CHECK(r.last.test.verdict == Verdict::Aborted);
    CHECK(card.probes.size() == 1);
    CHECK(r.final_step == 2);
    REQUIRE(c.profile->step_down);
    CHECK(c.profile->step_down->step == 2);
    CHECK(c.profile->step_down->failed_step == 1);
}

TEST_CASE("a step that holds its test is no longer a failed step") {
    Card card;
    Config c = card.saved_at(2);
    c.profile->step_down->tested = false;
    c.profile->step_down->failed_step = 2;   // failed before, holds now
    const SteppedRetestResult r = retest_stepping(c, "617.42", "GPU-1", card.gpu(), card.io(), 7500);
    REQUIRE(r.ok);
    CHECK(r.final_step == 2);
    REQUIRE(c.profile->step_down);
    CHECK(c.profile->step_down->failed_step == 1);
    CHECK(decide_boot(c, "617.42", "GPU-1") == BootDecision::Apply);
    Config d = card.saved_at(2);
    d.profile->step_down->failed_step = 1;   // below the step that holds: kept
    REQUIRE(retest_stepping(d, "617.42", "GPU-1", card.gpu(), card.io(), 7500).ok);
    CHECK(d.profile->step_down->failed_step == 1);
}

TEST_CASE("the first test names its step when the saved tune is lowered already") {
    Card card;
    Config c = card.saved_at(1);
    retest_stepping(c, "617.42", "GPU-1", card.gpu(), card.io(), 7500);
    bool named = false;
    for (const auto& a : card.announced) named = named || a.first == "Testing -5 % (step 1)";
    CHECK(named);
    CHECK(card.logged_line("testing -5 % (step 1)", "(step 1)"));
    // At step 0 it is the saved tune.
    Card found;
    Config f = found.saved();
    retest_stepping(f, "617.42", "GPU-1", found.gpu(), found.io(), 7500);
    REQUIRE_FALSE(found.announced.empty());
    CHECK(found.announced[0].first == "Testing the saved tune");
}

TEST_CASE("a failed step's verdict is logged once when the card does not come back") {
    for (const std::vector<double>& scores : {std::vector<double>{5900}, std::vector<double>{5900, 3000, 3000, 3000}}) {
        Card card;
        card.script = {Verdict::DeviceLost, scores.size() == 1 ? Verdict::DeviceLost : Verdict::Stable};
        card.scores = scores;
        card.failed_after_s = 93;
        Config c = card.saved_at(1);
        c.profile->against_stock = AgainstStock{MeasuredRun{.score = 5786}, MeasuredRun{.score = 5970}};
        const SteppedRetestResult r = retest_stepping(c, "617.42", "GPU-1", card.gpu(), card.io(), 7500);
        CHECK_FALSE(r.ok);
        // The RESULT lines show the last test; the log does not show it before them as well.
        CHECK_FALSE(card.logged_line("  DEVICE LOST score=", "(93 s)"));
        CHECK(r.last.test.verdict == Verdict::DeviceLost);
    }
}

TEST_CASE("a stop request during the health probe leaves the next step, not the failed one") {
    Card card;
    card.script = {Verdict::DeviceLost, Verdict::Aborted};   // step 1, the health probe
    card.failed_after_s = 93;
    Config c = card.saved_at(1);
    const SteppedRetestResult r = retest_stepping(c, "617.42", "GPU-1", card.gpu(), card.io(), 7500);
    CHECK_FALSE(r.ok);
    CHECK(r.stepped);
    CHECK(r.last.reason == "aborted");
    CHECK(r.last.test.verdict == Verdict::Aborted);
    CHECK(card.probes.size() == 2);
    CHECK(r.final_step == 2);
    REQUIRE(c.profile->step_down);
    CHECK(c.profile->step_down->step == 2);
    CHECK(c.profile->step_down->failed_step == 1);
    CHECK(card.stock());
    // Step 1's verdict is not in the result: it is in the log.
    CHECK(card.logged_line("  DEVICE LOST score=5900 it/s", "(93 s)"));
}

TEST_CASE("a health probe that computes almost nothing counts as the second reset") {
    Card card;
    card.script = {Verdict::DeviceLost};
    card.scores = {5900, 1000};   // under a quarter of the stock score: STALLED
    Config c = card.saved_at(1);
    c.profile->against_stock = AgainstStock{MeasuredRun{.score = 5786}, MeasuredRun{.score = 5970}};
    const SteppedRetestResult r = retest_stepping(c, "617.42", "GPU-1", card.gpu(), card.io(), 7500);
    CHECK_FALSE(r.ok);
    CHECK(card.probes.size() == 2);
    CHECK(r.last.reason.find("(STALLED)") != std::string::npos);
    CHECK(r.last.reason.ends_with("; optimize again"));
    CHECK(card.recovers == 2);
}
