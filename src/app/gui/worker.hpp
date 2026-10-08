#pragma once
#include "app/common.hpp"
#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace gao::gui {

// Runs one optimize, or one undervolt search (Preset::Undervolt), on a
// background thread so the window stays responsive.
// The UI thread polls snapshot(); `wake` is called (from the worker thread)
// whenever there is something new to draw.
class OptimizeWorker {
public:
    struct Snapshot {
        bool running = false;
        std::vector<std::string> log;
        std::optional<app::OptimizeOutcome> outcome;     // set once an optimize run has ended
        std::optional<app::UndervoltOutcome> undervolt;  // set once an undervolt search has ended
        // The long measurement that is under way, for a countdown. Empty: none.
        std::string measuring;
        double measuring_seconds = 0;   // its length
        double measuring_spent = 0;     // how long it has been running
        bool ended() const { return outcome || undervolt; }
        // The run got as far as the search: it ended at its result or at stock.
        bool ran() const { return outcome ? outcome->ran : undervolt && undervolt->ran; }
        // Its result is applied and saved: what the watchdog keeps from now on.
        bool kept() const {
            return outcome ? outcome->ran && outcome->result.ok && outcome->saved
                           : undervolt && undervolt->ran && undervolt->result.ok && undervolt->saved;
        }
    };

    explicit OptimizeWorker(std::function<void()> wake) : wake_(std::move(wake)) {}
    ~OptimizeWorker();

    bool start(Preset preset, std::optional<FanCurve> fan_curve);   // false when a run is already active
    void abort() { abort_ = true; }     // the run restores stock before it ends
    bool running() const { return running_; }
    Snapshot snapshot() const;

private:
    std::function<void()> wake_;
    mutable std::mutex mu_;
    std::vector<std::string> log_;
    std::optional<app::OptimizeOutcome> outcome_;
    std::optional<app::UndervoltOutcome> undervolt_;
    std::string measuring_;
    double measuring_seconds_ = 0;
    std::chrono::steady_clock::time_point measuring_from_;
    std::atomic<bool> running_{false};
    std::atomic<bool> abort_{false};
    std::jthread thread_;
};

}
