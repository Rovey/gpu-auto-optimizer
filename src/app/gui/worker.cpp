#include "app/gui/worker.hpp"
#include <exception>

namespace gao::gui {

OptimizeWorker::~OptimizeWorker() {
    abort_ = true;   // a closing window never leaves a candidate applied: the run resets to stock
}

bool OptimizeWorker::start(Preset preset, std::optional<FanCurve> fan_curve) {
    if (running_.exchange(true)) return false;
    if (thread_.joinable()) thread_.join();   // the previous run's thread has already finished
    {
        std::lock_guard lock(mu_);
        log_.clear();
        outcome_.reset();
        undervolt_.reset();
        all_in_one_.reset();
        measuring_.clear();
    }
    abort_ = false;
    thread_ = std::jthread([this, preset, fan_curve] {
        app::OptimizeHooks hooks;
        hooks.aborted = [this] { return abort_.load(); };
        hooks.log = [this](const std::string& line) {
            {
                std::lock_guard lock(mu_);
                log_.push_back(line);
            }
            wake_();
        };
        hooks.measuring = [this](const std::string& what, double seconds) {
            {
                std::lock_guard lock(mu_);
                measuring_ = what;
                measuring_seconds_ = seconds;
                measuring_from_ = std::chrono::steady_clock::now();
            }
            wake_();
        };
        app::OptimizeOutcome outcome;
        app::UndervoltOutcome undervolt;
        app::AllInOneOutcome all;
        const bool is_undervolt = preset == Preset::Undervolt, is_all = preset == Preset::AllInOne;
        try {
            if (is_all) all = app::run_all_in_one(hooks);
            else if (is_undervolt) undervolt = app::run_undervolt(hooks);
            else outcome = app::run_optimize(preset, hooks, fan_curve);
        } catch (const std::exception& e) {   // a thrown exception would otherwise end the process
            (is_all ? all.error : is_undervolt ? undervolt.error : outcome.error) = std::string("unexpected error: ") + e.what();
        }
        {
            std::lock_guard lock(mu_);
            if (is_all) all_in_one_ = std::move(all);
            else if (is_undervolt) undervolt_ = std::move(undervolt);
            else outcome_ = std::move(outcome);
            measuring_.clear();   // also after a run that ended in an exception
        }
        running_ = false;
        wake_();
    });
    return true;
}

OptimizeWorker::Snapshot OptimizeWorker::snapshot() const {
    std::lock_guard lock(mu_);
    Snapshot s{running_.load(), log_, outcome_, undervolt_, all_in_one_};
    if (!measuring_.empty()) {
        s.measuring = measuring_;
        s.measuring_seconds = measuring_seconds_;
        s.measuring_spent = std::chrono::duration<double>(std::chrono::steady_clock::now() - measuring_from_).count();
    }
    return s;
}

}
