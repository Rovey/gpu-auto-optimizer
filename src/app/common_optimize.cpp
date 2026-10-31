#include "app/common_internal.hpp"
#include "core/journal.hpp"
#include "hw/app_files.hpp"
#include "hw/gpu_users.hpp"
#include "hw/stress.hpp"
#include <algorithm>
#include <chrono>
#include <memory>

namespace gao::app {

OptimizeOutcome run_optimize(Preset preset, const OptimizeHooks& hooks, const std::optional<FanCurve>& fan_curve,
                             const std::optional<StabilityResult>& known_stock) {
    OptimizeOutcome out;
    auto log = [&](const std::string& m) { if (hooks.log) hooks.log(m); };
    auto fail = [&](const std::string& why) { out.error = why; return out; };
    if (preset == Preset::Undervolt) return fail("the undervolt is not a clock search; it has its own run");
    if (const std::string gpus = several_gpus_refusal(); !gpus.empty()) return fail(gpus);
    if (const std::string tool = other_tool(); !tool.empty()) return fail(tool);
    if (!is_elevated()) return fail("optimizing changes clocks and power limits and needs administrator rights");
    const TuningLock lock;
    if (!lock.owned()) return fail(kAlreadyRunning);
    std::string why;
    if (!prepare_state(&why)) return fail(why);
    const BusyGate busy = busy_before_run(hooks);
    if (!busy.refusal.empty()) return fail(busy.refusal);
    // Before the search and with drivers of its own, so that only one stress
    // load exists at a time. The lock is taken again there, on this thread.
    if (known_stock) {
        out.stock = *known_stock;
    } else {
        log("the card at stock, five minutes");
        const StockOutcome measured = measure_stock(hooks);
        if (!measured.error.empty()) return fail(measured.error);
        out.stock = measured.result;
    }
    // Every driver call of the run goes through `hw`: guarded against a fault
    // inside the driver DLLs, and reconnected by gpu.recover after a driver
    // reset. `gpu` is never reassigned, so the references the fan driver, the
    // search and the emergency handlers hold stay valid across a reconnect.
    GuardedGpu hw(kGpu);
    if (!hw.Init(&why)) return fail(why);
    // On the heap, so the path that follows an access violation during the
    // search can leave it alone instead of destroying it (see there).
    auto load = std::make_unique<Stress>();
    if (!load->Init()) return fail("stress init failed: " + load->Error());
    const GpuControl& gpu = hw.control();
    // The profile's curve drives the fans for the whole run, so the clocks it
    // finds hold at the temperatures that curve produces.
    const FanCurve curve = fan_curve.value_or(default_curve(preset));
    FanDriver fans(gpu, curve, objectives_for(preset).max_temp_c, fan_min_for(load_config(), hw.GpuUuid(), gpu.fan_min_pct));
    const FanRelease fan_release{fans, hw, gpu, hooks};
    if (!gpu.set_fan_pct) log("fans: not controllable on this card; the driver keeps them");

    const auto path = journal_path();
    if (path.empty()) return fail("the ProgramData folder could not be resolved; cannot keep the crash journal");
    const auto lines = read_lines(path);
    if (!lines) return fail("the crash journal " + path.string() + " exists but cannot be read; not tuning without it");
    // What a memory edge in the journal is bound to: this card with this driver.
    const std::string rig_card = hw.GpuUuid(), rig_driver = hw.DriverVersion();
    Journal journal(*lines, [&path](const std::string& l) { return append_line_durable(path, l); },
                    rig_card.empty() || rig_driver.empty() ? std::string() : rig_card + " / " + rig_driver);
    // Prove the journal is writable before any clock is touched; the parser
    // ignores lines without an id, so this one never becomes a ceiling.
    if (!append_line_durable(path, "{\"session\":\"" + now_text() + "\"}"))
        return fail("cannot write the crash journal " + path.string() + "; not tuning without it");
    for (const auto& f : journal.freeze_entries())
        log("warning: a previous run froze the machine at " + f.description +
            (f.caps_anything ? "; staying below it from now on" : "; there is no setting to stay below"));
    if (!gpu.set_power_limit) log("power limit: not adjustable on this card, skipped");

    // A run lasts minutes with a candidate applied and a journal entry open;
    // idle sleep must not interrupt it. The display may still turn off.
    struct KeepAwake {
        KeepAwake() { SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED); }
        ~KeepAwake() { SetThreadExecutionState(ES_CONTINUOUS); }   // every exit
    } keep_awake;
    OptimizeIo io;
    io.probe = [&](double seconds, int max_temp, double stall_below) {
        return run_stability([&] { return load->Batch(); }, [&] { return read_with_fans(gpu, fans, hooks); }, seconds, max_temp,
                             hooks.aborted, stall_below);
    };
    io.aborted = hooks.aborted;
    io.log = hooks.log;
    io.bandwidth = [&] {
        const auto gbps = load->MeasureBandwidth();
        if (!gbps) log("bandwidth measurement failed: " + load->Error());
        return gbps;
    };
    io.bandwidth_unsettled = [&] { return load->BandwidthUnsettled(); };
    // The wait between a driver reset and the next load. The card is left
    // alone: no load, no driver call, no fan tick, and no log line (the search
    // says that it rests). Short slices, so a stop request ends the wait
    // within a quarter of a second.
    io.rest = [&hooks](double seconds) {
        const auto stop_requested = [&hooks] { return hooks.aborted && hooks.aborted(); };
        if (stop_requested()) return false;
        if (!(seconds > 0)) return true;   // nothing to wait for
        using namespace std::chrono;
        const auto end = steady_clock::now() + duration_cast<steady_clock::duration>(duration<double>(std::min(seconds, 3600.0)));
        for (;;) {
            const auto left_ms = duration_cast<milliseconds>(end - steady_clock::now()).count();
            if (left_ms <= 0) return true;
            Sleep(static_cast<DWORD>(left_ms < 250 ? left_ms : 250));
            if (stop_requested()) return false;
        }
    };
    // After a driver reset the load may hold a removed device without having
    // noticed. A fresh one is built here, outside any probe, so the health
    // probe times the card and not the load's start-up.
    // Not guarded here: a fault inside D3D reaches the guard around the
    // search, like every other call of the load, and the run ends through the
    // crashed path, which never touches the load again.
    io.prepare_load = [&] {
        if (load->Recreate()) return true;
        log("the stress load could not be rebuilt: " + load->Error());
        return false;
    };
    io.measuring = hooks.measuring;
    // Whatever step the search announced last is over when it returns, however it returns.
    struct StepDone {
        const OptimizeHooks& hooks;
        ~StepDone() { if (hooks.measuring) hooks.measuring("", 0); }
    } step_done{hooks};
    if (hooks.active_gpu) hooks.active_gpu(&gpu);
    // The last line of defence: a fault that escapes the per-call guards (in
    // the stress load's D3D calls, say) must not kill the process with a
    // journal entry open. The frames of the search are skipped, not unwound;
    // everything that must run on the way out lives in this frame. A C++
    // exception passes through the guard to the catch below.
    bool crashed = false;
    try {
        crashed = !guarded([&] { out.result = optimize(gpu, objectives_for(preset), journal, io, out.stock); });
    } catch (...) {   // never leave a candidate applied, whatever went wrong
        if (hooks.active_gpu) hooks.active_gpu(nullptr);
        if (gpu.reset_to_stock) gpu.reset_to_stock();
        // The machine did not freeze: an entry left open would become a
        // ceiling the next run stays below.
        if (journal.open_id() >= 0) journal.complete(journal.open_id(), "CRASHED");
        throw;
    }
    if (crashed) {
        // An access violation somewhere in the search: in a driver DLL, in the
        // stress load's D3D calls or in our own code. A candidate may still be
        // applied and its journal entry open.
        //
        // Leaked on purpose, and before anything that can throw, so that no
        // exception from the steps below destroys it on the way out. The
        // fault skipped the frames of the search, so a D3D call of the stress
        // load may have been abandoned halfway; its destructor would release
        // COM objects into that same user-mode driver and could fault outside
        // any guard, or hang. `gao` is about to report the failure and exit,
        // and the device goes with the process. The window application lives
        // on: there the leaked load, with its device and buffers, stays until
        // the application exits.
        (void)load.release();
        bool stock = false;
        {
            // The emergency handler stays registered until the card and the
            // journal are dealt with: the steps below can take half a minute,
            // and without it Ctrl+C would end the process and a closed window
            // would reset nothing. Unregistered when this block ends, also by
            // an exception, and always before `hw` is destroyed.
            struct Unregister {
                const OptimizeHooks& h;
                ~Unregister() { if (h.active_gpu) h.active_gpu(nullptr); }
            } unregister{hooks};
            log("access violation during the search -- resetting the card to stock");
            // The machine did not freeze, so the candidate must not become a
            // ceiling. Closing the entry is a file append, no driver call, so
            // it comes first: nothing that happens during the reset can leave
            // it open.
            if (journal.open_id() >= 0 && !journal.complete(journal.open_id(), "CRASHED"))
                log("warning: the crash journal entry could not be closed; later runs will stay below this candidate");
            // The connection is often still good (the fault may have been
            // outside NVML and NVAPI), so reset first; reconnecting costs
            // seconds with the candidate applied.
            stock = gpu.reset_to_stock && gpu.reset_to_stock();
            if (!stock && gpu.reset_to_stock && gpu.recover) {
                log("the reset failed; reconnecting to the driver, up to 30 s");
                stock = gpu.recover() && gpu.reset_to_stock();
            }
            log(std::string("the search ended in an access violation -- ") +
                (stock ? "card restored to stock" : "reset FAILED, run `gao --reset`"));
        }
        out.ran = true;
        // optimize never returned, so out.result was never assigned: every
        // field not set here keeps its default. driver_resets reads 0,
        // whatever the search had counted before the fault.
        out.result.ok = false;
        out.result.stock_restored = stock;
        out.result.reason = "access violation during the search";
        return out;
    }
    if (hooks.active_gpu) hooks.active_gpu(nullptr);
    out.ran = true;
    // A second look, now that the load is off the card: a program that
    // started or stopped using it during the run moved the numbers by several
    // percent, and the measurement at stock was taken without that.
    if (!hooks.busy_asked && !(hooks.aborted && hooks.aborted())) {
        out.busy_note = busy_change(busy.programs, busy_programs(gpu_use(kBusyLookS)));
        if (!out.busy_note.empty()) log("note: " + out.busy_note + "; its numbers and the ones at stock were not measured on the same card");
    }
    if (!out.result.ok) return out;

    const std::string driver = hw.DriverVersion(), gpu_id = hw.GpuUuid();
    if (driver.empty() || gpu_id.empty()) {
        out.save_note = "NVML did not report the driver version or GPU id, so the profile could never be re-applied";
        return out;
    }
    Config cfg = load_config();
    cfg.profile = Profile{preset, out.result.power_pct, out.result.core_mhz, out.result.mem_mhz, driver, gpu_id, now_text()};
    cfg.profile->against_stock = against_stock(out.stock, out.result.soak);
    cfg.fan_curve.reset();   // a new tune starts from its own curve
    // Only a curve that actually drove the fans counts as tested.
    if (gpu.set_fan_pct && fans.state().mode != FanMode::Failed && fans.state().mode != FanMode::Foreign) {
        cfg.profile->fan_curve = curve;   // what the run was tested with
        cfg.fan_control = true;
        if (fans.state().min_pct > fan_min_for(cfg, gpu_id, gpu.fan_min_pct)) {   // learned during the run
            cfg.fan_min_pct = fans.state().min_pct;
            cfg.fan_min_gpu = gpu_id;
        }
    }
    cfg.boot_strikes = 0;   // strikes belong to the profile they were earned by
    out.saved = save_config(cfg);
    if (!out.saved) out.save_note = "could not write " + config_path().string();
    return out;
}

UndervoltOutcome run_undervolt(const OptimizeHooks& hooks, bool on_saved_tune, const UndervoltOnTune& how) {
    UndervoltOutcome out;
    auto log = [&](const std::string& m) { if (hooks.log) hooks.log(m); };
    auto fail = [&](const std::string& why) { out.error = why; return out; };
    if (const std::string gpus = several_gpus_refusal(); !gpus.empty()) return fail(gpus);
    if (const std::string tool = other_tool(); !tool.empty()) return fail(tool);
    if (!is_elevated()) return fail("undervolting changes the card's voltage/frequency curve and needs administrator rights");
    const TuningLock lock;
    if (!lock.owned()) return fail(kAlreadyRunning);
    std::string why;
    if (!prepare_state(&why)) return fail(why);
    if (const BusyGate busy = busy_before_run(hooks); !busy.refusal.empty()) return fail(busy.refusal);
    // The card at stock first, as every run measures it: the "before" of the
    // result, with the fans left to the driver, stopped at kStockTempC and
    // kept when it was, and followed by a cool-down (measure_stock). With
    // drivers of its own, before this run makes its own, as in run_optimize.
    // Not on top of the saved tune: the reference there is that overclock.
    std::optional<StabilityResult> stock_run;
    if (!on_saved_tune) {
        log("the card at stock, five minutes");
        const StockOutcome measured = measure_stock(hooks);
        if (!measured.error.empty()) return fail(measured.error);
        stock_run = measured.result;
    }
    GuardedGpu hw(kGpu);   // guarded driver calls and a reconnect, as in run_optimize
    if (!hw.Init(&why)) return fail(why);
    auto load = std::make_unique<Stress>();   // on the heap: left alone after an access violation
    if (!load->Init()) return fail("stress init failed: " + load->Error());
    const GpuControl& gpu = hw.control();
    // The fans follow a curve for the whole search, as in run_optimize: the
    // one in use (the overclock's own, in an all-in-one run), or the preset's.
    // Left to the driver, some cards get hotter under this load than the
    // search allows (a 3080 Ti reached 85 C at stock with its fans at 65 %),
    // and every probe would end TOO HOT.
    const Objectives obj = objectives_for(Preset::Undervolt);
    const Config at_start = load_config();
    // On a card that reached more than the profile's limit in the reference
    // the search runs with a higher one (temp_ceiling), and so do the fans:
    // with "100 % from 75 C" they would hold a card that reaches 86 C by
    // itself at 74 C and be far louder than at stock (93 % against 66 % on
    // the tester's RTX 3080 Ti). The curve moves up with the limit, by as
    // much as the limit is above the one the curve was drawn against: a curve
    // saved by a run on this card was drawn against that run's ceiling.
    const std::optional<StabilityResult>& reference = on_saved_tune ? how.measured : stock_run;
    const int ceiling = reference ? temp_ceiling(obj.max_temp_c, reference->peak_temp_c, gpu.temp_target_c) : obj.max_temp_c;
    const int drawn_against = at_start.profile ? profile_ceiling(*at_start.profile) : obj.max_temp_c;
    const FanCurve fan_curve =
        shifted_curve(active_fan_curve(at_start).value_or(default_curve(Preset::Undervolt)), ceiling - drawn_against);
    if (ceiling > obj.max_temp_c)
        log("fans: on this card the limit is " + std::to_string(ceiling) + " C, not the profile's " + std::to_string(obj.max_temp_c) +
            " C" + (ceiling > drawn_against ? "; the fan curve moves up with it by " + std::to_string(ceiling - drawn_against) + " C"
                                            : std::string("; the fan curve was drawn for it")));
    FanDriver fans(gpu, fan_curve, ceiling, fan_min_for(at_start, hw.GpuUuid(), gpu.fan_min_pct));
    const FanRelease fan_release{fans, hw, gpu, hooks};
    if (!gpu.set_fan_pct) log("fans: not controllable on this card; the driver keeps them");

    const auto path = journal_path();
    if (path.empty()) return fail("the ProgramData folder could not be resolved; cannot keep the crash journal");
    const auto lines = read_lines(path);
    if (!lines) return fail("the crash journal " + path.string() + " exists but cannot be read; not tuning without it");
    Journal journal(*lines, [&path](const std::string& l) { return append_line_durable(path, l); });
    if (!append_line_durable(path, "{\"session\":\"" + now_text() + "\"}"))
        return fail("cannot write the crash journal " + path.string() + "; not tuning without it");

    struct KeepAwake {
        KeepAwake() { SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED); }
        ~KeepAwake() { SetThreadExecutionState(ES_CONTINUOUS); }
    } keep_awake;
    UndervoltIo io;
    io.probe = [&](double seconds, int max_temp, double stall_below) {
        return run_stability([&] { return load->Batch(); }, [&] { return read_with_fans(gpu, fans, hooks); }, seconds, max_temp,
                             hooks.aborted, stall_below);
    };
    io.aborted = hooks.aborted;
    io.log = hooks.log;
    io.measuring = hooks.measuring;
    io.rest = [&hooks](double seconds) {   // no load, no driver call; a stop request ends it within a quarter of a second
        using namespace std::chrono;
        const auto end = steady_clock::now() + duration_cast<steady_clock::duration>(duration<double>(std::min(seconds, 3600.0)));
        for (;;) {
            if (hooks.aborted && hooks.aborted()) return false;
            const auto left_ms = duration_cast<milliseconds>(end - steady_clock::now()).count();
            if (left_ms <= 0) return true;
            Sleep(static_cast<DWORD>(left_ms < 250 ? left_ms : 250));
        }
    };
    io.prepare_load = [&] {
        if (load->Recreate()) return true;
        log("the stress load could not be rebuilt: " + load->Error());
        return false;
    };

    // The overclock the undervolt goes on top of, when asked for: the saved
    // tune, if it is this card's and this driver's (strikes only gate the
    // logon apply).
    UndervoltBase base;
    base.measured = stock_run;
    std::optional<Profile> over;
    if (on_saved_tune) {
        Config cfg = load_config();
        cfg.boot_strikes = 0;
        const std::string driver_now = hw.DriverVersion();
        const BootDecision d = decide_boot(cfg, driver_now, hw.GpuUuid());
        if (d != BootDecision::Apply) return fail("no saved tune to undervolt: " + decision_text(d, cfg, driver_now));
        over = cfg.profile;
        base = UndervoltBase{over->power_pct, over->core_mhz, over->mem_mhz, how.measured, how.clock_khz};
        log("on top of the saved tune: power " + std::to_string(base.power_pct) + " %, core +" + std::to_string(base.core_mhz) +
            " MHz, mem +" + std::to_string(base.mem_mhz) + " MHz");
    }
    if (hooks.active_gpu) hooks.active_gpu(&gpu);
    bool crashed = false;
    try {
        crashed = !guarded([&] { out.result = find_undervolt(gpu, journal, io, obj.max_temp_c, obj.perf_push, base); });
    } catch (...) {   // never leave a candidate applied, whatever went wrong
        if (hooks.active_gpu) hooks.active_gpu(nullptr);
        if (gpu.reset_to_stock) gpu.reset_to_stock();
        if (journal.open_id() >= 0) journal.complete(journal.open_id(), "CRASHED");
        throw;
    }
    if (crashed) {
        // As in run_optimize: the frames of the search were skipped, the load
        // may be halfway through a D3D call and is left alone.
        (void)load.release();
        log("access violation during the search -- resetting the card to stock");
        if (journal.open_id() >= 0 && !journal.complete(journal.open_id(), "CRASHED"))
            log("warning: the crash journal entry could not be closed; later runs will stay below this candidate");
        bool stock = gpu.reset_to_stock && gpu.reset_to_stock();
        if (!stock && gpu.reset_to_stock && gpu.recover) stock = gpu.recover() && gpu.reset_to_stock();
        out.result = {};
        out.result.reason = "access violation during the search";
        out.result.stock_restored = stock;
    }
    if (hooks.active_gpu) hooks.active_gpu(nullptr);
    out.ran = true;
    if (!out.result.ok) return out;

    const std::string driver = hw.DriverVersion(), gpu_id = hw.GpuUuid();
    if (driver.empty() || gpu_id.empty()) {
        out.save_note = "NVML did not report the driver version or GPU id, so the profile could never be re-applied";
        return out;
    }
    Config cfg = load_config();
    Profile p{Preset::Undervolt, 100, 0, 0, driver, gpu_id, now_text()};
    // The curve the fans followed before stays theirs: it is the one the
    // search ran with.
    if (cfg.profile) p.fan_curve = cfg.profile->fan_curve;
    if (over) {   // the overclock it was searched on keeps everything it had; the undervolt joins it
        p = *over;
        p.saved_at = now_text();
        if (how.save_as) p.preset = *how.save_as;
    }
    // The limit the tune was found with goes with it: the fans, the Fan page
    // and a later test of the tune use it (profile_ceiling).
    if (ceiling > objectives_for(p.preset).max_temp_c) p.temp_ceiling_c = ceiling;
    else p.temp_ceiling_c.reset();
    // Only a curve that actually drove the fans counts as tested.
    if (gpu.set_fan_pct && fans.state().mode != FanMode::Failed && fans.state().mode != FanMode::Foreign) {
        if (ceiling > drawn_against) {
            // The curve moved for this run: it is the one that was tested, and
            // the one drawn against the limit that is saved with it. An edited
            // curve went into it and does not stay next to it.
            p.fan_curve = fan_curve;
            cfg.fan_curve.reset();
        }
        if (!p.fan_curve) p.fan_curve = fan_curve;
        cfg.fan_control = true;
    }
    p.undervolt = UndervoltTune{out.result.applied_uv, out.result.freq_khz, out.result.raise_khz};
    // On a saved tune the baseline is that tune, not stock: the stock run it was saved with stays.
    if (!over) p.against_stock = against_stock(out.result.baseline, out.result.after);
    else if (p.against_stock && out.result.after.score > 0) p.against_stock->tuned = measured_run(out.result.after);
    cfg.profile = p;
    cfg.boot_strikes = 0;   // strikes belong to the profile they were earned by
    out.saved = save_config(cfg);
    if (!out.saved) out.save_note = "could not write " + config_path().string();
    return out;
}

}
