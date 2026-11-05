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

SteppedRetestResult retest_stepping(Config& c, const std::string& driver, const std::string& gpu_id, const GpuControl& gpu,
                                    const RetestIo& io, int grid_khz) {
    SteppedRetestResult s;
    s.first_step = s.final_step = c.profile ? current_step(*c.profile) : 0;
    RetestIo step_io = io;
    for (;;) {
        s.last = retest_saved_tune(c, driver, gpu_id, gpu, step_io);
        if (s.last.ok) {
            s.ok = true;
            if (c.profile->step_down) c.profile->step_down->tested = true;
            return s;
        }
        const Verdict v = s.last.test.verdict;
        const bool failed_test = s.last.test.seconds > 0 && v != Verdict::Stable && v != Verdict::Aborted;
        if (!failed_test) return s;   // refused before the test, or aborted: nothing to step down from
        const int step = current_step(*c.profile);
        if (!can_step_down(*c.profile, grid_khz)) {
            if (c.profile->step_down) {
                auto& failed = c.profile->step_down->failed_step;
                if (!failed || *failed < step) failed = step;
                s.stepped = true;
            }
            s.last.reason += "; optimize again";
            return s;
        }
        Profile next = at_step(*c.profile, step + 1, grid_khz);
        auto& failed = next.step_down->failed_step;
        if (!failed || *failed < step) failed = step;
        c.profile = next;
        s.stepped = true;
        s.final_step = step + 1;
        const std::string what = step_text(s.final_step);
        if (io.log) io.log("testing " + what);
        if (io.measuring) {
            step_io.measuring = [&io, what](const std::string& text, double seconds) {
                io.measuring(text.empty() ? text : "Testing " + what, seconds);
            };
        }
    }
}

}
