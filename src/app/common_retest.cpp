#include "app/common_internal.hpp"
#include "hw/app_files.hpp"
#include "hw/stress.hpp"
#include <chrono>
#include <memory>

namespace gao::app {

RetestOutcome run_retest(const OptimizeHooks& hooks) {
    RetestOutcome out;
    auto fail = [&](const std::string& why) { out.error = why; return out; };
    if (const std::string gpus = several_gpus_refusal(); !gpus.empty()) return fail(gpus);
    if (const std::string tool = other_tool(); !tool.empty()) return fail(tool);
    if (!is_elevated()) return fail("testing a tune sets clocks and power limits and needs administrator rights");
    const TuningLock lock;
    if (!lock.owned()) return fail("another optimize is already running (in the app or on the command line)");
    std::string why;
    if (!prepare_state(&why)) return fail(why);
    if (const BusyGate busy = busy_before_run(hooks); !busy.refusal.empty()) return fail(busy.refusal);
    GuardedGpu hw(kGpu);
    if (!hw.Init(&why)) return fail(why);
    auto load = std::make_unique<Stress>();
    if (!load->Init()) return fail("stress init failed: " + load->Error());
    const GpuControl& gpu = hw.control();
    Config cfg = load_config();
    if (!cfg.profile) return fail("there is no saved tune to test");
    out.driver_was = cfg.profile->driver;
    out.driver = hw.DriverVersion();
    const std::string gpu_id = hw.GpuUuid();
    const int limit = profile_ceiling(*cfg.profile);
    // Every way out: the fans go back to the driver. The app's own fan curve
    // driver takes them from there.
    struct FansBack {
        const GpuControl& gpu;
        ~FansBack() { if (gpu.set_fan_auto) gpu.set_fan_auto(); }
    } fans_back{gpu};
    const KeepAwake keep_awake;
    // The fans as the tune will be used with: the app's curve when it drives
    // them, the NVIDIA driver's own otherwise.
    std::optional<FanDriver> fans;
    if (cfg.fan_control && gpu.set_fan_pct)
        fans.emplace(gpu, active_fan_curve(cfg).value_or(default_curve(cfg.profile->preset)), limit,
                     fan_min_for(cfg, gpu_id, gpu.fan_min_pct));
    RetestIo io;
    io.probe = [&](double seconds, int max_temp, double stall_below) {
        auto read = [&] {
            const Telemetry t = gpu.read();
            if (fans) fans->tick(t.temp_c, t.power_w, std::chrono::steady_clock::now(), t.power_limit_w);
            return t;
        };
        return run_stability([&] { return load->Batch(); }, read, seconds, max_temp, hooks.aborted, stall_below);
    };
    io.log = hooks.log;
    io.measuring = hooks.measuring;
    if (hooks.active_gpu) hooks.active_gpu(&gpu);
    out.ran = true;
    const bool crashed = !guarded([&] {
        out.result = retest_saved_tune(cfg, out.driver, gpu_id, gpu, io);
        if (fans) fans->release();
    });
    if (hooks.active_gpu) hooks.active_gpu(nullptr);
    auto to_stock = [&](const std::string& reason) {
        const bool stock = gpu.reset_to_stock && gpu.reset_to_stock();
        return fail(reason + (stock ? " -- card at stock" : " -- reset to stock FAILED, run `gao --reset`"));
    };
    if (crashed) {
        if (hooks.measuring) hooks.measuring("", 0);
        (void)load.release();   // as in run_optimize: a load that faulted is left alone
        return to_stock("access violation while the tune was tested");
    }
    if (!out.result.ok) {
        if (out.result.test.seconds > 0)
            boot_log("the saved tune was tested on driver " + out.driver + " and is not applied: " + out.result.reason);
        return fail(out.result.reason + (out.result.at_stock ? " -- card at stock" : " -- reset to stock FAILED, run `gao --reset`"));
    }
    // Only the driver the tune belongs to, and the strikes, are this test's to change.
    Config latest = load_config();
    if (!latest.profile) return to_stock("the saved tune was removed while it was tested");
    latest.profile->driver = out.driver;
    latest.boot_strikes = 0;
    if (!save_config(latest)) return to_stock("the tune held, but that could not be saved (" + config_path().string() + ")");
    out.ok = true;
    out.profile = *latest.profile;
    boot_log("tested again on driver " + out.driver + " (it was tested on " + out.driver_was + "): applied " + profile_text(out.profile));
    return out;
}

}
