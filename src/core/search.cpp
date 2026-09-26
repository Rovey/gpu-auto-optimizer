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

int confirm_edge(int edge, int lo, int step, int tries, const std::function<bool(int)>& holds) {
    for (int t = 0, v = edge; t < tries && v > lo; ++t, v -= step)
        if (holds(v)) return v;
    return lo;
}

namespace {
constexpr int kCoreStep = 15, kCoreMax = kCoreMaxMhz;
constexpr int kMemStep = 50, kMemMax = kMemMaxMhz;
constexpr int kPowerStep = 5;
constexpr double kBaselineS = 30, kPowerProbeS = 20, kClockProbeS = 3, kSoakS = 60;
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
}

OptimizeResult optimize(const GpuControl& gpu, const Objectives& obj, Journal& journal, const OptimizeIo& io) {
    OptimizeResult r;
    std::string stopped;   // non-empty once the run must end; later probes become no-ops
    auto log = [&](const std::string& m) { if (io.log) io.log(m); };
    auto finish_fail = [&](const std::string& why) {
        r.stock_restored = gpu.reset_to_stock && gpu.reset_to_stock();
        r.ok = false;
        r.reason = why;
        log("stopped: " + why + (r.stock_restored ? " -- card restored to stock"
                                                   : " -- reset FAILED, run `gao --reset`"));
        return r;
    };
    bool power_ctl = obj.power && gpu.set_power_limit && gpu.power_limit_range_pct;   // false once skipped

    // Applies the full state for every candidate, so a TDR that reset the
    // driver (or a previous candidate) can never leave stale settings behind.
    auto set_state = [&](int power, int core, int mem) -> bool {
        if (power_ctl && !gpu.set_power_limit(power)) { stopped = "setting power " + std::to_string(power) + " % failed"; return false; }
        if ((obj.core_oc || core != 0) && !(gpu.set_core_offset && gpu.set_core_offset(core))) {
            stopped = "setting core +" + std::to_string(core) + " failed"; return false;
        }
        if ((obj.mem_oc || mem != 0) && !(gpu.set_mem_offset && gpu.set_mem_offset(mem))) {
            stopped = "setting mem +" + std::to_string(mem) + " failed"; return false;
        }
        return true;
    };
    // One probe, with the abort and blindness checks every step shares.
    auto probe = [&](double seconds, int max_temp) -> std::optional<StabilityResult> {
        if (!stopped.empty()) return std::nullopt;
        if (io.aborted && io.aborted()) { stopped = "aborted"; return std::nullopt; }
        const StabilityResult s = io.probe(seconds, max_temp);
        if (s.verdict == Verdict::NoTelemetry) { stopped = "lost telemetry"; return std::nullopt; }
        return s;
    };

    if (!gpu.reset_to_stock || !gpu.reset_to_stock()) return finish_fail("could not reset to stock");
    log("baseline: 30 s at stock");
    const auto base = probe(kBaselineS, kSafetyTempC);
    if (!base) return finish_fail(stopped);
    r.baseline = *base;
    log("baseline: " + describe(*base));
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
            auto run_power = [&](int pct) -> std::optional<StabilityResult> {
                if (!stopped.empty()) return std::nullopt;
                if (io.aborted && io.aborted()) { stopped = "aborted"; return std::nullopt; }
                if (!set_state(pct, 0, 0)) return std::nullopt;
                const auto s = probe(kPowerProbeS, kSafetyTempC);
                if (s) { seen[pct] = *s; log("power " + std::to_string(pct) + " %: " + describe(*s)); }
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
                if (!seen.count(cap) && !run_power(cap)) return finish_fail(stopped);
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

    // One clock candidate: journal first, then hardware, then the probe.
    // `extra` runs while the candidate is still applied and stable (e.g. a
    // bandwidth measurement), before the journal entry is closed. Returns the
    // verdict, or nothing when the candidate did not run.
    auto clock_candidate = [&](std::optional<int> core_j, std::optional<int> mem_j, int core, int mem,
                               double seconds, const std::function<void()>& extra = {}) -> std::optional<Verdict> {
        if (!stopped.empty()) return std::nullopt;
        if (io.aborted && io.aborted()) { stopped = "aborted"; return std::nullopt; }
        const int id = journal.begin(core_j, mem_j);
        if (id < 0) { stopped = "could not write the journal"; return std::nullopt; }
        if (!set_state(r.power_pct, core, mem)) { journal.complete(id, "SET FAILED"); return std::nullopt; }
        const auto s = probe(seconds, obj.max_temp_c);
        if (s && s->verdict == Verdict::Stable && extra) extra();
        journal.complete(id, s ? verdict_name(s->verdict) : "NOT RUN");
        if (!s) return std::nullopt;
        log(std::string(seconds == kConfirmProbeS ? "confirm " : "") + "core +" + std::to_string(core) +
            " / mem +" + std::to_string(mem) + ": " + describe(*s));
        return s->verdict;
    };
    auto is_stable = [](std::optional<Verdict> v) { return v == Verdict::Stable; };
    // A confirm probe only decides whether the clock holds. Running a little
    // hot over 30 s is the power step's and the soak's business (the soak
    // lowers power), not a reason to throw the clock edge away.
    auto holds = [](std::optional<Verdict> v) { return v == Verdict::Stable || v == Verdict::TooHot; };

    if (obj.core_oc) {
        r.core_max_stable = highest_stable(0, kCoreMax, kCoreStep, journal.ceilings().core_mhz,
                                           [&](int v) { return is_stable(clock_candidate(v, std::nullopt, v, 0, kClockProbeS)); });
        if (!stopped.empty()) return finish_fail(stopped);
        r.core_confirmed = confirm_edge(r.core_max_stable, 0, kCoreStep, kConfirmTries, [&](int v) {
            return holds(clock_candidate(v, std::nullopt, v, 0, kConfirmProbeS));
        });
        if (!stopped.empty()) return finish_fail(stopped);
        r.core_mhz = apply_margin(r.core_confirmed, kCoreStep, obj.perf_push);
        log("core: highest stable +" + std::to_string(r.core_max_stable) + ", confirmed +" +
            std::to_string(r.core_confirmed) + ", applying +" + std::to_string(r.core_mhz));
    }
    if (obj.mem_oc) {
        const int ceiling = journal.ceilings().mem_mhz;
        if (io.bandwidth) {
            r.mem_max_stable = best_bandwidth_offset(0, kMemMax, kMemStep, ceiling, [&](int v) {
                MemSample m;
                std::optional<double> gbps;
                m.stable = is_stable(clock_candidate(std::nullopt, v, r.core_mhz, v, kClockProbeS,
                                                     [&] { gbps = io.bandwidth(); }));
                // A failed measurement (device lost) makes the step unusable.
                if (m.stable && !gbps) m.stable = false;
                if (m.stable) {
                    m.gbps = *gbps;
                    char buf[64];
                    std::snprintf(buf, sizeof(buf), "mem +%d: bandwidth %.1f GB/s", v, m.gbps);
                    log(buf);
                }
                return m;
            });
        } else {
            r.mem_max_stable = highest_stable(0, kMemMax, kMemStep, ceiling, [&](int v) {
                return is_stable(clock_candidate(std::nullopt, v, r.core_mhz, v, kClockProbeS));
            });
        }
        if (!stopped.empty()) return finish_fail(stopped);
        r.mem_confirmed = confirm_edge(r.mem_max_stable, 0, kMemStep, kConfirmTries, [&](int v) {
            return holds(clock_candidate(std::nullopt, v, r.core_mhz, v, kConfirmProbeS));
        });
        if (!stopped.empty()) return finish_fail(stopped);
        r.mem_mhz = apply_margin(r.mem_confirmed, kMemStep, obj.perf_push);
        log(std::string("mem: ") + (io.bandwidth ? "bandwidth peak +" : "highest stable +") +
            std::to_string(r.mem_max_stable) + ", confirmed +" + std::to_string(r.mem_confirmed) +
            ", applying +" + std::to_string(r.mem_mhz));
    }

    // Soak. On failure: too hot -> one power step down (lower clocks barely
    // cool the card); anything else -> both clocks one step down.
    for (int attempt = 0; attempt <= kSoakRetries; ++attempt) {
        if (io.aborted && io.aborted()) return finish_fail("aborted");
        const int id = journal.begin(obj.core_oc ? std::optional<int>(r.core_mhz) : std::nullopt,
                                     obj.mem_oc ? std::optional<int>(r.mem_mhz) : std::nullopt);
        if (id < 0) return finish_fail("could not write the journal");
        if (!set_state(r.power_pct, r.core_mhz, r.mem_mhz)) { journal.complete(id, "SET FAILED"); return finish_fail(stopped); }
        log("soak: 60 s at power " + std::to_string(r.power_pct) + " %, core +" + std::to_string(r.core_mhz) +
            ", mem +" + std::to_string(r.mem_mhz));
        const auto s = probe(kSoakS, obj.max_temp_c);
        journal.complete(id, s ? verdict_name(s->verdict) : "NOT RUN");
        if (!s) return finish_fail(stopped);
        log("soak: " + describe(*s));
        // The soak is the longest probe; Ctrl+C during it must still win.
        if (io.aborted && io.aborted()) return finish_fail("aborted");
        if (s->verdict == Verdict::Stable) {
            r.soak = *s;
            r.ok = true;
            return r;
        }
        if (s->verdict == Verdict::TooHot && power_ctl && r.power_pct - kPowerStep >= power_floor) {
            r.power_pct -= kPowerStep;
            continue;
        }
        if (obj.core_oc) r.core_mhz = std::max(0, r.core_mhz - kCoreStep);
        if (obj.mem_oc) r.mem_mhz = std::max(0, r.mem_mhz - kMemStep);
    }
    return finish_fail("soak failed " + std::to_string(kSoakRetries + 1) + " times");
}

}
