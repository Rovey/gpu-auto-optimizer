#include "doctest/doctest.h"
#include "search_fixture.hpp"

TEST_CASE("the bandwidth scan ends at the first step that has clearly lost bandwidth") {
    int max_sampled = -1;
    const MemScanEnd end = bandwidth_scan(0, 1500, 50, INT_MAX, [&](int v) {
        max_sampled = std::max(max_sampled, v);
        return MemSample{true, v <= 600 ? 500 + v * 0.3 : 680 - (v - 600) * 0.9};
    });
    CHECK(end.why == MemScanEnd::Why::Dropped);
    // The best is the middle of three steps, 665 GB/s around the peak at
    // +600: +650 is 4.5 % below it, +700 is 11 %. It was stable, so it passed.
    CHECK(end.top == 700);
    CHECK(max_sampled == 700);
}

TEST_CASE("a single low bandwidth reading does not end the scan; three do") {
    // A step that is further from the best than readings scatter is read
    // kBandwidthReadings times and its middle reading counts. One dip (measured
    // on the 4070: a reading at half speed, the memory clock still in a lower
    // P-state) ends nothing; a drop that is there every time does.
    std::map<int, int> calls;
    MemScanEnd end = bandwidth_scan(0, 1500, 50, INT_MAX, [&](int v) {
        const bool dip = v == 300 && calls[v] == 0;
        ++calls[v];
        return MemSample{true, dip ? 250.0 : 500 + v * 0.1};
    });
    CHECK(end.why == MemScanEnd::Why::Limit);
    CHECK(end.top == 1500);
    CHECK(calls[250] == 1);   // nothing in doubt: one reading
    CHECK(calls[300] == 3);
    CHECK(calls[350] == 1);

    // Two low readings of three are the middle one.
    calls.clear();
    end = bandwidth_scan(0, 1500, 50, INT_MAX, [&](int v) {
        const int nth = calls[v]++;
        return MemSample{true, v == 300 && nth < 2 ? 250.0 : 500 + v * 0.1};
    });
    CHECK(end.why == MemScanEnd::Why::Dropped);
    CHECK(end.top == 300);
    CHECK(calls[300] == 3);
    CHECK(calls.count(350) == 0);

    // A step that stops being stable while it is read again ends the scan below it.
    calls.clear();
    end = bandwidth_scan(0, 1500, 50, INT_MAX, [&](int v) {
        const int nth = calls[v]++;
        return MemSample{!(v == 300 && nth == 1), v == 300 ? 250.0 : 500 + v * 0.1};
    });
    CHECK(end.why == MemScanEnd::Why::Failed);
    CHECK(end.top == 250);
    CHECK(calls[300] == 2);
}

TEST_CASE("a single high bandwidth reading does not become the best the scan is held against") {
    // The run that saved +100 MHz on a card that runs every game at +1500: one
    // reading came out far above its neighbours, and the steps after it were
    // "a drop".
    std::map<int, int> calls;
    const MemScanEnd end = bandwidth_scan(0, 1500, 50, INT_MAX, [&](int v) {
        const bool spike = v == 200 && calls[v] == 0;
        ++calls[v];
        return MemSample{true, (500 + v * 0.1) * (spike ? 1.08 : 1.0)};
    });
    CHECK(end.why == MemScanEnd::Why::Limit);
    CHECK(end.top == 1500);
    for (const auto& [v, n] : calls) CHECK(n == 1);   // and no step after it was in doubt

    // Two high steps in a row are the middle of three, and so a best: what
    // comes after them is that far below it, read three times, and ends the scan.
    const MemScanEnd two = bandwidth_scan(0, 1500, 50, INT_MAX, [&](int v) {
        return MemSample{true, (500 + v * 0.1) * (v == 200 || v == 250 ? 1.08 : 1.0)};
    });
    CHECK(two.why == MemScanEnd::Why::Dropped);
    CHECK(two.top == 300);
}

TEST_CASE("the bandwidth scan ends below the first unstable step") {
    int max_sampled = -1;
    const MemScanEnd end = bandwidth_scan(0, 1500, 50, INT_MAX, [&](int v) {
        max_sampled = std::max(max_sampled, v);
        return MemSample{v <= 400, 500 + v * 0.1};
    });
    CHECK(end.why == MemScanEnd::Why::Failed);
    CHECK(end.top == 400);
    CHECK(max_sampled == 450);
}

TEST_CASE("the bandwidth scan samples lo and respects the ceiling") {
    std::vector<int> sampled;
    const MemScanEnd end = bandwidth_scan(0, 1500, 50, 300, [&](int v) {
        sampled.push_back(v);
        return MemSample{true, 500 + v * 0.1};
    });
    REQUIRE_FALSE(sampled.empty());
    CHECK(sampled.front() == 0);
    CHECK(sampled.back() == 250);
    CHECK(end.top == 250);
    CHECK(end.why == MemScanEnd::Why::Limit);
    const MemScanEnd none = bandwidth_scan(0, 1500, 50, 300, [](int) { return MemSample{false, 0}; });
    CHECK(none.top == 0);
    CHECK(none.why == MemScanEnd::Why::Failed);
}

TEST_CASE("the second pass begins four steps below the highest step the scan passed") {
    using Why = MemScanEnd::Why;
    // The step above +2500 failed: +2500 passed, so +2300.
    CHECK(second_pass_top({2500, Why::Failed}, 0, 50, INT_MAX) == 2300);
    // +1000 passed and had lost bandwidth: +800.
    CHECK(second_pass_top({1000, Why::Dropped}, 0, 50, INT_MAX) == 800);
    // The driver's range, or the ceiling of a freeze: nothing failed, and the
    // margin is kept all the same. What is above was never tried.
    CHECK(second_pass_top({3000, Why::Limit}, 0, 50, INT_MAX) == 2800);
    // Never below lo, and a scan that passed nothing reads nothing above it.
    CHECK(second_pass_top({150, Why::Failed}, 0, 50, INT_MAX) == 0);
    CHECK(second_pass_top({0, Why::Failed}, 0, 50, INT_MAX) == 0);
}

TEST_CASE("a scan held below a known edge counts the margin from that edge, once") {
    using Why = MemScanEnd::Why;
    // The run that found the edge at +2550 read from +2300. The run after it is
    // held at +2350 and reads from +2300 as well, not from +2150.
    CHECK(second_pass_top({2500, Why::Failed}, 0, 50, INT_MAX) == 2300);
    CHECK(second_pass_top({2350, Why::Limit}, 0, 50, 2550) == 2300);
    // A step that failed below the known edge counts, not the edge.
    CHECK(second_pass_top({2000, Why::Failed}, 0, 50, 2550) == 1800);
    CHECK(second_pass_top({2000, Why::Dropped}, 0, 50, 2550) == 1800);
    // An edge that is no higher than where the scan got changes nothing.
    CHECK(second_pass_top({2350, Why::Limit}, 0, 50, 2400) == 2150);
}

TEST_CASE("the second pass goes down from the top and keeps the lowest offset within the tie") {
    // A curve that still rises at the top: 0.4 % a step.
    std::vector<int> sampled;
    int r = bandwidth_peak_below(2300, 0, 50, [&](int v) {
        sampled.push_back(v);
        return MemSample{true, 434.0 * (1 + 0.004 * v / 50)};
    });
    CHECK(r == 2200);
    CHECK(sampled == std::vector<int>{2300, 2250, 2200, 2150});

    // A peak below the top: the readings rise on the way down, then fall.
    sampled.clear();
    r = bandwidth_peak_below(1200, 0, 50, [&](int v) {
        sampled.push_back(v);
        return MemSample{true, v <= 900 ? 500 + v * 0.3 : 770 - (v - 900) * 0.6};
    });
    CHECK(r == 900);
    CHECK(sampled.front() == 1200);
    CHECK(sampled.back() == 850);   // 1.9 % below the peak: the pass ends

    // A flat curve keeps the lowest offset there is.
    CHECK(bandwidth_peak_below(400, 0, 50, [](int v) { return MemSample{true, 500.0 + ((v / 50) % 2)}; }) == 0);
    // Readings that scatter by twice what they do on a quiet 4070 leave a flat
    // curve at the bottom.
    CHECK(bandwidth_peak_below(400, 0, 50, [](int v) { return MemSample{true, 500.0 * (1 + (((v / 50) % 3) - 1) * 0.004)}; }) == 0);
}

TEST_CASE("the second pass keeps what it has when a step is not stable") {
    // Not one sample: the top it was given.
    CHECK(bandwidth_peak_below(2300, 0, 50, [](int) { return MemSample{false, 0}; }) == 2300);
    // One sample, then none: that one.
    int n = 0;
    CHECK(bandwidth_peak_below(2300, 0, 50, [&](int v) { return MemSample{n++ == 0, 500.0 + v * 0.1}; }) == 2300);
    // Never below lo.
    std::vector<int> sampled;
    CHECK(bandwidth_peak_below(100, 0, 50, [&](int v) { sampled.push_back(v); return MemSample{true, 500.0}; }) == 0);
    CHECK(sampled == std::vector<int>{100, 50, 0});
}

namespace {

// A card like the reference RTX 4070 (hardware check 94): the bandwidth rises
// by 0.4 % per 50 MHz step all the way to the first wrong result. There is no
// peak to find; only scatter can end the scan before the edge.
Run rising_to_the_edge(int edge = 2500) {
    Run run;
    run.card.mem_edge = edge;
    run.card.ranges = ClockOffsetRanges{{-500, 1000}, {-1000, 3000}};
    run.card.bw_curve = [](int m) { return 434.0 * (1 + 0.004 * m / 50); };
    return run;
}

// Scatter with a standard deviation of about `sd`, the same for the same
// (seed, offset, reading): three uniform numbers summed, which is bell-shaped
// enough and needs no library whose numbers differ between compilers.
std::function<double(int, int)> scatter(unsigned seed, double sd) {
    return [seed, sd](int mem, int nth) {
        unsigned x = seed * 2654435761u + static_cast<unsigned>(mem) * 40503u + static_cast<unsigned>(nth) * 9176u + 12345u;
        double sum = 0;
        for (int i = 0; i < 3; ++i) {
            x ^= x << 13; x ^= x >> 17; x ^= x << 5;
            sum += (x % 100000) / 100000.0 - 0.5;   // uniform in -0.5 .. 0.5, variance 1/12
        }
        return 1.0 + sum * 2.0 * sd;   // three of them: variance 1/4, so a deviation of sd
    };
}

}

TEST_CASE("one bandwidth reading that is far too high does not end the memory scan") {
    // The run that saved +100 MHz on a card that runs every game at +1500:
    // one reading came out far above its neighbours, the two after it were
    // under it, and that was "the peak".
    Run run = rising_to_the_edge();
    run.card.bw_noise = [](int mem, int nth) { return mem == 200 && nth == 0 ? 1.08 : 1.0; };
    const auto r = run.go(Preset::BestOfMyGpu);
    REQUIRE(r.ok);
    CHECK(run.card.max_mem_seen == 2550);   // the scan went to the first wrong result
    CHECK(run.logged("mem +0: bandwidth 434.0 GB/s"));
    CHECK(run.card.bw_measurements == 51);   // +0 .. +2500, each once: no step was in doubt
    // +2500 passed; the second pass reads from +2300 down, and +2200 is the
    // lowest offset within 1 % of what it read there.
    CHECK(r.mem_max_stable == 2200);
    CHECK(r.mem_mhz == 1500);
}

TEST_CASE("one low bandwidth reading does not end the memory scan; three low ones do") {
    {
        Run run = rising_to_the_edge();
        run.card.bw_noise = [](int mem, int nth) { return mem == 1000 && nth == 0 ? 0.9 : 1.0; };
        const auto r = run.go(Preset::BestOfMyGpu);
        REQUIRE(r.ok);
        CHECK(run.card.bw_readings_at[950] == 1);
        CHECK(run.card.bw_readings_at[1000] == kBandwidthReadings);
        CHECK(run.card.max_mem_seen == 2550);
        CHECK(r.mem_max_stable == 2200);
    }
    {
        // From +1000 on the memory has lost 8 %: that is a peak, whichever copy reads it.
        Run run = rising_to_the_edge();
        run.card.bw_curve = [](int m) { return 434.0 * (1 + 0.004 * m / 50) * (m >= 1000 ? 0.92 : 1.0); };
        const auto r = run.go(Preset::BestOfMyGpu);
        REQUIRE(r.ok);
        CHECK(run.card.bw_readings_at[1000] == kBandwidthReadings);
        CHECK(run.logged("mem: the scan passed +1000; reading again from +800 down"));
        CHECK(run.card.max_mem_seen == 1000);
        CHECK(run.card.max_again_mem == 800);
        CHECK(r.mem_max_stable == 700);   // the lowest offset within 1 % of the reading at +800
    }
}

TEST_CASE("readings that scatter as they do on the reference card leave the memory scan where the card puts it") {
    // Measured there: readings scatter by 0.2 % on a card that is left alone
    // (hardware check 96). A little more than that here, on two hundred
    // different days. Every scan gets to the first wrong result, and the
    // offset that is kept moves by five steps: +2050 to +2300.
    for (unsigned seed = 1; seed <= 200; ++seed) {
        Run run = rising_to_the_edge();
        run.card.bw_noise = scatter(seed, 0.0025);
        const auto r = run.go(Preset::BestOfMyGpu);
        CAPTURE(seed);
        REQUIRE(r.ok);
        CHECK(run.card.max_mem_seen == 2550);
        CHECK(run.card.max_again_mem == 2300);
        CHECK(r.mem_max_stable >= 2050);
        CHECK(r.mem_max_stable <= 2300);
    }
}

TEST_CASE("a real peak is still found where it is when the readings scatter") {
    // The card the method was made for: bandwidth turns down long before the
    // first wrong result. Scatter must not carry the scan past the peak.
    for (unsigned seed = 1; seed <= 20; ++seed) {
        Run run;
        run.card.mem_edge = 1400;
        run.card.bw_curve = [](int m) { return m <= 900 ? 500 + m * 0.3 : 770 - (m - 900) * 0.6; };
        run.card.bw_noise = scatter(seed, 0.013);
        const auto r = run.go(Preset::BestOfMyGpu);
        CAPTURE(seed);
        REQUIRE(r.ok);
        // +950 is 3.9 % below the peak and +1000 7.8 %: with this scatter the
        // scan ends at one of the two, now and then one step later.
        CHECK(run.card.max_mem_seen >= 950);
        CHECK(run.card.max_mem_seen <= 1050);
        CHECK(run.card.max_again_mem <= run.card.max_mem_seen - kPeakMarginSteps * 50);
        CHECK(r.mem_max_stable >= 700);
        CHECK(r.mem_max_stable <= 900);
    }
}

TEST_CASE("a reading that does not settle is taken again") {
    // On the reference card one reading in twenty-four did not settle, with
    // nothing wrong. One such reading used to end the scan.
    Run run = recovering();
    run.card.bw_curve = [](int m) { return 500 + m * 0.3; };
    run.card.bw_unsettled_once_at_mem = 300;
    const auto r = run.go(Preset::BestOfMyGpu);
    REQUIRE(r.ok);
    CHECK_FALSE(run.logged("did not settle"));
    CHECK(run.logged("mem +300: bandwidth 590.0 GB/s"));
    CHECK(run.card.bw_readings_at[300] == 2);
    CHECK(run.card.max_mem_seen > 300);
    CHECK(r.driver_resets == 0);
}

TEST_CASE("a stretch in which every reading is 2 % off does not end the memory scan") {
    // Measured on the reference card (hardware check 95): for tens of seconds
    // at a time every reading is about 1.8 % higher, whatever the offset, and
    // then it is back. The readings of one step are all inside such a
    // stretch, so reading it three times changes nothing.
    for (const double off : {0.98, 1.02}) {
        Run run = rising_to_the_edge();
        run.card.bw_noise = [&card = run.card, off](int, int) { return card.bw_measurements > 20 && card.bw_measurements <= 45 ? off : 1.0; };
        const auto r = run.go(Preset::BestOfMyGpu);
        CAPTURE(off);
        REQUIRE(r.ok);
        CHECK(run.card.max_mem_seen == 2550);
        // The stretch can leave a few steps that read as high as the top of
        // the scan; the tie rule then keeps the lowest of them.
        CHECK(r.mem_max_stable >= 2000);
    }
}

namespace {

// The run after: the same card and journal, the counters of the first run gone.
OptimizeResult again(Run& run) {
    run.log.clear();
    run.card.max_mem_seen = 0;
    run.card.bw_measurements = 0;
    run.card.bw_readings_at.clear();
    run.card.bw_again_measurements = 0;
    run.card.max_again_mem = -1;
    return run.go(Preset::BestOfMyGpu);
}

}

TEST_CASE("a memory scan that ran into a wrong result leaves the edge in the journal") {
    Run run = rising_to_the_edge();
    REQUIRE(run.go(Preset::BestOfMyGpu).ok);
    CHECK(run.card.max_mem_seen == 2550);
    CHECK(run.card.count("\"mem_edge\":2550") == 1);
    CHECK(run.card.count("card-a / 617.42") == 1);
    check_journal_rules(run);
}

TEST_CASE("the next run stays four steps below the memory edge of the run before") {
    // On the reference card every scan ended in a driver reset at the edge
    // (hardware check 95). Once is enough.
    Run run = rising_to_the_edge();
    const auto first = run.go(Preset::BestOfMyGpu);
    REQUIRE(first.ok);
    const auto second = again(run);
    REQUIRE(second.ok);
    CHECK(run.card.max_mem_seen == 2550 - kResetBackoffSteps * 50);
    CHECK(run.logged("mem: the memory gave out at +2550 in an earlier run on this card and driver; the scan stops at +2350"));
    CHECK_FALSE(run.logged("mem +2400"));   // no candidate above it, so none that could fail
    CHECK(second.driver_resets == 0);
    // The same offset as the run that found the edge: the margin counts from
    // the edge, not a second time from where the scan was held.
    CHECK(run.card.max_again_mem == 2300);
    CHECK(second.mem_max_stable == first.mem_max_stable);
    CHECK(second.mem_max_stable == 2200);
    CHECK(second.mem_mhz == 1500);
    CHECK(run.card.count("\"mem_edge\"") == 1);   // nothing new was found
    // And the run after that is the same.
    const auto third = again(run);
    REQUIRE(third.ok);
    CHECK(third.mem_mhz == second.mem_mhz);
}

TEST_CASE("a bandwidth measurement that lost the device leaves the edge too, and the next run has no reset") {
    Run run = recovering();
    run.card.bw_curve = [](int m) { return 500 + m * 0.3; };
    run.card.bw_fails_once_at_mem = 500;
    const auto first = run.go(Preset::BestOfMyGpu);
    REQUIRE(first.ok);
    CHECK(first.driver_resets == 1);
    CHECK(run.card.count("\"mem_edge\":500") == 1);
    const auto second = again(run);
    REQUIRE(second.ok);
    CHECK(second.driver_resets == 0);
    CHECK(run.card.max_mem_seen == 300);
    CHECK(second.mem_max_stable == 250);   // the margin counts from the edge at +500, once
}

TEST_CASE("the memory edge lapses with another driver") {
    Run run = rising_to_the_edge();
    REQUIRE(run.go(Preset::BestOfMyGpu).ok);
    run.rig = "card-a / 620.10";
    REQUIRE(again(run).ok);
    CHECK(run.card.max_mem_seen == 2550);   // searched afresh
    CHECK_FALSE(run.logged("in an earlier run"));
    CHECK(run.card.count("\"mem_edge\":2550") == 2);   // and found again, for the new driver
}

TEST_CASE("a scan that ended for another reason leaves no memory edge") {
    {   // a bandwidth peak
        Run run;
        run.card.mem_edge = 1400;
        run.card.bw_curve = [](int m) { return m <= 900 ? 500 + m * 0.3 : 770 - (m - 900) * 0.6; };
        REQUIRE(run.go(Preset::BestOfMyGpu).ok);
        CHECK(run.card.count("mem_edge") == 0);
    }
    {   // readings that did not settle
        Run run = recovering();
        run.card.bw_curve = [](int m) { return 500 + m * 0.3; };
        run.card.bw_unsettled_at_mem = 300;
        REQUIRE(run.go(Preset::BestOfMyGpu).ok);
        CHECK(run.card.count("mem_edge") == 0);
    }
    {   // a measurement that does not work on this machine
        Run run = recovering();
        run.card.bw_curve = [](int m) { return 500 + m * 0.3; };
        run.card.bw_fails_at_zero = true;
        REQUIRE(run.go(Preset::BestOfMyGpu).ok);
        CHECK(run.card.count("mem_edge") == 0);
    }
    {   // the driver's own range
        Run run = rising_to_the_edge(5000);
        REQUIRE(run.go(Preset::BestOfMyGpu).ok);
        CHECK(run.card.max_mem_seen == 3000);
        CHECK(run.card.count("mem_edge") == 0);
    }
}

namespace {

// The reference RTX 4070: bandwidth rises by 0.4 % a step until the memory
// gives out, and the stress load computes right up to +2500 and wrong at
// +2550. Nothing announces that edge (hardware checks 94 and 95).
Run reference_card() {
    Run run = recovering();
    run.card.mem_edge = 2500;
    run.card.ranges = ClockOffsetRanges{{-500, 1000}, {-1000, 3000}};
    run.card.bw_curve = [](int m) { return 434.0 * (1 + 0.004 * m / 50); };
    return run;
}

}

TEST_CASE("on a card whose bandwidth rises to the edge, the offset is read again well below it") {
    Run run = reference_card();
    const auto r = run.go(Preset::BestOfMyGpu);
    REQUIRE(r.ok);
    // The scan finds the end with the short copy and the stress load.
    CHECK(run.card.max_mem_seen == 2550);
    CHECK(run.logged("core +0 / mem +2550: WRONG RESULT"));
    CHECK(run.logged("mem: the scan passed +2500; reading again from +2300 down"));
    // The second pass: four readings, none above +2300.
    CHECK(run.card.max_again_mem == 2300);
    CHECK(run.card.bw_readings_at[2300] == 2);   // once going up, once coming down
    CHECK(run.card.bw_again_measurements == 4);
    CHECK(run.logged("mem +2300: bandwidth 513.9 GB/s, read again"));
    CHECK(r.driver_resets == 0);
    CHECK(run.card.recover_calls == 0);
    CHECK_FALSE(run.logged("treated as a driver reset"));
    CHECK(run.logged("mem: bandwidth peak +2200, confirmed +2200, applying +1500"));
    CHECK(r.mem_mhz == 1500);
    // The edge is written down, and the next run does not even meet the wrong result.
    CHECK(run.card.count("\"mem_edge\":2550") == 1);
    const auto second = again(run);
    REQUIRE(second.ok);
    CHECK(run.card.max_mem_seen == 2350);
    CHECK(run.card.max_again_mem == 2300);
    CHECK(second.driver_resets == 0);
    CHECK(second.mem_mhz == 1500);
    check_journal_rules(run);
}

TEST_CASE("the second pass stays below the margin on that card whatever the readings do") {
    // Readings that scatter as the worst series measured next to another
    // program, in both passes, with a high and a low stretch thrown in.
    for (unsigned seed = 1; seed <= 40; ++seed) {
        Run run = reference_card();
        const auto noise = scatter(seed, 0.013);
        run.card.bw_noise = [&card = run.card, noise, seed](int mem, int nth) {
            const int n = card.bw_measurements;
            const double level = n > 10 && n <= 20 ? 1.018 : n > 30 + static_cast<int>(seed % 10) && n <= 45 ? 0.982 : 1.0;
            return noise(mem, nth) * level;
        };
        const auto r = run.go(Preset::BestOfMyGpu);
        CAPTURE(seed);
        REQUIRE(r.ok);
        CHECK(run.card.max_again_mem <= run.card.max_mem_seen - 50 - kPeakMarginSteps * 50);
        CHECK(r.driver_resets == 0);
        CHECK(run.card.max_mem_seen == 2550);
        CHECK(r.mem_mhz >= 1400);   // +2050 or higher kept
    }
}

TEST_CASE("the margin holds for an edge anywhere") {
    // The same kind of card with its edge elsewhere.
    for (int edge = 300; edge <= 2900; edge += 150) {
        Run run = reference_card();
        run.card.mem_edge = edge;
        const auto r = run.go(Preset::BestOfMyGpu);
        CAPTURE(edge);
        REQUIRE(r.ok);
        CHECK(run.card.max_mem_seen == edge + 50);
        CHECK(run.card.max_again_mem == edge - 200);
        CHECK(r.driver_resets == 0);
        CHECK(r.mem_max_stable <= edge - 200);
    }
}

TEST_CASE("a reading of the second pass that loses the device is a reset the search recovers from") {
    // A card that is worse than the margin allows for. The run goes on as
    // after any reset in the memory search, and the offset is remembered.
    Run run = reference_card();
    run.card.bw_again_loses_device_from = 2300;
    const auto r = run.go(Preset::BestOfMyGpu);
    REQUIRE(r.ok);
    CHECK(r.driver_resets == 1);
    CHECK(run.card.bw_again_measurements == 1);
    CHECK(run.logged("mem +2300: the bandwidth measurement failed; treated as a driver reset"));
    CHECK(r.mem_max_stable == 2300);
    CHECK(r.mem_confirmed == 2300 - kResetBackoffSteps * 50);
    CHECK(run.card.max_core_seen > 0);   // the core is still searched
    CHECK(run.card.count("\"mem_edge\":2300") == 1);
    // The next run stays below that as well, with both copies.
    const auto second = again(run);
    REQUIRE(second.ok);
    CHECK(second.driver_resets == 0);
    CHECK(run.card.max_mem_seen == 2100);
    CHECK(run.card.max_again_mem == 2050);
    check_journal_rules(run);
}
