#include "doctest/doctest.h"
#include "core/search.hpp"
#include "core/journal.hpp"
#include "core/objectives.hpp"
#include <string>
#include <algorithm>
#include <climits>
#include <map>
#include <vector>

using namespace gao;

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

namespace {
// A modelled card: stable up to core +150 / mem +800; peak temp = 40 +
// 0.3 * power%; score rises with power up to 90 %, flat above.
struct FakeCard {
    int power = 100, core = 0, mem = 0;
    int core_edge = 150, mem_edge = 800;
    bool stock_unstable = false;
    int soak_failures = 0;           // how many 300 s probes fail before one passes
    int noisy_power_pct = -1;        // a 20 s probe at this power reads 10 % low
    int soak_extra_heat = 0;         // a 300 s probe peaks this much hotter than a 20 s one
    int confirm_extra_heat = 0;      // a 30 s probe peaks this much hotter than a 20 s one
    int abort_in_probe_s = -1;       // a probe of this length is cut short by a stop request
    bool abort_on_soak = false;      // Ctrl+C arrives while the soak probe runs
    bool aborted_now = false;
    int reset_calls = 0, reset_fails_from = -1;   // reset_to_stock fails from this call on
    int completes_ok = -1;           // journal 'complete' writes that succeed before they fail
    int fail_core_set_at = -1;       // set_core_offset(this) returns false
    int max_core_seen = 0;
    std::vector<int> confirm_fail_core;   // a 30 s probe at these core offsets fails
    std::function<double(int)> bw_curve;  // GB/s by memory offset; empty = no bandwidth
    bool bw_fails = false;                // every bandwidth measurement fails
    int max_mem_seen = 0;
    int probes = 0, power_probes = 0;
    std::vector<std::string> journal;
    std::string last_set_begin_ok;   // "" if every set had its begin line first

    GpuControl gpu(bool with_power = true, std::pair<int, int> range = {50, 120}) {
        GpuControl g;
        g.read = [this] { Telemetry t; t.ok = true; t.temp_c = 40; t.core_mhz = 2800; return t; };
        g.set_core_offset = [this](int v) {
            // Every non-stock clock must be preceded by an open journal line.
            if (v != 0 && (journal.empty() || journal.back().find("\"begin\"") == std::string::npos))
                last_set_begin_ok = "core " + std::to_string(v) + " set without begin";
            if (v == fail_core_set_at) return false;
            core = v; max_core_seen = std::max(max_core_seen, v); return true;
        };
        g.set_mem_offset = [this](int v) { mem = v; max_mem_seen = std::max(max_mem_seen, v); return true; };
        g.reset_to_stock = [this] {
            if (reset_fails_from >= 0 && ++reset_calls > reset_fails_from) return false;
            power = 100; core = 0; mem = 0; return true;
        };
        if (with_power) {
            g.set_power_limit = [this](int p) { power = p; return true; };
            g.power_limit_range_pct = [range] { return range; };
        }
        return g;
    }
    Probe probe() {
        return [this](double seconds, int max_temp) {
            ++probes;
            if (seconds == 20) ++power_probes;
            StabilityResult r;
            r.seconds = seconds;
            if (seconds == abort_in_probe_s) { r.verdict = Verdict::Aborted; return r; }
            r.peak_temp_c = static_cast<int>(40 + 0.3 * power) + (seconds == 300 ? soak_extra_heat : 0) +
                            (seconds == 30 ? confirm_extra_heat : 0);
            if (seconds == 300 && abort_on_soak) aborted_now = true;
            r.score = 1000.0 * std::min(power, 90) / 90;
            if (seconds == 20 && power == noisy_power_pct) r.score *= 0.9;
            if (seconds == 30 && std::find(confirm_fail_core.begin(), confirm_fail_core.end(), core) != confirm_fail_core.end()) {
                r.verdict = Verdict::WrongResult;
                return r;
            }
            if (stock_unstable || core > core_edge || mem > mem_edge) r.verdict = Verdict::WrongResult;
            else if (seconds == 300 && soak_failures > 0) { --soak_failures; r.verdict = Verdict::WrongResult; }
            else if (r.peak_temp_c > max_temp) r.verdict = Verdict::TooHot;
            return r;
        };
    }
};

struct Run {
    FakeCard card;
    std::vector<std::string> log;
    int abort_after_probes = -1;
    OptimizeResult go(Preset preset, GpuControl gpu) {
        Journal j(card.journal, [this](const std::string& l) {
            if (l.find("\"complete\"") != std::string::npos && card.completes_ok >= 0 && card.completes_ok-- == 0)
                return false;
            card.journal.push_back(l);
            return true;
        });
        OptimizeIo io;
        io.probe = card.probe();
        io.aborted = [this] { return card.aborted_now || (abort_after_probes >= 0 && card.probes >= abort_after_probes); };
        io.log = [this](const std::string& m) { log.push_back(m); };
        if (card.bw_curve)
            io.bandwidth = [this]() -> std::optional<double> {
                if (card.bw_fails) return std::nullopt;
                return card.bw_curve(card.mem);
            };
        return optimize(gpu, objectives_for(preset), j, io);
    }
    OptimizeResult go(Preset preset) { return go(preset, card.gpu()); }
};
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
    run.abort_after_probes = 6;   // baseline + ~3 power probes + into core
    const auto r = run.go(Preset::BestOfMyGpu);
    CHECK_FALSE(r.ok);
    CHECK(r.reason == "aborted");
    CHECK(run.card.core == 0);
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

TEST_CASE("bandwidth scan stops at the peak of a rising-then-falling curve") {
    int max_sampled = -1;
    const int r = best_bandwidth_offset(0, 1500, 50, INT_MAX, [&](int v) {
        max_sampled = std::max(max_sampled, v);
        return MemSample{true, v <= 600 ? 500 + v * 0.3 : 680 - (v - 600) * 0.9};
    });
    CHECK(r == 600);
    CHECK(max_sampled == 650);   // stopped at the first step more than 1 % below the peak
}

TEST_CASE("a single low bandwidth reading is re-measured before it ends the scan") {
    // Measured on the 4070: an occasional reading lands at half speed when the
    // memory clock is still in a lower P-state. One such dip must not end the
    // scan; a drop that repeats does.
    std::map<int, int> calls;
    const int r = best_bandwidth_offset(0, 1500, 50, INT_MAX, [&](int v) {
        const bool dip = v == 300 && calls[v] == 0;
        ++calls[v];
        return MemSample{true, dip ? 250.0 : (v <= 600 ? 500 + v * 0.3 : 680 - (v - 600) * 0.9)};
    });
    CHECK(r == 600);
    CHECK(calls[300] == 2);
    CHECK(calls[650] == 2);   // the real drop is confirmed once too
}

TEST_CASE("bandwidth scan stops at the first unstable step") {
    int max_sampled = -1;
    const int r = best_bandwidth_offset(0, 1500, 50, INT_MAX, [&](int v) {
        max_sampled = std::max(max_sampled, v);
        return MemSample{v <= 400, 500 + v * 0.3};
    });
    CHECK(r == 400);
    CHECK(max_sampled == 450);
}

TEST_CASE("a flat curve keeps the lowest offset") {
    const int r = best_bandwidth_offset(0, 1500, 50, INT_MAX, [](int v) {
        return MemSample{true, 500.0 + ((v / 50) % 2)};   // +-1 GB/s noise
    });
    CHECK(r == 0);
}

TEST_CASE("bandwidth scan samples lo and respects the ceiling") {
    std::vector<int> sampled;
    const int r = best_bandwidth_offset(0, 1500, 50, 300, [&](int v) {
        sampled.push_back(v);
        return MemSample{true, 500 + v * 0.3};
    });
    REQUIRE_FALSE(sampled.empty());
    CHECK(sampled.front() == 0);
    CHECK(sampled.back() == 250);
    CHECK(r == 250);
    CHECK(best_bandwidth_offset(0, 1500, 50, 300, [](int) { return MemSample{false, 0}; }) == 0);
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
    CHECK(r.mem_max_stable == 900);
    CHECK(r.mem_confirmed == 900);
    CHECK(r.mem_mhz == 600);
    CHECK(run.card.max_mem_seen <= 950);
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

TEST_CASE("a failing bandwidth measurement never raises memory") {
    Run run;
    run.card.bw_curve = [](int m) { return 500 + m * 0.1; };
    run.card.bw_fails = true;
    const auto r = run.go(Preset::BestOfMyGpu);
    REQUIRE(r.ok);
    CHECK(r.mem_mhz == 0);
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

TEST_CASE("bandwidth noise wider than one step does not pick a higher offset") {
    // Measured spread on the 4070 is ~0.4 %; one 50 MHz step is worth ~0.5 %.
    // Noise of +-0.45 % (0.9 % spread, twice what was measured) on a flat
    // curve must leave the result at the bottom.
    const int r = best_bandwidth_offset(0, 1500, 50, INT_MAX, [](int v) {
        return MemSample{true, 500.0 * (1 + (((v / 50) % 3) - 1) * 0.0045)};
    });
    CHECK(r == 0);
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
    run.card.abort_in_probe_s = 3;   // the first core candidate
    const auto r = run.go(Preset::BestOfMyGpu);
    CHECK_FALSE(r.ok);
    CHECK(r.reason == "aborted");
    CHECK(run.card.core == 0);
    CHECK(run.card.power == 100);
    REQUIRE(run.card.journal.size() == 2);   // one begin, one complete
    CHECK(run.card.journal.back().find("ABORTED") != std::string::npos);
    Journal reread(run.card.journal, [](const std::string&) { return true; });
    CHECK(reread.freezes().empty());
    CHECK(reread.ceilings().core_mhz == INT_MAX);
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
}