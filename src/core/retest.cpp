#include "core/retest.hpp"
#include "core/boot.hpp"
#include "core/objectives.hpp"
#include "core/step_down.hpp"

namespace gao {

RetestResult retest_saved_tune(Config& c, const std::string& driver, const std::string& gpu_id, const GpuControl& gpu,
                               const RetestIo& io) {
    RetestResult r;
    auto log = [&](const std::string& m) { if (io.log) io.log(m); };
    auto refuse = [&](const std::string& why) {
        r.reason = why;
        r.at_stock = true;   // nothing was written
        return r;
    };
    if (!c.profile) return refuse("there is no saved tune");
    if (driver.empty()) return refuse("the driver version is not known (NVML did not report it)");
    if (gpu_id.empty() || gpu_id != c.profile->gpu) return refuse("this is not the card the tune was tested on; optimize again");

    std::string why;
    bool left_clean = false;
    if (!apply_profile(gpu, *c.profile, &why, &left_clean)) {
        r.reason = "the tune could not be applied: " + why;
        r.at_stock = left_clean;
        return r;
    }
    log("testing the saved tune: " + std::to_string(static_cast<int>(kRetestS)) + " s on driver " + driver);
    if (io.measuring) io.measuring("Testing the saved tune", kRetestS);
    r.test = io.probe(kRetestS, objectives_for(c.profile->preset).max_temp_c, 0.0);
    if (io.measuring) io.measuring("", 0);

    if (r.test.verdict == Verdict::Stable) {
        c.profile->driver = driver;
        c.boot_strikes = 0;   // strikes belong to the driver they were earned on
        r.ok = true;
        return r;
    }
    r.reason = r.test.verdict == Verdict::Aborted
                   ? std::string("aborted")
                   : "the tune did not hold on driver " + driver + " (" + verdict_name(r.test.verdict) + ")";
    // After a driver reset the connections are dead: stock is written on new ones.
    const bool lost = r.test.verdict == Verdict::DeviceLost || r.test.verdict == Verdict::Stalled ||
                      r.test.verdict == Verdict::NoTelemetry;
    if (lost && gpu.recover) gpu.recover();
    r.at_stock = gpu.reset_to_stock && gpu.reset_to_stock();
    return r;
}

namespace {

// The verdicts that suggest the driver reset, as in the search (is_reset_verdict there).
bool lost(Verdict v) { return v == Verdict::DeviceLost || v == Verdict::Stalled || v == Verdict::NoTelemetry; }

enum class Health { Back, NotBack, Lost, Aborted };

// After a driver reset, as the search's recovery gate: retest_saved_tune has
// reconnected and written stock already. The card rests, gets a fresh stress
// load (once), and must prove at stock that it is back: a short probe that is
// STABLE and, when the run that saved the tune measured the card at stock,
// scores at least kHealthyScore of that (and not below kStalledScore of it).
// kHealthTries probes in all. Lost: the health probe itself reset the driver;
// the connections are made anew and stock written before this returns.
Health check_health(const Profile& p, const GpuControl& gpu, const RetestIo& io, bool* at_stock, std::string* seen) {
    auto log = [&](const std::string& m) { if (io.log) io.log(m); };
    auto step = [&](const std::string& what, double seconds) { if (io.measuring) io.measuring(what, seconds); };
    const double bar = p.against_stock ? p.against_stock->stock.score : 0;
    bool load_ready = !io.prepare_load;
    for (int t = 0; t < kHealthTries; ++t) {
        if (io.rest) {
            log("resting " + std::to_string(static_cast<int>(kRestAfterResetS)) + " s before the card is loaded again");
            step("The driver was reset: resting before the card is loaded again", kRestAfterResetS);
            const bool rested = io.rest(kRestAfterResetS);
            step("", 0);
            if (!rested) return Health::Aborted;
        }
        if (!load_ready) {
            log("preparing the stress load");
            load_ready = io.prepare_load();
            if (!load_ready) { log("the stress load is not ready yet"); continue; }
        }
        log("checking that the card is back: " + std::to_string(static_cast<int>(kHealthProbeS)) + " s at stock");
        step("Checking that the card is back", kHealthProbeS);
        StabilityResult h = io.probe(kHealthProbeS, objectives_for(p.preset).max_temp_c, 0.0);
        step("", 0);
        if (h.verdict == Verdict::Stable && bar > 0 && h.score < kStalledScore * bar) h.verdict = Verdict::Stalled;
        log(measured_line((std::string("health: ") + verdict_name(h.verdict)).c_str(), h, true));
        if (h.verdict == Verdict::Aborted) return Health::Aborted;
        if (lost(h.verdict)) {
            *seen = verdict_name(h.verdict);
            if (gpu.recover) gpu.recover();
            *at_stock = gpu.reset_to_stock && gpu.reset_to_stock();
            return Health::Lost;
        }
        // Without a stock score to measure against, a probe that ran clean is back.
        if (h.verdict == Verdict::Stable && (bar <= 0 || h.score >= kHealthyScore * bar)) return Health::Back;
        // Computing, but not back yet (slow, a wrong result, too hot).
    }
    return Health::NotBack;
}

// The failed step goes on record; at step 0 without step_down the profile is left as it was.
void record_failed(Profile& p, int step, SteppedRetestResult& s) {
    if (!p.step_down) return;
    auto& failed = p.step_down->failed_step;
    if (!failed || *failed < step) failed = step;
    s.stepped = true;
}

}

SteppedRetestResult retest_stepping(Config& c, const std::string& driver, const std::string& gpu_id, const GpuControl& gpu,
                                    const RetestIo& io, int grid_khz) {
    SteppedRetestResult s;
    s.first_step = s.final_step = c.profile ? current_step(*c.profile) : 0;
    RetestIo step_io = io;
    // A step's test is named by its step, the first one too when the saved tune is lowered already.
    auto name_step = [&](int step) {
        const std::string what = step_text(step);
        if (io.log) io.log("testing " + what);
        if (io.measuring) {
            step_io.measuring = [&io, what](const std::string& text, double seconds) {
                io.measuring(text.empty() ? text : "Testing " + what, seconds);
            };
        }
    };
    if (s.first_step > 0) name_step(s.first_step);
    int resets = 0;   // driver resets in this stepped retest: the second ends it, as it ends a tuning run
    for (;;) {
        s.last = retest_saved_tune(c, driver, gpu_id, gpu, step_io);
        if (s.last.ok) {
            s.ok = true;
            if (auto& sd = c.profile->step_down) {
                sd->tested = true;
                // It held: it is no longer a step that failed, whatever failed at it before.
                if (sd->failed_step && *sd->failed_step >= sd->step) {
                    if (sd->step - 1 < 0) sd->failed_step.reset();
                    else sd->failed_step = sd->step - 1;
                    s.stepped = true;
                }
            }
            return s;
        }
        const Verdict v = s.last.test.verdict;
        const bool failed_test = s.last.test.seconds > 0 && v != Verdict::Stable && v != Verdict::Aborted;
        if (!failed_test) return s;   // refused before the test, or aborted: nothing to step down from
        const int step = current_step(*c.profile);
        const bool reset = lost(v);
        if (reset && ++resets >= 2) {
            record_failed(*c.profile, step, s);
            s.last.reason += "; the driver reset twice; optimize again";
            return s;
        }
        if (!can_step_down(*c.profile, grid_khz)) {
            record_failed(*c.profile, step, s);
            s.last.reason += "; optimize again";
            return s;
        }
        // Why this step failed, when the next one is tested: the RESULT lines
        // show only the last test, and they show this one when it is the last.
        const std::string verdict_line = measured_line(verdict_name(v), s.last.test, true);
        auto log_verdict = [&] { if (io.log) io.log(verdict_line); };
        Profile next = at_step(*c.profile, step + 1, grid_khz);
        auto& failed = next.step_down->failed_step;
        if (!failed || *failed < step) failed = step;
        if (reset) {
            // Not loaded again until the card is back: a step tested right
            // after a reset fails on the reset, not on its clocks.
            std::string seen;
            switch (check_health(*c.profile, gpu, io, &s.last.at_stock, &seen)) {
                case Health::Back: break;
                case Health::Aborted:
                    // As an abort of the next step's test: that step, never the failed one, and the caller puts it back.
                    log_verdict();   // the result is the abort: the failed step is only in the log
                    c.profile = next;
                    s.stepped = true;
                    s.final_step = step + 1;
                    s.last.reason = "aborted";
                    s.last.test = StabilityResult{};
                    s.last.test.verdict = Verdict::Aborted;
                    return s;
                case Health::Lost:
                    record_failed(*c.profile, step, s);
                    s.last.reason += "; the driver reset again when the card was checked at stock (" + seen + "); optimize again";
                    return s;
                case Health::NotBack:
                    record_failed(*c.profile, step, s);
                    s.last.reason += "; the card did not recover after a driver reset; optimize again";
                    return s;
            }
        }
        log_verdict();
        c.profile = next;
        s.stepped = true;
        s.final_step = step + 1;
        name_step(s.final_step);
    }
}

}
