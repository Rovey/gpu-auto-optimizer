#include "app/common.hpp"
#include "app/guarded_gpu.hpp"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "core/journal.hpp"
#include "core/fan_curve.hpp"
#include "core/stability.hpp"
#include "core/task_xml.hpp"
#include "hw/app_files.hpp"
#include "hw/boot_task.hpp"
#include "hw/gpu_control.hpp"
#include "hw/nvapi.hpp"
#include "hw/nvml.hpp"
#include "hw/stress.hpp"
#include <algorithm>
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
    return std::string(preset_name(p.preset)) + ": power " + std::to_string(p.power_pct) + " %, core +" +
           std::to_string(p.core_mhz) + " MHz, mem +" + std::to_string(p.mem_mhz) + " MHz (driver " + p.driver +
           ", saved " + p.saved_at + ")";
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

OptimizeOutcome run_optimize(Preset preset, const OptimizeHooks& hooks, const std::optional<FanCurve>& fan_curve) {
    OptimizeOutcome out;
    auto log = [&](const std::string& m) { if (hooks.log) hooks.log(m); };
    auto fail = [&](const std::string& why) { out.error = why; return out; };
    if (!is_elevated()) return fail("optimizing changes clocks and power limits and needs administrator rights");
    const TuningLock lock;
    if (!lock.owned()) return fail("another optimize is already running (in the app or on the command line)");
    std::string why;
    if (!prepare_state(&why)) return fail(why);
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
    Nvapi nvapi;
    if (!nvapi.Init()) return done("NVAPI init failed: " + nvapi.Error());
    std::string why;
    if (!apply_profile(make_gpu_control(nvml, nvapi, kGpu), *cfg.profile, &why)) return done("apply failed: " + why);
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

}
