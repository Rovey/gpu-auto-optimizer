#include "app/common_internal.hpp"
#include "hw/app_files.hpp"
#include "hw/stress.hpp"
#include <chrono>
#include <cstdio>
#include <memory>

namespace gao::app {

namespace {

// The card with the saved tune under load while its fans are tuned: a
// measuring step of the all-in-one run. It brings its own drivers and stress
// load, like the searches and like measure_stock (common_run_steps.cpp).
struct FanStepOutcome {
    std::string error;       // why the step ended without a result; the card is at stock then
    FanTuneResult fan;
    StabilityResult final;   // the five minutes with everything applied; seconds 0 when not run
    std::string note;        // what was kept instead, when the tuned curve was not
};

// Steps 4 and 5: the saved tune applied, the fans tuned under load, then five
// minutes with the fans on the tuned curve. Saves the curve into the profile
// when that run is stable. A stop request ends at stock.
FanStepOutcome fan_step(const OptimizeHooks& hooks, int start_pct) {
    FanStepOutcome out;
    auto log = [&](const std::string& m) { if (hooks.log) hooks.log(m); };
    auto fail = [&](const std::string& why) { out.error = why; return out; };
    const TuningLock lock;
    if (!lock.owned()) return fail("another optimize is already running (in the app or on the command line)");
    std::string why;
    if (!prepare_state(&why)) return fail(why);
    GuardedGpu hw(kGpu);
    if (!hw.Init(&why)) return fail(why);
    auto load = std::make_unique<Stress>();
    if (!load->Init()) return fail("stress init failed: " + load->Error());
    const GpuControl& gpu = hw.control();
    Config cfg = load_config();
    if (!cfg.profile) return fail("there is no saved tune to tune the fans for");
    const Profile profile = *cfg.profile;
    const int limit = profile_ceiling(profile);
    const int min_pct = fan_min_for(cfg, hw.GpuUuid(), gpu.fan_min_pct);
    // Every way out: the fans go back to the driver. The app's own fan curve
    // driver takes them from there.
    struct FansBack {
        const GpuControl& gpu;
        ~FansBack() { if (gpu.set_fan_auto) gpu.set_fan_auto(); }
    } fans_back{gpu};
    auto to_stock = [&](const std::string& reason) {
        const bool stock = gpu.reset_to_stock && gpu.reset_to_stock();
        return fail(reason + (stock ? " -- card at stock" : " -- reset to stock FAILED, run `gao --reset`"));
    };
    if (!apply_profile(gpu, profile, &why)) return fail("the tune could not be applied for the fan tune: " + why);

    const KeepAwake keep_awake;
    std::optional<FanDriver> fans;   // drives the curve under test during the final run
    const Probe probe = [&](double seconds, int max_temp, double stall_below) {
        auto read = [&] {
            const Telemetry t = gpu.read();
            if (fans) fans->tick(t.temp_c, t.power_w, std::chrono::steady_clock::now(), t.power_limit_w);
            return t;
        };
        return run_stability([&] { return load->Batch(); }, read, seconds, max_temp, hooks.aborted, stall_below);
    };
    FanTuneIo io;
    io.probe = probe;
    io.aborted = hooks.aborted;
    io.log = hooks.log;
    io.measuring = hooks.measuring;
    const bool crashed = !guarded([&] {
        out.fan = tune_fan(gpu, io, limit, start_pct, min_pct);
        if (hooks.aborted && hooks.aborted()) return;
        // The tuned curve, or the profile's own when the tune gave none.
        const FanCurve curve = out.fan.ok ? out.fan.curve : active_fan_curve(cfg).value_or(default_curve(profile.preset));
        // The tune left the fans on a speed set by hand. The curve driver
        // would take a speed it did not set itself for another program's and
        // step aside: it starts from driver control.
        if (gpu.set_fan_auto) gpu.set_fan_auto();
        if (gpu.set_fan_pct) fans.emplace(gpu, curve, limit, min_pct);
        log("  final test: " + std::to_string(static_cast<int>(kLongRunS)) + " s with everything applied");
        if (hooks.measuring) hooks.measuring("Final test: everything together", kLongRunS);
        out.final = probe(kLongRunS, limit, 0.0);
        if (hooks.measuring) hooks.measuring("", 0);
        if (fans) fans->release();
        fans.reset();
    });
    if (crashed) {
        (void)load.release();
        return to_stock("access violation while the fans were tuned");
    }
    if ((hooks.aborted && hooks.aborted()) || out.final.verdict == Verdict::Aborted) return to_stock("aborted");
    if (out.final.seconds > 0 && out.final.verdict != Verdict::Stable) {
        // With the fans this quiet (or at all) the whole did not hold five
        // minutes. The tune passed its own soaks with the fans it had then:
        // it stays, the tuned curve does not. After a driver reset the card
        // is at stock and the tune is put back.
        out.note = std::string("the final test ended ") + verdict_name(out.final.verdict) + "; the tuned fan curve is not kept";
        log("  " + out.note);
        out.fan.ok = false;
        const bool lost = out.final.verdict == Verdict::DeviceLost || out.final.verdict == Verdict::Stalled ||
                          out.final.verdict == Verdict::NoTelemetry;
        if (lost && !(gpu.recover && gpu.recover() && apply_profile(gpu, profile, &why)))
            return to_stock("the tune could not be put back after a driver reset in the final test");
        return out;
    }
    if (!out.fan.ok) {
        out.note = "the fans were not tuned (" + out.fan.reason + "); the profile's own curve is kept";
        return out;
    }
    cfg = load_config();   // only the curve is this step's to change
    if (cfg.profile) {
        cfg.profile->fan_curve = out.fan.curve;
        cfg.fan_curve.reset();
        cfg.fan_control = true;
        if (!save_config(cfg)) out.note = "the tuned fan curve could not be saved";
    }
    return out;
}

}

AllInOneOutcome run_all_in_one(const OptimizeHooks& outer) {
    AllInOneOutcome out;
    auto log = [&](const std::string& m) { if (outer.log) outer.log(m); };
    auto stop = [&](const std::string& why) { out.error = why; return out; };
    auto aborted = [&] { return outer.aborted && outer.aborted(); };
    if (const std::string gpus = several_gpus_refusal(); !gpus.empty()) return stop(gpus);
    if (const std::string tool = other_tool(); !tool.empty()) return stop(tool);
    if (!is_elevated()) return stop("tuning changes clocks, the voltage/frequency curve and the fans and needs administrator rights");
    // Held across the steps (each takes it again, on this thread), so that
    // the watchdog does not put a half-made tune back in between.
    const TuningLock lock;
    if (!lock.owned()) return stop("another optimize is already running (in the app or on the command line)");
    if (const BusyGate busy = busy_before_run(outer); !busy.refusal.empty()) return stop(busy.refusal);
    OptimizeHooks hooks = outer;   // for the steps: the question about other programs has been asked
    hooks.busy_asked = true;

    log("step 1 of 5: the card at stock, five minutes");
    const StockOutcome stock = measure_stock(hooks);
    out.stock = stock.result;
    if (!stock.error.empty()) return stop(stock.error);
    out.ran = true;

    log("step 2 of 5: the overclock");
    out.overclock = run_optimize(Preset::AllInOne, hooks, {}, out.stock);
    if (!out.overclock.ran) return stop(out.overclock.error);
    if (!out.overclock.result.ok) return stop("the overclock: " + out.overclock.result.reason);
    if (!out.overclock.saved) return stop("the overclock could not be saved: " + out.overclock.save_note);
    out.now = out.overclock.result.soak;

    log("step 3 of 5: the undervolt on top of it, at a clock between stock and the overclock");
    UndervoltOnTune how;
    how.measured = out.overclock.result.soak;
    how.save_as = Preset::AllInOne;
    const int stock_mhz = out.stock.avg_core_mhz, oc_mhz = out.overclock.result.soak.avg_core_mhz;
    if (oc_mhz > stock_mhz && stock_mhz > 0) how.clock_khz = (stock_mhz + oc_mhz) / 2 * 1000;
    out.undervolt = run_undervolt(hooks, true, how);
    if (aborted()) return stop("aborted");
    if (out.undervolt.ran && out.undervolt.result.ok && out.undervolt.saved) {
        out.now = out.undervolt.result.after;
    } else {
        // The card is at stock now; the saved overclock goes back on in the fan step.
        out.undervolt_note = !out.undervolt.ran         ? out.undervolt.error
                             : !out.undervolt.result.ok ? out.undervolt.result.reason
                                                        : out.undervolt.save_note;
        log("  no undervolt (" + out.undervolt_note + "): the overclock stays as it is");
    }

    log("step 4 of 5: the quietest fan speed that holds the temperature; step 5: the final test");
    const FanStepOutcome fan = fan_step(hooks, out.now.end_fan_pct > 0 ? out.now.end_fan_pct : 70);
    if (!fan.error.empty()) return stop(fan.error);
    out.fan = fan.fan;
    out.fan_note = fan.note;
    if (fan.final.seconds > 0 && fan.final.verdict == Verdict::Stable) {
        // The same settings ran five minutes just before (the soak of the
        // step that found them). A final test that scores clearly less at
        // the same clock was not slowed by the tune: another program had
        // the card or the processor. On the reference card an emulator
        // taking 11 % of the graphics card cost 6 % (hardware check 75).
        const StabilityResult before = out.now;
        out.now = fan.final;
        char line[400];
        if (before.score > 0 && out.now.score < 0.97 * before.score && out.now.avg_core_mhz >= before.avg_core_mhz - 30) {
            std::snprintf(line, sizeof(line),
                          "the final test scored %.0f it/s where the same settings scored %.0f a few minutes earlier, at the same "
                          "clock: another program was using the graphics card or the processor. Close other programs and "
                          "run again for numbers that compare",
                          out.now.score, before.score);
            out.disturbed = line;
            log(std::string("  note: ") + line);
        }
    }
    Config cfg = load_config();
    if (!cfg.profile) return stop("the saved tune could not be read back");
    // The steps saved their own comparisons; the run's is its first and its last five minutes.
    if (const auto against = against_stock(out.stock, out.now)) {
        cfg.profile->against_stock = against;
        if (!save_config(cfg)) log("  note: the comparison with stock could not be saved (" + config_path().string() + ")");
    }
    out.profile = *cfg.profile;
    out.ok = true;
    return out;
}

}
