#include "doctest/doctest.h"
#include "search_fixture.hpp"

// The reset budget. A reset event is a probe that ends DEVICE LOST, STALLED or
// NO TELEMETRY, a write that fails, or a bandwidth measurement that fails above
// memory +0. The first one ends exploring; the second one ends the run.

TEST_CASE("a reset in the core climb ends the climb at the last value that passed") {
    for (const bool stall : {false, true}) {
        CAPTURE(stall);
        Run run = recovering();
        (stall ? run.card.stall_core_from : run.card.lost_core_from) = 135;
        const auto r = run.go(Preset::BestOfMyGpu);
        REQUIRE(r.ok);
        CHECK(r.driver_resets == 1);
        CHECK(r.core_max_stable == 120);
        CHECK(run.card.max_core_seen == 135);   // nothing above the candidate that reset the driver was tried
        // The climb stops at +135; the first entry after it is the confirm
        // probe, four steps below +120.
        CHECK(core_begins(run.card) == climb(15, 135, 15, {60}));
        CHECK(r.core_confirmed == 60);
        CHECK(r.core_mhz == 30);
        CHECK(run.logged(stall ? "core +135 / mem +550: STALLED" : "core +135 / mem +550: DEVICE LOST"));
        CHECK(run.logged("core: the driver reset at +135; using +120, the last value that passed"));
        CHECK(run.logged("confirm core +60 / mem +550: STABLE"));
        CHECK(first_events_after_lost(run.card, 5) == Events{"recover", "stock", "rest", "health", "begin"});
        CHECK(run.card.recover_calls == 1);
        CHECK_FALSE(run.card.stale);
        CHECK(run.card.core == r.core_mhz);   // the result is really applied
        CHECK(run.card.mem == 550);
        CHECK(run.card.power == 115);
        check_reconnected_up_front(run);
        check_journal_rules(run);
    }
}

TEST_CASE("a reset is never stored as a ceiling") {
    Run first = recovering();
    first.card.lost_core_from = 135;
    const auto a = first.go(Preset::BestOfMyGpu);
    REQUIRE(a.ok);
    CHECK(a.core_max_stable == 120);
    Journal reread(first.card.journal, [](const std::string&) { return true; });
    CHECK(reread.freezes().empty());
    CHECK(reread.ceilings().core_mhz == INT_MAX);
    CHECK(reread.ceilings().mem_mhz == INT_MAX);

    // The next run on the same journal, with nothing resetting the driver.
    Run second = recovering();
    second.card.journal = first.card.journal;
    const auto b = second.go(Preset::BestOfMyGpu);
    REQUIRE(b.ok);
    CHECK(b.core_max_stable == 150);
    CHECK(b.driver_resets == 0);
}

TEST_CASE("a second reset in a confirm probe ends the run and starts nothing") {
    Run run = recovering();
    run.card.lost_core_from = 135;
    run.card.lost_on_first_core_confirm = true;
    const auto r = run.go(Preset::BestOfMyGpu);
    CHECK_FALSE(r.ok);
    CHECK(r.reason == "the driver reset twice");
    CHECK(r.driver_resets == 2);
    CHECK(run.logged("confirm core +60 / mem +550: DEVICE LOST"));
    CHECK(core_begins(run.card) == climb(15, 135, 15, {60}));
    // After the lost confirm probe: one reconnect and one reset to stock on the way out.
    check_nothing_started(events_after_last_begin(run.card));
    CHECK(events_after_last_begin(run.card) == Events{"recover", "stock"});
    CHECK(run.card.rests == 1);
    CHECK(count_events(run.card, "health") == 1);
    // 23 probes up to the memory confirm, core +15 .. +135 (24-32), the health
    // probe (33), the lost confirm probe (34), and none after it.
    CHECK(run.card.probes == 34);
    CHECK(run.card.core == 0);
    CHECK(run.card.mem == 0);
    CHECK(run.card.power == 100);
    CHECK(r.stock_restored);
    REQUIRE_FALSE(run.card.journal.empty());
    CHECK(run.card.journal.back().find("DEVICE LOST") != std::string::npos);
    check_journal_rules(run);
}

TEST_CASE("a second reset in the memory confirm probe ends the run and starts nothing") {
    Run run = recovering();
    run.card.lost_mem_from = 300;                // the first event, in the memory climb
    run.card.lost_on_first_mem_confirm = true;   // the second, in the confirm probe at +50
    const auto r = run.go(Preset::BestOfMyGpu);
    CHECK_FALSE(r.ok);
    CHECK(r.reason == "the driver reset twice");
    CHECK(r.driver_resets == 2);
    CHECK(run.logged("core +0 / mem +300: DEVICE LOST"));
    CHECK(run.logged("confirm core +0 / mem +50: DEVICE LOST"));
    // The climb to +300, then the one confirm entry four steps below +250.
    CHECK(mem_begins(run.card) == climb(50, 300, 50, {50}));
    CHECK(core_begins(run.card).empty());
    check_nothing_started(events_after_last_begin(run.card));
    CHECK(events_after_last_begin(run.card) == Events{"recover", "stock"});
    CHECK(run.card.rests == 1);
    CHECK(count_events(run.card, "health") == 1);
    // The baseline (1), four power probes (2-5), memory +50 .. +300 (6-11), the
    // health probe (12), the lost confirm probe (13), and none after it.
    CHECK(run.card.probes == 13);
    CHECK(run.card.core == 0);
    CHECK(run.card.mem == 0);
    CHECK(run.card.power == 100);
    CHECK(r.stock_restored);
    CHECK_FALSE(run.card.stale);
    REQUIRE_FALSE(run.card.journal.empty());
    CHECK(run.card.journal.back().find("DEVICE LOST") != std::string::npos);
    check_journal_rules(run);
}

TEST_CASE("a first reset in the core confirm probe backs off four steps") {
    Run run = recovering();
    run.card.lost_on_first_core_confirm = true;
    const auto r = run.go(Preset::BestOfMyGpu);
    REQUIRE(r.ok);
    CHECK(r.driver_resets == 1);
    CHECK(r.core_max_stable == 150);
    CHECK(run.logged("confirm core +150 / mem +550: DEVICE LOST"));
    CHECK(run.logged("confirm core +90 / mem +550: STABLE"));
    // The climb, the lost confirm probe, and the next try four steps down.
    CHECK(core_begins(run.card) == climb(15, 165, 15, {150, 90}));
    CHECK(r.core_confirmed == 90);
    CHECK(r.core_mhz == 60);
    // The gate lies between the two confirm entries.
    CHECK(first_events_after_lost(run.card, 5) == Events{"recover", "stock", "rest", "health", "begin"});
    CHECK(run.card.core == 60);
    CHECK(run.card.mem == 550);    // the full state was rewritten after the reset
    CHECK(run.card.recover_calls == 1);
    CHECK_FALSE(run.card.stale);
    check_reconnected_up_front(run);
    check_journal_rules(run);
}

TEST_CASE("a second reset in the soak ends the run and starts nothing") {
    Run run = recovering();
    run.card.lost_core_from = 135;
    run.card.lost_on_first_soak = true;
    const auto r = run.go(Preset::BestOfMyGpu);
    CHECK_FALSE(r.ok);
    CHECK(r.reason == "the driver reset twice");
    CHECK(r.driver_resets == 2);
    CHECK(run.logged("soak: 300 s at power 115 %, core +30, mem +550"));
    CHECK(run.logged("soak: DEVICE LOST"));
    CHECK(soak_entries(run.card) == 1);
    REQUIRE_FALSE(run.card.journal.empty());
    CHECK(run.card.journal.back().find("DEVICE LOST") != std::string::npos);
    check_nothing_started(events_after_last_begin(run.card));
    CHECK(events_after_last_begin(run.card) == Events{"recover", "stock"});
    CHECK(run.card.rests == 1);
    CHECK(count_events(run.card, "health") == 1);
    // 32 probes up to the lost core +135, the health probe (33), the confirm
    // probe at +60 (34), the lost soak (35), and none after it.
    CHECK(run.card.probes == 35);
    CHECK(run.card.core == 0);
    CHECK(run.card.mem == 0);
    CHECK(run.card.power == 100);
    CHECK(r.stock_restored);
    check_journal_rules(run);
}

TEST_CASE("a second reset as a failed soak write ends the run and starts nothing") {
    Run run = recovering();
    run.card.lost_core_from = 135;            // the first event, in the core climb
    run.card.fail_set_on_first_soak = true;   // the second: the soak's core write, +30
    const auto r = run.go(Preset::BestOfMyGpu);
    CHECK_FALSE(r.ok);
    CHECK(r.reason == "the driver reset twice");
    CHECK(r.driver_resets == 2);
    CHECK(run.logged("setting core +30 failed; treated as a driver reset"));
    CHECK(soak_entries(run.card) == 1);
    REQUIRE_FALSE(run.card.journal.empty());
    CHECK(run.card.journal.back().find("SET FAILED") != std::string::npos);
    CHECK(run.card.count("SET FAILED") == 1);
    CHECK(run.count_logged("soak: 300 s at") == 0);   // the soak never ran
    check_nothing_started(events_after_last_begin(run.card));
    CHECK(events_after_last_begin(run.card) == Events{"recover", "stock"});
    CHECK(run.card.rests == 1);                        // the one of the first event
    CHECK(count_events(run.card, "health") == 1);
    // 32 probes up to the lost core +135, the health probe (33), the confirm
    // probe at +60 (34), and none after it: the soak's write failed.
    CHECK(run.card.probes == 34);
    CHECK(run.card.core == 0);
    CHECK(run.card.mem == 0);
    CHECK(run.card.power == 100);
    CHECK(r.stock_restored);
    check_journal_rules(run);
}

TEST_CASE("a first reset in the soak backs both clocks off four steps") {
    Run run = recovering();
    run.card.lost_on_first_soak = true;
    const auto r = run.go(Preset::BestOfMyGpu);
    REQUIRE(r.ok);
    CHECK(r.driver_resets == 1);
    CHECK(run.logged("soak: DEVICE LOST"));
    CHECK(run.logged("soak: 300 s at power 115 %, core +45, mem +350"));
    CHECK(soak_entries(run.card) == 2);
    CHECK(run.card.probes == 38);   // the lost soak (probe 36), the health probe at stock, the second soak
    CHECK(count_events(run.card, "health") == 1);
    CHECK(first_events_after_lost(run.card, 5) == Events{"recover", "stock", "rest", "health", "begin"});
    CHECK(r.soak.seconds == 300);
    CHECK(r.core_mhz == 45);
    CHECK(r.mem_mhz == 350);
    CHECK(run.card.core == 45);
    CHECK(run.card.mem == 350);
    CHECK(run.card.power == 115);
    CHECK(run.card.recover_calls == 1);
    CHECK_FALSE(run.card.stale);
    check_reconnected_up_front(run);
    check_journal_rules(run);
}

TEST_CASE("a reset during the health probe after a soak reset ends the run") {
    Run run = recovering();
    run.card.lost_on_first_soak = true;
    run.card.health_verdict = Verdict::DeviceLost;
    const auto r = run.go(Preset::BestOfMyGpu);
    CHECK_FALSE(r.ok);
    CHECK(r.reason == "the card was not usable when checked after a driver reset (DEVICE LOST)");
    CHECK(r.driver_resets == 2);
    CHECK(run.card.probes == 37);   // the lost soak is probe 36; then only the health probe
    CHECK(soak_entries(run.card) == 1);
    // The gate, called from the soak, stops the run: no rest, no second health
    // probe and no entry after it; one reconnect and one reset on the way out.
    CHECK(events_after_lost(run.card) == Events{"recover", "stock", "rest", "health", "recover", "stock"});
    CHECK(r.stock_restored);
    CHECK(run.card.core == 0);
    CHECK(run.card.mem == 0);
    CHECK(run.card.power == 100);
    check_journal_rules(run);
}

TEST_CASE("a reset during the baseline ends the run") {
    Run run = recovering();
    run.card.lost_at_probe = 1;   // the baseline
    const auto r = run.go(Preset::BestOfMyGpu);
    CHECK_FALSE(r.ok);
    CHECK(r.reason == "the driver reset at stock clocks (DEVICE LOST)");
    CHECK(r.driver_resets == 1);
    CHECK(r.baseline.seconds == 30);
    CHECK(run.card.probes == 1);
    CHECK(count_events(run.card, "health") == 0);
    CHECK(run.card.rests == 0);
    CHECK(r.stock_restored);
    CHECK_FALSE(run.card.stale);
    CHECK(run.card.recover_calls == 1);
    CHECK(run.card.journal.empty());
    check_journal_rules(run);
}

TEST_CASE("without a reconnect a reset during the baseline keeps its old reason") {
    Run run;
    run.card.lost_at_probe = 1;
    const auto r = run.go(Preset::BestOfMyGpu);
    CHECK_FALSE(r.ok);
    CHECK(r.reason == "stock is not stable (DEVICE LOST)");
    CHECK(r.driver_resets == 1);   // still counted
    CHECK(run.card.probes == 1);
}

TEST_CASE("a reset during the power step ends the run") {
    Run run = recovering();
    run.card.lost_at_probe = 2;   // the first 20 s probe, at 85 %
    const auto r = run.go(Preset::BestOfMyGpu);
    CHECK_FALSE(r.ok);
    CHECK(run.logged("power 85 %: DEVICE LOST"));
    CHECK(r.reason == "the driver reset at stock clocks (DEVICE LOST)");
    CHECK(r.driver_resets == 1);
    CHECK(run.card.probes == 2);
    CHECK(run.card.power == 100);
    CHECK(run.card.core == 0);
    CHECK(count_events(run.card, "health") == 0);
    CHECK(run.card.rests == 0);
    CHECK(r.stock_restored);
    CHECK_FALSE(run.card.stale);
    CHECK(run.card.recover_calls == 1);
    CHECK(run.card.journal.empty());
    check_journal_rules(run);
}

TEST_CASE("one blind probe ends the climb at the last value that passed") {
    Run run = recovering();
    run.card.blind_at_core = 135;
    const auto r = run.go(Preset::BestOfMyGpu);
    REQUIRE(r.ok);
    CHECK(r.driver_resets == 1);
    CHECK(r.core_max_stable == 120);
    CHECK(run.logged("core +135 / mem +550: NO TELEMETRY"));
    // No 3 s core probe after it: nine climb entries and one confirm entry,
    // four steps below +120. That 30 s probe is not blind and holds.
    CHECK(run.card.max_core_seen == 135);
    const auto begins = core_begins(run.card);
    REQUIRE(begins.size() == 10);
    CHECK(std::vector<int>(begins.begin(), begins.begin() + 9) == climb(15, 135, 15));
    CHECK(begins[9] == 60);
    CHECK(r.core_confirmed == 60);
    CHECK(run.card.recover_calls == 1);
    check_journal_rules(run);
}

TEST_CASE("a blind probe ends the clock search it is in: the memory search, then the core search") {
    Run run = recovering();
    run.card.blind_probes = 2;   // the first two 3 s probes are blind
    const auto r = run.go(Preset::BestOfMyGpu);
    REQUIRE(r.ok);
    CHECK(r.driver_resets == 2);
    // The first blind probe is the first 3 s one, memory +50. It is not a
    // measured failure and nothing is bisected below it: the memory stays at
    // stock. The core has a search of its own, and its first probe, +15, is
    // the second blind one: the core stays at stock too, and no third 3 s
    // probe runs.
    CHECK(run.logged("core +0 / mem +50: NO TELEMETRY"));
    CHECK(run.logged("core +15 / mem +0: NO TELEMETRY"));
    CHECK(run.card.blind_probes == 0);
    CHECK(mem_begins(run.card) == std::vector<int>{50});
    CHECK(core_begins(run.card) == std::vector<int>{15});
    CHECK(r.mem_max_stable == 0);
    CHECK(r.mem_mhz == 0);
    CHECK(r.core_mhz == 0);
    CHECK(run.card.max_mem_seen == 50);
    CHECK(run.card.max_core_seen == 15);
    CHECK(run.card.recover_calls == 2);
    CHECK_FALSE(run.logged("lost telemetry"));
    check_journal_rules(run);
}

TEST_CASE("a failed candidate write is a reset event and goes through the gate") {
    Run run = recovering();
    run.card.fail_core_set_once_at = 135;
    const auto r = run.go(Preset::BestOfMyGpu);
    REQUIRE(r.ok);
    CHECK(r.driver_resets == 1);
    CHECK(run.logged("setting core +135 failed; treated as a driver reset"));
    CHECK_FALSE(run.logged("trying once more"));
    // The +135 entry is closed as SET FAILED and the value gets no second entry.
    CHECK(run.card.count("SET FAILED") == 1);
    CHECK(core_begins(run.card) == climb(15, 135, 15, {60}));
    CHECK(run.card.max_core_seen == 120);
    CHECK(first_events_after_lost(run.card, 5) == Events{"recover", "stock", "rest", "health", "begin"});
    CHECK(r.core_max_stable == 120);
    CHECK(r.core_confirmed == 60);
    CHECK(run.card.recover_calls == 1);
    CHECK(run.card.recovers_with_entry_open == 0);
    check_journal_rules(run);
}

TEST_CASE("a write that keeps failing is never written again and the run completes below it") {
    Run run = recovering();
    run.card.fail_core_set_at = 150;   // every write of +150 fails
    const auto r = run.go(Preset::BestOfMyGpu);
    REQUIRE(r.ok);
    CHECK(r.driver_resets == 1);
    CHECK(run.logged("setting core +150 failed; treated as a driver reset"));
    CHECK(run.card.count("SET FAILED") == 1);    // one entry for the value, and no retry
    CHECK(core_begins(run.card) == climb(15, 150, 15, {75}));
    CHECK(run.card.max_core_seen == 135);
    CHECK(r.core_max_stable == 135);
    CHECK(r.core_confirmed == 75);
    CHECK(r.core_mhz == 45);
    CHECK(run.card.core == 45);
    CHECK(run.card.mem == 550);
    CHECK(run.card.power == 115);
    CHECK(run.card.recover_calls == 1);
    check_journal_rules(run);
}

TEST_CASE("a failed write on the first core candidate leaves the core at stock") {
    Run run = recovering();
    run.card.flaky_core_sets = 1;   // the first non-zero core write, +15
    const auto r = run.go(Preset::BestOfMyGpu);
    REQUIRE(r.ok);
    CHECK(r.driver_resets == 1);
    CHECK(run.card.count("SET FAILED") == 1);
    CHECK(core_begins(run.card) == std::vector<int>{15});   // no retry, and nothing to confirm above stock
    CHECK(r.core_max_stable == 0);
    CHECK(r.core_mhz == 0);
    CHECK(run.card.max_core_seen == 0);
    CHECK(r.mem_mhz == 550);
    CHECK(run.card.mem == 550);
    CHECK(run.card.recover_calls == 1);
    check_journal_rules(run);
}

TEST_CASE("a failed write after an earlier reset ends the run") {
    Run run = recovering();
    run.card.lost_core_from = 135;
    run.card.fail_next_core_set_after_recover = true;   // the write of the confirm candidate, +60
    const auto r = run.go(Preset::BestOfMyGpu);
    CHECK_FALSE(r.ok);
    CHECK(r.reason == "the driver reset twice");
    CHECK(r.driver_resets == 2);
    CHECK(run.logged("setting core +60 failed; treated as a driver reset"));
    CHECK(core_begins(run.card) == climb(15, 135, 15, {60}));
    REQUIRE_FALSE(run.card.journal.empty());
    CHECK(run.card.journal.back().find("SET FAILED") != std::string::npos);
    CHECK(run.card.count("SET FAILED") == 1);
    check_nothing_started(events_after_last_begin(run.card));
    CHECK(events_after_last_begin(run.card) == Events{"recover", "stock"});
    CHECK(r.stock_restored);
    CHECK(run.card.core == 0);
    CHECK(run.card.mem == 0);
    CHECK(run.card.power == 100);
    check_journal_rules(run);
}

TEST_CASE("a failed write in the power step ends the run") {
    Run run = recovering();
    run.card.flaky_power_sets = 1;   // the write of the first power step, 85 %
    const auto r = run.go(Preset::BestOfMyGpu);
    CHECK_FALSE(r.ok);
    CHECK(r.reason == "the driver reset at stock clocks (setting power 85 % failed)");
    CHECK(run.logged("setting power 85 % failed; treated as a driver reset"));
    CHECK(r.driver_resets == 1);
    CHECK(run.card.probes == 1);   // the baseline only
    CHECK(run.card.power == 100);
    CHECK(run.card.recover_calls == 1);   // on the way out; no retry of the write
    CHECK(count_events(run.card, "health") == 0);
    CHECK(run.card.rests == 0);
    CHECK(r.stock_restored);
    CHECK(run.card.journal.empty());
}

TEST_CASE("without a reconnect a failed write in the power step stops the run as before") {
    Run run;
    run.card.flaky_power_sets = 1;
    const auto r = run.go(Preset::BestOfMyGpu);
    CHECK_FALSE(r.ok);
    CHECK(r.reason == "setting power 85 % failed");
    CHECK(r.driver_resets == 0);
    CHECK(run.card.power == 100);
    CHECK(r.stock_restored);
}

TEST_CASE("a failed write of the soak state is a soak reset") {
    Run run = recovering();
    run.card.fail_set_on_first_soak = true;
    const auto r = run.go(Preset::BestOfMyGpu);
    REQUIRE(r.ok);
    CHECK(r.driver_resets == 1);
    CHECK(run.logged("setting core +105 failed; treated as a driver reset"));
    CHECK(soak_entries(run.card) == 2);
    CHECK(run.card.count("SET FAILED") == 1);
    // The first soak entry is the one closed as SET FAILED: the only soak that
    // ran is the second, four steps down, after the gate.
    CHECK(run.count_logged("soak: 300 s at") == 1);
    CHECK(run.logged("soak: 300 s at power 115 %, core +45, mem +350"));
    CHECK(first_events_after_lost(run.card, 5) == Events{"recover", "stock", "rest", "health", "begin"});
    CHECK(r.core_mhz == 45);
    CHECK(r.mem_mhz == 350);
    CHECK(run.card.core == 45);
    CHECK(run.card.mem == 350);
    CHECK(run.card.recover_calls == 1);
    check_journal_rules(run);
}

TEST_CASE("a failed bandwidth measurement ends the memory scan; the core is still searched") {
    Run run = recovering();
    run.card.bw_curve = [](int m) { return 500 + m * 0.3; };
    run.card.bw_fails_once_at_mem = 300;
    const auto r = run.go(Preset::BestOfMyGpu);
    REQUIRE(r.ok);
    CHECK(r.driver_resets == 1);
    // The scan samples +0 .. +300. +250 was the highest step that passed, so
    // the second pass would begin at +50; after a reset nothing is read, and
    // confirming starts four steps below that, which is stock. The +0 sample
    // names no clock in the journal.
    CHECK(mem_begins(run.card) == climb(50, 300, 50));
    CHECK(clockless_begins(run.card) == 1);
    // +0, +50 .. +300, core +15 .. +165, the core confirm probe, the soak
    CHECK(run.card.count("\"begin\"") == 20);
    CHECK(run.logged("core +0 / mem +0: STABLE"));
    CHECK(run.logged("mem +0: bandwidth 500.0 GB/s"));
    CHECK(run.card.max_mem_seen == 300);
    CHECK(run.card.bw_measurements == 7);
    CHECK(run.card.bw_again_measurements == 0);
    CHECK(r.mem_max_stable == 50);
    CHECK(r.mem_confirmed == 0);
    CHECK(r.mem_mhz == 0);
    CHECK(run.card.max_core_seen == 165);
    CHECK(r.core_mhz == 105);
    CHECK_FALSE(run.logged("core: not searched"));
    CHECK(run.card.recover_calls == 1);
    CHECK(run.card.count("SET FAILED") == 0);   // reconnected before the next entry, not after a failed write in it
    CHECK(run.logged("the driver was reset; reconnecting"));
    CHECK_FALSE(run.card.stale);
    check_journal_rules(run);
}

TEST_CASE("bandwidth readings that do not settle end the memory scan and are no reset") {
    Run run = recovering();
    run.card.bw_curve = [](int m) { return 500 + m * 0.3; };
    run.card.bw_unsettled_at_mem = 300;
    const auto r = run.go(Preset::BestOfMyGpu);
    REQUIRE(r.ok);
    CHECK(r.driver_resets == 0);
    CHECK(run.card.recover_calls == 0);
    CHECK_FALSE(run.logged("treated as a driver reset"));
    CHECK(run.logged("mem +300: the bandwidth readings did not settle; the memory scan ends here"));
    // The scan ends at +300; +250 was the highest step that passed, so the
    // second pass begins at +50, and the confirm probe runs at what it kept,
    // not four steps below that.
    CHECK(run.card.max_mem_seen == 300);
    CHECK(run.card.max_again_mem == 50);
    CHECK(r.mem_max_stable == 50);
    CHECK(r.mem_confirmed == 50);
    // The core is searched as in a run without any event.
    CHECK(run.card.max_core_seen > 0);
    CHECK(r.core_mhz > 0);
    CHECK_FALSE(run.logged("core: not searched"));
    check_journal_rules(run);
}

TEST_CASE("a reset in the memory scan ends that search only; the core is still searched") {
    Run run = recovering();
    run.card.lost_mem_from = 300;
    const auto r = run.go(Preset::BestOfMyGpu);
    REQUIRE(r.ok);
    CHECK(r.driver_resets == 1);
    CHECK(run.logged("core +0 / mem +300: DEVICE LOST"));
    CHECK(run.logged("mem: the driver reset at +300; using +250, the last value that passed"));
    // The climb ends at +300; the confirm probe starts four steps below +250.
    CHECK(mem_begins(run.card) == climb(50, 300, 50, {50}));
    CHECK(r.mem_max_stable == 250);
    CHECK(r.mem_confirmed == 50);
    CHECK(r.mem_mhz == 0);
    // Each clock has its own search: the core climbs to its edge as in a run
    // without a reset (+165 is the first value that fails), and is confirmed
    // at the edge, not four steps below it.
    CHECK_FALSE(run.logged("core: not searched"));
    // The last core entry is the soak's: with memory at +0 it names only the core.
    CHECK(core_begins(run.card) == climb(15, 165, 15, {150, 105}));
    CHECK(run.card.max_core_seen == 165);
    CHECK(r.core_max_stable == 150);
    CHECK(r.core_confirmed == 150);
    CHECK(r.core_mhz == 105);
    CHECK(run.card.mem == 0);
    CHECK(run.card.core == 105);
    CHECK(run.card.power == 115);  // the full state was rewritten after the reset
    CHECK(run.card.recover_calls == 1);
    CHECK_FALSE(run.card.stale);
    check_reconnected_up_front(run);
    check_journal_rules(run);
}

TEST_CASE("no bandwidth is measured after a reset") {
    Run run = recovering();
    run.card.bw_curve = [](int m) { return 500 + m * 0.3; };
    run.card.lost_mem_from = 300;
    const auto r = run.go(Preset::BestOfMyGpu);
    REQUIRE(r.ok);
    CHECK(run.logged("core +0 / mem +300: DEVICE LOST"));
    CHECK(run.card.bw_measurements == 6);   // +0 .. +250, and none after the lost candidate
    CHECK(run.card.max_mem_seen == 300);
}

TEST_CASE("a measurement that is unavailable from the start leaves the memory at stock") {
    // A 3080 Ti, 0.4.3: the measurement failed at +0, the search climbed on
    // stability alone to the first wrong result at +2300, and the machine froze
    // in the confirm probes just below it. The bandwidth peak is what keeps a
    // memory search away from there; without it nothing is searched.
    Run run = recovering();
    run.card.bw_curve = [](int m) { return 500 + m * 0.3; };
    run.card.bw_fails_at_zero = true;
    const auto r = run.go(Preset::BestOfMyGpu);
    REQUIRE(r.ok);
    CHECK(r.driver_resets == 0);
    CHECK(run.card.bw_measurements == 1);
    // The +0 sample ran and is the first entry of the run, without a clock.
    // No memory offset was ever written.
    CHECK(mem_begins(run.card).empty());
    CHECK(run.card.max_mem_seen == 0);
    CHECK(clockless_begins(run.card) == 1);
    REQUIRE_FALSE(run.card.journal.empty());
    CHECK(run.card.journal.front().find("\"mem\":") == std::string::npos);
    CHECK(run.card.journal.front().find("\"core\":") == std::string::npos);
    CHECK(run.logged("core +0 / mem +0: STABLE"));
    CHECK(r.mem_max_stable == 0);
    CHECK(r.mem_confirmed == 0);
    CHECK(r.mem_mhz == 0);
    CHECK(r.core_max_stable == 150);   // the core is still searched
    CHECK(r.core_mhz > 0);
    CHECK(run.card.rests == 0);
    CHECK(count_events(run.card, "health") == 0);
    CHECK(run.card.recover_calls == 0);
    CHECK(run.logged("the bandwidth measurement does not work here; the memory is not overclocked"));
    CHECK(run.logged("mem: left at stock"));
    CHECK_FALSE(run.logged("searching memory by stability only"));
    CHECK_FALSE(run.logged("mem: highest stable"));
    CHECK_FALSE(run.logged("bandwidth peak"));
    CHECK_FALSE(run.logged("the driver reset during this run"));
    check_journal_rules(run);
}

TEST_CASE("a measurement that fails at +0 because the device was lost is caught by the next write") {
    Run run = recovering();
    run.card.bw_curve = [](int m) { return 500 + m * 0.1; };
    run.card.bw_loses_device = true;
    const auto r = run.go(Preset::BestOfMyGpu);
    REQUIRE(r.ok);
    CHECK(r.driver_resets == 1);
    CHECK(run.logged("the bandwidth measurement does not work here; the memory is not overclocked"));
    CHECK(run.card.bw_measurements == 1);
    // Journal: +0 opened (without a clock) and closed. No memory offset is
    // tried; the write of stock that follows the failed measurement is the
    // next write, and it is what finds the reset: in the memory search, so the
    // core keeps its own. After it: the core search (+15 .. +165, the confirm
    // probe) and the soak.
    CHECK(mem_begins(run.card).empty());
    CHECK(clockless_begins(run.card) == 1);
    CHECK(run.card.count("\"begin\"") == 14);
    CHECK(run.logged("core +0 / mem +0: STABLE"));
    REQUIRE_FALSE(run.card.journal.empty());
    CHECK(run.card.journal[0].find("\"begin\"") != std::string::npos);
    CHECK(run.card.journal[0].find("\"mem\":") == std::string::npos);
    CHECK(run.card.count("SET FAILED") == 0);
    CHECK(run.logged("failed; treated as a driver reset"));
    CHECK(first_events_after_lost(run.card, 5) == Events{"recover", "stock", "rest", "health", "begin"});
    CHECK(r.mem_mhz == 0);
    CHECK(run.card.max_core_seen == 165);
    CHECK_FALSE(run.logged("core: not searched"));
    CHECK(r.core_mhz == 105);
    CHECK(run.card.core == r.core_mhz);
    CHECK(run.card.recover_calls == 1);
    check_journal_rules(run);
}

TEST_CASE("a reset verdict on the +0 memory sample ends the run") {
    Run run = recovering();
    run.card.bw_curve = [](int m) { return 500 + m * 0.3; };
    run.card.lost_mem_from = 0;   // every 3 s probe, the +0 sample first
    const auto r = run.go(Preset::BestOfMyGpu);
    CHECK_FALSE(r.ok);
    CHECK(r.reason == "the driver reset at stock clocks (DEVICE LOST)");
    CHECK(r.driver_resets == 1);
    // The +0 sample is the only entry of the run, and it names no clock.
    CHECK(run.card.count("\"begin\"") == 1);
    CHECK(clockless_begins(run.card) == 1);
    CHECK(mem_begins(run.card).empty());
    CHECK(run.logged("core +0 / mem +0: DEVICE LOST"));
    CHECK(run.card.max_mem_seen == 0);
    CHECK(run.card.bw_measurements == 0);
    CHECK(run.card.rests == 0);
    CHECK(count_events(run.card, "health") == 0);
    CHECK(events_after_last_begin(run.card) == Events{"recover", "stock"});
    CHECK(r.stock_restored);
    check_journal_rules(run);
}

TEST_CASE("a reset in the memory search and one in the core climb: both clocks back off, the run goes on") {
    Run run = recovering();
    run.card.lost_mem_from = 300;
    run.card.lost_core_from = 135;
    const auto r = run.go(Preset::BestOfMyGpu);
    REQUIRE(r.ok);
    CHECK(r.driver_resets == 2);
    CHECK(run.card.recover_calls == 2);
    CHECK(run.logged("mem: the driver reset at +300; using +250, the last value that passed"));
    CHECK(run.logged("core: the driver reset at +135; using +120, the last value that passed"));
    // The core climb ends at +135 and its confirm probe starts four steps
    // below +120; nothing above the lost value is tried. The last core entry
    // is the soak's, at +30.
    CHECK(core_begins(run.card) == climb(15, 135, 15, {60, 30}));
    CHECK(run.card.max_core_seen == 135);
    CHECK(r.core_max_stable == 120);
    CHECK(r.core_confirmed == 60);
    CHECK(r.mem_confirmed == 50);
    CHECK_FALSE(run.card.stale);
    check_journal_rules(run);
}

TEST_CASE("a third reset ends the run and starts nothing") {
    Run run = recovering();
    run.card.lost_mem_from = 300;          // the first event, in the memory climb
    run.card.lost_core_from = 135;         // the second, in the core climb
    run.card.lost_on_first_soak = true;    // the third
    const auto r = run.go(Preset::BestOfMyGpu);
    REQUIRE_FALSE(r.ok);
    CHECK(r.reason == "the driver reset three times");
    CHECK(r.driver_resets == 3);
    CHECK(run.card.core == 0);
    CHECK(run.card.mem == 0);
    check_journal_rules(run);
}

TEST_CASE("a first reset in the memory confirm probe: the core is still searched") {
    Run run = recovering();
    run.card.lost_on_first_mem_confirm = true;
    const auto r = run.go(Preset::BestOfMyGpu);
    REQUIRE(r.ok);
    CHECK(r.driver_resets == 1);
    CHECK(run.logged("confirm core +0 / mem +800: DEVICE LOST"));
    // The next confirm try is four steps down. The core then has a search of
    // its own, with memory +400 applied, and the soak names both clocks.
    CHECK(mem_begins(run.card) == climb(50, 850, 50, {800, 600}));
    CHECK(core_begins(run.card) == climb(15, 165, 15, {150}));
    CHECK(r.mem_max_stable == 800);
    CHECK(r.mem_confirmed == 600);
    CHECK(r.mem_mhz == 400);
    CHECK(run.card.mem == 400);
    CHECK(run.card.max_core_seen == 165);
    CHECK(r.core_mhz == 105);
    CHECK_FALSE(run.logged("core: not searched"));
    CHECK(run.logged("soak: 300 s at power 115 %, core +105, mem +400"));
    CHECK(run.card.recover_calls == 1);
    check_journal_rules(run);
}

TEST_CASE("a soak entry carries only the clocks that are above stock") {
    // A card whose core holds nothing above stock: the soak runs at core +0 /
    // memory +400.
    Run run = recovering();
    run.card.lost_on_first_mem_confirm = true;
    run.card.core_edge = 0;
    const auto r = run.go(Preset::BestOfMyGpu);
    REQUIRE(r.ok);
    CHECK(r.core_mhz == 0);
    CHECK(r.mem_mhz == 400);
    CHECK(run.logged("soak: 300 s at power 115 %, core +0, mem +400"));
    // The soak's entry is the last one of the run: its `begin` and its `complete`.
    REQUIRE(run.card.journal.size() >= 2);
    const std::string& soak_begin = run.card.journal[run.card.journal.size() - 2];
    REQUIRE(soak_begin.find("\"begin\"") != std::string::npos);
    CHECK(soak_begin.find("\"mem\":400") != std::string::npos);
    CHECK(soak_begin.find("\"core\":") == std::string::npos);
    // A machine that froze during that soak leaves the entry open. That must
    // not become a core ceiling of 0, which would keep every later run from
    // searching the core at all.
    REQUIRE(run.card.journal.back().find("\"complete\"") != std::string::npos);
    const std::vector<std::string> frozen(run.card.journal.begin(), run.card.journal.end() - 1);
    Journal reread(frozen, [](const std::string&) { return true; });
    CHECK(reread.freezes().size() == 1);
    CHECK(reread.ceilings().core_mhz == INT_MAX);
    CHECK(reread.ceilings().mem_mhz == 400);
}

TEST_CASE("the +0 sample of the bandwidth scan journals no clock") {
    Run run;
    run.card.bw_curve = [](int m) { return 500 + m * 0.3; };
    const auto r = run.go(Preset::BestOfMyGpu);
    REQUIRE(r.ok);
    CHECK(r.mem_max_stable == 600);   // +800 passed; the second pass began at +600
    // The first entry of the run is the +0 sample: it ran and was measured,
    // and its `begin` line names neither clock.
    CHECK(run.logged("core +0 / mem +0: STABLE"));
    CHECK(run.logged("mem +0: bandwidth 500.0 GB/s"));
    REQUIRE(run.card.journal.size() >= 3);
    const std::string& sample_begin = run.card.journal[0];
    REQUIRE(sample_begin.find("\"begin\"") != std::string::npos);
    CHECK(sample_begin.find("\"mem\":") == std::string::npos);
    CHECK(sample_begin.find("\"core\":") == std::string::npos);
    REQUIRE(run.card.journal[1].find("\"complete\"") != std::string::npos);
    // The entry after it is the first one above stock, and it names its clock.
    CHECK(run.card.journal[2].find("\"mem\":50") != std::string::npos);
    CHECK(clockless_begins(run.card) == 1);   // the soak runs above stock
    REQUIRE_FALSE(mem_begins(run.card).empty());
    CHECK(mem_begins(run.card).front() == 50);

    // A machine that froze during that sample leaves its entry open. That is
    // a freeze, and it must not become a ceiling of 0 for either clock.
    const std::vector<std::string> frozen{sample_begin};
    Journal reread(frozen, [](const std::string&) { return true; });
    CHECK(reread.freezes().size() == 1);
    CHECK(reread.ceilings().mem_mhz == INT_MAX);
    CHECK(reread.ceilings().core_mhz == INT_MAX);

    // The next run on that journal searches memory as far as the first did.
    Run next;
    next.card.bw_curve = run.card.bw_curve;
    next.card.journal = frozen;
    const auto b = next.go(Preset::BestOfMyGpu);
    REQUIRE(b.ok);
    CHECK(b.mem_max_stable == 600);
    CHECK(b.core_max_stable == 150);
}

TEST_CASE("a soak reset at stock clocks ends the run") {
    Run run = recovering();
    run.card.lost_on_first_soak = true;
    const auto r = run.go(Preset::CoolAndEfficient);
    CHECK_FALSE(r.ok);
    CHECK(r.reason == "the driver reset at stock clocks (DEVICE LOST)");
    CHECK(r.driver_resets == 1);
    CHECK(run.card.count("\"begin\"") == 1);   // exactly one soak entry
    CHECK(run.count_logged("soak: 300 s at") == 1);
    CHECK(run.card.rests == 0);
    CHECK(count_events(run.card, "health") == 0);
    CHECK(events_after_last_begin(run.card) == Events{"recover", "stock"});
    CHECK(r.stock_restored);
    CHECK(run.card.power == 100);
    check_journal_rules(run);
}

TEST_CASE("a failed soak write at stock clocks ends the run") {
    // The cool preset tunes neither clock: its soak runs at core +0 / memory
    // +0, and its entry is the only one of the run.
    Run clean = recovering();
    REQUIRE(clean.go(Preset::CoolAndEfficient).ok);

    Run run = recovering();
    run.card.fail_power_set_in_entry = true;   // the soak's power write
    const auto r = run.go(Preset::CoolAndEfficient);
    CHECK_FALSE(r.ok);
    CHECK(r.reason == "the driver reset at stock clocks (setting power 85 % failed)");
    CHECK(run.logged("setting power 85 % failed; treated as a driver reset"));
    CHECK(r.driver_resets == 1);
    CHECK_FALSE(run.card.fail_power_set_in_entry);   // the knob fired
    CHECK(run.card.count("\"begin\"") == 1);          // exactly one soak entry
    REQUIRE(run.card.journal.size() == 2);
    CHECK(run.card.journal.back().find("SET FAILED") != std::string::npos);
    CHECK(run.count_logged("soak: 300 s at") == 0);   // the soak never ran
    CHECK(run.card.probes == clean.card.probes - 1);  // every probe of a clean run but the soak
    CHECK(run.card.rests == 0);
    CHECK(count_events(run.card, "health") == 0);
    CHECK(events_after_last_begin(run.card) == Events{"recover", "stock"});
    CHECK(run.card.recover_calls == 1);
    CHECK(r.stock_restored);
    CHECK(run.card.power == 100);
    CHECK(run.card.core == 0);
    CHECK(run.card.mem == 0);
    check_journal_rules(run);
}

TEST_CASE("a failed write of the +0 memory sample ends the run") {
    Run run = recovering();
    run.card.bw_curve = [](int m) { return 500 + m * 0.3; };
    run.card.fail_power_set_in_entry = true;   // the first entry of the run is the +0 sample
    const auto r = run.go(Preset::BestOfMyGpu);
    CHECK_FALSE(r.ok);
    CHECK(r.reason == "the driver reset at stock clocks (setting power 115 % failed)");
    CHECK(run.logged("setting power 115 % failed; treated as a driver reset"));
    CHECK(r.driver_resets == 1);
    CHECK_FALSE(run.card.fail_power_set_in_entry);   // the knob fired
    // The +0 sample is the only entry of the run, and it names no clock.
    CHECK(run.card.count("\"begin\"") == 1);
    CHECK(clockless_begins(run.card) == 1);
    CHECK(mem_begins(run.card).empty());
    CHECK(run.card.max_mem_seen == 0);
    REQUIRE(run.card.journal.size() == 2);
    CHECK(run.card.journal.back().find("SET FAILED") != std::string::npos);
    CHECK(run.card.probes == 5);   // the baseline and four power probes; the sample never ran
    CHECK(run.card.bw_measurements == 0);
    CHECK(run.card.rests == 0);
    CHECK(count_events(run.card, "health") == 0);
    CHECK(events_after_last_begin(run.card) == Events{"recover", "stock"});
    CHECK(run.card.recover_calls == 1);
    CHECK(r.stock_restored);
    CHECK(run.card.power == 100);
    CHECK(run.card.core == 0);
    CHECK(run.card.mem == 0);
    check_journal_rules(run);
}

TEST_CASE("a completed run says that the driver reset") {
    Run run = recovering();
    run.card.lost_core_from = 135;
    const auto r = run.go(Preset::BestOfMyGpu);
    REQUIRE(r.ok);
    CHECK(run.logged("the driver reset during this run; the search stopped exploring there"));

    Run clean = recovering();
    const auto b = clean.go(Preset::BestOfMyGpu);
    REQUIRE(b.ok);
    CHECK_FALSE(clean.logged("the driver reset during this run"));
}

TEST_CASE("without a reconnect a failed write stops the run as before") {
    Run run;
    run.card.fail_core_set_at = 150;
    const auto r = run.go(Preset::BestOfMyGpu);
    CHECK_FALSE(r.ok);
    CHECK(r.reason == "setting core +150 failed");
    CHECK(r.driver_resets == 0);
    CHECK(r.stock_restored);
    CHECK(run.card.core == 0);
    CHECK(run.card.mem == 0);      // core +150 was written with memory +550 applied
    CHECK(run.card.power == 100);
    CHECK(run.card.count("SET FAILED") == 1);
    CHECK_FALSE(run.logged("treated as a driver reset"));
}

TEST_CASE("an ordinary failure costs no reset") {
    Run run = recovering();
    const auto r = run.go(Preset::BestOfMyGpu);
    REQUIRE(r.ok);
    CHECK(r.driver_resets == 0);
    CHECK(r.core_max_stable == 150);
    CHECK(r.core_confirmed == 150);
    CHECK(run.card.recover_calls == 0);
    CHECK(run.card.rests == 0);
}
