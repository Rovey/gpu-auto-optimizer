#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "app/common.hpp"
#include "core/stability.hpp"
#include "core/version.hpp"
#include "core/vf_curve.hpp"
#include "hw/app_files.hpp"
#include "hw/boot_task.hpp"
#include "hw/gpu_control.hpp"
#include "hw/nvapi.hpp"
#include "hw/nvml.hpp"
#include "hw/stress.hpp"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>
#include <charconv>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// The whole argument must be an int: std::atoi would read a typo like
// "--set-core +x" as 0 and silently reset the offset.
static bool ParseIntArg(const char* text, int* out) {
    const char* end = text + std::strlen(text);
    const auto [p, ec] = std::from_chars(text, end, *out);
    return ec == std::errc() && p == end && p != text;
}

static int probe() {
    gao::Nvml nvml;
    if (!nvml.Init()) {
        std::printf("NVML init failed: %s\n", nvml.Error().c_str());
        return 1;
    }
    const int count = nvml.DeviceCount();
    if (count < 0) {
        std::printf("NVML device count failed: %s\n", nvml.Error().c_str());
        return 1;
    }
    std::printf("GPUs: %d\n", count);
    for (int i = 0; i < count; ++i) {
        const gao::Telemetry t = nvml.Read(static_cast<unsigned>(i));
        if (!t.ok) { std::printf("  [%d] read failed\n", i); continue; }
        // Each field is -1 when the driver did not report it, even when t.ok.
        std::printf("  [%d] core=%s MHz  mem=%s MHz  temp=%s C  fan=%s  power=%s/%d W\n", i,
                    gao::reading(t.core_mhz).c_str(), gao::reading(t.mem_mhz).c_str(), gao::reading(t.temp_c).c_str(),
                    gao::reading(t.fan_pct, "%").c_str(), gao::reading(t.power_w).c_str(), t.power_limit_w);
    }
    // The search's outer bound (core/search.hpp: search_bounds).
    gao::Nvapi nvapi;
    if (!nvapi.Init()) {
        std::printf("offset ranges: unavailable (NVAPI init failed: %s)\n", nvapi.Error().c_str());
        return 0;
    }
    const auto ranges = nvapi.ReadOffsetRangesMhz(gao::app::kGpu);
    if (!ranges) {
        std::printf("offset ranges: unavailable (%s)\n", nvapi.Error().c_str());
        return 0;
    }
    std::printf("offset ranges as the driver reports them: core %+d to %+d MHz, mem %+d to %+d MHz\n",
                ranges->core.min_mhz, ranges->core.max_mhz, ranges->mem.min_mhz, ranges->mem.max_mhz);
    return 0;
}

using gao::app::kGpu;

// Shared by --set-core and --set-mem. Prints "requested X, read back Y" and
// OK/MISMATCH. Always prints the read-back, even on success -- the number
// being visible is the point, not just the verdict.
static int set_offset(const char* label, int mhz, bool core) {
    gao::Nvapi nvapi;
    if (!nvapi.Init()) { std::printf("NVAPI init failed: %s\n", nvapi.Error().c_str()); return 1; }
    const bool ok = core ? nvapi.SetCoreOffsetMhz(kGpu, mhz) : nvapi.SetMemOffsetMhz(kGpu, mhz);
    const auto readback = nvapi.ReadOffsetsMhz(kGpu);
    std::printf("%s: requested %d MHz, read back ", label, mhz);
    if (readback) std::printf("%d MHz\n", core ? readback->first : readback->second);
    else std::printf("unavailable (%s)\n", nvapi.Error().c_str());
    // Print the diagnostic before the verdict: "clamped to
    // 210 MHz" and "the call never reached an unelevated driver" both show
    // MISMATCH here, and without this line they are indistinguishable.
    if (!ok) std::printf("%s\n", nvapi.Error().c_str());
    std::printf("%s\n", ok ? "OK" : "MISMATCH");
    return ok ? 0 : 1;
}

static void print_curve(const std::vector<gao::VfPoint>& curve) {
    std::printf("voltage/frequency curve: %zu points, %d to %d mV\n", curve.size(), curve.front().volt_uv / 1000,
                curve.back().volt_uv / 1000);
    std::printf("  slot    mV     MHz  raw offset\n");
    for (const gao::VfPoint& p : curve)
        std::printf("  %4d  %4d  %6.1f  %d\n", p.index, p.volt_uv / 1000, p.freq_khz / 1000.0, p.raw_offset);
}

// Read-only: what the driver reports as the curve. Exit 1 when it cannot be read.
static int curve() {
    gao::Nvapi nvapi;
    if (!nvapi.Init()) { std::printf("NVAPI init failed: %s\n", nvapi.Error().c_str()); return 1; }
    const auto points = nvapi.ReadVfCurve(kGpu);
    if (!points) {
        std::printf("curve: unavailable (%s)%s\n", nvapi.Error().c_str(),
                    gao::app::is_elevated() ? "" : "; reading it may need an administrator shell");
        return 1;
    }
    print_curve(*points);
    return 0;
}

// Writes a flat top by hand: the point at or just above <mv> runs <mhz> and
// no point above it runs more; those are stored <tail_drop_mhz> lower (0: a
// top that is exactly flat). For the hardware checks; `gao --reset` undoes it.
static int curve_flatten(int mv, int mhz, int tail_drop_mhz) {
    const gao::app::TuningLock lock;
    if (!lock.owned()) { std::printf("another optimize is already running (in the app or on the command line)\n"); return 1; }
    // A running tray app would take the missing core offset for a driver
    // reset and write its saved tune over the flat top: it is told to leave
    // the card alone, as after `gao --reset`.
    gao::app::tell_tray(gao::app::TrayNotice::StockByChoice);
    gao::Nvml nvml;
    gao::Nvapi nvapi;
    if (!nvml.Init()) { std::printf("NVML init failed: %s\n", nvml.Error().c_str()); return 1; }
    if (!nvapi.Init()) { std::printf("NVAPI init failed: %s\n", nvapi.Error().c_str()); return 1; }
    const gao::GpuControl gpu = gao::make_gpu_control(nvml, nvapi, kGpu);
    const auto before = gpu.read_vf_curve();
    if (!before) { std::printf("curve: unavailable (%s)\n", nvapi.Error().c_str()); return 1; }
    int anchor = -1;
    for (const gao::VfPoint& p : *before)   // lowest voltage first
        if (p.volt_uv >= mv * 1000) { anchor = p.index; break; }
    if (anchor < 0) { std::printf("no curve point at or above %d mV\n", mv); return 1; }
    const gao::VfApplyResult r = gao::apply_flat_top(gpu, anchor, mhz * 1000, tail_drop_mhz * 1000);
    if (const auto after = gpu.read_vf_curve()) print_curve(*after);
    if (!r.ok) { std::printf("not applied: %s\n", r.why.c_str()); return 1; }
    std::printf("flat top applied: slot %d runs %d MHz, in %d write rounds -- OK\n", anchor, mhz, r.passes);
    return 0;
}

static int reset() {
    // Stock by choice: tell a running tray app first, or a watchdog tick
    // between the writes and the notice would take it for a TDR and re-apply.
    gao::app::tell_tray(gao::app::TrayNotice::StockByChoice);
    gao::Nvapi nvapi;
    if (!nvapi.Init()) { std::printf("NVAPI init failed: %s\n", nvapi.Error().c_str()); return 1; }
    // A flat top on the voltage/frequency curve first: the offset reset below
    // is not known to remove one.
    gao::GpuControl curve;
    curve.read_vf_curve = [&nvapi] { return nvapi.ReadVfCurve(kGpu); };
    curve.write_vf_offsets = [&nvapi](const std::vector<gao::VfOffset>& offsets) { return nvapi.WriteVfRawOffsets(kGpu, offsets); };
    std::string why;
    bool ok = gao::remove_vf_shape(curve, &why);
    if (!ok) std::printf("curve: %s\n", why.c_str());
    ok = nvapi.ResetOffsets(kGpu) && ok;
    const auto readback = nvapi.ReadOffsetsMhz(kGpu);
    std::printf("reset: requested core 0 MHz, mem 0 MHz\n");
    if (readback) std::printf("read back core %d MHz, mem %d MHz\n", readback->first, readback->second);
    else std::printf("read back unavailable (%s)\n", nvapi.Error().c_str());
    if (!ok) std::printf("%s\n", nvapi.Error().c_str());
    // Power limit back to the driver default, where the card supports it.
    gao::Nvml nvml;
    if (!nvml.Init()) {
        std::printf("power limit: not restored (NVML: %s)\n", nvml.Error().c_str());
        ok = false;
    } else if (nvml.PowerLimitRangePct(kGpu)) {
        const bool power_ok = nvml.SetPowerLimitPct(kGpu, 100);
        std::printf("power limit: default %s\n", power_ok ? "restored" : nvml.Error().c_str());
        ok = ok && power_ok;
    }
    std::printf("%s\n", ok ? "OK" : "MISMATCH");
    return ok ? 0 : 1;
}

// Runs the DX11 stress load for `seconds` of GPU time, printing one line per
// second, then the verdict. Changes no settings, so Ctrl+C is always safe.
// Assumes the first NVIDIA DXGI adapter is NVML device kGpu -- true on a
// single-GPU machine, which is all this CLI supports (see kGpu).
static int stress(int seconds, int max_temp_c, gao::StressSelftest selftest) {
    gao::Nvml nvml;
    if (!nvml.Init()) { std::printf("NVML init failed: %s\n", nvml.Error().c_str()); return 1; }
    gao::Stress load;
    if (!load.Init(selftest)) { std::printf("stress init failed: %s\n", load.Error().c_str()); return 1; }
    std::printf("stress: %s, %d s, abort above %d C\n", load.AdapterName().c_str(), seconds, max_temp_c);

    double t = 0, next_print = 1.0;
    long long window_its = 0;
    double window_s = 0;
    auto batch = [&] {
        const gao::StressBatch b = load.Batch();
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
    const gao::StabilityResult r = gao::run_stability(batch, read, seconds, max_temp_c);
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

static std::atomic<bool> g_abort{false};
static const gao::GpuControl* g_gpu = nullptr;   // set while --optimize runs

// Ctrl+C / Ctrl+Break do not kill the process during --optimize: they ask
// the search to stop, and the search restores stock before returning.
// Closing the window, logoff and shutdown do kill it (Windows allows ~5 s),
// so those reset to stock right here instead of leaving a candidate applied.
static BOOL WINAPI OnConsoleCtrl(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) {
        g_abort = true;
        std::printf("\nabort requested -- stopping the probe and restoring stock\n");
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

// Exit 0: updated, or already the latest. 1: the check or the update failed.
static int update() {
    const auto check = gao::app::check_for_update();
    if (!check.error.empty()) { std::printf("could not check for updates: %s\n", check.error.c_str()); return 1; }
    if (!check.release) { std::printf("up to date: this is %s and there is no newer release\n", gao::kVersion.data()); return 0; }
    std::printf("version %s is available (this is %s); downloading\n", check.release->version.c_str(), gao::kVersion.data());
    std::string message;
    const bool ok = gao::app::install_update(*check.release, &message);
    std::printf("%s\n", message.c_str());
    if (ok) std::printf("A running tray app keeps its old version until it is restarted.\n");
    return ok ? 0 : 1;
}

static int optimize(gao::Preset preset, const std::optional<gao::FanCurve>& fan_curve) {
    gao::app::OptimizeHooks hooks;
    hooks.aborted = [] { return g_abort.load(); };
    hooks.log = [](const std::string& msg) { std::printf("  %s\n", msg.c_str()); };
    hooks.active_gpu = [](const gao::GpuControl* gpu) {
        g_gpu = gpu;
        SetConsoleCtrlHandler(OnConsoleCtrl, gpu ? TRUE : FALSE);
    };
    const auto out = gao::app::run_optimize(preset, hooks, fan_curve);
    if (!out.ran) { std::printf("%s\n", out.error.c_str()); return 1; }
    const gao::OptimizeResult& r = out.result;
    if (!r.ok) {
        std::printf("RESULT: not applied -- %s (%s)\n", r.reason.c_str(),
                    r.stock_restored ? "card at stock" : "reset to stock FAILED, run `gao --reset`");
        if (r.driver_resets > 0) std::printf("  driver resets during this run: %d\n", r.driver_resets);
        return 1;
    }
    std::printf("RESULT: power %d %%, core +%d MHz (confirmed +%d), mem +%d MHz (confirmed +%d)\n",
                r.power_pct, r.core_mhz, r.core_confirmed, r.mem_mhz, r.mem_confirmed);
    if (r.driver_resets > 0) std::printf("  driver resets during this run: %d\n", r.driver_resets);
    // The two runs differ in length: the temperature and the fan speed of the short one are not those of a warm card.
    std::printf("  before: score=%.0f it/s  core=%d MHz  mem=%d MHz  peak=%d C  fan=%s  power=%d W  (%.0f s)\n",
                r.baseline.score, r.baseline.avg_core_mhz, r.baseline.avg_mem_mhz, r.baseline.peak_temp_c,
                gao::reading(r.baseline.end_fan_pct, " %").c_str(), r.baseline.avg_power_w, r.baseline.seconds);
    std::printf("  after:  score=%.0f it/s  core=%d MHz  mem=%d MHz  peak=%d C  fan=%s  power=%d W  (%.0f s)\n",
                r.soak.score, r.soak.avg_core_mhz, r.soak.avg_mem_mhz, r.soak.peak_temp_c,
                gao::reading(r.soak.end_fan_pct, " %").c_str(), r.soak.avg_power_w, r.soak.seconds);
    if (out.saved) {
        std::printf("Saved: `gao --apply` re-applies it, `gao --boot on` applies it at every logon.\n");
        if (const std::string note = gao::app::update_logon_copy(); !note.empty()) std::printf("%s\n", note.c_str());
    } else {
        std::printf("not saved: %s\n", out.save_note.c_str());
    }
    std::printf("Applied until reboot. `gao --reset` returns to stock.\n");
    return out.saved ? 0 : 2;   // 2: tuned and applied, but --apply and boot-apply cannot use it
}

// Finds the undervolt, leaves it applied and saves it as the profile.
static int undervolt() {
    gao::app::OptimizeHooks hooks;
    hooks.aborted = [] { return g_abort.load(); };
    hooks.log = [](const std::string& msg) { std::printf("  %s\n", msg.c_str()); };
    hooks.active_gpu = [](const gao::GpuControl* gpu) {
        g_gpu = gpu;
        SetConsoleCtrlHandler(OnConsoleCtrl, gpu ? TRUE : FALSE);
    };
    // A running tray app holds off for the whole run (the tuning lock), as
    // for --optimize, and afterwards finds the profile this run saved.
    const auto out = gao::app::run_undervolt(hooks);
    if (!out.ran) { std::printf("%s\n", out.error.c_str()); return 1; }
    const gao::UndervoltResult& r = out.result;
    if (!r.ok) {
        std::printf("RESULT: not applied -- %s (%s)\n", r.reason.c_str(),
                    r.stock_restored ? "card at stock" : "reset to stock FAILED, run `gao --reset`");
        if (r.driver_resets > 0) std::printf("  driver resets during this run: %d\n", r.driver_resets);
        return 1;
    }
    std::printf("RESULT: %d MHz at %d mV instead of %d mV (lowest stable %d mV, confirmed %d mV)\n", r.freq_khz / 1000,
                r.applied_uv / 1000, r.stock_uv / 1000, r.edge_uv / 1000, r.confirmed_uv / 1000);
    if (r.driver_resets > 0) std::printf("  driver resets during this run: %d\n", r.driver_resets);
    std::printf("  before: score=%.0f it/s  core=%d MHz  peak=%d C  fan=%s  power=%d W  (%.0f s)\n", r.baseline.score,
                r.baseline.avg_core_mhz, r.baseline.peak_temp_c, gao::reading(r.baseline.end_fan_pct, " %").c_str(),
                r.baseline.avg_power_w, r.baseline.seconds);
    std::printf("  after:  score=%.0f it/s  core=%d MHz  peak=%d C  fan=%s  power=%d W  (%.0f s)\n", r.after.score,
                r.after.avg_core_mhz, r.after.peak_temp_c, gao::reading(r.after.end_fan_pct, " %").c_str(),
                r.after.avg_power_w, r.after.seconds);
    if (out.saved) {
        std::printf("Saved: `gao --apply` re-applies it, `gao --boot on` applies it at every logon.\n");
        if (const std::string note = gao::app::update_logon_copy(); !note.empty()) std::printf("%s\n", note.c_str());
    } else {
        std::printf("not saved: %s\n", out.save_note.c_str());
    }
    std::printf("Applied until reboot. `gao --reset` returns to stock.\n");
    return out.saved ? 0 : 2;   // 2: found and applied, but --apply and boot-apply cannot use it
}

static int apply() {
    if (!gao::app::is_elevated()) { std::printf("--apply needs an elevated (administrator) shell\n"); return 1; }
    std::string why;
    if (!gao::app::prepare_state(&why)) { std::printf("%s\n", why.c_str()); return 1; }
    gao::Nvml nvml;
    if (!nvml.Init()) { std::printf("NVML init failed: %s\n", nvml.Error().c_str()); return 1; }
    gao::Nvapi nvapi;
    if (!nvapi.Init()) { std::printf("NVAPI init failed: %s\n", nvapi.Error().c_str()); return 1; }
    gao::Config cfg = gao::app::load_config();
    cfg.boot_strikes = 0;   // strikes only gate boot-apply
    const std::string driver = nvml.DriverVersion();
    const auto d = gao::decide_boot(cfg, driver, nvml.GpuUuid(kGpu));
    if (d != gao::BootDecision::Apply) {
        std::printf("not applied: %s\n", gao::app::decision_text(d, cfg, driver).c_str());
        return 1;
    }
    const gao::GpuControl gpu = gao::make_gpu_control(nvml, nvapi, kGpu);
    if (!gao::apply_profile(gpu, *cfg.profile, &why)) { std::printf("not applied: %s\n", why.c_str()); return 1; }
    gao::app::tell_tray(gao::app::TrayNotice::TuneApplied);   // a running tray keeps it applied from now on
    std::printf("applied %s\nOK\n", gao::app::profile_text(*cfg.profile).c_str());
    return 0;
}

static int boot(bool on) {
    std::string message;
    const bool ok = on ? gao::app::enable_boot(&message) : gao::app::disable_boot(&message);
    std::printf("%s\n", message.c_str());
    return ok ? 0 : 1;
}

static int status() {
    const auto text = gao::read_file(gao::config_path());
    const gao::Config cfg = text ? gao::from_json(*text) : gao::Config{};
    if (cfg.profile) std::printf("profile:    %s\n", gao::app::profile_text(*cfg.profile).c_str());
    else if (text) std::printf("profile:    none valid in %s\n", gao::config_path().string().c_str());
    else std::printf("profile:    none (run `gao --optimize`)\n");
    gao::Nvml nvml;
    const std::string driver = nvml.Init() ? nvml.DriverVersion() : std::string();
    if (cfg.profile) {
        std::printf("driver:     %s (%s)\n", driver.empty() ? "unknown" : driver.c_str(),
                    !driver.empty() && driver == cfg.profile->driver ? "matches" : "CHANGED -- run `gao --optimize` again");
        const std::string gpu_id = nvml.GpuUuid(kGpu);
        std::printf("gpu:        %s\n", !gpu_id.empty() && gpu_id == cfg.profile->gpu
                                         ? "matches the profile" : "DIFFERENT card or unknown -- run `gao --optimize` again");
    }
    // What the driver reports right now, whoever set it.
    gao::Nvapi nvapi;
    const auto applied = nvapi.Init() ? gao::make_gpu_control(nvml, nvapi, kGpu).read_applied() : std::nullopt;
    if (applied)
        std::printf("applied:    power %d %%, core %+d MHz, mem %+d MHz\n", applied->power_pct, applied->core_mhz, applied->mem_mhz);
    else
        std::printf("applied:    unknown (could not read the driver)\n");
    // The offsets above say nothing about a flat top: the curve does.
    if (cfg.profile && cfg.profile->undervolt) {
        const auto points = nvapi.ReadVfCurve(kGpu);
        const char* state = "unknown (the curve could not be read)";
        if (points) {
            switch (gao::curve_state(*points, cfg.profile->undervolt->volt_uv)) {
                case gao::CurveState::FlatTop: state = "applied"; break;
                case gao::CurveState::Stock: state = "not applied (the curve is at stock)"; break;
                case gao::CurveState::Other: state = "not applied (the curve carries other offsets)"; break;
            }
        }
        std::printf("undervolt:  %s\n", state);
    }
    if (const auto fan = nvml.ReadFan(kGpu))
        std::printf("fans:       %s\n", fan->manual ? ("manual " + std::to_string(fan->target_pct) + " %").c_str() : "driver control");
    const auto curve = gao::active_fan_curve(cfg);
    std::printf("fan curve:  %s", cfg.fan_control && curve ? "on" : "off");
    if (curve) {
        if (curve->stop_below_c) std::printf(", driver below %d C", *curve->stop_below_c);
        for (const gao::FanPoint& p : curve->points) std::printf(", %d C %d %%", p.temp_c, p.pct);
    }
    std::printf("\n");
    if (cfg.fan_min_pct > 0) std::printf("fan minimum: %d %% (learned: the fans stall below it)\n", cfg.fan_min_pct);
    std::printf("boot-apply: %s\n", gao::boot_task_exists() ? "on (logon task registered)" : "off");
    std::error_code ec;
    if (!std::filesystem::exists(gao::installed_exe_path(), ec)) std::printf("boot copy:  not installed\n");
    else if (gao::app::logon_copy_is_this_build()) std::printf("boot copy:  up to date\n");
    else std::printf("boot copy:  OUTDATED -- run `gao --boot on` to install this build\n");
    std::printf("strikes:    %d of %d\n", cfg.boot_strikes, gao::kMaxBootStrikes);
    const auto log = gao::read_lines(gao::boot_log_path());
    if (log && !log->empty()) std::printf("last boot:  %s\n", log->back().c_str());
    return 0;
}

// Emergency exit: every fan back to the driver, whatever set it.
static int fan_auto() {
    gao::Nvml nvml;
    if (!nvml.Init()) { std::printf("NVML init failed: %s\n", nvml.Error().c_str()); return 1; }
    const bool ok = nvml.SetFanAuto(kGpu);
    if (!ok) std::printf("%s\n", nvml.Error().c_str());
    std::printf("fans: %s\n", ok ? "driver control -- OK" : "MISMATCH");
    return ok ? 0 : 1;
}

static int bandwidth() {
    gao::Stress load;
    if (!load.Init()) { std::printf("stress init failed: %s\n", load.Error().c_str()); return 1; }
    const auto gbps = load.MeasureBandwidth();
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
static int stress_recreate() {
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

int main(int argc, char** argv) {
    // Before anything loads a DLL: System32 only (the delay-loaded
    // d3dcompiler_47.dll included), never the exe's own folder.
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);
    // The manifest makes the process code page UTF-8; match the console so
    // paths print correctly.
    SetConsoleOutputCP(CP_UTF8);
    if (argc > 1 && std::strcmp(argv[1], "--version") == 0) {
        std::printf("%s %s\n", gao::kProductName.data(), gao::kVersion.data());
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "--probe") == 0) return probe();
    if (argc > 2 && std::strcmp(argv[1], "--set-core") == 0) {
        int mhz = 0;
        if (!ParseIntArg(argv[2], &mhz)) { std::printf("--set-core expects an integer MHz value, got '%s'\n", argv[2]); return 1; }
        return set_offset("core offset", mhz, true);
    }
    if (argc > 2 && std::strcmp(argv[1], "--set-mem") == 0) {
        int mhz = 0;
        if (!ParseIntArg(argv[2], &mhz)) { std::printf("--set-mem expects an integer MHz value, got '%s'\n", argv[2]); return 1; }
        return set_offset("mem offset", mhz, false);
    }
    if (argc > 1 && std::strcmp(argv[1], "--reset") == 0) return reset();
    if (argc > 2 && std::strcmp(argv[1], "--stress") == 0) {
        int seconds = 0;
        if (!ParseIntArg(argv[2], &seconds) || seconds < 1 || seconds > 3600) {
            std::printf("--stress expects a duration of 1-3600 seconds, got '%s'\n", argv[2]);
            return 1;
        }
        int max_temp = 85;
        auto selftest = gao::StressSelftest::None;
        for (int i = 3; i < argc; i += 2) {
            if (i + 1 >= argc) { std::printf("%s expects a value\n", argv[i]); return 1; }
            if (std::strcmp(argv[i], "--max-temp") == 0) {
                if (!ParseIntArg(argv[i + 1], &max_temp) || max_temp < 40 || max_temp > 95) {
                    std::printf("--max-temp expects 40-95 C, got '%s'\n", argv[i + 1]);
                    return 1;
                }
            } else if (std::strcmp(argv[i], "--stress-selftest") == 0) {
                if (std::strcmp(argv[i + 1], "wrong") == 0) selftest = gao::StressSelftest::WrongResult;
                else { std::printf("--stress-selftest expects 'wrong', got '%s'\n", argv[i + 1]); return 1; }
            } else {
                std::printf("unknown --stress option '%s'\n", argv[i]);
                return 1;
            }
        }
        return stress(seconds, max_temp, selftest);
    }
    if (argc > 1 && std::strcmp(argv[1], "--bandwidth") == 0) return bandwidth();
    if (argc > 1 && std::strcmp(argv[1], "--stress-recreate") == 0) return stress_recreate();
    if (argc > 1 && std::strcmp(argv[1], "--apply") == 0) return apply();
    if (argc > 2 && std::strcmp(argv[1], "--boot") == 0) {
        if (std::strcmp(argv[2], "on") == 0) return boot(true);
        if (std::strcmp(argv[2], "off") == 0) return boot(false);
        std::printf("--boot expects on or off, got '%s'\n", argv[2]);
        return 1;
    }
    if (argc > 2 && std::strcmp(argv[1], "--fan") == 0) {
        if (std::strcmp(argv[2], "auto") == 0) return fan_auto();
        std::printf("--fan expects auto, got '%s'\n", argv[2]);
        return 1;
    }
    if (argc > 1 && std::strcmp(argv[1], "--status") == 0) return status();
    if (argc > 1 && std::strcmp(argv[1], "--curve") == 0) return curve();
    if (argc > 1 && std::strcmp(argv[1], "--undervolt") == 0) return undervolt();
    if (argc > 1 && std::strcmp(argv[1], "--curve-flatten") == 0) {
        int mv = 0, mhz = 0, drop = gao::kVfTailDropKhz / 1000;
        if (argc < 4 || !ParseIntArg(argv[2], &mv) || !ParseIntArg(argv[3], &mhz) ||
            (argc > 4 && (!ParseIntArg(argv[4], &drop) || drop < 0 || drop > 200))) {
            std::printf("--curve-flatten expects <mV> <MHz> [<MHz the points above it are stored lower, 0-200; default %d>]\n",
                        gao::kVfTailDropKhz / 1000);
            return 1;
        }
        return curve_flatten(mv, mhz, drop);
    }
    if (argc > 1 && std::strcmp(argv[1], "--update") == 0) return update();
    if (argc > 1 && std::strcmp(argv[1], "--optimize") == 0) {
        gao::Preset preset = gao::Preset::BestOfMyGpu;
        if (argc > 2) {
            const std::string p = argv[2];
            if (p == "best") preset = gao::Preset::BestOfMyGpu;
            else if (p == "quiet") preset = gao::Preset::Quiet;
            else if (p == "cool") preset = gao::Preset::CoolAndEfficient;
            else if (p == "max") preset = gao::Preset::MaxPerformance;
            else { std::printf("--optimize expects best, quiet, cool or max, got '%s'\n", argv[2]); return 1; }
        }
        std::optional<gao::FanCurve> fan_curve;
        if (argc > 3) {
            const auto fp = argc > 4 && std::strcmp(argv[3], "--fan-curve") == 0 ? gao::fan_preset_from_name(argv[4]) : std::nullopt;
            if (!fp) { std::printf("--optimize <profile> accepts --fan-curve silent|normal|cool|aggressive\n"); return 1; }
            fan_curve = gao::fan_preset_curve(*fp);
        }
        return optimize(preset, fan_curve);
    }
    std::printf("usage: gao [--version | --probe | --set-core <mhz> | --set-mem <mhz> | --reset\n"
                "            | --stress <sec> [--max-temp <c>] | --bandwidth\n"
                "            | --stress-recreate (loads the card for two 3 s runs around a device rebuild;\n"
                "              changes no setting; exit 0 = both STABLE, rebuilt and a clean first batch, 1 = anything else,\n"
                "              never --stress's 2 / 3)\n"
                "            | --optimize [best|quiet|cool|max [--fan-curve silent|normal|cool|aggressive]]\n"
                "            | --apply | --boot on|off | --fan auto | --status\n"
                "            | --curve (prints the voltage/frequency curve; changes nothing)\n"
                "            | --undervolt (the stock load clock on the lowest stable voltage; saved like --optimize,\n"
                "              and it replaces a saved overclock)\n"
                "            | --curve-flatten <mV> <MHz> [<MHz>] (the point at <mV> runs <MHz> and no point above it more;\n"
                "              the third number is how much lower those are stored, 0 for exactly flat; --reset undoes it)\n"
                "            | --update (installs the latest release into this folder, if there is a newer one)]\n");
    return argc > 1 ? 1 : 0;
}
