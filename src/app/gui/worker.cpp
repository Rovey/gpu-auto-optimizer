#include "app/gui/worker.hpp"
#include <exception>

namespace gao::gui {

OptimizeWorker::~OptimizeWorker() {
    abort();   // a closing window never leaves a candidate applied: the run resets to stock
}

void OptimizeWorker::abort() {
    abort_ = true;
    busy_cv_.notify_all();   // a run that waits for an answer stops waiting
}

void OptimizeWorker::answer_busy(BusyAnswer answer) {
    {
        std::lock_guard lock(mu_);
        if (busy_question_.empty()) return;
        busy_answer_ = answer;
    }
    busy_cv_.notify_all();
}

bool OptimizeWorker::launch(bool retest, std::function<void(const app::OptimizeHooks&)> job) {
    if (running_.exchange(true)) return false;
    if (thread_.joinable()) thread_.join();   // the previous run's thread has already finished
    {
        std::lock_guard lock(mu_);
        log_.clear();
        outcome_.reset();
        undervolt_.reset();
        all_in_one_.reset();
        retest_.reset();
        is_retest_ = retest;
        measuring_.clear();
        busy_question_.clear();
        busy_answer_.reset();
    }
    abort_ = false;
    thread_ = std::jthread([this, job = std::move(job)] {
        app::OptimizeHooks hooks;
        hooks.aborted = [this] { return abort_.load(); };
        hooks.log = app::logged([this](const std::string& line) {
            {
                std::lock_guard lock(mu_);
                log_.push_back(line);
            }
            wake_();
        });
        hooks.measuring = [this](const std::string& what, double seconds) {
            {
                std::lock_guard lock(mu_);
                measuring_ = what;
                measuring_seconds_ = seconds;
                measuring_from_ = std::chrono::steady_clock::now();
            }
            wake_();
        };
        // Other programs use the card: the window shows the question, and
        // the run waits here for its answer. Aborting answers it too.
        hooks.busy = [this](const std::vector<BusyProgram>& programs) {
            std::unique_lock lock(mu_);
            busy_question_ = programs;
            busy_answer_.reset();
            lock.unlock();
            wake_();
            lock.lock();
            busy_cv_.wait(lock, [this] { return busy_answer_.has_value() || abort_.load(); });
            const BusyAnswer answer = busy_answer_.value_or(BusyAnswer::Stop);
            busy_question_.clear();
            busy_answer_.reset();
            return answer;
        };
        job(hooks);
        {
            std::lock_guard lock(mu_);
            measuring_.clear();   // also after a run that ended in an exception
        }
        running_ = false;
        wake_();
    });
    return true;
}

bool OptimizeWorker::start(Preset preset, std::optional<FanCurve> fan_curve) {
    return launch(false, [this, preset, fan_curve](const app::OptimizeHooks& hooks) {
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
        std::lock_guard lock(mu_);
        if (is_all) all_in_one_ = std::move(all);
        else if (is_undervolt) undervolt_ = std::move(undervolt);
        else outcome_ = std::move(outcome);
    });
}

bool OptimizeWorker::start_retest() {
    return launch(true, [this](const app::OptimizeHooks& hooks) {
        app::RetestOutcome retest;
        try {
            retest = app::run_retest(hooks);
        } catch (const std::exception& e) {
            retest.error = std::string("unexpected error: ") + e.what();
        }
        std::lock_guard lock(mu_);
        retest_ = std::move(retest);
    });
}

OptimizeWorker::Snapshot OptimizeWorker::snapshot() const {
    std::lock_guard lock(mu_);
    Snapshot s{running_.load(), log_, outcome_, undervolt_, all_in_one_, retest_, is_retest_};
    s.busy_question = busy_question_;
    if (!measuring_.empty()) {
        s.measuring = measuring_;
        s.measuring_seconds = measuring_seconds_;
        s.measuring_spent = std::chrono::duration<double>(std::chrono::steady_clock::now() - measuring_from_).count();
    }
    return s;
}

}
