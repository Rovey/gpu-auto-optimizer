#include "app/common.hpp"
#include "app/guarded_gpu.hpp"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "core/journal.hpp"
#include "core/fan_curve.hpp"
#include "core/fan_tune.hpp"
#include "core/stability.hpp"
#include "core/task_xml.hpp"
#include "hw/app_files.hpp"
#include "hw/boot_task.hpp"
#include "hw/gpu_control.hpp"
#include "hw/nvapi.hpp"
#include "hw/nvml.hpp"
#include "hw/stress.hpp"
#include "hw/update_io.hpp"
#include "core/version.hpp"
#include <algorithm>
#include <cstdio>
#include <ctime>
#include <chrono>
#include <filesystem>
#include <memory>

namespace gao::app {

namespace {
constexpr wchar_t kTuningMutex[] = L"Local\\GpuAutoOptimizer.Tuning";
constexpr wchar_t kTrayWindowClass[] = L"GpuAutoOptimizerWindow";
}

TuningLock::TuningLock() {
    handle_ = CreateMutexW(nullptr, FALSE, kTuningMutex);
    if (!handle_) return;
    // Not 0: tuning_in_progress() holds the mutex for an instant while it looks.
    const DWORD r = WaitForSingleObject(handle_, 100);
    owned_ = r == WAIT_OBJECT_0 || r == WAIT_ABANDONED;   // abandoned: the last owner died mid-run
}

TuningLock::~TuningLock() {
    if (owned_) ReleaseMutex(handle_);
    if (handle_) CloseHandle(handle_);
}

bool tuning_in_progress() {
    const HANDLE h = OpenMutexW(SYNCHRONIZE, FALSE, kTuningMutex);
    if (!h) return false;
    const DWORD r = WaitForSingleObject(h, 0);
    if (r == WAIT_OBJECT_0 || r == WAIT_ABANDONED) ReleaseMutex(h);
    CloseHandle(h);
    return r == WAIT_TIMEOUT;
}

unsigned tray_notice_message() {
    static const UINT msg = RegisterWindowMessageW(L"GpuAutoOptimizer.TrayNotice");
    return msg;
}

void tell_tray(TrayNotice notice) {
    if (const HWND tray = FindWindowW(kTrayWindowClass, nullptr))
        PostMessageW(tray, tray_notice_message(), static_cast<WPARAM>(notice), 0);
}

bool is_elevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    const bool ok = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size);
    CloseHandle(token);
    return ok && elevation.TokenIsElevated;
}

std::string now_text() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm);
    return buf;
}

Config load_config() {
    const auto text = read_file(config_path());
    return text ? from_json(*text) : Config{};
}

bool save_config(const Config& c) {
    return !config_path().empty() && write_file_atomic(config_path(), to_json(c));
}

void boot_log(const std::string& msg) {
    append_line_durable(boot_log_path(), now_text() + "  " + msg);
}

bool prepare_state(std::string* why) { return ensure_app_dir(why); }

std::string profile_text(const Profile& p) {
    const std::string tail = " (driver " + p.driver + ", saved " + p.saved_at + ")";
    const std::string clocks = "power " + std::to_string(p.power_pct) + " %, core +" + std::to_string(p.core_mhz) +
                               " MHz, mem +" + std::to_string(p.mem_mhz) + " MHz";
    if (!p.undervolt) return std::string(preset_name(p.preset)) + ": " + clocks + tail;
    return std::string(preset_name(p.preset)) + ": " + std::to_string(p.undervolt->freq_khz / 1000) + " MHz at " +
           std::to_string(p.undervolt->volt_uv / 1000) + " mV, " + clocks + tail;
}

std::string decision_text(BootDecision d, const Config& c, const std::string& driver) {
    switch (d) {
        case BootDecision::NoProfile: return "no saved profile; run an optimize first";
        case BootDecision::TooManyStrikes:
            return "disabled after " + std::to_string(kMaxBootStrikes) + " crashes; turn boot-apply on again to retry";
        case BootDecision::DriverChanged:
            if (driver.empty()) return "driver version unknown (NVML did not report it); not applied";
            return "driver changed (" + c.profile->driver + " -> " + driver + "); optimize again";
        case BootDecision::GpuChanged: return "this is not the card the profile was tested on; optimize again";
        case BootDecision::Apply: return "apply";
    }
    return "unknown";
}

namespace {

// The card at stock under load, five minutes: the "before" of every result.
// It brings its own drivers and stress load, like the searches, and leaves the
// fans to the NVIDIA driver.
struct KeepAwake {
    KeepAwake() { SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED); }
    ~KeepAwake() { SetThreadExecutionState(ES_CONTINUOUS); }
};

constexpr int kStockTempC = 85;        // a card at stock is only stopped by this
constexpr double kLongRunS = 300;      // as the soaks: the two sides of the comparison last equally long

struct StockOutcome {
    std::string error;       // why it did not run
    StabilityResult result;
};

StockOutcome measure_stock(const OptimizeHooks& hooks) {
    StockOutcome out;
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
    if (!gpu.reset_to_stock || !gpu.reset_to_stock()) return fail("could not set the card to stock");
    const KeepAwake keep_awake;
    if (hooks.measuring) hooks.measuring("Measuring the card at stock", kLongRunS);
    const bool crashed = !guarded([&] {
        out.result = run_stability([&] { return load->Batch(); }, [&] { return gpu.read(); }, kLongRunS, kStockTempC, hooks.aborted);
    });
    if (hooks.measuring) hooks.measuring("", 0);
    if (crashed) {
        (void)load.release();   // as in run_optimize: a load that faulted is left alone
        return fail("access violation while the card was measured at stock");
    }
    return out;
}

std::string stock_line(const StabilityResult& s) {
    char line[400];
    std::snprintf(line, sizeof(line), "  stock: %s  score=%.0f it/s  clock=%d MHz  peak=%d C  fan=%s  power=%d W", verdict_name(s.verdict),
                  s.score, s.avg_core_mhz, s.peak_temp_c, reading(s.end_fan_pct, " %").c_str(), s.avg_power_w);
    return line;
}

}

OptimizeOutcome run_optimize(Preset preset, const OptimizeHooks& hooks, const std::optional<FanCurve>& fan_curve,
                             const std::optional<StabilityResult>& known_stock) {
    OptimizeOutcome out;
    auto log = [&](const std::string& m) { if (hooks.log) hooks.log(m); };
    auto fail = [&](const std::string& why) { out.error = why; return out; };
    if (preset == Preset::Undervolt) return fail("the undervolt is not a clock search; it has its own run");
    if (!is_elevated()) return fail("optimizing changes clocks and power limits and needs administrator rights");
    const TuningLock lock;
    if (!lock.owned()) return fail("another optimize is already running (in the app or on the command line)");
    std::string why;
    if (!prepare_state(&why)) return fail(why);
    // Before the search and with drivers of its own, so that only one stress
    // load exists at a time. The lock is taken again there, on this thread.
    if (known_stock) {
        out.stock = *known_stock;
    } else {
        log("the card at stock, five minutes");
        const StockOutcome measured = measure_stock(hooks);
        if (!measured.error.empty()) return fail(measured.error);
        log(stock_line(measured.result));
        if (measured.result.verdict == Verdict::Aborted) return fail("aborted");
        if (measured.result.verdict != Verdict::Stable)
            return fail(std::string("the card is not stable at stock (") + verdict_name(measured.result.verdict) + ")");
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
    struct FanRelease {
        FanDriver& f;
        const GuardedGpu& hw;
        const GpuControl& gpu;
        const OptimizeHooks& hooks;
        ~FanRelease() {   // every exit: done, aborted, failed or thrown
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
    } fan_release{fans, hw, gpu, hooks};
    if (!gpu.set_fan_pct) log("fans: not controllable on this card; the driver keeps them");

    const auto path = journal_path();
    if (path.empty()) return fail("the ProgramData folder could not be resolved; cannot keep the crash journal");
    const auto lines = read_lines(path);
    if (!lines) return fail("the crash journal " + path.string() + " exists but cannot be read; not tuning without it");
    Journal journal(*lines, [&path](const std::string& l) { return append_line_durable(path, l); });
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
        auto read = [&] {
            const Telemetry t = gpu.read();
            const FanMode before = fans.state().mode;
            const FanState now = fans.tick(t.temp_c, t.power_w, std::chrono::steady_clock::now());
            if (now.mode != before && now.mode == FanMode::Failed) log("fans: a write did not verify; the driver has them again");
            if (now.mode != before && now.mode == FanMode::Foreign) log("fans: another program set them; leaving them alone");
            return t;
        };
        return run_stability([&] { return load->Batch(); }, read, seconds, max_temp, hooks.aborted,
                             stall_below);
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
    if (hooks.active_gpu) hooks.active_gpu(&gpu);
    // The last line of defence: a fault that escapes the per-call guards (in
    // the stress load's D3D calls, say) must not kill the process with a
    // journal entry open. The frames of the search are skipped, not unwound;
    // everything that must run on the way out lives in this frame. A C++
    // exception passes through the guard to the catch below.
    bool crashed = false;
    try {
        crashed = !guarded([&] { out.result = optimize(gpu, objectives_for(preset), journal, io); });
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
    if (!out.result.ok) return out;

    const std::string driver = hw.DriverVersion(), gpu_id = hw.GpuUuid();
    if (driver.empty() || gpu_id.empty()) {
        out.save_note = "NVML did not report the driver version or GPU id, so the profile could never be re-applied";
        return out;
    }
    Config cfg = load_config();
    cfg.profile = Profile{preset, out.result.power_pct, out.result.core_mhz, out.result.mem_mhz, driver, gpu_id, now_text()};
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
    if (!is_elevated()) return fail("undervolting changes the card's voltage/frequency curve and needs administrator rights");
    const TuningLock lock;
    if (!lock.owned()) return fail("another optimize is already running (in the app or on the command line)");
    std::string why;
    if (!prepare_state(&why)) return fail(why);
    GuardedGpu hw(kGpu);   // guarded driver calls and a reconnect, as in run_optimize
    if (!hw.Init(&why)) return fail(why);
    auto load = std::make_unique<Stress>();   // on the heap: left alone after an access violation
    if (!load->Init()) return fail("stress init failed: " + load->Error());
    const GpuControl& gpu = hw.control();

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
        return run_stability([&] { return load->Batch(); }, [&] { return gpu.read(); }, seconds, max_temp, hooks.aborted, stall_below);
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
    const Objectives obj = objectives_for(Preset::Undervolt);
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
    // The search left the fans to the driver and tested no curve. The curve
    // the fans followed before stays theirs: an undervolt only makes the
    // card cooler than the tune that curve was tested with.
    if (cfg.profile) p.fan_curve = cfg.profile->fan_curve;
    if (over) {   // the overclock it was searched on keeps everything it had; the undervolt joins it
        p = *over;
        p.saved_at = now_text();
        if (how.save_as) p.preset = *how.save_as;
    }
    p.undervolt = UndervoltTune{out.result.applied_uv, out.result.freq_khz, out.result.raise_khz};
    cfg.profile = p;
    cfg.boot_strikes = 0;   // strikes belong to the profile they were earned by
    out.saved = save_config(cfg);
    if (!out.saved) out.save_note = "could not write " + config_path().string();
    return out;
}

BootApplyOutcome apply_at_logon() {
    BootApplyOutcome out;
    auto done = [&](const std::string& msg) { out.message = msg; boot_log(msg); return out; };
    Nvml nvml;
    if (!nvml.Init()) return done("NVML init failed: " + nvml.Error());
    Config cfg = load_config();
    const std::string driver = nvml.DriverVersion();
    const auto d = decide_boot(cfg, driver, nvml.GpuUuid(kGpu));
    out.decision = d;
    if (d != BootDecision::Apply) return done("not applied: " + decision_text(d, cfg, driver));
    // The strike is on disk before the hardware is touched: a crash from here
    // on counts.
    ++cfg.boot_strikes;
    if (!save_config(cfg)) return done("could not record the strike; not applying");
    // A strike is for a logon that may have crashed the machine. An apply
    // that ended here, with nothing of the profile on the card, did not: the
    // strike is taken back, or three refused logons would read as "disabled
    // after 3 crashes".
    auto no_strike = [&] {
        --cfg.boot_strikes;
        if (!save_config(cfg)) boot_log("could not take the strike back; it stays");
    };
    Nvapi nvapi;
    if (!nvapi.Init()) { no_strike(); return done("NVAPI init failed: " + nvapi.Error()); }
    std::string why;
    bool left_clean = false;
    if (!apply_profile(make_gpu_control(nvml, nvapi, kGpu), *cfg.profile, &why, &left_clean)) {
        if (left_clean) no_strike();
        return done("apply failed: " + why);
    }
    out.applied = true;
    out.profile = *cfg.profile;
    return done("applied " + profile_text(*cfg.profile) + ", strike " + std::to_string(cfg.boot_strikes) +
                " clears in 2 minutes");
}

void clear_boot_strike() {
    // Reload: an optimize may have saved a new profile meanwhile; only the
    // counter is ours to change. If the file cannot be read, or reads without
    // a profile, writing would destroy it: leave the strike (fails safe).
    for (int attempt = 0; attempt < 3; ++attempt) {
        const auto text = read_file(config_path());
        Config latest = text ? from_json(*text) : Config{};
        if (!latest.profile) { boot_log("could not re-read gao.json to clear the strike; it stays"); return; }
        latest.boot_strikes = 0;
        if (save_config(latest)) { boot_log("ran 2 minutes without a crash; strike cleared"); return; }
        Sleep(1000);
    }
    boot_log("could not save gao.json to clear the strike; it stays");
}

bool enable_boot(std::string* message) {
    auto say = [&](const std::string& m, bool ok) { if (message) *message = m; return ok; };
    if (!is_elevated()) return say("boot-apply needs administrator rights", false);
    std::string why;
    if (!prepare_state(&why)) return say(why, false);
    const std::string sid = current_user_sid();
    if (sid.empty()) return say("could not determine the current user's SID", false);
    wchar_t self[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, self, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return say("could not find this program's own path", false);
    if (!install_app(std::filesystem::path(self).parent_path(), &why))
        return say(why, false);
    // The logon task starts the tray app, which applies the tune and stays
    // resident to keep it applied: no time limit.
    const auto exe = installed_tray_path();
    const auto u8 = exe.u8string();
    const std::string xml = boot_task_xml(std::string(u8.begin(), u8.end()), "--tray", sid, "PT0S");
    const auto xml_path = app_dir() / L"BootApply.xml";
    if (!write_utf16_file(xml_path, xml)) return say("could not write " + xml_path.string(), false);
    const int code = boot_task_create_xml(xml_path);
    std::error_code ec;
    std::filesystem::remove(xml_path, ec);
    if (code != 0) return say("could not create the task (schtasks exit " + std::to_string(code) + ")", false);
    Config cfg = load_config();
    cfg.boot_strikes = 0;
    if (!save_config(cfg)) return say("task created, but could not reset the strike counter", false);
    return say("boot-apply on: " + exe.string() + " --tray runs at every logon and keeps the tune applied (strikes reset)" +
               (cfg.profile ? "" : "; note: there is no saved profile yet, optimize first"), true);
}

bool disable_boot(std::string* message) {
    auto say = [&](const std::string& m, bool ok) { if (message) *message = m; return ok; };
    if (!is_elevated()) return say("boot-apply needs administrator rights", false);
    std::string why;
    if (!prepare_state(&why)) return say(why, false);
    const int code = boot_task_remove();
    const bool removed_now = uninstall_app();
    return say(std::string("boot-apply off: task ") + (code == 0 ? "removed" : "not removed") + ", installed copy " +
                   (removed_now ? "removed" : "in use by the running tray app; it is removed at the next restart"),
               code == 0);
}

namespace {
std::filesystem::path own_dir() {
    wchar_t self[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, self, MAX_PATH);
    return n == 0 || n >= MAX_PATH ? std::filesystem::path() : std::filesystem::path(self).parent_path();
}
}

bool logon_copy_is_this_build() {
    const auto dir = own_dir();
    if (dir.empty()) return false;
    return files_equal(dir / L"gao.exe", installed_exe_path()) && files_equal(dir / L"GpuAutoOptimizer.exe", installed_tray_path());
}

std::string update_logon_copy() {
    if (!is_elevated() || !boot_task_exists()) return {};
    const auto dir = own_dir();
    std::error_code ec;
    if (dir.empty() || std::filesystem::equivalent(dir, installed_dir(), ec)) return {};
    if (!std::filesystem::exists(installed_exe_path(), ec) || logon_copy_is_this_build()) return {};
    // Never a downgrade: an old zip that is still lying around must not
    // replace a newer installed copy just because it was started.
    const auto installed = file_version_number(installed_exe_path());
    if (installed && *installed > kVersionNumber) return {};
    std::string message;
    if (!enable_boot(&message)) return "The copy that starts at logon could not be updated: " + message;
    return "The copy that starts at logon was updated to this version (" + std::string(kVersion) + ").";
}

UpdateCheck check_for_update() {
    UpdateCheck out;
    const auto body = https_get("https://api.github.com/repos/Rovey/gpu-auto-optimizer/releases/latest", 1024 * 1024, &out.error);
    if (!body) return out;
    auto release = parse_latest_release(*body);
    if (!release) { out.error = "the answer did not describe a release"; return out; }
    if (*version_number(release->version) > kVersionNumber) out.release = std::move(release);
    return out;
}

bool install_update(const ReleaseInfo& release, std::string* message) {
    auto say = [&](const std::string& m, bool ok) { if (message) *message = m; return ok; };
    const auto dir = own_dir();
    if (dir.empty()) return say("could not find this program's own folder", false);
    // Elevated, the download is unpacked where only administrators can write:
    // between the hash check and the copy, nobody else may swap the files.
    std::filesystem::path work;
    std::string why;
    if (is_elevated()) {
        if (!prepare_state(&why)) return say(why, false);
        work = app_dir() / L"update";
    } else {
        std::error_code ec;
        work = std::filesystem::temp_directory_path(ec) / L"GpuAutoOptimizer-update";
        if (ec) return say("no temporary folder", false);
    }
    std::error_code ec;
    std::filesystem::remove_all(work, ec);
    const auto unpacked = work / L"files";
    if (!std::filesystem::create_directories(unpacked, ec)) return say("could not create " + work.string(), false);
    struct Cleanup {
        std::filesystem::path p;
        ~Cleanup() { std::error_code e; std::filesystem::remove_all(p, e); }
    } cleanup{work};

    // What to do and in which order is install_release's (core, tested with a
    // fake source); here are only the real steps.
    const auto zip = work / L"update.zip";
    UpdateSteps steps;
    steps.download = [&](const std::string& url, std::string* error) { return https_download(url, zip, 64 * 1024 * 1024, error); };
    steps.sha256 = [&] { return sha256_hex(zip); };
    steps.unpack = [&](std::string* error) { return extract_zip(zip, unpacked, error); };
    steps.unpacked_is = [&](unsigned version) {
        for (const wchar_t* name : kAppExes)
            if (file_version_number(unpacked / name) != version) return false;
        return true;
    };
    steps.replace = [&](std::string* error) { return copy_app(unpacked, dir, error); };
    return install_release(release, steps, message);
}

namespace {

// The card with the saved tune under load while its fans are tuned: a
// measuring step of the all-in-one run. It brings its own drivers and stress
// load, like the searches and like measure_stock above.
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
    const int limit = objectives_for(profile.preset).max_temp_c;
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
            if (fans) fans->tick(t.temp_c, t.power_w, std::chrono::steady_clock::now());
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

AllInOneOutcome run_all_in_one(const OptimizeHooks& hooks) {
    AllInOneOutcome out;
    auto log = [&](const std::string& m) { if (hooks.log) hooks.log(m); };
    auto stop = [&](const std::string& why) { out.error = why; return out; };
    auto aborted = [&] { return hooks.aborted && hooks.aborted(); };
    if (!is_elevated()) return stop("tuning changes clocks, the voltage/frequency curve and the fans and needs administrator rights");
    // Held across the steps (each takes it again, on this thread), so that
    // the watchdog does not put a half-made tune back in between.
    const TuningLock lock;
    if (!lock.owned()) return stop("another optimize is already running (in the app or on the command line)");

    log("step 1 of 5: the card at stock, five minutes");
    const StockOutcome stock = measure_stock(hooks);
    if (!stock.error.empty()) return stop(stock.error);
    out.stock = stock.result;
    log(stock_line(out.stock));
    if (out.stock.verdict == Verdict::Aborted) return stop("aborted");
    if (out.stock.verdict != Verdict::Stable)
        return stop(std::string("the card is not stable at stock (") + verdict_name(out.stock.verdict) + ")");
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
    const Config cfg = load_config();
    if (!cfg.profile) return stop("the saved tune could not be read back");
    out.profile = *cfg.profile;
    out.ok = true;
    return out;
}

}
