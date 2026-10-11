#include "doctest/doctest.h"
#include "core/boot.hpp"
#include "core/objectives.hpp"
#include "core/retest.hpp"
#include <string>
#include <utility>
#include <vector>

using namespace gao;

namespace {
// A card with a tune saved on driver 617.14, now running 617.42.
struct Card {
    int power = 100, core = 0, mem = 0;
    Verdict verdict = Verdict::Stable;   // how the five minutes end
    bool reset_ok = true;
    int recovers = 0;
    std::vector<std::string> events;     // "recover" and "stock", in the order they happen
    std::vector<std::pair<double, int>> probes;                 // seconds, temperature limit
    std::vector<std::pair<std::string, double>> announced;
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
            power_at_probe = power; core_at_probe = core; mem_at_probe = mem;
            StabilityResult s;
            s.verdict = verdict;
            s.seconds = seconds;
            s.score = 5900;
            return s;
        };
        io.measuring = [this](const std::string& what, double seconds) { announced.emplace_back(what, seconds); };
        return io;
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
