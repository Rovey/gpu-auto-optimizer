#pragma once
// What the run files (common_*.cpp) share among themselves; not part of
// the interface of common.hpp.
#include "app/common.hpp"
#include "app/guarded_gpu.hpp"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "core/fan_curve.hpp"
#include "core/stability.hpp"
#include <optional>
#include <string>

namespace gao::app {

struct KeepAwake {
    KeepAwake() { SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED); }
    ~KeepAwake() { SetThreadExecutionState(ES_CONTINUOUS); }
};

constexpr double kLongRunS = 300;      // as the soaks: the two sides of the comparison last equally long

// How long a look at what else uses the card lasts, in samples a second apart.
constexpr int kBusyLookS = 3;

MeasuredRun measured_run(const StabilityResult& r);
std::optional<AgainstStock> against_stock(const StabilityResult& stock, const StabilityResult& tuned);
std::string several_gpus_refusal();
std::string other_tool();
BusyGate busy_before_run(const OptimizeHooks& hooks);

// The card at stock under load, five minutes: the "before" of every result.
// It brings its own drivers and stress load, like the searches, and leaves the
// fans to the NVIDIA driver.
struct StockOutcome {
    std::string error;       // why no run follows: it did not run, was stopped, or the card failed at stock
    StabilityResult result;
};

StockOutcome measure_stock(const OptimizeHooks& hooks);
Telemetry read_with_fans(const GpuControl& gpu, FanDriver& fans, const OptimizeHooks& hooks);

// The fans of a run back to the driver, on every exit: done, aborted, failed
// or thrown.
struct FanRelease {
    FanDriver& f;
    const GuardedGpu& hw;
    const GpuControl& gpu;
    const OptimizeHooks& hooks;
    ~FanRelease() {
        f.release();
        // A hand-back that did not reach the driver, from a fan driver
        // that failed earlier or from the release() above, is remembered
        // by `hw`; the fans may still be at a manual speed. One more
        // attempt, and only then: a hand-back that was delivered is not
        // repeated, it would take the fans from another program that set
        // them since. Hand back only, never a speed.
        if (hw.FanAutoOwed() && gpu.set_fan_auto) gpu.set_fan_auto();
        if (!hw.FanAutoOwed()) return;
        // No reconnect at exit, so nothing will deliver it any more. A
        // destructor must not throw; the line is lost if it cannot be built.
        try {
            if (hooks.log)
                hooks.log("fans: could not be handed back to the driver; they may still be at a manual speed -- run `gao --fan auto`");
        } catch (...) {
        }
    }
};

}
