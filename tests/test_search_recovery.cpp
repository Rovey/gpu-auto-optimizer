#include "doctest/doctest.h"
#include "search_fixture.hpp"

TEST_CASE("a probe that passes at a fraction of the baseline score is stalled, not stable") {
    // Measured on an RTX 5070: at +480 the card stopped computing, the probe
    // found no wrong value and no lost device, and scored 167 against 5869.
    Run run;
    run.card.stall_core_from = 135;
    const auto r = run.go(Preset::BestOfMyGpu);
    REQUIRE(r.ok);
    CHECK(r.core_max_stable == 120);
    // Without a reconnect the reset budget does not apply: the stall is
    // counted, the confirm probe runs at the edge with no back-off, and the
    // log does not speak of a reset.
    CHECK(r.driver_resets == 1);
    CHECK(r.core_confirmed == 120);
    CHECK_FALSE(run.logged("the driver reset"));
    bool journaled = false;
    for (const auto& l : run.card.journal) journaled |= l.find("STALLED") != std::string::npos;
    CHECK(journaled);
    bool logged = false;
    for (const auto& m : run.log) logged |= m.find("STALLED") != std::string::npos;
    CHECK(logged);
}

TEST_CASE("a low score at a low power limit is not a stall") {
    // The quiet preset probes power limits down to the card's minimum; the
    // fixture's score at 50 % power is 56 % of the baseline.
    Run run;
    const auto r = run.go(Preset::Quiet);
    REQUIRE(r.ok);
    for (const auto& m : run.log) CHECK(m.find("STALLED") == std::string::npos);
}

TEST_CASE("without a reconnect a driver reset still ends the run cleanly") {
    // Today's behaviour, kept for a GpuControl that cannot reconnect.
    Run run;
    run.card.lost_core_from = 135;
    const auto r = run.go(Preset::BestOfMyGpu);
    CHECK_FALSE(r.ok);
    CHECK(r.reason.find("failed") != std::string::npos);
    CHECK_FALSE(r.stock_restored);
    Journal reread(run.card.journal, [](const std::string&) { return true; });
    CHECK(reread.freezes().empty());
}

TEST_CASE("a driver that does not come back ends the run without opening another journal entry") {
    Run run;
    run.card.with_recover = true;
    run.card.recover_fails = true;
    run.card.lost_core_from = 135;
    const auto r = run.go(Preset::BestOfMyGpu);
    CHECK_FALSE(r.ok);
    CHECK(r.reason == "the driver did not come back after a reset");
    CHECK_FALSE(r.stock_restored);
    CHECK(run.card.recover_calls == 1);   // a reconnect that failed is not tried again on the way out
    CHECK(run.card.count("\"begin\"") == run.card.begins_when_recover_failed);   // nothing was journaled after the failed reconnect
    check_journal_rules(run);
}

TEST_CASE("without a reconnect a probe without telemetry ends the run at once") {
    Run run;
    run.card.blind_probes = 1;
    const auto r = run.go(Preset::BestOfMyGpu);
    CHECK_FALSE(r.ok);
    CHECK(r.reason == "lost telemetry");
}

TEST_CASE("a run stopped while the card is stale reconnects before it resets to stock") {
    // Probe 32 is core +135, the lost candidate (23 probes up to the memory
    // confirm, then core +15 .. +120); the stop request is seen before the
    // next candidate (the confirm probe at +60) and before its recovery gate,
    // with the old connections still dead.
    Run run;
    run.card.with_recover = true;
    run.card.lost_core_from = 135;
    run.abort_after_probes = 32;
    const auto r = run.go(Preset::BestOfMyGpu);
    CHECK_FALSE(r.ok);
    CHECK(r.reason == "aborted");
    CHECK(run.card.probes == 32);
    CHECK(run.logged("core +135 / mem +550: DEVICE LOST"));
    CHECK(r.stock_restored);
    CHECK(run.card.core == 0);
    CHECK(run.card.mem == 0);
    CHECK(run.card.power == 100);
    CHECK_FALSE(run.card.stale);
    CHECK(run.card.recover_calls == 1);
    CHECK(run.card.rests == 0);
    CHECK(count_events(run.card, "health") == 0);
    CHECK(begins_with_only(run.card, "core", "mem") == std::vector<int>{15, 30, 45, 60, 75, 90, 105, 120, 135});
    check_journal_rules(run);
}

// The probes of a default best-preset run, in order: 1 baseline; 2-5 power
// (85, 105, 115, 120 %); 6-22 memory (+50, +100, ... +850); 23 memory confirm
// (+800); 24-34 core (+15, +30, ... +165); 35 core confirm (+150); 36 soak.
TEST_CASE("the probe order the driver-reset tests rely on") {
    Run run;
    const auto r = run.go(Preset::BestOfMyGpu);
    REQUIRE(r.ok);
    CHECK(run.card.probes == 36);
    // Each probe as "seconds:power:core:mem".
    std::vector<std::string> want{"30:100:0:0", "20:85:0:0", "20:105:0:0", "20:115:0:0", "20:120:0:0"};
    for (int mem = 50; mem <= 850; mem += 50) want.push_back("3:115:0:" + std::to_string(mem));
    want.push_back("30:115:0:800");
    for (int core = 15; core <= 165; core += 15) want.push_back("3:115:" + std::to_string(core) + ":550");
    want.push_back("30:115:150:550");
    want.push_back("300:115:105:550");
    REQUIRE(want.size() == 36);
    CHECK(run.card.probe_order == want);
}

TEST_CASE("a stop request during a failing reconnect is reported as aborted") {
    Run run;
    run.card.with_recover = true;
    run.card.recover_fails = true;
    run.card.abort_in_recover = true;
    run.card.lost_core_from = 135;
    const auto r = run.go(Preset::BestOfMyGpu);
    CHECK_FALSE(r.ok);
    CHECK(r.reason == "aborted");
    CHECK(run.card.recover_calls == 1);
    check_journal_rules(run);
}

TEST_CASE("a stale connection at the start is reconnected before the first reset") {
    Run run;
    run.card.with_recover = true;
    run.card.stale = true;
    const auto r = run.go(Preset::BestOfMyGpu);
    REQUIRE(r.ok);
    CHECK(run.card.recover_calls == 1);
    CHECK(r.core_max_stable == 150);
    check_journal_rules(run);
}

TEST_CASE("a start that cannot reset to stock even after a reconnect says so") {
    Run dead;   // the reconnect fails
    dead.card.with_recover = true;
    dead.card.recover_fails = true;
    dead.card.stale = true;
    const auto a = dead.go(Preset::BestOfMyGpu);
    CHECK_FALSE(a.ok);
    CHECK(a.reason == "could not reset to stock");
    CHECK_FALSE(a.stock_restored);
    CHECK(dead.logged("reset FAILED"));
    CHECK(dead.card.recover_calls == 1);
    CHECK(dead.card.probes == 0);

    Run stuck;   // the reconnect works, the reset keeps failing
    stuck.card.with_recover = true;
    stuck.card.reset_fails_from = 0;
    const auto b = stuck.go(Preset::BestOfMyGpu);
    CHECK_FALSE(b.ok);
    CHECK(b.reason == "could not reset to stock");
    CHECK_FALSE(b.stock_restored);
    CHECK(stuck.logged("reset FAILED"));
    CHECK(stuck.card.recover_calls == 1);
    CHECK(stuck.card.probes == 0);
}

// The recovery gate. Unless stated, the card loses the device at core +135:
// probe 32 of the run, the only driver reset, between the climb and the
// confirm step.

TEST_CASE("after a reset the search reconnects, goes to stock, rests and proves the card is back before the next candidate") {
    Run run = lost_at_135();
    const auto r = run.go(Preset::BestOfMyGpu);
    REQUIRE(r.ok);
    CHECK(first_events_after_lost(run.card, 5) == Events{"recover", "stock", "rest", "health", "begin"});
    CHECK(run.card.rests == 1);
    CHECK(count_events(run.card, "health") == 1);
    CHECK(r.driver_resets == 1);
    CHECK(run.card.recovers_with_entry_open == 0);
    CHECK(run.logged("resting 20 s before the card is loaded again"));
    CHECK(run.logged("checking that the card is back: 5 s at stock"));
    CHECK(run.count_logged("health: ") == 1);
    CHECK(run.logged("health: STABLE"));
    check_journal_rules(run);
}

TEST_CASE("a card that needs a second rest is accepted") {
    Run run = lost_at_135();
    run.card.sick_health_probes = 1;
    const auto r = run.go(Preset::BestOfMyGpu);
    REQUIRE(r.ok);
    CHECK(run.card.rests == 2);
    CHECK(count_events(run.card, "health") == 2);
    // No candidate between the two health probes.
    CHECK(first_events_after_lost(run.card, 7) == Events{"recover", "stock", "rest", "health", "rest", "health", "begin"});
    CHECK(r.driver_resets == 1);
    CHECK(run.card.recover_calls == 1);
    check_journal_rules(run);
}

TEST_CASE("a card that never recovers ends the run at stock without opening another entry") {
    Run run = lost_at_135();
    run.card.sick_health_probes = 99;
    const auto r = run.go(Preset::BestOfMyGpu);
    CHECK_FALSE(r.ok);
    CHECK(r.reason == "the card did not recover after a driver reset");
    CHECK(run.card.rests == 3);
    CHECK(count_events(run.card, "health") == 3);
    // Three rests and probes, no entry after the lost candidate's, and one
    // reset to stock on the way out without another reconnect.
    CHECK(events_after_lost(run.card) == Events{"recover", "stock", "rest", "health", "rest", "health", "rest", "health", "stock"});
    CHECK(r.stock_restored);
    CHECK(run.card.core == 0);
    CHECK(run.card.mem == 0);
    CHECK(run.card.power == 100);
    CHECK(r.driver_resets == 1);
    // The connections were good on the way out: no reconnect, and no line that says one was made.
    CHECK_FALSE(run.logged("reconnecting to the driver to restore stock"));
    check_journal_rules(run);
}

TEST_CASE("a driver reset during the health probe ends the run at once") {
    Run run = lost_at_135();
    run.card.health_verdict = Verdict::DeviceLost;
    const auto r = run.go(Preset::BestOfMyGpu);
    CHECK_FALSE(r.ok);
    CHECK(r.reason == "the card was not usable when checked after a driver reset (DEVICE LOST)");
    CHECK(r.driver_resets == 2);
    CHECK(count_events(run.card, "health") == 1);
    CHECK(run.card.rests == 1);
    // After the health probe: no entry, no rest, no second health probe; one
    // reconnect and one reset to stock on the way out.
    CHECK(events_after_lost(run.card) == Events{"recover", "stock", "rest", "health", "recover", "stock"});
    CHECK(run.card.recover_calls == 2);
    CHECK(r.stock_restored);
    CHECK(run.card.core == 0);
    CHECK(run.card.mem == 0);
    CHECK(run.card.power == 100);
    CHECK_FALSE(run.card.stale);
    CHECK(run.logged("health: DEVICE LOST"));
    // The reconnect on the way out can take half a minute: the log says so first.
    CHECK(run.count_logged("reconnecting to the driver to restore stock") == 1);
    check_journal_rules(run);
}

TEST_CASE("the stall floor of every probe is a quarter of the baseline score, except the baseline and the health probe") {
    Run run = lost_at_135();
    const auto r = run.go(Preset::BestOfMyGpu);
    REQUIRE(r.ok);
    REQUIRE(r.baseline.score > 0);
    const double floor = kStalledScore * r.baseline.score;
    const auto& probes = run.card.probe_floors;
    REQUIRE(probes.size() == static_cast<std::size_t>(run.card.probes));
    // The baseline has nothing to be measured against yet.
    CHECK(probes.front().first == 30);
    CHECK(probes.front().second == 0);
    int power = 0, clock = 0, confirm = 0, soak = 0, health = 0;
    for (std::size_t i = 1; i < probes.size(); ++i) {
        const double seconds = probes[i].first, stall_below = probes[i].second;
        CAPTURE(i);
        CAPTURE(seconds);
        if (seconds == 5) {
            // The health probe is judged on its whole run, after it ends.
            ++health;
            CHECK(stall_below == 0);
            continue;
        }
        if (seconds == 20) ++power;
        else if (seconds == 3) ++clock;
        else if (seconds == 30) ++confirm;
        else if (seconds == 300) ++soak;
        else FAIL_CHECK("a probe of an unexpected length");
        CHECK(stall_below == floor);
    }
    // Every kind of probe ran: 4 power steps, 17 memory and 9 core candidates,
    // the memory and the core confirm probe, one health probe and the soak.
    CHECK(power == 4);
    CHECK(clock == 26);
    CHECK(confirm == 2);
    CHECK(health == 1);
    CHECK(soak == 1);
}

TEST_CASE("a blind health probe ends the run at once") {
    Run run = lost_at_135();
    run.card.health_verdict = Verdict::NoTelemetry;
    const auto r = run.go(Preset::BestOfMyGpu);
    CHECK_FALSE(r.ok);
    CHECK(r.reason == "the card was not usable when checked after a driver reset (NO TELEMETRY)");
    CHECK(r.driver_resets == 2);
    CHECK(count_events(run.card, "health") == 1);
    CHECK(run.card.rests == 1);
    CHECK(events_after_lost(run.card) == Events{"recover", "stock", "rest", "health", "recover", "stock"});
    CHECK(r.stock_restored);
    check_journal_rules(run);
}

TEST_CASE("a health probe that computes almost nothing ends the run at once") {
    Run run = lost_at_135();
    run.card.health_stalls = true;
    const auto r = run.go(Preset::BestOfMyGpu);
    CHECK_FALSE(r.ok);
    CHECK(r.reason == "the card computed almost nothing after a driver reset");
    CHECK(r.driver_resets == 2);
    CHECK(count_events(run.card, "health") == 1);
    CHECK(run.card.rests == 1);
    CHECK(events_after_lost(run.card) == Events{"recover", "stock", "rest", "health", "recover", "stock"});
    CHECK(run.logged("health: STALLED"));
    CHECK(r.stock_restored);
    CHECK(run.card.core == 0);
    check_journal_rules(run);
}

TEST_CASE("a health probe that only computes wrong or runs hot is retried, not counted") {
    for (const Verdict v : {Verdict::WrongResult, Verdict::TooHot}) {
        Run run = lost_at_135();
        run.card.health_verdict = v;
        const auto r = run.go(Preset::BestOfMyGpu);
        REQUIRE(r.ok);
        CHECK(r.driver_resets == 1);
        CHECK(count_events(run.card, "health") == 2);
        CHECK(run.card.rests == 2);
        CHECK(first_events_after_lost(run.card, 7) == Events{"recover", "stock", "rest", "health", "rest", "health", "begin"});
        CHECK(run.card.recover_calls == 1);
        check_journal_rules(run);
    }
}

TEST_CASE("a stock write that fails while recovering ends the run without a rest or a health probe") {
    Run run = lost_at_135();
    run.card.stock_write_fails_after_recover = true;
    const auto r = run.go(Preset::BestOfMyGpu);
    CHECK_FALSE(r.ok);
    CHECK(r.reason == "could not set the card to stock while recovering");
    CHECK(run.card.rests == 0);
    CHECK(count_events(run.card, "health") == 0);
    // The failed write pushes nothing; the way out reconnects once and resets.
    CHECK(events_after_lost(run.card) == Events{"recover", "recover", "stock"});
    CHECK(r.driver_resets == 1);
    CHECK(r.stock_restored);
    check_journal_rules(run);
}

TEST_CASE("a driver that does not come back is not asked twice") {
    Run run = lost_at_135();
    run.card.recover_fails = true;
    const auto r = run.go(Preset::BestOfMyGpu);
    CHECK_FALSE(r.ok);
    CHECK(r.reason == "the driver did not come back after a reset");
    CHECK(run.card.recover_calls == 1);
    CHECK(run.card.rests == 0);
    CHECK(count_events(run.card, "health") == 0);
    CHECK(events_after_lost(run.card) == Events{"recover"});
    CHECK_FALSE(r.stock_restored);
    CHECK(r.driver_resets == 1);
    check_journal_rules(run);
}

TEST_CASE("an abort while resting ends as aborted") {
    Run run = lost_at_135();
    run.card.abort_during_rest = true;
    const auto r = run.go(Preset::BestOfMyGpu);
    CHECK_FALSE(r.ok);
    CHECK(r.reason == "aborted");
    CHECK(run.card.core == 0);
    CHECK(run.card.mem == 0);
    CHECK(run.card.power == 100);
    CHECK(count_events(run.card, "health") == 0);
    CHECK(events_after_lost(run.card) == Events{"recover", "stock", "rest", "stock"});
    CHECK(r.stock_restored);
    check_journal_rules(run);
}

namespace {
Run lost_at_135_with_prepare() {
    Run run = lost_at_135();
    run.card.with_prepare = true;
    return run;
}
}

TEST_CASE("the load is prepared after the rest and before the health probe") {
    Run run = lost_at_135_with_prepare();
    const auto r = run.go(Preset::BestOfMyGpu);
    REQUIRE(r.ok);
    CHECK(first_events_after_lost(run.card, 6) == Events{"recover", "stock", "rest", "prepare", "health", "begin"});
    CHECK(run.card.prepares == 1);
    CHECK(run.card.prepares_with_entry_open == 0);
    CHECK(r.driver_resets == 1);
    CHECK(run.logged("preparing the stress load"));
    check_journal_rules(run);
}

TEST_CASE("the load is prepared at no other time") {
    Run run;
    run.card.with_recover = true;
    run.card.with_prepare = true;
    const auto r = run.go(Preset::BestOfMyGpu);
    REQUIRE(r.ok);
    CHECK(run.card.prepares == 0);
}

TEST_CASE("a slow health probe does not rebuild the load") {
    Run run = lost_at_135_with_prepare();
    run.card.sick_health_probes = 1;
    const auto r = run.go(Preset::BestOfMyGpu);
    REQUIRE(r.ok);
    CHECK(first_events_after_lost(run.card, 8) ==
          Events{"recover", "stock", "rest", "prepare", "health", "rest", "health", "begin"});
    CHECK(run.card.prepares == 1);
}

TEST_CASE("a preparation that fails once costs a rest, not a probe") {
    Run run = lost_at_135_with_prepare();
    run.card.prepare_fails = 1;
    const auto r = run.go(Preset::BestOfMyGpu);
    REQUIRE(r.ok);
    CHECK(first_events_after_lost(run.card, 8) ==
          Events{"recover", "stock", "rest", "prepare", "rest", "prepare", "health", "begin"});
    CHECK(count_events(run.card, "health") == 1);
    CHECK(r.driver_resets == 1);
    CHECK(run.logged("the stress load is not ready yet"));
    check_journal_rules(run);
}

TEST_CASE("a load that cannot be prepared ends the run without a health probe") {
    Run run = lost_at_135_with_prepare();
    run.card.prepare_fails = 99;
    const auto r = run.go(Preset::BestOfMyGpu);
    CHECK_FALSE(r.ok);
    CHECK(r.reason == "the card did not recover after a driver reset");
    CHECK(events_after_lost(run.card) ==
          Events{"recover", "stock", "rest", "prepare", "rest", "prepare", "rest", "prepare", "stock"});
    CHECK_FALSE(run.logged("checking that the card is back"));
    CHECK(r.stock_restored);
    CHECK(run.card.core == 0);
    CHECK(r.driver_resets == 1);
    Journal reread(run.card.journal, [](const std::string&) { return true; });
    CHECK(reread.freezes().empty());
}

TEST_CASE("a stop request during the preparation ends the run as aborted") {
    Run run = lost_at_135_with_prepare();
    run.card.abort_after_prepare = true;
    const auto r = run.go(Preset::BestOfMyGpu);
    CHECK_FALSE(r.ok);
    CHECK(r.reason == "aborted");
    CHECK(events_after_lost(run.card) == Events{"recover", "stock", "rest", "prepare", "stock"});
    CHECK(r.stock_restored);
    CHECK(run.card.core == 0);
}

TEST_CASE("the stale-start rest does not prepare the load") {
    Run run;
    run.card.with_recover = true;
    run.card.with_prepare = true;
    run.card.stale = true;
    const auto r = run.go(Preset::BestOfMyGpu);
    REQUIRE(r.ok);
    CHECK(run.card.prepares == 0);
    REQUIRE(run.card.events.size() >= 3);
    CHECK(Events(run.card.events.begin(), run.card.events.begin() + 3) == Events{"recover", "stock", "rest"});
}

TEST_CASE("without a preparation callback the gate is unchanged") {
    Run run = lost_at_135();
    const auto r = run.go(Preset::BestOfMyGpu);
    REQUIRE(r.ok);
    CHECK(first_events_after_lost(run.card, 4) == Events{"recover", "stock", "rest", "health"});
    CHECK(run.card.prepares == 0);
}

TEST_CASE("without a rest callback failed preparations follow each other") {
    Run run = lost_at_135_with_prepare();
    run.no_rest = true;
    run.card.prepare_fails = 99;
    const auto r = run.go(Preset::BestOfMyGpu);
    CHECK_FALSE(r.ok);
    CHECK(r.reason == "the card did not recover after a driver reset");
    CHECK(run.card.prepares == 3);
    CHECK(run.card.rests == 0);
}

TEST_CASE("the health probe is not journaled") {
    Run run = lost_at_135();
    const auto r = run.go(Preset::BestOfMyGpu);
    REQUIRE(r.ok);
    CHECK(count_events(run.card, "health") == 1);
    // Every clock candidate logs "core +C / mem +M: ...", every soak attempt
    // "soak: 300 s at ...": one journal entry each, and none for the health probe.
    const int entries = run.count_logged(" / mem +") + run.count_logged("soak: 300 s at");
    CHECK(entries > 0);
    CHECK(run.card.count("\"begin\"") == entries);
    CHECK(run.card.count("\"complete\"") == entries);
}

TEST_CASE("without a rest callback the gate still proves the card is back") {
    Run run = lost_at_135();
    run.no_rest = true;
    const auto r = run.go(Preset::BestOfMyGpu);
    REQUIRE(r.ok);
    CHECK(count_events(run.card, "health") == 1);
    CHECK(run.card.rests == 0);
    CHECK(first_events_after_lost(run.card, 4) == Events{"recover", "stock", "health", "begin"});
    CHECK_FALSE(run.logged("resting"));   // nothing waited, so the log does not say it did
    check_journal_rules(run);
}

TEST_CASE("a connection that is stale at the start gets a rest before the baseline") {
    Run run;
    run.card.with_recover = true;
    run.card.stale = true;
    const auto r = run.go(Preset::BestOfMyGpu);
    REQUIRE(r.ok);
    // The first reset fails on the stale connection and pushes nothing.
    REQUIRE(run.card.events.size() >= 4);
    CHECK(Events(run.card.events.begin(), run.card.events.begin() + 4) == Events{"recover", "stock", "rest", "begin"});
    CHECK(run.card.rests == 1);
    CHECK(run.logged("resting 20 s before the card is loaded again"));
    CHECK(count_events(run.card, "health") == 0);
    CHECK(r.driver_resets == 0);
    CHECK(run.card.probes == 36);   // the run of a card with no reset
    check_journal_rules(run);
}

TEST_CASE("an abort while resting at the start ends before the baseline") {
    Run run;
    run.card.with_recover = true;
    run.card.stale = true;
    run.card.abort_during_rest = true;
    const auto r = run.go(Preset::BestOfMyGpu);
    CHECK_FALSE(r.ok);
    CHECK(r.reason == "aborted");
    CHECK(run.card.probes == 0);
    CHECK(r.stock_restored);
    CHECK(run.card.recover_calls == 1);
}

TEST_CASE("without a reconnect nothing changes") {
    Run run;
    run.card.lost_core_from = 135;
    const auto r = run.go(Preset::BestOfMyGpu);
    CHECK_FALSE(r.ok);
    CHECK(r.reason.find("failed") != std::string::npos);
    CHECK_FALSE(r.stock_restored);
    CHECK(run.card.rests == 0);
    CHECK(count_events(run.card, "health") == 0);
    CHECK(count_events(run.card, "recover") == 0);
    Journal reread(run.card.journal, [](const std::string&) { return true; });
    CHECK(reread.freezes().empty());
}

TEST_CASE("a stop request during the reconnect after a failed write is reported as aborted") {
    Run run = recovering();
    run.card.abort_in_recover = true;
    run.card.fail_core_set_at = 150;
    const auto r = run.go(Preset::BestOfMyGpu);
    CHECK_FALSE(r.ok);
    CHECK(r.reason == "aborted");
    CHECK(r.stock_restored);
    CHECK(run.card.core == 0);
    CHECK(run.card.mem == 0);
    CHECK(run.card.power == 100);
    CHECK(run.card.count("SET FAILED") == 1);
    // The reconnect of the gate, stock, and then nothing but the reset on the way out.
    CHECK(events_after_lost(run.card) == Events{"recover", "stock", "stock"});
    check_journal_rules(run);
}

TEST_CASE("a stop request that arrived during the reconnect ends the run before the rest") {
    Run run = recovering();
    run.card.lost_core_from = 135;
    run.card.abort_in_recover = true;   // the reconnect itself succeeds
    const auto r = run.go(Preset::BestOfMyGpu);
    CHECK_FALSE(r.ok);
    CHECK(r.reason == "aborted");
    CHECK(run.card.rests == 0);
    CHECK_FALSE(run.logged("resting"));
    CHECK(count_events(run.card, "health") == 0);
    CHECK(events_after_lost(run.card) == Events{"recover", "stock", "stock"});
    CHECK(r.stock_restored);
    CHECK(run.card.core == 0);
    CHECK(run.card.mem == 0);
    check_journal_rules(run);
}

TEST_CASE("a health probe that is stopped ends the run as aborted") {
    Run run = recovering();
    run.card.lost_core_from = 135;
    run.card.abort_in_probe_s = 5;
    const auto r = run.go(Preset::BestOfMyGpu);
    CHECK_FALSE(r.ok);
    CHECK(r.reason == "aborted");
    CHECK(r.driver_resets == 1);
    CHECK(count_events(run.card, "health") == 1);
    CHECK(events_after_lost(run.card) == Events{"recover", "stock", "rest", "health", "stock"});
    CHECK(r.stock_restored);
    CHECK(run.card.core == 0);
    CHECK(run.card.mem == 0);
    CHECK(run.card.power == 100);
    check_journal_rules(run);
}

TEST_CASE("a reconnect that fails on the way out after a lost health probe is reported, not claimed as stock") {
    Run run = recovering();
    run.card.lost_core_from = 135;
    run.card.health_verdict = Verdict::DeviceLost;
    run.card.recover_fails_from_call = 2;   // the reconnect of the gate works, the one on the way out does not
    const auto r = run.go(Preset::BestOfMyGpu);
    CHECK_FALSE(r.ok);
    CHECK(r.reason == "the card was not usable when checked after a driver reset (DEVICE LOST)");
    CHECK(r.driver_resets == 2);
    CHECK_FALSE(r.stock_restored);
    CHECK(run.card.recover_calls == 2);
    CHECK(run.logged("reset FAILED"));
    CHECK(events_after_lost(run.card) == Events{"recover", "stock", "rest", "health", "recover"});
    check_journal_rules(run);
}

// The core offsets of a run's 30 s confirm probes, in the order they ran.
static std::vector<int> core_confirms(const FakeCard& card) {
    std::vector<int> out;
    for (const std::string& p : card.probe_order) {
        if (p.rfind("30:", 0) != 0) continue;
        const auto core_at = p.find(':', 3) + 1;
        const int core = std::stoi(p.substr(core_at, p.find(':', core_at) - core_at));
        if (core != 0) out.push_back(core);   // core 0 is the memory's confirm probe
    }
    return out;
}

TEST_CASE("a confirm probe that is too hot before the clock was tested confirms nothing") {
    // A 3080 Ti, 0.4.5: the driver reset at core +300, the health probe at
    // stock power left the card at 76 C, and `confirm core +225: TOO HOT`
    // ended within a second. That counted as holding, and +225 was confirmed
    // by a probe that had tested nothing.
    Run run = lost_at_135();
    run.card.hot_after_health_s = 0.6;
    const auto r = run.go(Preset::BestOfMyGpu);
    REQUIRE(r.ok);
    CHECK(r.driver_resets == 1);
    // The reset came at +135, +120 was the last value that passed, and the
    // confirm starts four steps below it. That probe ends too hot at once and
    // does not count; the card cools, and one step lower runs its 30 s.
    CHECK(core_confirms(run.card) == std::vector<int>{60, 45});
    CHECK(r.core_confirmed == 45);
    CHECK(r.core_mhz <= 45);
    CHECK(run.logged("confirm core +60 / mem +550: TOO HOT"));
    CHECK(run.logged("too hot after 0.6 s, before the clock was tested: not confirmed"));
    CHECK(run.card.rests == 2);   // after the reset, and to cool down before the next try
    check_journal_rules(run);
}

TEST_CASE("a confirm probe that got too hot after the clock was tested still keeps the clock edge") {
    Run run = lost_at_135();
    run.card.hot_after_health_s = kConfirmMinS;
    const auto r = run.go(Preset::BestOfMyGpu);
    REQUIRE(r.ok);
    CHECK(core_confirms(run.card) == std::vector<int>{60});
    CHECK(r.core_confirmed == 60);
    CHECK(run.card.rests == 1);
    CHECK_FALSE(run.logged("not confirmed"));
}

TEST_CASE("a stop request while the card cools after such a confirm probe ends the run at stock") {
    Run run = lost_at_135();
    run.card.hot_after_health_s = 0.6;
    run.card.abort_on_rest = 2;   // the second rest is the cool-down
    const auto r = run.go(Preset::BestOfMyGpu);
    CHECK_FALSE(r.ok);
    CHECK(r.reason == "aborted");
    CHECK(r.stock_restored);
    CHECK(core_confirms(run.card) == std::vector<int>{60});
}
