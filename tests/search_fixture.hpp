// The fixture the search tests share: a modelled card, the run that drives
// optimize() against it, and the helpers that read its journal and events.
#pragma once
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
    int extra_heat = 0;              // every probe peaks this much hotter: a card that runs warm
    double hot_after_health_s = -1;  // >= 0: the first 30 s probe after a health probe is too hot after this many seconds
    bool hot_from_health = false;    // armed by that health probe
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
    int abort_on_rest = -1;          // a stop request comes during this rest (1 = the first)
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
    int bw_measurements = 0;         // bandwidth measurements taken with the short copy
    // The readings of the second pass: those taken below an offset the card
    // has already been at. bw_again_noise: what they are off by, when that is
    // to differ from bw_noise. From bw_again_loses_device_from on, such a
    // reading takes the device down.
    int bw_again_loses_device_from = INT_MAX;
    std::function<double(int mem, int nth)> bw_again_noise;
    int bw_again_measurements = 0;
    int max_again_mem = -1;          // the highest offset one was taken at
    bool bw_fails_at_zero = false;   // the measurement at memory +0 fails; nothing is wrong with the card
    bool lost_on_first_soak = false;      // the first 300 s probe ends DEVICE LOST
    bool fail_set_on_first_soak = false;  // the first write of the soak state fails once
    int bw_fails_once_at_mem = -1;   // the bandwidth measurement at this offset fails once and the driver resets
    int bw_unsettled_at_mem = -1;    // the bandwidth readings at this offset do not agree; the driver is fine
    int bw_unsettled_once_at_mem = -1;   // the first reading at this offset does not settle, the next ones do
    // What a reading is off by: a factor on the curve, for the n-th reading
    // (from 0) taken at that offset. Empty: none.
    std::function<double(int mem, int nth)> bw_noise;
    std::map<int, int> bw_readings_at;   // readings taken so far, by offset
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
            if (health && hot_after_health_s >= 0) hot_from_health = true;
            if (seconds == 30 && hot_from_health) {   // the card was above the limit when the probe began
                hot_from_health = false;
                r.seconds = hot_after_health_s;
                r.peak_temp_c = max_temp + 1;
                r.verdict = Verdict::TooHot;
                return r;
            }
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
            r.peak_temp_c = static_cast<int>(40 + 0.3 * power) + extra_heat + (seconds == 300 ? soak_extra_heat : 0) +
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
    std::optional<StabilityResult> stock;   // the measurement at stock that went before the run, when there was one
    std::string rig = "card-a / 617.42";    // the card and driver the journal's memory edges are bound to
    std::vector<std::pair<std::string, double>> steps;   // what the search said it was doing, and for how long
    OptimizeResult go(Preset preset, GpuControl gpu) {
        Journal j(card.journal, [this](const std::string& l) {
            if (l.find("\"complete\"") != std::string::npos && card.completes_ok >= 0 && card.completes_ok-- == 0)
                return false;
            if (l.find("\"begin\"") != std::string::npos) card.events.push_back("begin");
            card.journal.push_back(l);
            return true;
        }, rig);
        OptimizeIo io;
        io.probe = card.probe();
        io.aborted = [this] { return card.aborted_now || (abort_after_probes >= 0 && card.probes >= abort_after_probes); };
        io.log = [this](const std::string& m) { log.push_back(m); };
        if (!no_rest)
            io.rest = [this](double) {
                ++card.rests;
                card.events.push_back("rest");
                if (card.abort_during_rest || card.rests == card.abort_on_rest) { card.aborted_now = true; return false; }
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
                const int nth = card.bw_readings_at[card.mem]++;
                if (card.mem < card.max_mem_seen) {   // the second pass
                    ++card.bw_again_measurements;
                    card.max_again_mem = std::max(card.max_again_mem, card.mem);
                    card.bw_unsettled = false;
                    if (card.mem >= card.bw_again_loses_device_from) { card.driver_reset(); return std::nullopt; }
                    const auto& noise = card.bw_again_noise ? card.bw_again_noise : card.bw_noise;
                    return card.bw_curve(card.mem) * (noise ? noise(card.mem, nth) : 1.0);
                }
                ++card.bw_measurements;
                card.bw_unsettled = card.mem == card.bw_unsettled_at_mem || (card.mem == card.bw_unsettled_once_at_mem && nth == 0);
                if (card.bw_unsettled) return std::nullopt;
                if (card.bw_loses_device) { card.driver_reset(); return std::nullopt; }
                if (card.bw_fails_at_zero && card.mem == 0) return std::nullopt;
                if (card.mem == card.bw_fails_once_at_mem) {
                    card.bw_fails_once_at_mem = -1;
                    card.driver_reset();
                    return std::nullopt;
                }
                if (card.bw_fails) return std::nullopt;
                return card.bw_curve(card.mem) * (card.bw_noise ? card.bw_noise(card.mem, nth) : 1.0);
            };
        if (card.bw_curve) io.bandwidth_unsettled = [this] { return card.bw_unsettled; };
        io.measuring = [this](const std::string& what, double seconds) { steps.emplace_back(what, seconds); };
        return optimize(gpu, objectives_for(preset), j, io, stock);
    }
    OptimizeResult go(Preset preset) { return go(preset, card.gpu()); }
    bool said(const std::string& what, double seconds) const {
        return std::find(steps.begin(), steps.end(), std::make_pair(what, seconds)) != steps.end();
    }
    bool logged(const std::string& what) const { return count_logged(what) > 0; }
    int count_logged(const std::string& what) const {
        int n = 0;
        for (const auto& m : log) n += m.find(what) != std::string::npos;
        return n;
    }
};

using Events = std::vector<std::string>;

inline int count_events(const FakeCard& card, const std::string& what) {
    return static_cast<int>(std::count(card.events.begin(), card.events.end(), what));
}

// Everything that happened after the journal entry of the candidate the
// driver reset under was opened: the events after the last "begin" before the
// first "recover".
inline Events events_after_lost(const FakeCard& card) {
    const auto first_recover = std::find(card.events.begin(), card.events.end(), "recover");
    auto from = first_recover;
    while (from != card.events.begin() && *(from - 1) != "begin") --from;
    if (from == card.events.begin()) return {};   // no candidate was open before the first reconnect
    return {from, card.events.end()};
}

// The events after the last journal entry of the run was opened.
inline Events events_after_last_begin(const FakeCard& card) {
    auto from = card.events.end();
    while (from != card.events.begin() && *(from - 1) != "begin") --from;
    return {from, card.events.end()};
}

// The soak entries of a run whose soak has both clocks above stock: the
// `begin` lines that carry both a core and a memory offset. (A soak journals
// only the clocks that are above stock.)
inline int soak_entries(const FakeCard& card) {
    int n = 0;
    for (const auto& l : card.journal)
        n += l.find("\"begin\"") != std::string::npos && l.find("\"core\":") != std::string::npos &&
             l.find("\"mem\":") != std::string::npos;
    return n;
}

// The first n of them.
inline Events first_events_after_lost(const FakeCard& card, std::size_t n) {
    auto all = events_after_lost(card);
    if (all.size() > n) all.resize(n);
    return all;
}

// The offsets of the journal's `begin` lines that carry `field` and not
// `other`, in the order they were written.
inline std::vector<int> begins_with_only(const FakeCard& card, const std::string& field, const std::string& other) {
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
inline void check_journal_rules(const Run& run) {
    CHECK(run.card.recovers_with_entry_open == 0);
    CHECK(run.card.count("\"begin\"") == run.card.count("\"complete\""));
    Journal reread(run.card.journal, [](const std::string&) { return true; });
    CHECK(reread.freezes().empty());
}

// A reset the probe reported is handled by the gate before the next step
// starts: no write is tried on the dead connections first, which would fail
// and count as a second reset event.
inline void check_reconnected_up_front(const Run& run) {
    CHECK(run.logged("the driver was reset; reconnecting"));
    CHECK_FALSE(run.logged("trying once more"));
    CHECK(run.card.count("SET FAILED") == 0);
}

// The recovery gate. Unless stated, the card loses the device at core +135:
// probe 32 of the run, the only driver reset, between the climb and the
// confirm step.
inline Run lost_at_135() {
    Run run;
    run.card.with_recover = true;
    run.card.lost_core_from = 135;
    return run;
}

inline Run recovering() {
    Run run;
    run.card.with_recover = true;
    return run;
}

// from, from + step, ... to, followed by `then`.
inline std::vector<int> climb(int from, int to, int step, std::vector<int> then = {}) {
    std::vector<int> out;
    for (int v = from; v <= to; v += step) out.push_back(v);
    out.insert(out.end(), then.begin(), then.end());
    return out;
}

inline std::vector<int> core_begins(const FakeCard& card) { return begins_with_only(card, "core", "mem"); }
inline std::vector<int> mem_begins(const FakeCard& card) { return begins_with_only(card, "mem", "core"); }
// The `begin` lines that carry neither clock: the +0 sample of the bandwidth
// scan, and a soak with both clocks at stock.
inline int clockless_begins(const FakeCard& card) {
    int n = 0;
    for (const auto& l : card.journal)
        n += l.find("\"begin\"") != std::string::npos && l.find("\"core\":") == std::string::npos &&
             l.find("\"mem\":") == std::string::npos;
    return n;
}

// Nothing that loads the card or waits for it, and no journal entry.
inline void check_nothing_started(const Events& events) {
    for (const auto& e : events) {
        CHECK(e != "rest");
        CHECK(e != "health");
        CHECK(e != "begin");
    }
}
