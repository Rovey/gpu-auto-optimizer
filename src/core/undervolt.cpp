#include "core/undervolt.hpp"
#include "core/vf_curve.hpp"

#include <algorithm>
#include <cstdio>
#include <optional>

namespace gao {

namespace {

constexpr int kStepMhz = 15;   // the grid the raise and its margin are counted on

bool is_reset_verdict(Verdict v) {
    return v == Verdict::DeviceLost || v == Verdict::Stalled || v == Verdict::NoTelemetry;
}

std::string describe(const char* verdict, const StabilityResult& s) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%s  score=%.0f it/s  clock=%d MHz  peak=%d C  power=%d W", verdict, s.score, s.avg_core_mhz,
                  s.peak_temp_c, s.avg_power_w);
    return buf;
}

}

UndervoltResult find_undervolt(const GpuControl& gpu, Journal& journal, const UndervoltIo& io, int max_temp_c, float perf_push) {
    UndervoltResult r;
    auto log = [&](const std::string& m) { if (io.log) io.log("  " + m); };
    auto aborted = [&] { return io.aborted && io.aborted(); };
    auto to_stock = [&] { return gpu.reset_to_stock && gpu.reset_to_stock(); };
    // Every way out that is not a success: the card goes back to stock, with
    // one reconnect if the plain reset does not get through.
    auto finish_fail = [&](const std::string& why) {
        r.ok = false;
        r.reason = why;
        r.stock_restored = to_stock();
        if (!r.stock_restored && gpu.recover && gpu.recover()) r.stock_restored = to_stock();
        log("stopped: " + why + (r.stock_restored ? " -- card restored to stock" : " -- reset FAILED, run `gao --reset`"));
        return r;
    };

    if (!gpu.read_vf_curve || !gpu.write_vf_offsets) {
        r.reason = "this card or driver does not offer the voltage/frequency curve";
        return r;   // nothing was touched
    }
    if (!to_stock()) return finish_fail("could not set the card to stock");
    {   // before any load: a curve that cannot be read or is not at stock ends the run here
        const auto first = gpu.read_vf_curve();
        if (!first || first->empty()) return finish_fail("the curve could not be read");
        if (std::any_of(first->begin(), first->end(), [](const VfPoint& p) { return p.raw_offset != 0; }))
            return finish_fail("the curve did not go back to stock");
    }

    log("baseline: 30 s at stock");
    r.baseline = io.probe(kUvBaselineS, max_temp_c, 0.0);
    log("baseline: " + describe(verdict_name(r.baseline.verdict), r.baseline));
    if (r.baseline.verdict == Verdict::Aborted) return finish_fail("aborted");
    if (r.baseline.verdict != Verdict::Stable)
        return finish_fail(std::string("the card is not stable at stock (") + verdict_name(r.baseline.verdict) + ")");
    if (r.baseline.avg_core_mhz <= 0 || r.baseline.score <= 0) return finish_fail("the card did not report its clock under load");
    // Read now, with the card warm from the baseline: the built-in curve can
    // sit a step lower hot than cold, and the candidates are counted on it.
    const auto stock = gpu.read_vf_curve();
    if (!stock || stock->empty()) return finish_fail("the curve could not be read");
    const std::vector<VfPoint>& curve = *stock;   // the built-in curve, lowest voltage first

    // The clock to keep: the highest point of the curve that is not above
    // what the card ran. Its own position on the curve is where the descent
    // starts from.
    const int load_khz = r.baseline.avg_core_mhz * 1000;
    int target = -1;
    for (int i = 0; i < static_cast<int>(curve.size()); ++i)
        if (curve[static_cast<std::size_t>(i)].freq_khz <= load_khz + kVfToleranceKhz) target = i;
    if (target < 0) return finish_fail("the load clock is below the card's curve");
    r.freq_khz = curve[static_cast<std::size_t>(target)].freq_khz;
    int own = 0;   // the lowest-voltage point that reaches the target by itself
    while (curve[static_cast<std::size_t>(own)].freq_khz < r.freq_khz) ++own;
    r.stock_uv = curve[static_cast<std::size_t>(own)].volt_uv;
    auto raise_mhz = [&](int pos) { return (r.freq_khz - curve[static_cast<std::size_t>(pos)].freq_khz) / 1000; };
    auto mv = [&](int pos) { return curve[static_cast<std::size_t>(pos)].volt_uv / 1000; };
    log("target: " + std::to_string(r.freq_khz / 1000) + " MHz, which the card reaches at " + std::to_string(r.stock_uv / 1000) +
        " mV by itself");

    const int ceiling = journal.ceilings().uv_mhz;
    if (ceiling != INT_MAX)
        log("warning: a previous run froze the machine at undervolt +" + std::to_string(ceiling) + "; staying below it from now on");

    std::string stopped;         // non-empty: the run must end
    StabilityResult last;        // of the candidate that ran last
    enum class Step { Pass, Fail, Reset, Stop };
    // One candidate: journal it, write its flat top, run the load, close the
    // entry. `what` names the probe in the log.
    auto candidate = [&](int pos, double seconds, const char* what) -> Step {
        if (aborted()) { stopped = "aborted"; return Step::Stop; }
        const int id = journal.begin(std::nullopt, std::nullopt, raise_mhz(pos));
        if (id < 0) { stopped = "could not write the journal"; return Step::Stop; }
        const VfApplyResult written = apply_flat_top(gpu, curve[static_cast<std::size_t>(pos)].index, r.freq_khz);
        if (!written.ok) {
            // Not a verdict on the candidate's stability: the card did not
            // take this curve. With the curve back at stock that only means
            // this candidate is not to be had, and the search goes on with
            // what passed; a curve that could not be cleared ends the run.
            journal.complete(id, "SET FAILED");
            log(std::string(what) + std::to_string(mv(pos)) + " mV (+" + std::to_string(raise_mhz(pos)) + " MHz): not set -- " + written.why);
            if (written.at_stock) return Step::Fail;
            stopped = "writing the curve failed: " + written.why;
            return Step::Stop;
        }
        last = io.probe(seconds, max_temp_c, kStalledScore * r.baseline.score);
        const char* verdict = verdict_name(last.verdict);
        bool pass = last.verdict == Verdict::Stable;
        if (pass && (last.avg_core_mhz * 1000 < r.freq_khz - kUvClockSlackKhz || last.score < kUvScoreKeep * r.baseline.score)) {
            pass = false;
            verdict = "SLOW";
        }
        if (!journal.complete(id, verdict)) { stopped = "could not write the journal"; return Step::Stop; }
        log(std::string(what) + std::to_string(mv(pos)) + " mV (+" + std::to_string(raise_mhz(pos)) + " MHz): " + describe(verdict, last));
        if (last.verdict == Verdict::Aborted) { stopped = "aborted"; return Step::Stop; }
        if (is_reset_verdict(last.verdict)) return Step::Reset;
        return pass ? Step::Pass : Step::Fail;
    };
    // After a driver reset: reconnect, stock, rest, and a short probe at stock
    // that must compute normally again. False: `stopped` says why the run ends.
    auto recovered = [&]() -> bool {
        if (++r.driver_resets >= 2) { stopped = "the driver reset twice"; return false; }
        log("the driver was reset; reconnecting");
        if (!gpu.recover || !gpu.recover()) { stopped = "the driver did not come back after a reset"; return false; }
        if (!to_stock()) { stopped = "could not set the card to stock while recovering"; return false; }
        if (io.rest) {
            log("resting " + std::to_string(static_cast<int>(kRestAfterResetS)) + " s before the card is loaded again");
            if (!io.rest(kRestAfterResetS)) { stopped = "aborted"; return false; }
        }
        if (io.prepare_load && !io.prepare_load()) { stopped = "the stress load could not be rebuilt after a driver reset"; return false; }
        const StabilityResult health = io.probe(kHealthProbeS, max_temp_c, 0.0);
        log("health: " + describe(verdict_name(health.verdict), health));
        if (health.verdict == Verdict::Aborted) { stopped = "aborted"; return false; }
        if (is_reset_verdict(health.verdict)) { ++r.driver_resets; stopped = "the card was not usable when checked after a driver reset"; return false; }
        if (health.verdict != Verdict::Stable || health.score < kHealthyScore * r.baseline.score) {
            stopped = "the card did not recover after a driver reset";
            return false;
        }
        return true;
    };

    // The descent: one point lower each time, until a candidate fails.
    int edge = -1;
    bool reset = false;
    for (int pos = own - 1; pos >= 0; --pos) {
        if (raise_mhz(pos) <= 0) continue;                       // the same clock at this voltage already
        if (raise_mhz(pos) * 1000 > kVfMaxRaiseKhz || raise_mhz(pos) >= ceiling) break;
        const Step step = candidate(pos, kUvProbeS, "");
        if (step == Step::Pass) { edge = pos; continue; }
        reset = step == Step::Reset;
        break;
    }
    if (!stopped.empty()) return finish_fail(stopped);
    if (reset && !recovered()) return finish_fail(stopped);
    if (edge < 0) return finish_fail("the card held no undervolt: the first step below its own voltage already failed");
    r.edge_uv = curve[static_cast<std::size_t>(edge)].volt_uv;

    // Confirm with a long probe; give up one point at a time, or
    // kResetBackoffSteps after a reset.
    int confirmed = -1;
    int pos = reset ? edge + kResetBackoffSteps : edge;
    for (int tries = 0; tries < kUvConfirmTries && pos < own; ++tries) {
        const Step step = candidate(pos, kUvConfirmS, "confirm ");
        if (step == Step::Pass) { confirmed = pos; break; }
        if (step == Step::Stop) return finish_fail(stopped);
        if (step == Step::Reset && !recovered()) return finish_fail(stopped);
        pos += step == Step::Reset ? kResetBackoffSteps : 1;
    }
    if (confirmed < 0) return finish_fail("no undervolt held the 30 s probe");
    r.confirmed_uv = curve[static_cast<std::size_t>(confirmed)].volt_uv;

    // The margin, on the raise, as for a core offset: the lowest voltage
    // whose raise is within what is kept.
    const int kept_mhz = apply_margin(raise_mhz(confirmed), kStepMhz, perf_push);
    int applied = -1;
    for (int p = confirmed; p < own; ++p)
        if (raise_mhz(p) > 0 && raise_mhz(p) <= kept_mhz) { applied = p; break; }
    if (applied < 0) return finish_fail("nothing was left of the undervolt after the safety margin");
    log("undervolt: lowest stable " + std::to_string(mv(edge)) + " mV, confirmed " + std::to_string(mv(confirmed)) + " mV, applying " +
        std::to_string(mv(applied)) + " mV (+" + std::to_string(raise_mhz(applied)) + " MHz at that point)");

    // The soak decides. A failure moves two points up; a reset more.
    for (int attempt = 0; attempt < kUvSoakAttempts && applied < own; ++attempt) {
        const Step step = candidate(applied, kUvSoakS, "soak ");
        if (step == Step::Pass) {
            r.ok = true;
            r.applied_uv = curve[static_cast<std::size_t>(applied)].volt_uv;
            r.applied_index = curve[static_cast<std::size_t>(applied)].index;
            r.after = last;
            return r;
        }
        if (step == Step::Stop) return finish_fail(stopped);
        if (step == Step::Reset && !recovered()) return finish_fail(stopped);
        applied += step == Step::Reset ? kResetBackoffSteps : 2;
    }
    return finish_fail("the soak failed");
}

}
