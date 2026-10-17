#include "app/gui/worker.hpp"
#include <algorithm>
#include <cstdio>
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

bool OptimizeWorker::launch(bool retest, bool puts_back, std::function<void(const app::OptimizeHooks&)> job) {
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
        put_back_ = false;
    }
    abort_ = false;
    thread_ = std::jthread([this, puts_back, job = std::move(job)] {
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
        // What a Stop puts back: read before the run touches the card, and
        // put back here, before running() turns false and the watchdog
        // looks again.
        const std::optional<Profile> before = puts_back ? app::tune_before_run() : std::nullopt;
        job(hooks);
        if (abort_.load() && !snapshot().kept() && app::put_back_after_stop(before, hooks)) {
            std::lock_guard lock(mu_);
            put_back_ = true;
        }
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
    return launch(false, true, [this, preset, fan_curve](const app::OptimizeHooks& hooks) {
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
        // The end of the run into its log as well: the window shows it from the
        // outcome, run.log only has what went through the log.
        const std::vector<std::string> end = is_all ? app::result_lines(all)
                                            : is_undervolt ? app::result_lines(undervolt)
                                                           : app::result_lines(outcome);
        for (const std::string& line : end) hooks.log(line);
        std::lock_guard lock(mu_);
        if (is_all) all_in_one_ = std::move(all);
        else if (is_undervolt) undervolt_ = std::move(undervolt);
        else outcome_ = std::move(outcome);
    });
}

bool OptimizeWorker::start_demo(bool ask) {
    return launch(false, false, [this, ask](const app::OptimizeHooks& hooks) {
        app::OptimizeOutcome out;
        const auto stopped = [&] { return hooks.aborted(); };
        // Waits in short slices, so Abort and closing the window end it at once.
        const auto wait = [&](double seconds) {
            for (double t = 0; t < seconds && !stopped(); t += 0.05) std::this_thread::sleep_for(std::chrono::milliseconds(50));
            return !stopped();
        };
        // One step with its countdown, then the line it ends with.
        const auto step = [&](const char* what, double seconds, const std::string& line) {
            hooks.measuring(what, seconds);
            const bool done = wait(seconds);
            hooks.measuring("", 0);
            if (done) hooks.log(line);
            return done;
        };
        const auto finish = [&] {
            std::lock_guard lock(mu_);
            outcome_ = std::move(out);
        };
        hooks.log("demo: this run is played. Nothing is measured, and nothing on the card or on disk is changed.");

        // The question about other programs: fewer of them every time it is asked again.
        std::vector<std::vector<BusyProgram>> looks = {{{"chrome", 11.7, true}, {"Trae", 2.2, false}}, {{"Trae", 2.2, false}}, {}};
        std::vector<BusyProgram> at_start;
        for (size_t look = ask ? 0 : looks.size() - 1;; look = std::min(look + 1, looks.size() - 1)) {
            if (!step("Looking at what else uses the card", 3, "other programs on the card: " + busy_text(looks[look]))) break;
            at_start = looks[look];
            if (at_start.empty()) break;
            const BusyAnswer answer = hooks.busy(at_start);
            if (answer == BusyAnswer::Stop) {
                out.error = "other programs are using the graphics card: " + busy_text(at_start);
                return finish();
            }
            if (answer == BusyAnswer::GoOn) {
                hooks.log("going on with them: the run will measure lower and less steadily");
                break;
            }
        }

        StabilityResult stock;
        stock.score = 5311;
        stock.seconds = 300;
        stock.peak_temp_c = 66;
        stock.end_fan_pct = 70;
        stock.avg_power_w = 185;
        stock.avg_core_mhz = 2793;
        stock.avg_mem_mhz = 10501;
        bool ok = step("Measuring the card at stock", 12, "  stock: STABLE  score=5311 it/s  clock=2793 MHz  peak=66 C  fan=70 %  power=185 W") &&
                  step("The baseline", 4, "baseline: STABLE  score=5259 it/s  peak=66 C  power=179 W") &&
                  step("The power limit", 5, "power: 105 %");
        for (int mem = 0; ok && mem <= 600; mem += 50) {
            char line[96];
            std::snprintf(line, sizeof(line), "core +0 / mem +%d: STABLE  score=5330 it/s  peak=67 C  power=175 W", mem);
            ok = step("The memory clock", 0.6, line);
        }
        ok = ok && step("Confirming the memory clock", 4, "mem: bandwidth peak +2200, confirmed +2200, applying +1500");
        for (int core = 15; ok && core <= 210; core += 15) {
            char line[96];
            std::snprintf(line, sizeof(line), "core +%d / mem +1500: STABLE  score=%d it/s  peak=68 C  power=189 W", core, 5330 + core * 2);
            ok = step("The core clock", 0.5, line);
        }
        ok = ok && step("Confirming the core clock", 4, "core: highest stable +210, confirmed +210, applying +135") &&
             step("The result under full load", 15, "soak: STABLE  score=5555 it/s  peak=69 C  power=196 W");
        if (!ok) {
            out.error = "aborted";
            return finish();
        }
        out.ran = true;
        out.stock = stock;
        out.result.ok = true;
        out.result.stock_restored = true;
        out.result.power_pct = 105;
        out.result.core_mhz = 135;
        out.result.core_max_stable = out.result.core_confirmed = 210;
        out.result.mem_mhz = 1500;
        out.result.mem_max_stable = out.result.mem_confirmed = 2200;
        out.result.baseline = stock;
        out.result.baseline.score = 5259;
        out.result.baseline.seconds = 30;
        out.result.soak = stock;
        out.result.soak.score = 5555;
        out.result.soak.peak_temp_c = 69;
        out.result.soak.end_fan_pct = 62;
        out.result.soak.avg_power_w = 196;
        out.result.soak.avg_core_mhz = 2928;
        out.result.soak.avg_mem_mhz = 12001;
        out.saved = false;
        out.save_note = "this was a demo. Nothing was measured, applied or saved.";
        // What the second look of a real run says when a program went away under it.
        if (!at_start.empty()) out.busy_note = busy_change(at_start, {});
        finish();
    });
}

bool OptimizeWorker::start_retest() {
    return launch(true, true, [this](const app::OptimizeHooks& hooks) {
        app::RetestOutcome retest;
        try {
            retest = app::run_retest(hooks);
        } catch (const std::exception& e) {
            retest.error = std::string("unexpected error: ") + e.what();
        }
        for (const std::string& line : app::result_lines(retest)) hooks.log(line);
        std::lock_guard lock(mu_);
        retest_ = std::move(retest);
    });
}

OptimizeWorker::Snapshot OptimizeWorker::snapshot() const {
    std::lock_guard lock(mu_);
    Snapshot s{running_.load(), log_, outcome_, undervolt_, all_in_one_, retest_, is_retest_};
    s.busy_question = busy_question_;
    s.put_back = put_back_;
    if (!measuring_.empty()) {
        s.measuring = measuring_;
        s.measuring_seconds = measuring_seconds_;
        s.measuring_spent = std::chrono::duration<double>(std::chrono::steady_clock::now() - measuring_from_).count();
    }
    return s;
}

}
