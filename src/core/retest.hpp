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

}
