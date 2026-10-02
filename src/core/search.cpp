#include "core/search.hpp"
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <map>
#include <optional>
#include <utility>
#include <vector>

namespace gao {

int highest_stable(int lo, int hi, int step, int ceiling, const std::function<bool(int)>& is_stable) {
    const int top = std::min(hi, ceiling - 1);
    const int n = top > lo ? (top - lo) / step : 0;   // candidates lo+step .. lo+n*step
    int good = 0, bad = n + 1;                          // indices; 0 = lo, assumed stable
    while (bad - good > 1) {
        const int mid = (good + bad) / 2;
        if (is_stable(lo + mid * step)) good = mid;
        else bad = mid;
    }
    return lo + good * step;
}

int climb_to_edge(int lo, int hi, int step, int stride, int ceiling, const std::function<bool(int)>& is_stable) {
    const int top = std::min(hi, ceiling - 1);
    const int n = top > lo ? (top - lo) / step : 0;     // candidates lo+step .. lo+n*step
    const int per = std::max(1, stride / step);         // grid steps per stride
    int good = 0, bad = n + 1;                          // indices; 0 = lo, assumed stable
    while (good < n && bad == n + 1) {
        const int next = std::min(good + per, n);
        if (is_stable(lo + next * step)) good = next;
        else bad = next;
    }
    while (bad - good > 1) {
        const int mid = (good + bad) / 2;
        if (is_stable(lo + mid * step)) good = mid;
        else bad = mid;
    }
    return lo + good * step;
}

int lowest_passing(int lo, int hi, int step, const std::function<bool(int)>& passes) {
    const int n = hi > lo ? (hi - lo) / step : 0;       // candidates hi - n*step .. hi
    int good = n, bad = -1;                             // index n = hi, assumed passing
    while (good - bad > 1) {
        const int mid = (good + bad) / 2;
        if (passes(hi - (n - mid) * step)) good = mid;
        else bad = mid;
    }
    return hi - (n - good) * step;
}

int apply_margin(int max, int step, float perf_push) {
    if (max <= 0) return 0;
    // +1e-4: perf_push is a float, and 0.7f * 150 / 15 is 6.9999999, which a
    // bare floor would turn into one step less than intended.
    const int steps = static_cast<int>(std::floor(double(perf_push) * max / step + 1e-4));
    return std::max(0, std::min(steps * step, max - step));
}

int best_bandwidth_offset(int lo, int hi, int step, int ceiling, const std::function<MemSample(int)>& sample) {
    const int top = std::min(hi, ceiling - 1);
    std::vector<std::pair<int, double>> seen;
    double best = 0;
    for (int v = lo; v <= top; v += step) {
        MemSample s = sample(v);
        if (!s.stable) break;
        // A drop is measured twice before it ends the scan: a single reading
        // can land while the memory clock is still in a lower P-state.
        if (s.gbps < best * (1 - kBandwidthDrop)) {
            s = sample(v);
            if (!s.stable) break;
        }
        seen.emplace_back(v, s.gbps);
        best = std::max(best, s.gbps);
        if (s.gbps < best * (1 - kBandwidthDrop)) break;
    }
    for (const auto& [v, gbps] : seen)
        if (gbps >= best * (1 - kBandwidthTie)) return v;
    return lo;
}

int confirm_edge(int edge, int lo, int step, int tries, const std::function<bool(int)>& holds,
                 const std::function<int()>& step_down) {
    for (int t = 0, v = edge; t < tries && v > lo; ++t) {
        if (holds(v)) return v;
        v -= step * (step_down ? step_down() : 1);
    }
    return lo;
}

bool plausible_range(const OffsetRange& range, int applied_mhz, int sanity_max_mhz) {
    return range.min_mhz <= 0 && range.max_mhz > 0 && range.max_mhz <= sanity_max_mhz &&
           applied_mhz >= range.min_mhz && applied_mhz <= range.max_mhz;
}

SearchBounds search_bounds(const GpuControl& gpu) {
    SearchBounds b;
    if (!gpu.clock_offset_range_mhz || !gpu.read_applied) return b;
    const auto ranges = gpu.clock_offset_range_mhz();
    const auto applied = gpu.read_applied();
    if (!ranges || !applied) return b;
    if (plausible_range(ranges->core, applied->core_mhz, kCoreRangeSanityMhz)) {
        b.core_max_mhz = ranges->core.max_mhz;
        b.core_from_card = true;
    }
    if (plausible_range(ranges->mem, applied->mem_mhz, kMemRangeSanityMhz)) {
        b.mem_max_mhz = ranges->mem.max_mhz;
        b.mem_from_card = true;
    }
    return b;
}

namespace {
constexpr int kCoreStep = 15;
constexpr int kMemStep = 50;
// The strides equal the steps, so climb_to_edge never bisects: the first value
// that fails is exactly one step above the last one that passed, and the climb
// probes nothing more after a value that failed or reset the driver.
constexpr int kCoreStride = kCoreStep;
constexpr int kMemStride = kMemStep;
constexpr int kPowerStep = 5;
constexpr double kBaselineS = 30, kPowerProbeS = 20, kClockProbeS = 3, kSoakS = 300;
constexpr int kSafetyTempC = 85;
constexpr int kSoakRetries = 3;
constexpr double kEfficiencyScore = 0.98;
constexpr float kEfficiencyBelowPush = 0.5f;
constexpr double kConfirmProbeS = 30;
constexpr int kConfirmTries = 3;

std::string describe(const StabilityResult& s) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%s  score=%.0f it/s  peak=%d C  power=%d W",
                  verdict_name(s.verdict), s.score, s.peak_temp_c, s.avg_power_w);
    return buf;
}

// The verdicts that suggest the driver reset. Only DEVICE LOST proves it; a
// card that stalled or went blind is handled the same, because it must not be
// loaded again without the recovery. None of them says the candidate is
// unstable.
bool is_reset_verdict(Verdict v) {
    return v == Verdict::DeviceLost || v == Verdict::Stalled || v == Verdict::NoTelemetry;
}
}

OptimizeResult optimize(const GpuControl& gpu, const Objectives& obj, Journal& journal, const OptimizeIo& io) {
    OptimizeResult r;
    std::string stopped;   // non-empty once the run must end; later probes become no-ops
    auto log = [&](const std::string& m) { if (io.log) io.log(m); };
    bool hw_suspect = false;         // the connections may be dead: a probe, a write or a measurement ended in a way a driver reset explains
    bool reconnect_futile = false;   // a reconnect failed, or did not help: the way out does not try another
    // The reset budget. These three are only ever set with gpu.recover:
    // without a way to reconnect, a reset verdict is counted and that is all.
    bool explored_enough = false;    // a reset event happened: no new value is tried in this run
    bool reset_in_step = false;      // the candidate or soak attempt under way had a reset event
    std::string reset_seen;          // what the last reset event looked like: a verdict, or the write that failed
    bool power_ctl = obj.power && gpu.set_power_limit && gpu.power_limit_range_pct;   // false once skipped

    // The only caller of gpu.recover, which can take half a minute, fail,
    // crash or hang. Rule: never call this while a journal entry is open, or a
    // crash in it would leave a ceiling for a candidate that never ran.
    // Callers check gpu.recover first.
    auto reconnect = [&]() -> bool {
        if (gpu.recover()) return true;
        reconnect_futile = true;
        return false;
    };
    auto reset_to_stock = [&]() -> bool { return gpu.reset_to_stock && gpu.reset_to_stock(); };
    // Ends the run because a write, a reconnect or the recovery after a driver
    // reset failed. A stop request that arrived meanwhile is the more useful
    // reason of the two.
    auto stop = [&](const std::string& why) {
        stopped = gpu.recover && io.aborted && io.aborted() ? "aborted" : why;
    };
    // Every failing return goes through here, with no journal entry open
    // (unless the journal itself could not close it). It runs no load and
    // does not wait: at most one reconnect and the reset to stock.
    auto finish_fail = [&](const std::string& why) {
        // After a driver reset the old connections are dead: reconnect first,
        // or the reset to stock fails on a card the driver already reset. A
        // failed reset gets one reconnect too, unless one was just made or an
        // earlier one failed.
        const bool can_reconnect = gpu.recover && !reconnect_futile;
        const bool reconnected = hw_suspect && can_reconnect;
        if (reconnected) reconnect();
        r.stock_restored = reset_to_stock();
        if (!r.stock_restored && can_reconnect && !reconnected && reconnect())
            r.stock_restored = reset_to_stock();
        r.ok = false;
        r.reason = why;
        log("stopped: " + why + (r.stock_restored ? " -- card restored to stock"
                                                   : " -- reset FAILED, run `gao --reset`"));
        return r;
    };

    // One reset event: a probe that ended DEVICE LOST, STALLED or NO
    // TELEMETRY, or (callers check gpu.recover) a write or a bandwidth
    // measurement that failed. `seen` says which. The first one ends
    // exploring. The second one ends the run, and nothing is started after
    // it: every step below checks `stopped` before it touches the card, the
    // journal or the clock. Without gpu.recover the event is counted, the
    // connections are taken for dead as they always were, and nothing else
    // changes.
    auto reset_event = [&](const std::string& seen) {
        ++r.driver_resets;
        hw_suspect = true;
        if (!gpu.recover) return;
        reset_seen = seen;
        reset_in_step = true;
        explored_enough = true;
        if (r.driver_resets >= 2) stopped = "the driver reset twice";
    };
    // Why the run ends after a reset event with no clock offset applied:
    // there is nothing to back off to, and repeating the state would load
    // what just reset the driver.
    auto at_stock_clocks = [&] { return "the driver reset at stock clocks (" + reset_seen + ")"; };

    // Leaves the card alone after a driver reset. Without io.rest nothing
    // waits, and the log does not claim it did. False: a stop request came,
    // before the rest (during the reconnect, say) or during it.
    auto rest = [&]() -> bool {
        if (io.aborted && io.aborted()) return false;
        if (io.rest) {
            log("resting " + std::to_string(static_cast<int>(kRestAfterResetS)) + " s before the card is loaded again");
            if (!io.rest(kRestAfterResetS)) return false;
        }
        return !(io.aborted && io.aborted());
    };

    // The recovery gate: after a reset event it reconnects, writes stock,
    // rests, and loads the card again only to prove at stock that it is back.
    // Called at the start of every power step, clock candidate and soak
    // attempt, before its journal entry is opened and before its write, so
    // nothing in here ever leaves an entry behind. False: `stopped` is set and
    // the run must end. On a run that is already stopped it does nothing.
    // Nothing in here starts another recovery: a reset verdict in the health
    // probe ends the run.
    auto ensure_hw = [&]() -> bool {
        if (!stopped.empty()) return false;
        if (!hw_suspect) return true;
        if (!gpu.recover) {   // nothing to reconnect with; the next write decides
            hw_suspect = false;
            return true;
        }
        log("the driver was reset; reconnecting");
        if (!reconnect()) {
            stop("the driver did not come back after a reset");
            return false;
        }
        // Stock first, so no candidate stays applied while the card rests. On
        // failure hw_suspect stays set: the way out makes its one reconnect.
        if (!reset_to_stock()) {
            stop("could not set the card to stock while recovering");
            return false;
        }
        hw_suspect = false;
        for (int t = 0; t < kHealthTries; ++t) {
            if (!rest()) { stopped = "aborted"; return false; }
            log("checking that the card is back: " + std::to_string(static_cast<int>(kHealthProbeS)) + " s at stock");
            // Called directly, not through `probe`, which would count a reset
            // verdict as an event of its own. No stall floor: the probe is
            // judged on its whole run, because its first batch may include
            // re-creating the device.
            StabilityResult s = io.probe(kHealthProbeS, kSafetyTempC, 0.0);
            if (s.verdict == Verdict::Stable && s.score < kStalledScore * r.baseline.score) s.verdict = Verdict::Stalled;
            log("health: " + describe(s));
            if (s.verdict == Verdict::Aborted) { stopped = "aborted"; return false; }
            // The health probe is a load and can reset the driver itself. That
            // is counted, and the card is not loaded again in this run. The
            // connections may be dead again: the way out reconnects once.
            if (is_reset_verdict(s.verdict)) {
                ++r.driver_resets;
                hw_suspect = true;
                stop(s.verdict == Verdict::Stalled
                         ? std::string("the card computed almost nothing after a driver reset")
                         : std::string("the card was not usable when checked after a driver reset (") + verdict_name(s.verdict) + ")");
                return false;
            }
            // A baseline that scored nothing gives no bar to measure against:
            // then no health probe counts as healthy.
            if (s.verdict == Verdict::Stable && r.baseline.score > 0 && s.score >= kHealthyScore * r.baseline.score) return true;
            // Computing, but not back yet (slow, a wrong result, too hot).
        }
        stop("the card did not recover after a driver reset");
        return false;
    };

    // Writes the full state (power, core, mem), so a TDR that reset the driver
    // (or a previous candidate) can never leave stale settings behind. Returns
    // what failed, or nothing. Never reconnects and never retries: what a
    // failed write means is the caller's business.
    auto write_state = [&](int power, int core, int mem) -> std::string {
        if (power_ctl && !gpu.set_power_limit(power)) return "setting power " + std::to_string(power) + " % failed";
        if ((obj.core_oc || core != 0) && !(gpu.set_core_offset && gpu.set_core_offset(core)))
            return "setting core +" + std::to_string(core) + " failed";
        if ((obj.mem_oc || mem != 0) && !(gpu.set_mem_offset && gpu.set_mem_offset(mem)))
            return "setting mem +" + std::to_string(mem) + " failed";
        return {};
    };
    // Opens a journal entry for a clock candidate and applies it. Returns the
    // id of the open entry, or nothing: then no entry is open, and either
    // `stopped` is set, or (`stopped` still empty) the write failed and was
    // counted as a reset event, and the candidate did not pass. A driver reset
    // the probe did not report looks exactly like a failed write, so it gets
    // no second try: the entry is closed as SET FAILED, and the gate runs
    // before whatever comes next. Without gpu.recover a failed write stops the
    // run.
    auto begin_candidate = [&](std::optional<int> core_j, std::optional<int> mem_j, int core, int mem) -> std::optional<int> {
        const int id = journal.begin(core_j, mem_j);
        if (id < 0) { stopped = "could not write the journal"; return std::nullopt; }
        const std::string why = write_state(r.power_pct, core, mem);
        if (why.empty()) return id;
        const bool closed = journal.complete(id, "SET FAILED");
        if (!gpu.recover || !closed) { stop(why); return std::nullopt; }
        log(why + "; treated as a driver reset");
        reset_event(why);
        return std::nullopt;
    };
    // One probe, with the checks every step shares: a stopped run, a stop
    // request, a card that stopped computing, and the count of reset events.
    // At the second reset event it sets `stopped` and still returns the
    // result, so the caller closes its journal entry with the real verdict.
    auto probe = [&](double seconds, int max_temp) -> std::optional<StabilityResult> {
        if (!stopped.empty()) return std::nullopt;
        if (io.aborted && io.aborted()) { stopped = "aborted"; return std::nullopt; }
        // A card that stops computing ends the probe itself, so a long soak never loads a dead card.
        StabilityResult s = io.probe(seconds, max_temp, r.baseline.score > 0 ? kStalledScore * r.baseline.score : 0.0);
        if (s.verdict == Verdict::Aborted) { stopped = "aborted"; return std::nullopt; }
        // r.baseline.score is 0 until the baseline itself has been judged.
        if (s.verdict == Verdict::Stable && r.baseline.score > 0 && s.score < kStalledScore * r.baseline.score)
            s.verdict = Verdict::Stalled;
        if (is_reset_verdict(s.verdict)) reset_event(verdict_name(s.verdict));
        // Telemetry goes blind when the driver resets. Without a way to
        // reconnect there is nothing to do about it but end the run.
        if (s.verdict == Verdict::NoTelemetry && !gpu.recover) { stopped = "lost telemetry"; return std::nullopt; }
        return s;
    };
    // What a journal entry is closed with. A candidate the user stopped is
    // ABORTED: closed, so it never becomes a ceiling.
    auto closed_as = [&](const std::optional<StabilityResult>& s) -> std::string {
        if (s) return verdict_name(s->verdict);
        return stopped == "aborted" ? "ABORTED" : "NOT RUN";
    };

    if (!reset_to_stock()) {
        // The connections may be stale from a driver reset before this run:
        // one reconnect and one more reset before giving up. If that does not
        // help either, the way out does not reconnect again.
        if (gpu.recover) {
            log("could not reset to stock; reconnecting and trying once more");
            if (!reconnect() || !reset_to_stock()) reconnect_futile = true;
        }
        if (!gpu.recover || reconnect_futile) return finish_fail("could not reset to stock");
        // A baseline taken on a card that is not back from that reset would
        // lower every later threshold. No health probe here: there is no
        // baseline to judge it by yet.
        if (!rest()) return finish_fail("aborted");
    }
    const SearchBounds bounds = search_bounds(gpu);
    auto range_text = [](const char* what, int max_mhz, bool from_card) {
        return std::string(what) + " range: up to +" + std::to_string(max_mhz) + " MHz (" +
               (from_card ? "reported by the card" : "built-in limit; the card reported no usable range") + ")";
    };
    if (obj.core_oc) log(range_text("core", bounds.core_max_mhz, bounds.core_from_card));
    if (obj.mem_oc) log(range_text("mem", bounds.mem_max_mhz, bounds.mem_from_card));
    log("baseline: 30 s at stock");
    const auto base = probe(kBaselineS, kSafetyTempC);
    if (!base) return finish_fail(stopped);
    r.baseline = *base;
    log("baseline: " + describe(*base));
    // The baseline runs at stock clocks: a reset there is nothing the search
    // can work around.
    if (reset_in_step) return finish_fail(at_stock_clocks());
    if (base->verdict != Verdict::Stable) return finish_fail(std::string("stock is not stable (") + verdict_name(base->verdict) + ")");

    // Power.
    int power_floor = 100;   // lowest power the soak may fall back to
    if (power_ctl) {
        const auto [min_pct, max_pct] = gpu.power_limit_range_pct();
        if (min_pct > 100 || max_pct < 100 || min_pct >= max_pct) {
            log("power: range " + std::to_string(min_pct) + "-" + std::to_string(max_pct) + " % has no room around 100 %, skipped");
            power_ctl = false;
        } else {
            const int grid_lo = 100 - (100 - min_pct) / kPowerStep * kPowerStep;
            power_floor = grid_lo;
            std::map<int, StabilityResult> seen;
            // One power step, at stock clocks. A reset event here (a probe
            // with a reset verdict, or a failed write) ends the run: `stopped`
            // is set, and every later step is a no-op.
            auto run_power = [&](int pct) -> std::optional<StabilityResult> {
                if (!stopped.empty()) return std::nullopt;
                if (io.aborted && io.aborted()) { stopped = "aborted"; return std::nullopt; }
                if (!ensure_hw()) return std::nullopt;
                reset_in_step = false;
                // The power step opens no journal entry. A failed write is not
                // retried: with a way to reconnect it is a reset event.
                const std::string why = write_state(pct, 0, 0);
                if (!why.empty()) {
                    if (!gpu.recover) { stop(why); return std::nullopt; }
                    log(why + "; treated as a driver reset");
                    reset_event(why);
                    if (stopped.empty()) stopped = at_stock_clocks();
                    return std::nullopt;
                }
                const auto s = probe(kPowerProbeS, kSafetyTempC);
                if (s) { seen[pct] = *s; log("power " + std::to_string(pct) + " %: " + describe(*s)); }
                if (reset_in_step && stopped.empty()) stopped = at_stock_clocks();
                return s;
            };
            const int cap = highest_stable(grid_lo, max_pct, kPowerStep, INT_MAX, [&](int pct) {
                const auto s = run_power(pct);
                return s && s->verdict == Verdict::Stable && s->peak_temp_c <= obj.max_temp_c;
            });
            if (!stopped.empty()) return finish_fail(stopped);
            r.power_pct = cap;
            if (obj.perf_push < kEfficiencyBelowPush) {
                // Reference = best stable score at or below the cap (baseline
                // included). One probe scatters a few percent; using a single
                // low reading as the reference let quiet cost 4 % on the 4070.
                if (!seen.count(cap)) run_power(cap);
                if (!stopped.empty()) return finish_fail(stopped);
                double ref_score = cap >= 100 ? r.baseline.score : 0;
                for (const auto& [pct, s] : seen)
                    if (pct <= cap && s.verdict == Verdict::Stable) ref_score = std::max(ref_score, s.score);
                r.power_pct = lowest_passing(grid_lo, cap, kPowerStep, [&](int pct) {
                    const auto s = run_power(pct);
                    return s && s->verdict == Verdict::Stable && s->score >= kEfficiencyScore * ref_score;
                });
                if (!stopped.empty()) return finish_fail(stopped);
            }
            log("power: " + std::to_string(r.power_pct) + " %");
        }
    }

    // One clock candidate: the recovery gate if the driver was reset, then
    // journal, then hardware, then the probe.
    // `extra` runs while the candidate is still applied and stable (e.g. a
    // bandwidth measurement), before the journal entry is closed. Returns the
    // verdict, or nothing when the candidate did not run: the run is stopped,
    // or its write failed and was counted as a reset event. A reset event on
    // a candidate with no clock offset (the +0 sample of the bandwidth scan)
    // ends the run.
    auto clock_candidate = [&](std::optional<int> core_j, std::optional<int> mem_j, int core, int mem,
                               double seconds, const std::function<void()>& extra = {}) -> std::optional<Verdict> {
        if (!stopped.empty()) return std::nullopt;
        if (io.aborted && io.aborted()) { stopped = "aborted"; return std::nullopt; }
        if (!ensure_hw()) return std::nullopt;
        reset_in_step = false;
        const bool stock_clocks = core == 0 && mem == 0;
        const auto id = begin_candidate(core_j, mem_j, core, mem);
        if (!id) {
            if (reset_in_step && stock_clocks && stopped.empty()) stopped = at_stock_clocks();
            return std::nullopt;
        }
        const auto s = probe(seconds, obj.max_temp_c);
        if (s && s->verdict == Verdict::Stable && extra) extra();
        if (!journal.complete(*id, closed_as(s))) {
            stopped = "could not write the journal";
            return std::nullopt;
        }
        if (!s) return std::nullopt;
        log(std::string(seconds == kConfirmProbeS ? "confirm " : "") + "core +" + std::to_string(core) +
            " / mem +" + std::to_string(mem) + ": " + describe(*s));
        if (reset_in_step && stock_clocks && stopped.empty()) stopped = at_stock_clocks();
        return s->verdict;
    };
    auto is_stable = [](std::optional<Verdict> v) { return v == Verdict::Stable; };
    // A confirm probe only decides whether the clock holds. Running a little
    // hot over 30 s is the power step's and the soak's business (the soak
    // lowers power), not a reason to throw the clock edge away.
    auto holds = [](std::optional<Verdict> v) { return v == Verdict::Stable || v == Verdict::TooHot; };

    // One step of a climb or a scan. After a reset event nothing new is
    // explored: the step reports "not stable" without touching the card, and
    // that ends the climb at the last value that passed. reset_at receives the
    // value whose candidate had the reset event.
    auto explore = [&](int v, int& reset_at, const std::function<bool()>& candidate) -> bool {
        if (explored_enough) return false;
        const bool passed = candidate();
        if (explored_enough) reset_at = v;
        return passed;
    };
    // Where confirmation starts: at the edge, or kResetBackoffSteps below it
    // when a reset event ended the climb. A value one step below a reset is
    // no place for a 30 s probe.
    auto confirm_from = [](int edge, int step, int reset_at) {
        return reset_at < 0 ? edge : std::max(0, edge - kResetBackoffSteps * step);
    };
    // How far the next confirm try goes down after one that failed.
    const std::function<int()> confirm_step_down = [&] { return reset_in_step ? kResetBackoffSteps : 1; };
    auto offset_text = [](int mhz) { return "+" + std::to_string(mhz); };

    // Memory comes first, at core 0: the core climb is where a card is most
    // likely to give out, and it must not keep the memory from being tuned.
    if (obj.mem_oc) {
        const int ceiling = journal.ceilings().mem_mhz;
        int reset_at = -1;   // the memory offset at which a reset event ended the scan
        bool by_bandwidth = static_cast<bool>(io.bandwidth);
        if (by_bandwidth) {
            bool unavailable = false;
            const int peak = best_bandwidth_offset(0, bounds.mem_max_mhz, kMemStep, ceiling, [&](int v) {
                MemSample m;
                std::optional<double> gbps;
                m.stable = explore(v, reset_at, [&] {
                    if (!is_stable(clock_candidate(std::nullopt, v, 0, v, kClockProbeS, [&] { gbps = io.bandwidth(); })))
                        return false;
                    if (gbps) return true;
                    // The candidate passed, the measurement failed: the step is unusable.
                    if (!gpu.recover) {
                        hw_suspect = true;   // the driver may have been reset under it; the next write decides
                    } else if (v == 0) {
                        // Nothing is overclocked yet, so this is not the card
                        // giving out: the measurement does not work on this
                        // machine. Not a reset event, and no recovery. If the
                        // driver did reset, the next write fails and is counted.
                        unavailable = true;
                        log("bandwidth measurement not available; searching memory by stability only");
                    } else {
                        log("mem " + offset_text(v) + ": the bandwidth measurement failed; treated as a driver reset");
                        reset_event("the bandwidth measurement failed");
                    }
                    return false;
                });
                if (m.stable) {
                    m.gbps = *gbps;
                    char buf[64];
                    std::snprintf(buf, sizeof(buf), "mem +%d: bandwidth %.1f GB/s", v, m.gbps);
                    log(buf);
                }
                return m;
            });
            // An unavailable measurement ended the scan at +0: its result
            // says nothing, and the stability climb below takes over.
            if (unavailable) by_bandwidth = false;
            else r.mem_max_stable = peak;
        }
        if (!by_bandwidth) {
            r.mem_max_stable = climb_to_edge(0, bounds.mem_max_mhz, kMemStep, kMemStride, ceiling, [&](int v) {
                return explore(v, reset_at, [&] { return is_stable(clock_candidate(std::nullopt, v, 0, v, kClockProbeS)); });
            });
        }
        if (!stopped.empty()) return finish_fail(stopped);
        if (reset_at >= 0)
            log("mem: the driver reset at " + offset_text(reset_at) + "; using " + offset_text(r.mem_max_stable) +
                (by_bandwidth ? ", the best of the values that passed" : ", the last value that passed"));
        r.mem_confirmed = confirm_edge(confirm_from(r.mem_max_stable, kMemStep, reset_at), 0, kMemStep, kConfirmTries, [&](int v) {
            return holds(clock_candidate(std::nullopt, v, 0, v, kConfirmProbeS));
        }, confirm_step_down);
        if (!stopped.empty()) return finish_fail(stopped);
        r.mem_mhz = apply_margin(r.mem_confirmed, kMemStep, obj.perf_push);
        log(std::string("mem: ") + (by_bandwidth ? "bandwidth peak +" : "highest stable +") +
            std::to_string(r.mem_max_stable) + ", confirmed +" + std::to_string(r.mem_confirmed) +
            ", applying +" + std::to_string(r.mem_mhz));
    }
    // The core comes second and runs with the chosen memory offset applied
    // (0 when the profile does not tune memory). After a reset event in the
    // memory phase it stays at stock: its climb would be exploring.
    if (obj.core_oc && explored_enough) {
        log("core: not searched; the driver reset earlier in this run");
    } else if (obj.core_oc) {
        int reset_at = -1;   // the core offset at which a reset event ended the climb
        r.core_max_stable = climb_to_edge(0, bounds.core_max_mhz, kCoreStep, kCoreStride, journal.ceilings().core_mhz, [&](int v) {
            return explore(v, reset_at, [&] { return is_stable(clock_candidate(v, std::nullopt, v, r.mem_mhz, kClockProbeS)); });
        });
        if (!stopped.empty()) return finish_fail(stopped);
        if (reset_at >= 0)
            log("core: the driver reset at " + offset_text(reset_at) + "; using " + offset_text(r.core_max_stable) +
                ", the last value that passed");
        r.core_confirmed = confirm_edge(confirm_from(r.core_max_stable, kCoreStep, reset_at), 0, kCoreStep, kConfirmTries, [&](int v) {
            return holds(clock_candidate(v, std::nullopt, v, r.mem_mhz, kConfirmProbeS));
        }, confirm_step_down);
        if (!stopped.empty()) return finish_fail(stopped);
        r.core_mhz = apply_margin(r.core_confirmed, kCoreStep, obj.perf_push);
        log("core: highest stable +" + std::to_string(r.core_max_stable) + ", confirmed +" +
            std::to_string(r.core_confirmed) + ", applying +" + std::to_string(r.core_mhz));
    }

    // Soak. On failure: a reset event -> both clocks kResetBackoffSteps down,
    // or the end of the run when no clock offset was applied; too hot -> one
    // power step down (lower clocks barely cool the card); anything else ->
    // both clocks one step down.
    auto lower_clocks = [&](int steps) {
        if (obj.core_oc) r.core_mhz = std::max(0, r.core_mhz - steps * kCoreStep);
        if (obj.mem_oc) r.mem_mhz = std::max(0, r.mem_mhz - steps * kMemStep);
    };
    for (int attempt = 0; attempt <= kSoakRetries; ++attempt) {
        if (io.aborted && io.aborted()) return finish_fail("aborted");
        if (!ensure_hw()) return finish_fail(stopped);
        reset_in_step = false;
        const bool stock_clocks = r.core_mhz == 0 && r.mem_mhz == 0;
        const auto id = begin_candidate(obj.core_oc ? std::optional<int>(r.core_mhz) : std::nullopt,
                                        obj.mem_oc ? std::optional<int>(r.mem_mhz) : std::nullopt, r.core_mhz, r.mem_mhz);
        if (!id) {
            if (!stopped.empty()) return finish_fail(stopped);
            // The write failed and was counted as a reset event: this attempt
            // is over, and the next one runs through the gate.
            if (stock_clocks) return finish_fail(at_stock_clocks());
            lower_clocks(kResetBackoffSteps);
            continue;
        }
        log("soak: " + std::to_string(static_cast<int>(kSoakS)) + " s at power " + std::to_string(r.power_pct) + " %, core +" + std::to_string(r.core_mhz) +
            ", mem +" + std::to_string(r.mem_mhz));
        const auto s = probe(kSoakS, obj.max_temp_c);
        if (!journal.complete(*id, closed_as(s)))
            return finish_fail("could not write the journal");
        if (!s) return finish_fail(stopped);
        log("soak: " + describe(*s));
        // The second reset event: the run ends here, before the clocks are
        // stepped down and before the gate or another entry could start.
        if (!stopped.empty()) return finish_fail(stopped);
        // The soak is the longest probe; Ctrl+C during it must still win.
        if (io.aborted && io.aborted()) return finish_fail("aborted");
        if (s->verdict == Verdict::Stable) {
            r.soak = *s;
            r.ok = true;
            if (gpu.recover && r.driver_resets > 0) log("the driver reset during this run; the search stopped exploring there");
            return r;
        }
        if (reset_in_step) {
            if (stock_clocks) return finish_fail(at_stock_clocks());
            lower_clocks(kResetBackoffSteps);
            continue;
        }
        if (s->verdict == Verdict::TooHot && power_ctl && r.power_pct - kPowerStep >= power_floor) {
            r.power_pct -= kPowerStep;
            continue;
        }
        lower_clocks(1);
    }
    return finish_fail("soak failed " + std::to_string(kSoakRetries + 1) + " times");
}

}
