#include "app/common_internal.hpp"
#include <tlhelp32.h>
#include "core/watchdog.hpp"
#include "hw/gpu_users.hpp"
#include "hw/nvidia_app.hpp"
#include "hw/nvml.hpp"
#include "hw/stress.hpp"
#include <chrono>
#include <cstdio>
#include <memory>

namespace gao::app {

MeasuredRun measured_run(const StabilityResult& r) {
    return {r.score, r.avg_core_mhz, r.avg_mem_mhz, r.peak_temp_c, r.end_fan_pct, r.avg_power_w};
}

// Both runs, or nothing: a comparison with half its numbers says nothing.
std::optional<AgainstStock> against_stock(const StabilityResult& stock, const StabilityResult& tuned) {
    if (stock.score <= 0 || tuned.score <= 0) return std::nullopt;
    return AgainstStock{measured_run(stock), measured_run(tuned)};
}

namespace {

// After a measurement at stock that the temperature stopped: without load,
// this far below it before the run loads the card again, or this long. The
// steps that follow have limits of their own.
constexpr int kCoolDownC = 15;
constexpr int kCoolDownMaxS = 60;

std::string stock_line(const StabilityResult& s) {
    char line[400];
    std::snprintf(line, sizeof(line), "  stock: %s  score=%.0f it/s  clock=%d MHz  peak=%d C  fan=%s  power=%d W", verdict_name(s.verdict),
                  s.score, s.avg_core_mhz, s.peak_temp_c, reading(s.end_fan_pct, " %").c_str(), s.avg_power_w);
    return line;
}
}

bool is_refusal(const std::string& reason) {
    return reason == kSeveralGpusRefusal || reason == kAutoTuningRefusal || reason == kAlreadyRunning ||
           reason.ends_with(kOtherToolRefusalTail);
}

// Why no run starts on this PC (core/boot.hpp: several_gpus); empty when it
// may. A failed count reads as one card: the run's own NVML init reports it.
std::string several_gpus_refusal() {
    Nvml nvml;
    if (!nvml.Init()) return {};
    return several_gpus(nvml.DeviceCount()) ? kSeveralGpusRefusal : std::string();
}

// Why no run starts next to another tuning tool (core/search.hpp); empty when
// none runs. A list of programs that cannot be had reads as none: this check
// is a courtesy, the run's own verifications are what it relies on.
std::string other_tool() {
    std::vector<std::string> running;
    const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return {};
    PROCESSENTRY32W e{};
    e.dwSize = sizeof(e);
    for (BOOL more = Process32FirstW(snap, &e); more; more = Process32NextW(snap, &e)) {
        std::string name;   // the names looked for are ASCII; anything else cannot match
        for (const wchar_t* c = e.szExeFile; *c; ++c) name += *c < 128 ? static_cast<char>(*c) : '?';
        running.push_back(std::move(name));
    }
    CloseHandle(snap);
    if (const std::string tool = other_tool_refusal(running); !tool.empty()) return tool;
    // The NVIDIA app may run; its Automatic Tuning may not be on. Unknown
    // (nothing documents where it is kept) counts as off.
    if (nvidia_auto_tuning().value_or(false)) return kAutoTuningRefusal;
    return {};
}

// The look at what else uses the card, and the question about it, before a
// run (core/busy_card.hpp). A run that is one step of a longer one does not
// ask again.
BusyGate busy_before_run(const OptimizeHooks& hooks) {
    if (hooks.busy_asked) return {};
    return busy_gate(
        [&] {
            if (hooks.measuring) hooks.measuring("Looking at what else uses the card", kBusyLookS);
            auto use = gpu_use(kBusyLookS);
            if (hooks.measuring) hooks.measuring("", 0);
            return use;
        },
        hooks.busy, [&](const std::string& m) { if (hooks.log) hooks.log(m); });
}

StockOutcome measure_stock(const OptimizeHooks& hooks) {
    StockOutcome out;
    auto fail = [&](const std::string& why) { out.error = why; return out; };
    const TuningLock lock;
    if (!lock.owned()) return fail(kAlreadyRunning);
    std::string why;
    if (!prepare_state(&why)) return fail(why);
    GuardedGpu hw(kGpu);
    if (!hw.Init(&why)) return fail(why);
    auto load = std::make_unique<Stress>();
    if (!load->Init()) return fail("stress init failed: " + load->Error());
    const GpuControl& gpu = hw.control();
    // The emergency handlers for the whole measurement, as during the search:
    // without them Ctrl+C on the command line ends the process here, and the
    // tune the run found on the card is not put back.
    if (hooks.active_gpu) hooks.active_gpu(&gpu);
    struct Unregister {
        const OptimizeHooks& h;
        ~Unregister() { if (h.active_gpu) h.active_gpu(nullptr); }
    } unregister{hooks};
    if (!gpu.reset_to_stock || !gpu.reset_to_stock()) return fail("could not set the card to stock");
    const KeepAwake keep_awake;
    auto log = [&](const std::string& m) { if (hooks.log) hooks.log(m); };
    // In every run's log, whatever the profile: a target that another tool
    // left on the card makes this a measurement of something else than stock.
    log(temp_target_text(gpu.temp_target_c, gpu.temp_target_default_c));
    if (hooks.measuring) hooks.measuring("Measuring the card at stock", kLongRunS);
    // What the card did meanwhile, a line every ten seconds: the readings the
    // measurement takes anyway, written down. Nothing of the measurement changes.
    TraceClock trace;
    const auto began = std::chrono::steady_clock::now();
    std::optional<std::pair<double, Telemetry>> unlogged;   // the latest reading that got no line
    const bool crashed = !guarded([&] {
        out.result = run_stability(
            [&] { return load->Batch(); },
            [&] {
                const Telemetry t = gpu.read();
                const double at = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
                if (t.ok && trace.due(at)) {
                    log(trace_line(at, t));
                    unlogged.reset();
                } else if (t.ok) {
                    unlogged = std::make_pair(at, t);
                }
                return t;
            },
            kLongRunS, kStockTempC, hooks.aborted);
    });
    if (hooks.measuring) hooks.measuring("", 0);
    if (crashed) {
        (void)load.release();   // as in run_optimize: a load that faulted is left alone
        return fail("access violation while the card was measured at stock");
    }
    if (unlogged) log(trace_line(unlogged->first, unlogged->second));   // how it ended, when that fell between two lines
    log(stock_line(out.result));
    out.error = stock_refusal(out.result);
    if (!out.error.empty() || out.result.verdict != Verdict::TooHot) return out;
    // The driver's own fan control let the card reach the limit. That is the
    // "before"; the run goes on, with the fans on a curve from here.
    log("  the card reached " + std::to_string(out.result.peak_temp_c) + " C at stock with the driver's own fan control (fans at " +
        reading(out.result.end_fan_pct, " %") + "): the measurement stopped there, after " +
        std::to_string(static_cast<int>(out.result.seconds)) + " s. The run goes on with the fans on a curve.");
    if (hooks.measuring) hooks.measuring("Letting the card cool down", kCoolDownMaxS);
    for (int i = 0; i < kCoolDownMaxS * 4 && !(hooks.aborted && hooks.aborted()); ++i) {
        const Telemetry t = gpu.read();
        if (!t.ok || t.temp_c <= kStockTempC - kCoolDownC) break;
        Sleep(250);
    }
    if (hooks.measuring) hooks.measuring("", 0);
    return out;
}

// One reading of a run, with the fans moved along their curve.
Telemetry read_with_fans(const GpuControl& gpu, FanDriver& fans, const OptimizeHooks& hooks) {
    const Telemetry t = gpu.read();
    const FanMode before = fans.state().mode;
    const FanState now = fans.tick(t.temp_c, t.power_w, std::chrono::steady_clock::now(), t.power_limit_w);
    if (now.mode != before && now.mode == FanMode::Failed && hooks.log) hooks.log("fans: a write did not verify; the driver has them again");
    if (now.mode != before && now.mode == FanMode::Foreign && hooks.log) hooks.log("fans: another program set them; leaving them alone");
    return t;
}

std::vector<BusyProgram> busy_now() { return busy_programs(gpu_use(kBusyLookS)); }

std::optional<Profile> tune_before_run() {
    const Config cfg = load_config();
    if (!cfg.profile) return std::nullopt;
    GuardedGpu hw(kGpu);
    std::string why;
    if (!hw.Init(&why) || !tune_on_card(hw.control(), *cfg.profile)) return std::nullopt;
    return cfg.profile;
}

bool put_back_after_stop(const std::optional<Profile>& before, const OptimizeHooks& hooks) {
    if (!before) return false;
    auto log = [&](const std::string& m) { if (hooks.log) hooks.log(m); };
    const TuningLock lock;   // the watchdog keeps out of it meanwhile
    GuardedGpu hw(kGpu);
    std::string why;
    if (!hw.Init(&why)) {
        log("stopped: the tune that was on the card could not be put back (" + why + ")");
        return false;
    }
    const GpuControl& gpu = hw.control();
    if (tune_on_card(gpu, *before)) return true;   // stopped before the card was touched
    bool clean = false;
    if (!apply_profile(gpu, *before, &why, &clean)) {
        log("stopped: the tune that was on the card could not be put back (" + why + ") -- " +
            (clean ? "card at stock" : "reset to stock FAILED, run `gao --reset`"));
        return false;
    }
    log("stopped: the tune that was on the card before the run is back: " + profile_text(*before));
    return true;
}

}
