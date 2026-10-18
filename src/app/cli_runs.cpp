// The gao.exe commands that load the card: the stress tests, the bandwidth
// measurement and the runs (optimize, undervolt, retest, all in one), with
// the Ctrl+C handling the runs share.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "app/cli_commands.hpp"
#include "app/common.hpp"
#include "core/stability.hpp"
#include "core/stress_math.hpp"
#include "hw/gpu_control.hpp"
#include "hw/nvml.hpp"
#include "hw/stress.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <cstdio>

namespace gao::cli {

using gao::app::kGpu;

// Runs the DX11 stress load for `seconds` of GPU time, printing one line per
// second, then the verdict. Changes no settings, so Ctrl+C is always safe.
// duty_pct < 100: a light load, the card working that share of `seconds` of
// wall time and idle in between (duty_pause_ms); the lines then come per
// second of GPU time.
// Assumes the first NVIDIA DXGI adapter is NVML device kGpu -- true on a
// single-GPU machine, which is all this CLI supports (see kGpu).
int stress(int seconds, int max_temp_c, gao::StressSelftest selftest, int duty_pct) {
    gao::Nvml nvml;
    if (!nvml.Init()) { std::printf("NVML init failed: %s\n", nvml.Error().c_str()); return 1; }
    gao::Stress load;
    if (!load.Init(selftest)) { std::printf("stress init failed: %s\n", load.Error().c_str()); return 1; }
    std::printf("stress: %s, %d s, abort above %d C", load.AdapterName().c_str(), seconds, max_temp_c);
    if (duty_pct < 100) std::printf(", working %d %% of the time", duty_pct);
    std::printf("\n");
    const double gpu_seconds = duty_pct < 100 ? std::max(1.0, seconds * duty_pct / 100.0) : seconds;

    double t = 0, next_print = 1.0;
    long long window_its = 0;
    double window_s = 0;
    auto batch = [&] {
        const gao::StressBatch b = load.Batch();
        if (const int pause = gao::duty_pause_ms(b.elapsed_ms, duty_pct); pause > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(pause));
        t += b.elapsed_ms / 1000.0;
        window_s += b.elapsed_ms / 1000.0;
        window_its += b.iterations;
        return b;
    };
    auto read = [&] {
        const gao::Telemetry tel = nvml.Read(kGpu);
        if (t >= next_print) {
            std::printf("  t=%.0fs  score=%.0f it/s  core=%s MHz  mem=%s MHz  temp=%s C  power=%s/%d W\n", t,
                        window_its / window_s, gao::reading(tel.core_mhz).c_str(), gao::reading(tel.mem_mhz).c_str(),
                        gao::reading(tel.temp_c).c_str(), gao::reading(tel.power_w).c_str(), tel.power_limit_w);
            window_its = 0;
            window_s = 0;
            next_print = t + 1.0;
        }
        return tel;
    };
    const gao::StabilityResult r = gao::run_stability(batch, read, gpu_seconds, max_temp_c);
    std::printf("VERDICT: %s  score=%.0f it/s  %.1f s  peak=%s C  avg power=%s W  avg core=%s MHz  avg mem=%s MHz\n",
                gao::verdict_name(r.verdict), r.score, r.seconds, gao::reading(r.peak_temp_c).c_str(),
                gao::reading(r.avg_power_w).c_str(), gao::reading(r.avg_core_mhz).c_str(),
                gao::reading(r.avg_mem_mhz).c_str());
    switch (r.verdict) {
        case gao::Verdict::Stable: return 0;
        case gao::Verdict::WrongResult:
        case gao::Verdict::DeviceLost:
        case gao::Verdict::Stalled: return 2;
        case gao::Verdict::TooHot:
        case gao::Verdict::NoTelemetry: return 3;
        case gao::Verdict::Aborted: return 1;
    }
    return 1;
}

std::atomic<bool> g_abort{false};
static const gao::GpuControl* g_gpu = nullptr;   // set while --optimize runs

// Ctrl+C / Ctrl+Break do not kill the process during --optimize: they ask
// the search to stop, and the search restores stock before returning.
// Closing the window, logoff and shutdown do kill it (Windows allows ~5 s),
// so those reset to stock right here instead of leaving a candidate applied.
static BOOL WINAPI OnConsoleCtrl(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) {
        g_abort = true;
        std::printf("\nabort requested -- stopping the probe; the card goes back to stock, or to the tune it had before the run\n");
        return TRUE;
    }
    if (type == CTRL_CLOSE_EVENT || type == CTRL_LOGOFF_EVENT || type == CTRL_SHUTDOWN_EVENT) {
        g_abort = true;
        if (g_gpu && g_gpu->reset_to_stock) g_gpu->reset_to_stock();
        if (g_gpu && g_gpu->set_fan_auto) g_gpu->set_fan_auto();
        return TRUE;
    }
    return FALSE;
}

// --busy-ok, as the last argument of a run: other programs may stay on the card.
bool g_busy_ok = false;

// Other programs use the card: the run says which, and does not start unless
// it was told that this is fine.
static gao::BusyAnswer on_busy(const std::vector<gao::BusyProgram>& programs) {
    if (g_busy_ok) return gao::BusyAnswer::GoOn;
    std::printf("Other programs are using the graphics card: %s.\n"
                "A run measures the card under full load, and what they take from it makes every number lower and less\n"
                "steady: the result, and its comparison with stock. Close them and start again, or add --busy-ok as the\n"
                "last argument to go on as it is.\n", gao::busy_text(programs).c_str());
    return gao::BusyAnswer::Stop;
}

int optimize(gao::Preset preset, const std::optional<gao::FanCurve>& fan_curve) {
    gao::app::OptimizeHooks hooks;
    hooks.aborted = [] { return g_abort.load(); };
    hooks.log = gao::app::logged([](const std::string& msg) { std::printf("  %s\n", msg.c_str()); });
    hooks.busy = on_busy;
    hooks.active_gpu = [](const gao::GpuControl* gpu) {
        g_gpu = gpu;
        SetConsoleCtrlHandler(OnConsoleCtrl, gpu ? TRUE : FALSE);
    };
    const auto out = gao::app::run_optimize(preset, hooks, fan_curve);
    // Through the log: on screen and in run.log alike.
    for (const std::string& line : gao::app::result_lines(out)) hooks.log(line);
    if (!out.ran || !out.result.ok) return 1;
    if (out.saved) {
        std::printf("Saved: `gao --apply` re-applies it, `gao --boot on` applies it at every logon.\n");
        if (const std::string note = gao::app::update_logon_copy(); !note.empty()) std::printf("%s\n", note.c_str());
    }
    std::printf("Applied until reboot. `gao --reset` returns to stock.\n");
    return out.saved ? 0 : 2;   // 2: tuned and applied, but --apply and boot-apply cannot use it
}

// Finds the undervolt, leaves it applied and saves it as the profile; on_overclock: on top of the saved tune, into it.
int undervolt(bool on_overclock) {
    gao::app::OptimizeHooks hooks;
    hooks.aborted = [] { return g_abort.load(); };
    hooks.log = gao::app::logged([](const std::string& msg) { std::printf("  %s\n", msg.c_str()); });
    hooks.busy = on_busy;
    hooks.active_gpu = [](const gao::GpuControl* gpu) {
        g_gpu = gpu;
        SetConsoleCtrlHandler(OnConsoleCtrl, gpu ? TRUE : FALSE);
    };
    // A running tray app holds off for the whole run (the tuning lock), as
    // for --optimize, and afterwards finds the profile this run saved.
    const auto out = gao::app::run_undervolt(hooks, on_overclock);
    for (const std::string& line : gao::app::result_lines(out)) hooks.log(line);
    if (!out.ran || !out.result.ok) return 1;
    if (out.saved) {
        std::printf("Saved: `gao --apply` re-applies it, `gao --boot on` applies it at every logon.\n");
        if (const std::string note = gao::app::update_logon_copy(); !note.empty()) std::printf("%s\n", note.c_str());
    }
    std::printf("Applied until reboot. `gao --reset` returns to stock.\n");
    return out.saved ? 0 : 2;   // 2: found and applied, but --apply and boot-apply cannot use it
}

// The saved tune, tested again on the driver that is installed now.
int retest() {
    gao::app::OptimizeHooks hooks;
    hooks.aborted = [] { return g_abort.load(); };
    hooks.log = gao::app::logged([](const std::string& msg) { std::printf("  %s\n", msg.c_str()); });
    hooks.busy = on_busy;
    hooks.active_gpu = [](const gao::GpuControl* gpu) {
        g_gpu = gpu;
        SetConsoleCtrlHandler(OnConsoleCtrl, gpu ? TRUE : FALSE);
    };
    const gao::app::RetestOutcome out = gao::app::run_retest(hooks);
    for (const std::string& line : gao::app::result_lines(out)) hooks.log(line);
    return out.ok ? 0 : 1;
}

// The all-in-one run: stock, overclock, undervolt on top, fans, final test.
int all_in_one() {
    gao::app::OptimizeHooks hooks;
    hooks.aborted = [] { return g_abort.load(); };
    hooks.log = gao::app::logged([](const std::string& msg) { std::printf("  %s\n", msg.c_str()); });
    hooks.busy = on_busy;
    hooks.active_gpu = [](const gao::GpuControl* gpu) {
        g_gpu = gpu;
        SetConsoleCtrlHandler(OnConsoleCtrl, gpu ? TRUE : FALSE);
    };
    // The measuring steps have no clock candidate applied, but they run for
    // minutes: Ctrl+C must stop them too, and end at stock.
    SetConsoleCtrlHandler(OnConsoleCtrl, TRUE);
    const gao::app::AllInOneOutcome out = gao::app::run_all_in_one(hooks);
    for (const std::string& line : gao::app::result_lines(out)) hooks.log(line);
    if (!out.ok) return 1;
    std::printf("Saved: `gao --apply` re-applies it, `gao --boot on` applies it at every logon.\n");
    if (const std::string note = gao::app::update_logon_copy(); !note.empty()) std::printf("%s\n", note.c_str());
    std::printf("Applied until reboot. `gao --reset` returns to stock.\n");
    return 0;
}

// The memory bandwidth as a run measures it, and what the measurement looked
// like. after_load_s: that many seconds of the stress load first, on the same
// device, as the memory search measures right after a probe: the card is then
// warm and at whatever limit holds it back. Changes no setting.
int bandwidth(int after_load_s) {
    gao::Stress load;
    if (!load.Init()) { std::printf("stress init failed: %s\n", load.Error().c_str()); return 1; }
    gao::Nvml nvml;
    const bool telemetry = nvml.Init();
    auto card = [&](const char* when) {
        if (!telemetry) return;
        const gao::Telemetry t = nvml.Read(kGpu);
        std::printf("  %s: core=%s MHz  mem=%s MHz  temp=%s C  power=%s/%d W\n", when, gao::reading(t.core_mhz).c_str(),
                    gao::reading(t.mem_mhz).c_str(), gao::reading(t.temp_c).c_str(), gao::reading(t.power_w).c_str(), t.power_limit_w);
    };
    if (after_load_s > 0) {
        if (!telemetry) { std::printf("NVML init failed: %s\n", nvml.Error().c_str()); return 1; }
        const gao::StabilityResult r = gao::run_stability([&] { return load.Batch(); }, [&] { return nvml.Read(kGpu); }, after_load_s, 85);
        std::printf("  load: %s  score=%.0f it/s  %.1f s  peak=%s C  avg power=%s W  avg core=%s MHz\n", gao::verdict_name(r.verdict), r.score,
                    r.seconds, gao::reading(r.peak_temp_c).c_str(), gao::reading(r.avg_power_w).c_str(), gao::reading(r.avg_core_mhz).c_str());
        if (r.verdict != gao::Verdict::Stable) return 1;
    }
    const auto gbps = load.MeasureBandwidth();
    card("after the measurement");
    std::string each;
    for (const double run : load.BandwidthRuns()) {
        char one[16] = " -";   // a run that was thrown away
        if (run > 0) std::snprintf(one, sizeof(one), " %.1f", run);
        each += one;
    }
    std::printf("  runs: %zu, %s\n  each:%s\n", load.BandwidthRuns().size(), gao::bandwidth_runs_text(load.BandwidthRuns()).c_str(), each.c_str());
    if (!gbps) { std::printf("bandwidth measurement failed: %s\n", load.Error().c_str()); return 1; }
    std::printf("memory bandwidth: %.1f GB/s (%s)\n", *gbps, load.AdapterName().c_str());
    return 0;
}

// Measures what the optimize run assumes after a driver reset: how long the
// stress load takes to rebuild its device, and what the first batch on it
// costs. Changes no settings and needs no elevation, but it does load the card
// (two 3 s runs). Exit 0 when both runs are STABLE, the rebuild succeeded and
// the first batch after it computed without a lost device or a wrong value,
// 1 otherwise -- not --stress's 2 / 3 codes.
int stress_recreate() {
    gao::Nvml nvml;
    if (!nvml.Init()) { std::printf("NVML init failed: %s\n", nvml.Error().c_str()); return 1; }
    gao::Stress load;
    if (!load.Init()) { std::printf("stress init failed: %s\n", load.Error().c_str()); return 1; }
    std::printf("stress-recreate: %s\n", load.AdapterName().c_str());

    const auto run = [&](const char* label) {
        const gao::StabilityResult r = gao::run_stability([&] { return load.Batch(); },
                                                          [&] { return nvml.Read(kGpu); }, 3, 85);
        std::printf("%s: %s  score=%.0f it/s\n", label, gao::verdict_name(r.verdict), r.score);
        return r.verdict == gao::Verdict::Stable;
    };
    const bool first_ok = run("before");

    const auto t0 = std::chrono::steady_clock::now();
    const bool recreated = load.Recreate();
    const double recreate_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (!recreated) {
        std::printf("recreate: failed after %.1f ms: %s\n", recreate_ms, load.Error().c_str());
        return 1;
    }
    std::printf("recreate: ok, %.1f ms\n", recreate_ms);

    const gao::StressBatch b = load.Batch();
    // A batch that lost the device or computed wrong has no time worth printing.
    const bool batch_ok = !b.device_lost && b.wrong_values == 0;
    if (b.device_lost) std::printf("first batch: DEVICE LOST\n");
    else if (b.wrong_values != 0) std::printf("first batch: %d wrong values\n", b.wrong_values);
    else std::printf("first batch: %.1f ms, %lld iterations\n", b.elapsed_ms, b.iterations);

    const bool second_ok = run("after");
    return first_ok && batch_ok && second_ok ? 0 : 1;
}

}  // namespace gao::cli
