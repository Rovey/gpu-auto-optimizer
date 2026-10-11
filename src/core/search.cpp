#include "core/search.hpp"
#include <algorithm>
#include <cctype>
#include <climits>
#include <cmath>
#include <cstdio>
#include <map>
#include <optional>
#include <string_view>
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

MemScanEnd bandwidth_scan(int lo, int hi, int step, int ceiling, const std::function<MemSample(int)>& sample) {
    MemScanEnd end{lo, MemScanEnd::Why::Limit};
    auto middle = [](std::vector<double> of) {   // of an even number: the lower one
        std::sort(of.begin(), of.end());
        return of[(of.size() - 1) / 2];
    };
    std::vector<double> steps;   // the reading of every step that passed
    double best = 0;
    for (int v = lo; v <= std::min(hi, ceiling - 1); v += step) {
        MemSample s = sample(v);
        // A reading that would end the scan is in doubt: the step is read
        // kBandwidthReadings times and the middle one counts. Only such a
        // step: three readings at every step cost two minutes a scan and
        // decided nothing a single reading did not (hardware check 95).
        if (s.stable && s.gbps < best * (1 - kBandwidthDrop)) {
            std::vector<double> readings{s.gbps};
            while (static_cast<int>(readings.size()) < kBandwidthReadings && (s = sample(v)).stable) readings.push_back(s.gbps);
            s.gbps = middle(readings);
        }
        if (!s.stable) { end.why = MemScanEnd::Why::Failed; break; }
        end.top = v;
        if (s.gbps < best * (1 - kBandwidthDrop)) { end.why = MemScanEnd::Why::Dropped; break; }
        // The best is the highest middle of three steps in a row, not the
        // highest reading: one reading that came out high would be a best
        // every later step is held against, and the highest of fifty readings
        // that scatter is always such a one.
        steps.push_back(s.gbps);
        best = std::max(best, middle({steps.end() - static_cast<std::ptrdiff_t>(std::min<size_t>(steps.size(), 3)), steps.end()}));
    }
    return end;
}

int second_pass_top(const MemScanEnd& end, int lo, int step, int known_edge) {
    // The lowest offset that is known, or has to be assumed, not to hold.
    int bound = end.top + step;
    if (end.why == MemScanEnd::Why::Limit && known_edge != INT_MAX && known_edge > bound) bound = known_edge;
    return std::max(lo, bound - step - kPeakMarginSteps * step);
}

int bandwidth_peak_below(int top, int lo, int step, const std::function<MemSample(int)>& sample) {
    std::vector<std::pair<int, double>> seen;
    double best = 0;
    for (int v = top; v >= lo; v -= step) {
        const MemSample s = sample(v);
        if (!s.stable) break;
        seen.emplace_back(v, s.gbps);
        best = std::max(best, s.gbps);
        if (s.gbps < best * (1 - kBandwidthTie)) break;
    }
    int peak = top;
    for (const auto& [v, gbps] : seen)
        if (gbps >= best * (1 - kBandwidthTie)) peak = std::min(peak, v);
    return peak;
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

std::string stock_refusal(const StabilityResult& stock) {
    if (stock.verdict == Verdict::Stable) return {};
    if (stock.verdict == Verdict::Aborted) return "aborted";
    if (stock.verdict != Verdict::TooHot) return std::string("the card is not stable at stock (") + verdict_name(stock.verdict) + ")";
    if (stock.seconds >= kStockMinS) return {};
    return "the card was at " + std::to_string(stock.peak_temp_c) + " C after " + std::to_string(static_cast<int>(stock.seconds)) +
           " s at stock; let it cool down and start again";
}

std::string other_tool_refusal(const std::vector<std::string>& running) {
    auto runs = [&](std::string_view exe) {
        return std::any_of(running.begin(), running.end(), [&](const std::string& name) {
            return std::equal(name.begin(), name.end(), exe.begin(), exe.end(),
                              [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == b; });
        });
    };
    const char* tool = runs("msiafterburner.exe") ? "MSI Afterburner" : runs("rtss.exe") ? "RivaTuner Statistics Server (RTSS)" : nullptr;
    if (!tool) return {};
    return std::string(tool) + " is running. Close it, also its icon in the notification area, and start again: a run cannot share "
           "the card with a program that sets clocks and fans or draws over what the card shows";
}

namespace {
constexpr int kCoreStep = 15;
constexpr int kMemStep = 50;
// The strides equal the steps, so climb_to_edge never bisects: the first value
// that fails is exactly one step above the last one that passed, and the climb
// probes nothing more after a value that failed or reset the driver. The
// confirm probe then runs one step below an ordinary failure (at the last
// value that passed) and, after a reset, kResetBackoffSteps (four) steps below
// the last value that passed.
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

OptimizeResult optimize(const GpuControl& gpu, const Objectives& obj, Journal& journal, const OptimizeIo& io,
                        const std::optional<StabilityResult>& stock) {
    OptimizeResult r;
    std::string stopped;   // non-empty once the run must end; later probes become no-ops
    auto log = [&](const std::string& m) { if (io.log) io.log(m); };
    auto step = [&](const std::string& what, double seconds) { if (io.measuring) io.measuring(what, seconds); };
    bool hw_suspect = false;         // the connections may be dead: a probe, a write or a measurement ended in a way a driver reset explains
    bool reconnect_futile = false;   // a reconnect failed, or did not help: the way out does not try another
    // The reset budget. These are only ever set with gpu.recover: without a
    // way to reconnect, a reset verdict is counted and that is all.
    bool explored_enough = false;    // a reset event happened: the search under way tries no new value
    int resets_allowed = 1;          // one more reset event than this ends the run
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
    // does not wait: at most one reconnect, announced in the log, and the
    // reset to stock.
    auto finish_fail = [&](const std::string& why) {
        // After a driver reset the old connections are dead: reconnect first,
        // or the reset to stock fails on a card the driver already reset. A
        // failed reset gets one reconnect too, unless one was just made or an
        // earlier one failed.
        const bool can_reconnect = gpu.recover && !reconnect_futile;
        const bool reconnected = hw_suspect && can_reconnect;
        // The reconnect can take half a minute: say so before it starts.
        const char* const reconnecting = "reconnecting to the driver to restore stock";
        if (reconnected) {
            log(reconnecting);
            reconnect();
        }
        r.stock_restored = reset_to_stock();
        if (!r.stock_restored && can_reconnect && !reconnected) {
            log(reconnecting);
            if (reconnect()) r.stock_restored = reset_to_stock();
        }
        r.ok = false;
        r.reason = why;
        log("stopped: " + why + (r.stock_restored ? " -- card restored to stock"
                                                   : " -- reset FAILED, run `gao --reset`"));
        return r;
    };

    // One reset event: a probe that ended DEVICE LOST, STALLED or NO
    // TELEMETRY, or (callers check gpu.recover) a write or a bandwidth
    // measurement that failed. `seen` says which. One ends the exploring of
    // the clock search under way. One more than the run allows ends the run,
    // and nothing is started after it: every step below checks `stopped`
    // before it touches the card, the journal or the clock. A run allows one,
    // and two when the first came in the memory search and the core is
    // searched after it. Without gpu.recover the event is counted, the
    // connections are taken for dead as they always were, and nothing else
    // changes.
    auto reset_event = [&](const std::string& seen) {
        ++r.driver_resets;
        hw_suspect = true;
        if (!gpu.recover) return;
        reset_seen = seen;
        reset_in_step = true;
        explored_enough = true;
        if (r.driver_resets > resets_allowed)
            stopped = r.driver_resets == 2 ? "the driver reset twice" : "the driver reset three times";
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
            step("The driver was reset: resting before the card is loaded again", kRestAfterResetS);
            if (!io.rest(kRestAfterResetS)) return false;
        }
        return !(io.aborted && io.aborted());
    };

    // The recovery gate: after a reset event it reconnects, writes stock,
    // rests, prepares the stress load (io.prepare_load, once per recovery),
    // and loads the card again only to prove at stock that it is back.
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
        bool load_ready = !io.prepare_load;   // nothing to prepare: ready
        for (int t = 0; t < kHealthTries; ++t) {
            if (!rest()) { stopped = "aborted"; return false; }
            // Once per recovery: a failed preparation is tried again after the
            // next rest and is no reset event; a prepared load stays prepared.
            if (!load_ready) {
                log("preparing the stress load");
                load_ready = io.prepare_load();
                if (io.aborted && io.aborted()) { stopped = "aborted"; return false; }
                if (!load_ready) { log("the stress load is not ready yet"); continue; }
            }
            log("checking that the card is back: " + std::to_string(static_cast<int>(kHealthProbeS)) + " s at stock");
            step("Checking that the card is back", kHealthProbeS);
            // Called directly, not through `probe`, which would count a reset
            // verdict as an event of its own. No stall floor: the load was
            // prepared just before, and five seconds are bounded already.
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
    // run. The write is not undone here: a partial one (the core written, the
    // memory write failed) can leave the new, not yet probed value of one
    // clock applied until the gate or the way out writes stock. Nothing loads
    // the card in between.
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
    if (stock && stock->verdict == Verdict::Stable && stock->score > 0) {
        // Measured a moment ago, for longer and with the same load: another
        // half minute at stock would say nothing new.
        r.baseline = *stock;
        log("baseline: the measurement at stock, " + describe(*stock));
    } else {
        log("baseline: 30 s at stock");
        step("Baseline at stock", kBaselineS);
        const auto base = probe(kBaselineS, kSafetyTempC);
        if (!base) return finish_fail(stopped);
        r.baseline = *base;
        log("baseline: " + describe(*base));
        // The baseline runs at stock clocks: a reset there is nothing the
        // search can work around.
        if (reset_in_step) return finish_fail(at_stock_clocks());
        if (base->verdict != Verdict::Stable) return finish_fail(std::string("stock is not stable (") + verdict_name(base->verdict) + ")");
    }

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
                step("Power limit " + std::to_string(pct) + " %", kPowerProbeS);
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
    double candidate_s = 0;   // how long the probe of the last candidate lasted
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
        // A search probe lasts seconds and has no countdown; a confirm probe has.
        const bool confirm = seconds == kConfirmProbeS;
        step(std::string(confirm ? "Confirming " : "Trying ") +
                 (core_j ? "core clock +" + std::to_string(core) : "memory clock +" + std::to_string(mem)) + " MHz",
             confirm ? seconds : 0);
        const auto s = probe(seconds, obj.max_temp_c);
        if (s && s->verdict == Verdict::Stable && extra) extra();
        if (!journal.complete(*id, closed_as(s))) {
            stopped = "could not write the journal";
            return std::nullopt;
        }
        if (!s) return std::nullopt;
        candidate_s = s->seconds;
        log(std::string(seconds == kConfirmProbeS ? "confirm " : "") + "core +" + std::to_string(core) +
            " / mem +" + std::to_string(mem) + ": " + describe(*s));
        if (reset_in_step && stock_clocks && stopped.empty()) stopped = at_stock_clocks();
        return s->verdict;
    };
    auto is_stable = [](std::optional<Verdict> v) { return v == Verdict::Stable; };
    // A confirm probe only decides whether the clock holds. Running a little
    // hot over 30 s is the power step's and the soak's business (the soak
    // lowers power), not a reason to throw the clock edge away.
    // But a probe that was too hot before kConfirmMinS has decided nothing:
    // the card was above the limit when it began. After a driver reset that
    // is the rule on a card that runs hot, because the health probe loads it
    // at stock power. Such a probe does not confirm, and the card cools
    // before the next try: without that every lower value would end the same
    // way, and the edge would be stepped down to nothing by heat alone.
    auto holds = [&](std::optional<Verdict> v) {
        if (v != Verdict::TooHot) return v == Verdict::Stable;
        if (candidate_s >= kConfirmMinS) return true;
        char why[96];
        std::snprintf(why, sizeof(why), "too hot after %.1f s, before the clock was tested: not confirmed", candidate_s);
        log(why);
        if (!rest() && stopped.empty()) stopped = "aborted";
        return false;
    };

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
        int ceiling = journal.ceilings().mem_mhz;
        // Where the memory gave out in an earlier scan on this card and
        // driver, the scan does not go again: it stops as far below it as
        // confirming starts below a reset. A card whose bandwidth rises until
        // its memory fails would otherwise end every run in a wrong result or
        // a driver reset (hardware check 95).
        if (const int edge = journal.mem_edge(); edge != INT_MAX) {
            const int top = std::max(0, edge - kResetBackoffSteps * kMemStep);
            log("mem: the memory gave out at " + offset_text(edge) + " in an earlier run on this card and driver; the scan stops at " +
                offset_text(top));
            ceiling = std::min(ceiling, top + 1);
        }
        int reset_at = -1;   // the memory offset at which a reset event ended the scan
        int gave_out_at = 0; // the offset whose candidate computed a wrong result
        const bool by_bandwidth = static_cast<bool>(io.bandwidth);
        bool left_at_stock = false;   // the measurement does not work: nothing was searched
        if (by_bandwidth) {
            bool unavailable = false;
            // One step of either pass: the candidate, then the reading while
            // it is still applied. `again`: in the second pass.
            const auto sample = [&](int v, bool again) {
                MemSample m;
                // One reading. One that only did not settle is taken again,
                // kBandwidthReadings times at most; any other failure ends
                // the step at once.
                std::optional<double> gbps;
                bool failed = false;
                // The +0 sample is journaled without a clock, like a soak at
                // stock: if the machine froze in it, an open entry that said
                // memory 0 would read as "never search memory again".
                const std::optional<int> mem_j = v > 0 ? std::optional<int>(v) : std::nullopt;
                m.stable = explore(v, reset_at, [&] {
                    const auto verdict = clock_candidate(std::nullopt, mem_j, 0, v, kClockProbeS, [&] {
                        for (int i = 0; i < kBandwidthReadings && !gbps && !failed; ++i) {
                            gbps = io.bandwidth();
                            failed = !gbps && !(io.bandwidth_unsettled && io.bandwidth_unsettled());
                        }
                    });
                    if (verdict == Verdict::WrongResult) gave_out_at = v;
                    if (!is_stable(verdict)) return false;
                    if (gbps) return true;
                    // The candidate passed, the measurement failed: the step is unusable.
                    if (v > 0 && !failed) {
                        // The load computed and the readings disagreed: noise,
                        // not the card giving out. The scan ends with the best
                        // of what was measured; the core is still searched.
                        log("mem " + offset_text(v) + ": the bandwidth readings did not settle; the memory scan ends here");
                    } else if (!gpu.recover) {
                        hw_suspect = true;   // the driver may have been reset under it; the next write decides
                    } else if (v == 0) {
                        // Nothing is overclocked yet, so this is not the card
                        // giving out: the measurement does not work on this
                        // machine. Not a reset event, and no recovery.
                        unavailable = true;
                        log("the bandwidth measurement does not work here; the memory is not overclocked");
                    } else {
                        log("mem " + offset_text(v) + ": the bandwidth measurement failed; treated as a driver reset");
                        reset_event("the bandwidth measurement failed");
                    }
                    return false;
                });
                if (m.stable) {
                    m.gbps = *gbps;
                    char buf[64];
                    std::snprintf(buf, sizeof(buf), "mem +%d: bandwidth %.1f GB/s%s", v, m.gbps, again ? ", read again" : "");
                    log(buf);
                }
                return m;
            };
            const MemScanEnd end = bandwidth_scan(0, bounds.mem_max_mhz, kMemStep, ceiling, [&](int v) { return sample(v, false); });
            int peak = second_pass_top(end, 0, kMemStep, journal.mem_edge());
            // The second pass, when there is something to read and the scan
            // left the card in a state to read it: not after a reset event
            // (nothing new is explored then, and confirming starts
            // kResetBackoffSteps below `peak`), not on a stopped run.
            if (!unavailable && stopped.empty() && !explored_enough && peak > 0) {
                log("mem: the scan passed " + offset_text(end.top) + "; reading again from " + offset_text(peak) + " down");
                peak = bandwidth_peak_below(peak, 0, kMemStep, [&](int v) { return sample(v, true); });
            }
            // An unavailable measurement ended the scan at +0, and nothing
            // is searched without it. Memory that corrects its own errors
            // computes right far beyond the point where it stops getting
            // faster: a climb on stability alone took a 3080 Ti to +2300 MHz,
            // and the machine froze in the confirm probes below that. The
            // peak of the bandwidth is what keeps the search away from there.
            if (!unavailable) {
                r.mem_max_stable = peak;
            } else if (const std::string why = write_state(r.power_pct, 0, 0); !why.empty()) {
                // The next write, made here and at stock: if the driver was
                // reset under the measurement it fails now, and the event is
                // the memory search's, not the core's first candidate's.
                log(why + "; treated as a driver reset");
                reset_event(why);
            }
            left_at_stock = unavailable;
            // The edge of this card's memory, for the runs after this one. A
            // reset event counts as much as a wrong result: the scan ended
            // there because the memory did.
            if (reset_at > 0) gave_out_at = reset_at;
            if (gave_out_at > 0 && !journal.note_mem_edge(gave_out_at))
                log("warning: the memory's edge could not be written to the journal; the next run will find it again");
        }
        if (!by_bandwidth) {
            r.mem_max_stable = climb_to_edge(0, bounds.mem_max_mhz, kMemStep, kMemStride, ceiling, [&](int v) {
                return explore(v, reset_at, [&] { return is_stable(clock_candidate(std::nullopt, v, 0, v, kClockProbeS)); });
            });
        }
        if (!stopped.empty()) return finish_fail(stopped);
        if (reset_at >= 0)
            log("mem: the driver reset at " + offset_text(reset_at) + "; using " + offset_text(r.mem_max_stable) +
                (by_bandwidth ? ", well below the values that passed" : ", the last value that passed"));
        r.mem_confirmed = confirm_edge(confirm_from(r.mem_max_stable, kMemStep, reset_at), 0, kMemStep, kConfirmTries, [&](int v) {
            return holds(clock_candidate(std::nullopt, v, 0, v, kConfirmProbeS));
        }, confirm_step_down);
        if (!stopped.empty()) return finish_fail(stopped);
        r.mem_mhz = apply_margin(r.mem_confirmed, kMemStep, obj.perf_push);
        if (left_at_stock)
            log("mem: left at stock");
        else
            log(std::string("mem: ") + (by_bandwidth ? "bandwidth peak +" : "highest stable +") +
                std::to_string(r.mem_max_stable) + ", confirmed +" + std::to_string(r.mem_confirmed) +
                ", applying +" + std::to_string(r.mem_mhz));
    }
    // The core comes second and runs with the chosen memory offset applied
    // (0 when the profile does not tune memory). Each clock has its own
    // search: a reset event in the memory phase ended that search, not this
    // one, or a card whose memory gives out with a reset would never get a
    // core offset. The core may explore, and the run takes one more reset
    // event before it ends.
    if (obj.core_oc) {
        if (explored_enough) {
            explored_enough = false;
            ++resets_allowed;
        }
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
        // A clock at stock is not journaled: if the machine froze in this
        // soak, the open entry would leave a ceiling of 0 for that clock, and
        // no later run would search it at all.
        const auto id = begin_candidate(r.core_mhz > 0 ? std::optional<int>(r.core_mhz) : std::nullopt,
                                        r.mem_mhz > 0 ? std::optional<int>(r.mem_mhz) : std::nullopt, r.core_mhz, r.mem_mhz);
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
        step("Final test of the result", kSoakS);
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
            // Tested, and still not kept: a profile that promises speed has
            // nothing to offer a card it made slower. That happens where the
            // card is warmer at stock than the profile allows: the power
            // limit has to come down to meet the temperature, and that costs
            // more than the clocks win back (an RTX 3080 Ti: 4749 it/s after
            // the run, 4937 at stock). Applying it and calling it done would
            // be a lie about what the profile is for.
            if (obj.faster_than_stock && stock && stock->score > 0 && s->score < kNotSlowerScore * stock->score) {
                char why[400];
                std::snprintf(why, sizeof(why), "the result is slower than stock: %.0f it/s against %.0f (%+.1f %%). %s Nothing was saved or applied. "
                              "If the card runs warm, the Undervolt profile is the one that helps: the same speed on less power",
                              s->score, stock->score, (s->score / stock->score - 1) * 100,
                              r.power_pct < 100
                                  ? ("To stay under " + std::to_string(obj.max_temp_c) + " C the power limit went down to " +
                                     std::to_string(r.power_pct) + " %, and that costs more speed than the clocks win back.").c_str()
                                  : "The clocks this card holds do not make it faster.");
                return finish_fail(why);
            }
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
