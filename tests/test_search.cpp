#include "doctest/doctest.h"
#include "core/search.hpp"
#include "core/journal.hpp"
#include "core/objectives.hpp"
#include <string>
#include <algorithm>
#include <climits>
#include <map>
#include <optional>
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
    int stall_core_from = -1;        // at or above this core offset a probe "passes" at 3 % of the normal score
    int lost_core_from = -1;         // at or above this core offset a probe ends DEVICE LOST and the driver resets
    bool stale = false;              // after a driver reset: every write fails until recover()
    bool with_recover = false;       // the card offers GpuControl::recover
    bool recover_fails = false;
    int recover_calls = 0;
    int blind_probes = 0;            // this many 3 s probes report NO TELEMETRY
    int flaky_core_sets = 0;         // this many set_core_offset calls fail although nothing is wrong
    int begins_when_recover_failed = -1;   // journal `begin` lines at the moment recover() first failed
    int recovers_with_entry_open = 0;      // recover() calls made while the last journal line was a `begin`
    bool abort_in_recover = false;   // a stop request arrives while recover() runs
    int lost_at_probe = -1;          // the N-th probe of the run ends DEVICE LOST and the driver resets
    bool bw_loses_device = false;    // a bandwidth measurement fails and the driver resets
    int sick_health_probes = 0;      // this many 5 s probes after a reset score 50 % of normal (computing, not back)
    std::optional<Verdict> health_verdict;   // the first 5 s probe ends with this verdict instead
    bool health_stalls = false;      // the first 5 s probe "passes" at 3 % of the normal score
    bool stock_write_fails_after_recover = false;   // reset_to_stock fails once, right after a successful recover
    bool fail_next_stock = false;    // armed by that recover, cleared by the reset it fails
    std::vector<std::string> events; // "recover", "stock", "rest", "health", "begin" in the order they happen
    bool abort_during_rest = false;  // a stop request arrives while the search rests
    int rests = 0;
    bool with_prepare = false;       // the run offers OptimizeIo::prepare_load
    int prepare_fails = 0;           // this many preparations return false
    int prepares = 0;
    int prepares_with_entry_open = 0;   // preparations made while the last journal line was a `begin`
    bool abort_after_prepare = false;   // a stop request arrives while the load is being prepared
    int probes = 0, power_probes = 0;
    std::vector<std::string> journal;
    std::string last_set_begin_ok;   // "" if every set had its begin line first
    std::optional<ClockOffsetRanges> ranges;   // what the card reports; empty = it reports nothing
    bool ranges_read_fails = false;            // the callback exists but returns nothing
    std::vector<int> mem_during_core_probes;   // the memory offset applied at each probe with a core offset
    int blind_at_core = -1;          // the 3 s probe at this core offset reports NO TELEMETRY
    int fail_core_set_once_at = -1;  // the first set_core_offset(this) fails; nothing is wrong with the card
    bool fail_next_core_set_after_recover = false;   // the first non-zero core write after a recover fails once
    bool fail_next_core_set = false; // armed by that recover, cleared by the write it fails
    int flaky_power_sets = 0;        // this many set_power_limit calls fail although nothing is wrong
    bool lost_on_first_core_confirm = false;   // the first 30 s probe with a non-zero core offset ends DEVICE LOST
    bool lost_on_first_mem_confirm = false;    // the first 30 s probe with a non-zero memory offset ends DEVICE LOST
    int bw_measurements = 0;         // bandwidth measurements taken
    bool bw_fails_at_zero = false;   // the measurement at memory +0 fails; nothing is wrong with the card
    bool lost_on_first_soak = false;      // the first 300 s probe ends DEVICE LOST
    bool fail_set_on_first_soak = false;  // the first write of the soak state fails once
    int bw_fails_once_at_mem = -1;   // the bandwidth measurement at this offset fails once and the driver resets
    int bw_unsettled_at_mem = -1;    // the bandwidth readings at this offset do not agree; the driver is fine
    bool bw_unsettled = false;       // what the last failed measurement was
    int lost_mem_from = -1;          // at or above this memory offset a 3 s probe ends DEVICE LOST
    int recover_fails_from_call = -1;     // recover() fails from this call on (1 = the first)
    std::vector<std::string> probe_order; // every probe as "seconds:power:core:mem", in the order they ran
    bool fail_power_set_in_entry = false; // the first power write made while a journal entry is open fails once
    std::vector<std::pair<double, double>> probe_floors;   // every probe as (seconds, stall_below), in the order they ran

    bool entry_open() const { return !journal.empty() && journal.back().find("\"begin\"") != std::string::npos; }
    // True while the journal's last line opens the entry of a soak with both
    // clocks above stock: the only entry that carries both a core and a memory
    // offset.
    bool soak_entry_open() const {
        if (journal.empty()) return false;
        const std::string& l = journal.back();
        return l.find("\"begin\"") != std::string::npos && l.find("\"core\":") != std::string::npos &&
               l.find("\"mem\":") != std::string::npos;
    }

    GpuControl gpu(bool with_power = true, std::pair<int, int> range = {50, 120}) {
        GpuControl g;
        g.read = [this] { Telemetry t; t.ok = true; t.temp_c = 40; t.core_mhz = 2800; return t; };
        g.set_core_offset = [this](int v) {
            if (stale) return false;
            if (v != 0 && flaky_core_sets > 0) { --flaky_core_sets; return false; }
            if (v != 0 && fail_next_core_set) { fail_next_core_set = false; return false; }
            if (v == fail_core_set_once_at) { fail_core_set_once_at = -1; return false; }
            if (fail_set_on_first_soak && soak_entry_open()) { fail_set_on_first_soak = false; return false; }
            // Every non-stock clock must be preceded by an open journal line.
            if (v != 0 && (journal.empty() || journal.back().find("\"begin\"") == std::string::npos))
                last_set_begin_ok = "core " + std::to_string(v) + " set without begin";
            if (v == fail_core_set_at) return false;
            core = v; max_core_seen = std::max(max_core_seen, v); return true;
        };
        g.set_mem_offset = [this](int v) { if (stale) return false; mem = v; max_mem_seen = std::max(max_mem_seen, v); return true; };
        g.reset_to_stock = [this] {
            if (stale) return false;
            if (fail_next_stock) { fail_next_stock = false; return false; }
            if (reset_fails_from >= 0 && ++reset_calls > reset_fails_from) return false;
            events.push_back("stock");
            power = 100; core = 0; mem = 0; return true;
        };
        if (with_power) {
            g.set_power_limit = [this](int p) {
                if (stale) return false;
                if (flaky_power_sets > 0) { --flaky_power_sets; return false; }
                if (fail_power_set_in_entry && entry_open()) { fail_power_set_in_entry = false; return false; }
                power = p; return true;
            };
            g.power_limit_range_pct = [range] { return range; };
        }
        if (ranges || ranges_read_fails) {
            g.clock_offset_range_mhz = [this]() -> std::optional<ClockOffsetRanges> {
                if (ranges_read_fails) return std::nullopt;
                return ranges;
            };
            g.read_applied = [this] { return std::optional<AppliedState>(AppliedState{core, mem, power}); };
        }
        if (with_recover)
            g.recover = [this] {
                ++recover_calls;
                events.push_back("recover");
                // The reconnect can crash or hang: it must never run inside an
                // open journal entry, or that candidate becomes a false ceiling.
                if (!journal.empty() && journal.back().find("\"begin\"") != std::string::npos) ++recovers_with_entry_open;
                if (abort_in_recover) aborted_now = true;
                if (recover_fails || (recover_fails_from_call >= 0 && recover_calls >= recover_fails_from_call)) {
                    if (begins_when_recover_failed < 0) {
                        begins_when_recover_failed = 0;
                        for (const auto& l : journal) begins_when_recover_failed += l.find("\"begin\"") != std::string::npos;
                    }
                    return false;
                }
                stale = false;
                if (stock_write_fails_after_recover) { stock_write_fails_after_recover = false; fail_next_stock = true; }
                if (fail_next_core_set_after_recover) { fail_next_core_set_after_recover = false; fail_next_core_set = true; }
                return true;
            };
        return g;
    }
    // What a real TDR does: the driver comes back at stock and the old
    // connections to it are dead.
    void driver_reset() { core = 0; mem = 0; power = 100; stale = true; }
    int count(const std::string& what) const {
        int n = 0;
        for (const auto& l : journal) n += l.find(what) != std::string::npos;
        return n;
    }
    Probe probe() {
        return [this](double seconds, int max_temp, double stall_below) {
            ++probes;
            probe_floors.emplace_back(seconds, stall_below);
            if (seconds == 20) ++power_probes;
            if (core != 0) mem_during_core_probes.push_back(mem);
            StabilityResult r;
            r.seconds = seconds;
            probe_order.push_back(std::to_string(static_cast<int>(seconds)) + ":" + std::to_string(power) + ":" +
                                  std::to_string(core) + ":" + std::to_string(mem));
            // The 5 s probe is the health probe after a driver reset.
            const bool health = seconds == 5;
            if (health) events.push_back("health");
            if (seconds == abort_in_probe_s) { r.verdict = Verdict::Aborted; return r; }
            if (seconds == 3 && blind_probes > 0) { --blind_probes; r.verdict = Verdict::NoTelemetry; return r; }
            if (seconds == 3 && core == blind_at_core) { r.verdict = Verdict::NoTelemetry; return r; }
            if (health && health_verdict) {
                r.verdict = *health_verdict;
                health_verdict.reset();
                if (r.verdict == Verdict::DeviceLost) driver_reset();
                return r;
            }
            bool lost = probes == lost_at_probe || (lost_core_from >= 0 && core >= lost_core_from) ||
                        (seconds == 3 && lost_mem_from >= 0 && mem >= lost_mem_from);
            if (seconds == 30 && core != 0 && lost_on_first_core_confirm) { lost_on_first_core_confirm = false; lost = true; }
            if (seconds == 30 && mem != 0 && lost_on_first_mem_confirm) { lost_on_first_mem_confirm = false; lost = true; }
            if (seconds == 300 && lost_on_first_soak) { lost_on_first_soak = false; lost = true; }
            if (lost) {
                driver_reset();
                r.verdict = Verdict::DeviceLost;
                return r;
            }
            r.peak_temp_c = static_cast<int>(40 + 0.3 * power) + (seconds == 300 ? soak_extra_heat : 0) +
                            (seconds == 30 ? confirm_extra_heat : 0);
            if (seconds == 300 && abort_on_soak) aborted_now = true;
            r.score = 1000.0 * std::min(power, 90) / 90;
            if (health && health_stalls) { health_stalls = false; r.score *= 0.03; return r; }   // verdict stays Stable
            // Half the normal score is above kStalledScore: a slow card, not a reset verdict.
            if (health && sick_health_probes > 0) { --sick_health_probes; r.score *= 0.5; }
            if (stall_core_from >= 0 && core >= stall_core_from) { r.score *= 0.03; return r; }   // verdict stays Stable
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
    bool no_rest = false;            // the application gives the search no way to wait
    OptimizeResult go(Preset preset, GpuControl gpu) {
        Journal j(card.journal, [this](const std::string& l) {
            if (l.find("\"complete\"") != std::string::npos && card.completes_ok >= 0 && card.completes_ok-- == 0)
                return false;
            if (l.find("\"begin\"") != std::string::npos) card.events.push_back("begin");
            card.journal.push_back(l);
            return true;
        });
        OptimizeIo io;
        io.probe = card.probe();
        io.aborted = [this] { return card.aborted_now || (abort_after_probes >= 0 && card.probes >= abort_after_probes); };
        io.log = [this](const std::string& m) { log.push_back(m); };
        if (!no_rest)
            io.rest = [this](double) {
                ++card.rests;
                card.events.push_back("rest");
                if (card.abort_during_rest) { card.aborted_now = true; return false; }
                return true;
            };
        if (card.with_prepare)
            io.prepare_load = [this] {
                ++card.prepares;
                card.events.push_back("prepare");
                if (card.entry_open()) ++card.prepares_with_entry_open;
                if (card.abort_after_prepare) card.aborted_now = true;
                if (card.prepare_fails > 0) { --card.prepare_fails; return false; }
                return true;
            };
        if (card.bw_curve)
            io.bandwidth = [this]() -> std::optional<double> {
                ++card.bw_measurements;
                card.bw_unsettled = card.mem == card.bw_unsettled_at_mem;
                if (card.bw_unsettled) return std::nullopt;
                if (card.bw_loses_device) { card.driver_reset(); return std::nullopt; }
                if (card.bw_fails_at_zero && card.mem == 0) return std::nullopt;
                if (card.mem == card.bw_fails_once_at_mem) {
                    card.bw_fails_once_at_mem = -1;
                    card.driver_reset();
                    return std::nullopt;
                }
                if (card.bw_fails) return std::nullopt;
                return card.bw_curve(card.mem);
            };
        if (card.bw_curve) io.bandwidth_unsettled = [this] { return card.bw_unsettled; };
        return optimize(gpu, objectives_for(preset), j, io);
    }
    OptimizeResult go(Preset preset) { return go(preset, card.gpu()); }
    bool logged(const std::string& what) const { return count_logged(what) > 0; }
    int count_logged(const std::string& what) const {
        int n = 0;
        for (const auto& m : log) n += m.find(what) != std::string::npos;
        return n;
    }
};

using Events = std::vector<std::string>;

int count_events(const FakeCard& card, const std::string& what) {
    return static_cast<int>(std::count(card.events.begin(), card.events.end(), what));
}

// Everything that happened after the journal entry of the candidate the
// driver reset under was opened: the events after the last "begin" before the
// first "recover".
Events events_after_lost(const FakeCard& card) {
    const auto first_recover = std::find(card.events.begin(), card.events.end(), "recover");
    auto from = first_recover;
    while (from != card.events.begin() && *(from - 1) != "begin") --from;
    if (from == card.events.begin()) return {};   // no candidate was open before the first reconnect
    return {from, card.events.end()};
}

// The events after the last journal entry of the run was opened.
Events events_after_last_begin(const FakeCard& card) {
    auto from = card.events.end();
    while (from != card.events.begin() && *(from - 1) != "begin") --from;
    return {from, card.events.end()};
}

// The soak entries of a run whose soak has both clocks above stock: the
// `begin` lines that carry both a core and a memory offset. (A soak journals
// only the clocks that are above stock.)
int soak_entries(const FakeCard& card) {
    int n = 0;
    for (const auto& l : card.journal)
        n += l.find("\"begin\"") != std::string::npos && l.find("\"core\":") != std::string::npos &&
             l.find("\"mem\":") != std::string::npos;
    return n;
}

// The first n of them.
Events first_events_after_lost(const FakeCard& card, std::size_t n) {
    auto all = events_after_lost(card);
    if (all.size() > n) all.resize(n);
    return all;
}

// The offsets of the journal's `begin` lines that carry `field` and not
// `other`, in the order they were written.
std::vector<int> begins_with_only(const FakeCard& card, const std::string& field, const std::string& other) {
    std::vector<int> out;
    const std::string key = "\"" + field + "\":";
    for (const auto& l : card.journal) {
        if (l.find("\"begin\"") == std::string::npos || l.find("\"" + other + "\":") != std::string::npos) continue;
        const auto at = l.find(key);
        if (at != std::string::npos) out.push_back(std::stoi(l.substr(at + key.size())));
    }
    return out;
}

// What every run with a reconnect must leave behind: no reconnect was made
// inside an open journal entry, and no entry is left open.
void check_journal_rules(const Run& run) {
    CHECK(run.card.recovers_with_entry_open == 0);
    CHECK(run.card.count("\"begin\"") == run.card.count("\"complete\""));
    Journal reread(run.card.journal, [](const std::string&) { return true; });
    CHECK(reread.freezes().empty());
}
}

// A reset the probe reported is handled by the gate before the next step
// starts: no write is tried on the dead connections first, which would fail
// and count as a second reset event.
void check_reconnected_up_front(const Run& run) {
    CHECK(run.logged("the driver was reset; reconnecting"));
    CHECK_FALSE(run.logged("trying once more"));
    CHECK(run.card.count("SET FAILED") == 0);
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

TEST_CASE("without a reconnect a failing bandwidth measurement never raises memory") {
    // As it was before the reset budget: the scan ends at the first failed
    // measurement, here the one at +0. With a reconnect that case falls back
    // to the stability climb (tested below).
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

// The recovery gate. Unless stated, the card loses the device at core +135:
// probe 32 of the run, the only driver reset, between the climb and the
// confirm step.
namespace {
Run lost_at_135() {
    Run run;
    run.card.with_recover = true;
    run.card.lost_core_from = 135;
    return run;
}
}

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

// The reset budget. A reset event is a probe that ends DEVICE LOST, STALLED or
// NO TELEMETRY, a write that fails, or a bandwidth measurement that fails above
// memory +0. The first one ends exploring; the second one ends the run.
namespace {
Run recovering() {
    Run run;
    run.card.with_recover = true;
    return run;
}

// from, from + step, ... to, followed by `then`.
std::vector<int> climb(int from, int to, int step, std::vector<int> then = {}) {
    std::vector<int> out;
    for (int v = from; v <= to; v += step) out.push_back(v);
    out.insert(out.end(), then.begin(), then.end());
    return out;
}

std::vector<int> core_begins(const FakeCard& card) { return begins_with_only(card, "core", "mem"); }
std::vector<int> mem_begins(const FakeCard& card) { return begins_with_only(card, "mem", "core"); }
// The `begin` lines that carry neither clock: the +0 sample of the bandwidth
// scan, and a soak with both clocks at stock.
int clockless_begins(const FakeCard& card) {
    int n = 0;
    for (const auto& l : card.journal)
        n += l.find("\"begin\"") != std::string::npos && l.find("\"core\":") == std::string::npos &&
             l.find("\"mem\":") == std::string::npos;
    return n;
}

// Nothing that loads the card or waits for it, and no journal entry.
void check_nothing_started(const Events& events) {
    for (const auto& e : events) {
        CHECK(e != "rest");
        CHECK(e != "health");
        CHECK(e != "begin");
    }
}
}

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

TEST_CASE("a blind probe in the memory climb ends exploring; a second blind probe is never taken") {
    Run run = recovering();
    run.card.blind_probes = 2;   // the first two 3 s probes would be blind
    const auto r = run.go(Preset::BestOfMyGpu);
    REQUIRE(r.ok);
    CHECK(r.driver_resets == 1);
    // The blind probe is the first 3 s one, memory +50. It is not a measured
    // failure and nothing is bisected below it: the memory stays at stock and
    // the core is not searched, so no second 3 s probe runs.
    CHECK(run.logged("core +0 / mem +50: NO TELEMETRY"));
    CHECK(run.card.blind_probes == 1);
    CHECK(mem_begins(run.card) == std::vector<int>{50});
    CHECK(r.mem_max_stable == 0);
    CHECK(r.mem_mhz == 0);
    CHECK(run.card.max_mem_seen == 50);
    CHECK(run.card.max_core_seen == 0);
    CHECK(run.logged("core: not searched; the driver reset earlier in this run"));
    CHECK(run.card.recover_calls == 1);
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

TEST_CASE("a failed bandwidth measurement ends the memory scan and leaves the core unsearched") {
    Run run = recovering();
    run.card.bw_curve = [](int m) { return 500 + m * 0.3; };
    run.card.bw_fails_once_at_mem = 300;
    const auto r = run.go(Preset::BestOfMyGpu);
    REQUIRE(r.ok);
    CHECK(r.driver_resets == 1);
    // The scan samples +0 .. +300; the confirm probe runs four steps below
    // +250, the best of the values that were measured. The +0 sample names no
    // clock in the journal, and neither does the soak, which runs at stock.
    CHECK(mem_begins(run.card) == climb(50, 300, 50, {50}));
    CHECK(clockless_begins(run.card) == 2);
    CHECK(run.card.count("\"begin\"") == 9);   // +0, +50 .. +300, the confirm probe, the soak
    CHECK(run.logged("core +0 / mem +0: STABLE"));
    CHECK(run.logged("mem +0: bandwidth 500.0 GB/s"));
    CHECK(run.card.max_mem_seen == 300);
    CHECK(run.card.bw_measurements == 7);
    CHECK(r.mem_max_stable == 250);
    CHECK(r.mem_confirmed == 50);
    CHECK(r.mem_mhz == 0);
    CHECK(run.card.max_core_seen == 0);
    CHECK(r.core_mhz == 0);
    CHECK(run.logged("core: not searched; the driver reset earlier in this run"));
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
    // The scan ends at +300 with the best of the measured values, +250, and
    // the confirm probe runs there, not four steps below.
    CHECK(run.card.max_mem_seen == 300);
    CHECK(r.mem_max_stable == 250);
    CHECK(r.mem_confirmed == 250);
    // The core is searched as in a run without any event.
    CHECK(run.card.max_core_seen > 0);
    CHECK(r.core_mhz > 0);
    CHECK_FALSE(run.logged("core: not searched"));
    check_journal_rules(run);
}

TEST_CASE("a reset in the memory scan leaves the core unsearched") {
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
    CHECK(run.card.max_core_seen == 0);
    CHECK(core_begins(run.card).empty());
    CHECK(r.core_mhz == 0);
    CHECK(run.logged("core: not searched; the driver reset earlier in this run"));
    CHECK(run.card.mem == 0);
    CHECK(run.card.core == 0);
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

TEST_CASE("a measurement that is unavailable from the start falls back to the stability climb") {
    Run run = recovering();
    run.card.bw_curve = [](int m) { return 500 + m * 0.3; };
    run.card.bw_fails_at_zero = true;
    const auto r = run.go(Preset::BestOfMyGpu);
    REQUIRE(r.ok);
    CHECK(r.driver_resets == 0);
    CHECK(run.card.bw_measurements == 1);   // a failed measurement never raises memory: stability alone does
    // The +0 sample ran and is the first entry of the run, without a clock;
    // the stability climb then goes +50 .. +850, and +800 is confirmed.
    CHECK(mem_begins(run.card) == climb(50, 850, 50, {800}));
    CHECK(clockless_begins(run.card) == 1);
    REQUIRE_FALSE(run.card.journal.empty());
    CHECK(run.card.journal.front().find("\"mem\":") == std::string::npos);
    CHECK(run.card.journal.front().find("\"core\":") == std::string::npos);
    CHECK(run.logged("core +0 / mem +0: STABLE"));
    CHECK(r.mem_max_stable == 800);
    CHECK(r.mem_confirmed == 800);
    CHECK(r.mem_mhz == 550);
    CHECK(r.core_max_stable == 150);
    CHECK(run.card.rests == 0);
    CHECK(count_events(run.card, "health") == 0);
    CHECK(run.card.recover_calls == 0);
    CHECK(run.logged("bandwidth measurement not available; searching memory by stability only"));
    CHECK(run.logged("mem: highest stable +800"));
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
    CHECK(run.logged("bandwidth measurement not available; searching memory by stability only"));
    CHECK(run.card.bw_measurements == 1);
    // Journal: +0 opened (without a clock) and closed, +50 opened and closed
    // as SET FAILED. The only entry after them is the soak, at stock.
    CHECK(mem_begins(run.card) == std::vector<int>{50});
    CHECK(clockless_begins(run.card) == 2);
    CHECK(run.card.count("\"begin\"") == 3);
    CHECK(run.logged("core +0 / mem +0: STABLE"));
    REQUIRE(run.card.journal.size() >= 4);
    CHECK(run.card.journal[0].find("\"begin\"") != std::string::npos);
    CHECK(run.card.journal[0].find("\"mem\":") == std::string::npos);
    CHECK(run.card.journal[2].find("\"mem\":50") != std::string::npos);
    CHECK(run.card.journal[3].find("SET FAILED") != std::string::npos);
    CHECK(run.card.count("SET FAILED") == 1);
    CHECK(run.logged("failed; treated as a driver reset"));
    CHECK(first_events_after_lost(run.card, 5) == Events{"recover", "stock", "rest", "health", "begin"});
    CHECK(r.mem_mhz == 0);
    CHECK(run.card.max_core_seen == 0);
    CHECK(run.logged("core: not searched; the driver reset earlier in this run"));
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

TEST_CASE("a first reset in the memory confirm probe leaves the core unsearched") {
    Run run = recovering();
    run.card.lost_on_first_mem_confirm = true;
    const auto r = run.go(Preset::BestOfMyGpu);
    REQUIRE(r.ok);
    CHECK(r.driver_resets == 1);
    CHECK(run.logged("confirm core +0 / mem +800: DEVICE LOST"));
    // The next confirm try is four steps down. The last entry is the soak's:
    // it runs at core +0 / memory +400 and journals only the memory.
    CHECK(mem_begins(run.card) == climb(50, 850, 50, {800, 600, 400}));
    CHECK(core_begins(run.card).empty());
    CHECK(r.mem_max_stable == 800);
    CHECK(r.mem_confirmed == 600);
    CHECK(r.mem_mhz == 400);
    CHECK(run.card.mem == 400);
    CHECK(run.card.max_core_seen == 0);
    CHECK(run.logged("core: not searched; the driver reset earlier in this run"));
    CHECK(run.card.recover_calls == 1);
    check_journal_rules(run);
}

TEST_CASE("a soak entry carries only the clocks that are above stock") {
    // The same run: the core is not searched, so the soak runs at core +0 /
    // memory +400.
    Run run = recovering();
    run.card.lost_on_first_mem_confirm = true;
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
    CHECK(r.mem_max_stable == 800);
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
    CHECK(b.mem_max_stable == 800);
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
