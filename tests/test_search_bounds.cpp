#include "doctest/doctest.h"
#include "search_fixture.hpp"

TEST_CASE("climb_to_edge climbs in strides, then refines on the step grid") {
    std::vector<int> probed;
    const int r = climb_to_edge(0, 300, 15, 60, INT_MAX, [&](int v) { probed.push_back(v); return v <= 150; });
    CHECK(r == 150);
    CHECK(probed == std::vector<int>{60, 120, 180, 150, 165});
}

TEST_CASE("climb_to_edge finds an edge between strides and between grid points") {
    CHECK(climb_to_edge(0, 300, 15, 60, INT_MAX, [](int v) { return v <= 100; }) == 90);
    CHECK(climb_to_edge(0, 300, 15, 60, INT_MAX, [](int v) { return v <= 120; }) == 120);
    CHECK(climb_to_edge(0, 300, 15, 60, INT_MAX, [](int v) { return v <= 15; }) == 15);
}

TEST_CASE("climb_to_edge never probes lo, and returns lo when nothing above holds") {
    std::vector<int> probed;
    const int r = climb_to_edge(0, 300, 15, 60, INT_MAX, [&](int v) { probed.push_back(v); return false; });
    CHECK(r == 0);
    CHECK(probed == std::vector<int>{60, 30, 15});
}

TEST_CASE("climb_to_edge never exceeds hi, even when hi is off the grid") {
    int max_probed = 0;
    const int r = climb_to_edge(0, 1000, 15, 60, INT_MAX, [&](int v) { max_probed = std::max(max_probed, v); return true; });
    CHECK(r == 990);          // the highest grid point at or below +1000
    CHECK(max_probed == 990);
    CHECK(climb_to_edge(0, 300, 15, 60, INT_MAX, [](int) { return true; }) == 300);
}

TEST_CASE("climb_to_edge stays below the journal ceiling") {
    int max_probed = 0;
    const int r = climb_to_edge(0, 300, 15, 60, 120, [&](int v) { max_probed = std::max(max_probed, v); return true; });
    CHECK(r == 105);
    CHECK(max_probed == 105);
    int calls = 0;
    CHECK(climb_to_edge(0, 300, 15, 60, 0, [&](int) { ++calls; return true; }) == 0);
    CHECK(calls == 0);
}

TEST_CASE("climb_to_edge never probes a value twice, and never above the first failure") {
    // A marginal clock can pass one probe and fail the next. A value that
    // failed is not given a second chance, and nothing above it is tried.
    std::map<int, int> calls;
    int first_failure = INT_MAX;
    climb_to_edge(0, 600, 15, 60, INT_MAX, [&](int v) {
        ++calls[v];
        CHECK(v < first_failure);
        const bool ok = v <= 260;
        if (!ok) first_failure = std::min(first_failure, v);
        return ok;
    });
    for (const auto& [v, n] : calls) CHECK(n == 1);
}

TEST_CASE("the core search probes upward from stock, never from the middle of the range") {
    Run run;
    const auto r = run.go(Preset::BestOfMyGpu);
    REQUIRE(r.ok);
    CHECK(r.core_max_stable == 150);
    CHECK(run.card.max_core_seen == 165);   // one step past the edge, not +225 or +300
}

TEST_CASE("plausible_range accepts a normal range and rejects each kind of nonsense") {
    CHECK(plausible_range({-500, 1000}, 0, 2000));
    CHECK(plausible_range({0, 1000}, 0, 2000));          // a card that allows no negative offset
    CHECK(plausible_range({-500, 1000}, 210, 2000));     // a tune is applied
    CHECK_FALSE(plausible_range({100, 1000}, 0, 2000));  // minimum above zero
    CHECK_FALSE(plausible_range({-500, 0}, 0, 2000));    // maximum not above zero
    CHECK_FALSE(plausible_range({0, 0}, 0, 2000));       // an empty buffer
    CHECK_FALSE(plausible_range({-500, 1000}, 1200, 2000));   // the applied offset is outside it
    CHECK_FALSE(plausible_range({-500, 1000}, -600, 2000));
    CHECK_FALSE(plausible_range({-500000, 1000000}, 0, 2000));   // kHz read as MHz
    CHECK(plausible_range({-500, 2000}, 0, 2000));       // the sanity limit itself is allowed
    CHECK_FALSE(plausible_range({-500, 2001}, 0, 2000));
}

TEST_CASE("search_bounds falls back when the card reports nothing usable") {
    GpuControl none;
    CHECK(search_bounds(none).core_max_mhz == 300);
    CHECK(search_bounds(none).mem_max_mhz == 1500);
    CHECK_FALSE(search_bounds(none).core_from_card);

    GpuControl no_read_back;   // a range, but no way to check the applied offset against it
    no_read_back.clock_offset_range_mhz = [] { return std::optional<ClockOffsetRanges>(ClockOffsetRanges{{-500, 1000}, {-1000, 3000}}); };
    CHECK(search_bounds(no_read_back).core_max_mhz == 300);

    GpuControl read_fails = no_read_back;
    read_fails.read_applied = []() -> std::optional<AppliedState> { return std::nullopt; };
    CHECK(search_bounds(read_fails).core_max_mhz == 300);

    GpuControl range_fails;
    range_fails.clock_offset_range_mhz = []() -> std::optional<ClockOffsetRanges> { return std::nullopt; };
    range_fails.read_applied = [] { return std::optional<AppliedState>(AppliedState{}); };
    CHECK(search_bounds(range_fails).mem_max_mhz == 1500);
}

TEST_CASE("search_bounds takes core and memory from the card independently") {
    GpuControl g;
    g.read_applied = [] { return std::optional<AppliedState>(AppliedState{}); };
    g.clock_offset_range_mhz = [] { return std::optional<ClockOffsetRanges>(ClockOffsetRanges{{-500, 1000}, {-1000, 3000}}); };
    const SearchBounds both = search_bounds(g);
    CHECK(both.core_max_mhz == 1000);
    CHECK(both.mem_max_mhz == 3000);
    CHECK(both.core_from_card);
    CHECK(both.mem_from_card);

    g.clock_offset_range_mhz = [] { return std::optional<ClockOffsetRanges>(ClockOffsetRanges{{-500, 1000}, {50, 9000}}); };
    const SearchBounds core_only = search_bounds(g);
    CHECK(core_only.core_max_mhz == 1000);
    CHECK(core_only.mem_max_mhz == 1500);
    CHECK_FALSE(core_only.mem_from_card);
}

TEST_CASE("a card that holds more than the old cap is searched to its own edge") {
    Run run;
    run.card.ranges = ClockOffsetRanges{{-500, 1000}, {-1000, 3000}};
    run.card.core_edge = 450;
    run.card.mem_edge = 2000;
    const auto r = run.go(Preset::BestOfMyGpu);
    REQUIRE(r.ok);
    CHECK(r.core_max_stable == 450);
    CHECK(r.core_confirmed == 450);
    CHECK(r.core_mhz == 315);            // 70 % of 450
    CHECK(r.mem_max_stable == 2000);
    CHECK(r.mem_mhz == 1400);            // 70 % of 2000
    CHECK(run.card.max_core_seen == 465);    // one step past the edge
    CHECK(run.card.max_mem_seen == 2050);
    bool told = false;
    for (const auto& m : run.log) told |= m.find("core range: up to +1000 MHz (reported by the card)") != std::string::npos;
    CHECK(told);
}

TEST_CASE("a card stable to the top of its reported range stops at that range") {
    Run run;
    run.card.ranges = ClockOffsetRanges{{-500, 400}, {-1000, 1000}};
    run.card.core_edge = 5000;
    run.card.mem_edge = 5000;
    const auto r = run.go(Preset::MaxPerformance);
    REQUIRE(r.ok);
    CHECK(r.core_max_stable == 390);     // the highest 15 MHz grid point at or below +400
    CHECK(run.card.max_core_seen == 390);
    CHECK(r.mem_max_stable == 1000);
    CHECK(run.card.max_mem_seen == 1000);
}

TEST_CASE("an implausible reported range leaves the built-in limit in place and says so") {
    Run run;
    run.card.ranges = ClockOffsetRanges{{100, 1000}, {-1000, 3000}};   // core minimum above zero
    run.card.core_edge = 450;
    const auto r = run.go(Preset::BestOfMyGpu);
    REQUIRE(r.ok);
    CHECK(r.core_max_stable == 300);
    CHECK(run.card.max_core_seen == 300);
    bool told = false;
    for (const auto& m : run.log) told |= m.find("core range: up to +300 MHz (built-in limit") != std::string::npos;
    CHECK(told);
}

TEST_CASE("a failing range read leaves the built-in limits in place") {
    Run run;
    run.card.ranges_read_fails = true;
    run.card.core_edge = 450;
    const auto r = run.go(Preset::BestOfMyGpu);
    REQUIRE(r.ok);
    CHECK(r.core_max_stable == 300);
}

TEST_CASE("memory is searched before the core") {
    Run run;
    run.card.bw_curve = [](int m) { return 500 + m * 0.3; };
    const auto r = run.go(Preset::BestOfMyGpu);
    REQUIRE(r.ok);
    int mem_begins = 0, core_begins = 0, mem_after_core = 0;
    for (const auto& l : run.card.journal) {
        if (l.find("\"begin\"") == std::string::npos) continue;
        const bool has_core = l.find("\"core\":") != std::string::npos;
        const bool has_mem = l.find("\"mem\":") != std::string::npos;
        if (has_core && !has_mem) ++core_begins;
        if (has_mem && !has_core) {
            ++mem_begins;
            if (core_begins > 0) ++mem_after_core;
        }
    }
    CHECK(mem_begins > 0);
    CHECK(core_begins > 0);
    CHECK(mem_after_core == 0);
}

TEST_CASE("the core climb runs with the chosen memory applied") {
    Run run;
    const auto r = run.go(Preset::BestOfMyGpu);
    REQUIRE(r.ok);
    CHECK(r.mem_mhz == 550);
    // 11 core candidates, the core confirm probe and the soak.
    CHECK(run.card.mem_during_core_probes.size() == 13);
    for (int mem : run.card.mem_during_core_probes) CHECK(mem == r.mem_mhz);
}

TEST_CASE("the core climb advances one step at a time") {
    Run run;
    const auto r = run.go(Preset::BestOfMyGpu);
    REQUIRE(r.ok);
    // The climb, then the one 30 s confirm probe at the edge.
    CHECK(begins_with_only(run.card, "core", "mem") ==
          std::vector<int>{15, 30, 45, 60, 75, 90, 105, 120, 135, 150, 165, 150});
    CHECK(r.core_max_stable == 150);
    CHECK(run.card.max_core_seen == 165);
}

TEST_CASE("the stability-only memory climb advances one step at a time") {
    Run run;   // no bw_curve: no bandwidth measurement
    const auto r = run.go(Preset::BestOfMyGpu);
    REQUIRE(r.ok);
    // The climb, then the one 30 s confirm probe at the edge.
    CHECK(begins_with_only(run.card, "mem", "core") ==
          std::vector<int>{50, 100, 150, 200, 250, 300, 350, 400, 450, 500, 550, 600, 650, 700, 750, 800, 850, 800});
    CHECK(r.mem_max_stable == 800);
    CHECK(run.card.max_mem_seen == 850);
}

TEST_CASE("every profile gives today's result on a card with no reset") {
    struct Want { Preset preset; int power, core, mem; };
    const Want wants[] = {{Preset::BestOfMyGpu, 115, 105, 550}, {Preset::Quiet, 90, 60, 300},
                          {Preset::MaxPerformance, 120, 135, 750}, {Preset::CoolAndEfficient, 85, 0, 0}};
    for (const auto& w : wants) {
        Run run;
        const auto r = run.go(w.preset);
        REQUIRE(r.ok);
        CHECK(r.power_pct == w.power);
        CHECK(r.core_mhz == w.core);
        CHECK(r.mem_mhz == w.mem);
        CHECK(run.card.power == w.power);
        CHECK(run.card.core == w.core);
        CHECK(run.card.mem == w.mem);
    }
}

TEST_CASE("a profile without core tuning and a card without power control still run") {
    Run cool;
    const auto a = cool.go(Preset::CoolAndEfficient);
    REQUIRE(a.ok);
    CHECK(cool.card.max_core_seen == 0);
    CHECK(cool.card.max_mem_seen == 0);
    CHECK(cool.card.journal.empty() == false);   // the soak is journaled

    Run fixed;
    const auto b = fixed.go(Preset::BestOfMyGpu, fixed.card.gpu(/*with_power=*/false));
    REQUIRE(b.ok);
    CHECK(fixed.card.power_probes == 0);
    CHECK(b.power_pct == 100);
    CHECK(b.core_mhz == 105);
    CHECK(b.mem_mhz == 550);
}
