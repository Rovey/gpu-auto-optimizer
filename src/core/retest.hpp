#pragma once
#include "core/config.hpp"
#include "core/search.hpp"
#include "core/stability.hpp"
#include "core/types.hpp"
#include <functional>
#include <string>

namespace gao {

// A saved tune is tied to the driver it was tested on: after a driver update
// boot-apply refuses it (decide_boot). Finding it again takes a whole run;
// testing whether it still holds takes five minutes.

// As long as a result is tested at the end of a run: a card is not heated
// through sooner.
inline constexpr double kRetestS = 300;

struct RetestIo {
    Probe probe;                                    // one stress run, as in OptimizeIo
    std::function<void(const std::string&)> log;    // optional
    // The test starts (what, its length) and has ended ("", 0). Optional.
    std::function<void(const std::string& what, double seconds)> measuring;
    // After a driver reset, as in OptimizeIo: waits without load (false: cut
    // short by a stop request), and makes the stress load ready on a fresh
    // device (false: not ready yet). Optional: empty, nothing waits or is
    // prepared.
    std::function<bool(double seconds)> rest;
    std::function<bool()> prepare_load;
};

struct RetestResult {
    bool ok = false;        // the tune held: it is applied, and the profile carries the driver it was tested on now
    std::string reason;     // why not, when !ok
    bool at_stock = false;  // when !ok: nothing of the tune is on the card (false: the reset to stock failed too)
    StabilityResult test;   // the five minutes; seconds 0 when they did not run
};

// Applies c.profile and loads the card with it for kRetestS, under the
// temperature limit of its preset. When that is stable the profile is stamped
// with `driver` and the strikes are cleared: the caller saves `c`, and
// decide_boot applies the tune from then on. Anything else ends at stock with
// `c` untouched, so the tune stays refused. Refused before anything is
// written: no profile, an unknown driver, another card than the one the tune
// was tested on (`gpu_id`).
RetestResult retest_saved_tune(Config& c, const std::string& driver, const std::string& gpu_id, const GpuControl& gpu,
                               const RetestIo& io);

struct SteppedRetestResult {
    RetestResult last;          // the last 5-minute test
    bool ok = false;            // a step held: c.profile is that step, tested, stamped with `driver`
    bool stepped = false;       // c.profile changed (its step or its failed_step): save c in every case where this is true
    int first_step = 0, final_step = 0;
};

// retest_saved_tune, then while it fails (not Aborted, and the test ran) and
// can_step_down: failed_step = the failed step, c.profile = at_step(next,
// grid_khz), test again. Logs "testing -15 % (step 3)" and passes
// "Testing -15 % (step 3)" to io.measuring, for the first test too when the
// saved tune is at a step already. On ok: step_down->tested = true, and a
// failed_step at or above the step that held becomes the step below it.
// When no step is left: c.profile stays at the last failed step (failed_step ==
// step; at step 0 it is untouched) and the reason ends with "; optimize again".
// Aborted: c.profile is the step being tested (never a failed one), not applied
// by this function; the caller re-applies it. A refusal before the test
// (another card, nothing written) does not step.
// A failed step's verdict is logged when the next step is tested, or when a
// stop request ends it before that (the result then shows the abort); when the
// failed step's test is the last one, only the result shows it. After a
// test that ended DEVICE LOST, STALLED or NO TELEMETRY the card is not loaded
// with the next step until it is back, as in the tuning run: rest
// kRestAfterResetS, io.prepare_load, a kHealthProbeS probe at stock (up to
// kHealthTries). A second driver reset (in a test or in that health probe),
// or a card that does not come back, ends it at stock like "no step left"
// (failed_step == the step that failed) with "; optimize again". A stop
// request during the rest or the health probe is an abort of the next step.
SteppedRetestResult retest_stepping(Config& c, const std::string& driver, const std::string& gpu_id, const GpuControl& gpu,
                                    const RetestIo& io, int grid_khz);

}
