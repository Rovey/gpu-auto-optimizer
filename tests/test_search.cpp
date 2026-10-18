#include "doctest/doctest.h"
#include "search_fixture.hpp"

TEST_CASE("highest_stable finds the edge of a monotonic range") {
    std::vector<int> probed;
    const int r = highest_stable(0, 300, 15, INT_MAX, [&](int v) { probed.push_back(v); return v <= 150; });
    CHECK(r == 150);
    CHECK(probed.size() <= 5);   // 20 candidates -> binary search
    for (int v : probed) CHECK(v % 15 == 0);
}

TEST_CASE("highest_stable never probes lo and returns lo when nothing above is stable") {
    std::vector<int> probed;
    const int r = highest_stable(0, 300, 15, INT_MAX, [&](int v) { probed.push_back(v); return false; });
    CHECK(r == 0);
    for (int v : probed) CHECK(v != 0);
}

TEST_CASE("highest_stable stays below the ceiling") {
    int max_probed = 0;
    const int r = highest_stable(0, 300, 15, 120, [&](int v) { max_probed = std::max(max_probed, v); return true; });
    CHECK(r == 105);
    CHECK(max_probed == 105);
}

TEST_CASE("ceiling at or below lo probes nothing") {
    int calls = 0;
    CHECK(highest_stable(0, 300, 15, 0, [&](int) { ++calls; return true; }) == 0);
    CHECK(calls == 0);
}

TEST_CASE("highest_stable with everything stable returns the top of the grid") {
    CHECK(highest_stable(50, 120, 5, INT_MAX, [](int) { return true; }) == 120);
    CHECK(highest_stable(0, 1500, 50, INT_MAX, [](int) { return true; }) == 1500);
}

TEST_CASE("lowest_passing finds the lowest value that still passes and never probes hi") {
    std::vector<int> probed;
    const int r = lowest_passing(50, 115, 5, [&](int v) { probed.push_back(v); return v >= 90; });
    CHECK(r == 90);
    for (int v : probed) CHECK(v != 115);
    CHECK(lowest_passing(50, 115, 5, [](int) { return true; }) == 50);
    CHECK(lowest_passing(50, 115, 5, [](int) { return false; }) == 115);
}

TEST_CASE("apply_margin scales and keeps one step of headroom") {
    CHECK(apply_margin(150, 15, 1.0f) == 135);
    CHECK(apply_margin(150, 15, 0.4f) == 60);
    CHECK(apply_margin(800, 50, 0.7f) == 550);
    CHECK(apply_margin(0, 15, 1.0f) == 0);
    CHECK(apply_margin(15, 15, 1.0f) == 0);
}

TEST_CASE("margin survives float rounding") {
    CHECK(apply_margin(150, 15, 0.7f) == 105);   // 0.7f * 150 / 15 = 6.99999...
}

TEST_CASE("best preset converges to the card's edges and applies the margin") {
    Run run;
    const auto r = run.go(Preset::BestOfMyGpu);
    REQUIRE(r.ok);
    CHECK(r.power_pct == 115);          // highest with 40 + 0.3p <= 75
    CHECK(r.core_max_stable == 150);
    CHECK(r.core_mhz == 105);
    CHECK(r.mem_max_stable == 800);
    CHECK(r.mem_mhz == 550);
    CHECK(run.card.core == 105);        // left applied
    CHECK(run.card.mem == 550);
    CHECK(run.card.power == 115);
    CHECK(r.soak.verdict == Verdict::Stable);
    CHECK(run.card.last_set_begin_ok.empty());
    CHECK(r.core_confirmed == 150);
    CHECK(r.mem_confirmed == 800);
}

TEST_CASE("a stock measurement the caller made is the baseline: the search takes none of its own") {
    Run plain;
    const auto without = plain.go(Preset::BestOfMyGpu);
    REQUIRE(without.ok);
    CHECK(plain.logged("baseline: 30 s at stock"));

    Run run;
    run.stock = StabilityResult{};
    run.stock->score = without.baseline.score;
    const auto r = run.go(Preset::BestOfMyGpu);
    REQUIRE(r.ok);
    CHECK_FALSE(run.logged("baseline: 30 s at stock"));
    CHECK(run.logged("baseline: the measurement at stock"));
    CHECK(run.card.probes == plain.card.probes - 1);   // the half minute at stock, and nothing else
    CHECK(r.baseline.score == without.baseline.score);
    CHECK(r.power_pct == without.power_pct);
    CHECK(r.core_mhz == without.core_mhz);
    CHECK(r.mem_mhz == without.mem_mhz);
}

TEST_CASE("a stock measurement that was not stable is no baseline") {
    Run run;
    run.stock = StabilityResult{};
    run.stock->verdict = Verdict::TooHot;
    run.stock->score = 1000;
    REQUIRE(run.go(Preset::BestOfMyGpu).ok);
    CHECK(run.logged("baseline: 30 s at stock"));
}

TEST_CASE("the search says which step it is at, and how long the long ones last") {
    Run run;
    const auto r = run.go(Preset::BestOfMyGpu);
    REQUIRE(r.ok);
    REQUIRE_FALSE(run.steps.empty());
    CHECK(run.steps.front() == std::make_pair(std::string("Baseline at stock"), 30.0));
    CHECK(run.said("Power limit 115 %", 20));
    CHECK(run.said("Trying memory clock +800 MHz", 0));     // a search probe: seconds, no countdown
    CHECK(run.said("Confirming memory clock +800 MHz", 30));
    CHECK(run.said("Trying core clock +150 MHz", 0));
    CHECK(run.said("Confirming core clock +150 MHz", 30));
    CHECK(run.steps.back() == std::make_pair(std::string("Final test of the result"), 300.0));
}

TEST_CASE("quiet preset picks the lowest power within 2 % of the reference score") {
    Run run;
    const auto r = run.go(Preset::Quiet);
    REQUIRE(r.ok);
    CHECK(r.power_pct == 90);
    CHECK(r.core_mhz == 60);
    CHECK(r.mem_mhz == 300);
}

TEST_CASE("max preset takes the thermal cap and one step below the clock edges") {
    Run run;
    const auto r = run.go(Preset::MaxPerformance);
    REQUIRE(r.ok);
    CHECK(r.power_pct == 120);
    CHECK(r.core_mhz == 135);
    CHECK(r.mem_mhz == 750);
}

TEST_CASE("cool preset tunes power only") {
    Run run;
    const auto r = run.go(Preset::CoolAndEfficient);
    REQUIRE(r.ok);
    CHECK(r.power_pct == 85);   // peak int(40 + 25.5) = 65 <= 65; 80 would cost 6 % score
    CHECK(r.core_mhz == 0);
    CHECK(r.mem_mhz == 0);
    CHECK(run.card.max_core_seen == 0);
}

TEST_CASE("journal ceilings are respected") {
    Run run;
    run.card.journal = {"{\"id\":3,\"core\":120,\"state\":\"begin\"}"};
    const auto r = run.go(Preset::BestOfMyGpu);
    REQUIRE(r.ok);
    CHECK(r.core_max_stable == 105);
    CHECK(run.card.max_core_seen < 120);
}

TEST_CASE("an unstable stock card aborts before tuning") {
    Run run;
    run.card.stock_unstable = true;
    const auto r = run.go(Preset::BestOfMyGpu);
    CHECK_FALSE(r.ok);
    CHECK(r.reason.find("stock") != std::string::npos);
    CHECK(run.card.probes == 1);
    CHECK(run.card.core == 0);
    CHECK(run.card.power == 100);
    CHECK(r.stock_restored);
}

TEST_CASE("a failed soak steps both clocks down once and succeeds") {
    Run run;
    run.card.soak_failures = 1;
    const auto r = run.go(Preset::BestOfMyGpu);
    REQUIRE(r.ok);
    CHECK(r.core_mhz == 90);
    CHECK(r.mem_mhz == 500);
}

TEST_CASE("four failed soaks give up and restore stock") {
    Run run;
    run.card.soak_failures = 4;
    const auto r = run.go(Preset::BestOfMyGpu);
    CHECK_FALSE(r.ok);
    CHECK(run.card.core == 0);
    CHECK(run.card.mem == 0);
    CHECK(run.card.power == 100);
}

TEST_CASE("abort during the clock search restores stock") {
    Run run;
    // Probe 25 is core +30 with memory +550 applied: baseline, 4 power, 17
    // memory, the memory confirm, core +15, then this one.
    run.abort_after_probes = 25;
    const auto r = run.go(Preset::BestOfMyGpu);
    CHECK_FALSE(r.ok);
    CHECK(r.reason == "aborted");
    CHECK(run.card.probes == 25);
    CHECK(run.logged("core +30 / mem +550: STABLE"));
    CHECK(run.card.core == 0);
    CHECK(run.card.mem == 0);
    CHECK(run.card.power == 100);
}

TEST_CASE("abort during the power step restores stock power") {
    Run run;
    run.abort_after_probes = 2;   // baseline + first power probe
    const auto r = run.go(Preset::BestOfMyGpu);
    CHECK_FALSE(r.ok);
    CHECK(r.reason == "aborted");
    CHECK(run.card.power == 100);
}

TEST_CASE("no power control skips the power step") {
    Run run;
    const auto r = run.go(Preset::BestOfMyGpu, run.card.gpu(/*with_power=*/false));
    REQUIRE(r.ok);
    CHECK(r.power_pct == 100);
    CHECK(run.card.power_probes == 0);
}

TEST_CASE("power range without 100 % skips the power step") {
    Run run;
    const auto r = run.go(Preset::BestOfMyGpu, run.card.gpu(true, {110, 120}));
    REQUIRE(r.ok);
    CHECK(r.power_pct == 100);
    CHECK(run.card.power_probes == 0);
}

TEST_CASE("a setter that fails stops the run at stock") {
    Run run;
    run.card.fail_core_set_at = 150;
    const auto r = run.go(Preset::BestOfMyGpu);
    CHECK_FALSE(r.ok);
    CHECK(r.reason.find("core") != std::string::npos);
    CHECK(run.card.core == 0);
    CHECK(run.card.mem == 0);      // core +150 was written with memory +550 applied
    CHECK(run.card.power == 100);
}

TEST_CASE("every clock candidate is closed in the journal") {
    Run run;
    run.go(Preset::BestOfMyGpu);
    Journal reread(run.card.journal, [](const std::string&) { return true; });
    CHECK(reread.freezes().empty());
}

TEST_CASE("efficiency reference ignores one noisy low probe at the cap") {
    // Probes scatter a few percent; measured on the 4070, the cap probe read
    // 3 % below its neighbour and quiet ended up costing 4 %, not < 2 %.
    Run run;
    run.card.noisy_power_pct = 120;
    const auto r = run.go(Preset::Quiet);
    REQUIRE(r.ok);
    CHECK(r.power_pct == 90);
}

TEST_CASE("Ctrl+C during the soak still restores stock") {
    Run run;
    run.card.abort_on_soak = true;   // the soak itself would pass
    const auto r = run.go(Preset::BestOfMyGpu);
    CHECK_FALSE(r.ok);
    CHECK(r.reason == "aborted");
    CHECK(run.card.core == 0);
    CHECK(run.card.mem == 0);
    CHECK(run.card.power == 100);
}

TEST_CASE("a failed reset is reported, not claimed as stock") {
    Run run;
    run.card.stock_unstable = true;
    run.card.reset_fails_from = 1;   // the first reset (before baseline) works, the stop path's fails
    const auto r = run.go(Preset::BestOfMyGpu);
    CHECK_FALSE(r.ok);
    CHECK_FALSE(r.stock_restored);
    bool told = false;
    for (const auto& m : run.log) told |= m.find("reset FAILED") != std::string::npos;
    CHECK(told);
}

TEST_CASE("a soak that runs too hot lowers power, not clocks") {
    // 20 s power probes read cooler than a 300 s soak; lower clocks barely
    // change temperature, lower power does.
    Run run;
    run.card.soak_extra_heat = 2;    // at 115 %: 74 + 2 = 76 > 75
    const auto r = run.go(Preset::BestOfMyGpu);
    REQUIRE(r.ok);
    CHECK(r.power_pct == 110);
    CHECK(r.core_mhz == 105);
    CHECK(r.mem_mhz == 550);
}

TEST_CASE("confirm_edge keeps a holding edge and steps down otherwise") {
    int calls = 0;
    CHECK(confirm_edge(150, 0, 15, 3, [&](int) { ++calls; return true; }) == 150);
    CHECK(calls == 1);
    CHECK(confirm_edge(150, 0, 15, 3, [](int v) { return v <= 135; }) == 135);
    calls = 0;
    CHECK(confirm_edge(150, 0, 15, 3, [&](int) { ++calls; return false; }) == 0);
    CHECK(calls == 3);
    calls = 0;
    CHECK(confirm_edge(0, 0, 15, 3, [&](int) { ++calls; return false; }) == 0);
    CHECK(calls == 0);
    CHECK(confirm_edge(15, 0, 15, 3, [](int) { return false; }) == 0);
}

TEST_CASE("memory search stops at the bandwidth peak, not the stability edge") {
    Run run;
    run.card.mem_edge = 1400;
    run.card.bw_curve = [](int m) { return m <= 900 ? 500 + m * 0.3 : 770 - (m - 900) * 0.6; };
    const auto r = run.go(Preset::BestOfMyGpu);
    REQUIRE(r.ok);
    // The scan ends at +1000, the first step that has lost 5 % (7.8 % below
    // the peak at +900), far below the first wrong result at +1450. The second
    // pass reads from four steps below that, +800, and keeps +800: the peak
    // itself is inside the margin. That is the price of the margin on a card
    // that has a peak.
    CHECK(run.card.max_mem_seen == 1000);
    CHECK(run.card.max_again_mem == 800);
    CHECK(r.mem_max_stable == 800);
    CHECK(r.mem_confirmed == 800);
    CHECK(r.mem_mhz == 550);
    Journal reread(run.card.journal, [](const std::string&) { return true; });
    CHECK(reread.freezes().empty());
}

TEST_CASE("the margin applies to the confirmed core edge") {
    Run run;
    run.card.confirm_fail_core = {150};
    const auto r = run.go(Preset::BestOfMyGpu);
    REQUIRE(r.ok);
    CHECK(r.core_max_stable == 150);
    CHECK(r.core_confirmed == 135);
    CHECK(r.core_mhz == 90);
}

TEST_CASE("without a reconnect a failing bandwidth measurement never raises memory") {
    // As it was before the reset budget: the scan ends at the first failed
    // measurement, here the one at +0. With a reconnect that case leaves the
    // memory at stock as well, and says why (tested below).
    Run run;
    run.card.bw_curve = [](int m) { return 500 + m * 0.1; };
    run.card.bw_fails = true;
    const auto r = run.go(Preset::BestOfMyGpu);
    REQUIRE(r.ok);
    CHECK(r.mem_mhz == 0);
    CHECK(r.driver_resets == 0);
    CHECK(run.card.bw_measurements == 1);
    CHECK(run.card.max_mem_seen == 0);
    CHECK(run.logged("mem: bandwidth peak +0"));
    CHECK_FALSE(run.logged("bandwidth measurement not available"));
}

TEST_CASE("a confirm probe that only runs too hot keeps the clock edge") {
    // Heat is the power step's and the soak's business: confirming clocks at a
    // slightly hotter 30 s must not step core and memory down to nothing.
    Run run;
    run.card.confirm_extra_heat = 2;   // at 115 %: 74 + 2 = 76 > 75
    const auto r = run.go(Preset::BestOfMyGpu);
    REQUIRE(r.ok);
    CHECK(r.core_confirmed == 150);
    CHECK(r.mem_confirmed == 800);
}

TEST_CASE("a journal entry that cannot be closed stops the run at stock") {
    // An open entry would blacklist the candidate forever as if it had frozen
    // the machine; carrying on would hide that the journal is failing.
    Run run;
    run.card.completes_ok = 2;
    const auto r = run.go(Preset::BestOfMyGpu);
    CHECK_FALSE(r.ok);
    CHECK(r.reason.find("journal") != std::string::npos);
    CHECK(run.card.core == 0);
    CHECK(run.card.mem == 0);     // the entry that fails to close is memory +150
    CHECK(begins_with_only(run.card, "mem", "core") == std::vector<int>{50, 100, 150});
    CHECK(run.card.power == 100);
}

TEST_CASE("the soak runs 300 s and the log says so") {
    Run run;
    const auto r = run.go(Preset::BestOfMyGpu);
    REQUIRE(r.ok);
    CHECK(r.soak.seconds == 300);
    bool told = false;
    for (const auto& m : run.log) told |= m.find("soak: 300 s at power") != std::string::npos;
    CHECK(told);
}

TEST_CASE("a soak stopped while it runs ends at stock and leaves no ceiling") {
    Run run;
    run.card.abort_in_probe_s = 300;
    const auto r = run.go(Preset::BestOfMyGpu);
    CHECK_FALSE(r.ok);
    CHECK(r.reason == "aborted");
    CHECK(r.stock_restored);
    CHECK(run.card.core == 0);
    CHECK(run.card.mem == 0);
    CHECK(run.card.power == 100);
    REQUIRE_FALSE(run.card.journal.empty());
    CHECK(run.card.journal.back().find("ABORTED") != std::string::npos);
    Journal reread(run.card.journal, [](const std::string&) { return true; });
    CHECK(reread.freezes().empty());
    CHECK(reread.ceilings().core_mhz == INT_MAX);
    CHECK(reread.ceilings().mem_mhz == INT_MAX);
}

TEST_CASE("a clock probe stopped while it runs is closed as ABORTED, not left open") {
    Run run;
    run.card.abort_in_probe_s = 3;   // the first clock candidate, memory +50
    const auto r = run.go(Preset::BestOfMyGpu);
    CHECK_FALSE(r.ok);
    CHECK(r.reason == "aborted");
    CHECK(run.card.core == 0);
    CHECK(run.card.mem == 0);
    CHECK(run.card.power == 100);
    REQUIRE(run.card.journal.size() == 2);   // one begin, one complete
    CHECK(begins_with_only(run.card, "mem", "core") == std::vector<int>{50});
    CHECK(run.card.journal.back().find("ABORTED") != std::string::npos);
    Journal reread(run.card.journal, [](const std::string&) { return true; });
    CHECK(reread.freezes().empty());
    CHECK(reread.ceilings().core_mhz == INT_MAX);
    CHECK(reread.ceilings().mem_mhz == INT_MAX);
}

TEST_CASE("a baseline stopped while it runs ends before any clock is touched") {
    Run run;
    run.card.abort_in_probe_s = 30;   // the baseline is the first 30 s probe
    const auto r = run.go(Preset::BestOfMyGpu);
    CHECK_FALSE(r.ok);
    CHECK(r.reason == "aborted");
    CHECK(run.card.probes == 1);
    CHECK(run.card.max_core_seen == 0);
    CHECK(run.card.journal.empty());
}

TEST_CASE("a power probe stopped while it runs restores stock power") {
    Run run;
    run.card.abort_in_probe_s = 20;
    const auto r = run.go(Preset::BestOfMyGpu);
    CHECK_FALSE(r.ok);
    CHECK(r.reason == "aborted");
    CHECK(run.card.power == 100);
    CHECK(run.card.max_core_seen == 0);
    CHECK(run.card.journal.empty());
}

TEST_CASE("confirm_edge steps down by what step_down says") {
    std::vector<int> tried;
    const auto fails_at_150_and_above = [&](int v) { tried.push_back(v); return v < 150; };
    CHECK(confirm_edge(150, 0, 15, 3, fails_at_150_and_above, [] { return 4; }) == 90);
    CHECK(tried == std::vector<int>{150, 90});
    // Never below lo, and never lo itself: +30 fails, four steps down is past stock.
    int calls = 0;
    CHECK(confirm_edge(30, 0, 15, 3, [&](int) { ++calls; return false; }, [] { return 4; }) == 0);
    CHECK(calls == 1);
    // step_down is asked after each failed try.
    tried.clear();
    int asked = 0;
    CHECK(confirm_edge(300, 0, 15, 3, [&](int v) { tried.push_back(v); return false; },
                       [&] { return ++asked == 1 ? 4 : 1; }) == 0);
    CHECK(tried == std::vector<int>{300, 240, 225});
    // An empty step_down is one step, as before.
    CHECK(confirm_edge(150, 0, 15, 3, [](int v) { return v <= 135; }, {}) == 135);
    CHECK(confirm_edge(150, 0, 15, 3, [](int v) { return v <= 135; }) == 135);
}
