// The gao.exe commands that read or set the card directly, one call each:
// probe, offsets, curve, reset, apply, boot, status, fans and update.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "app/cli_commands.hpp"
#include "app/common.hpp"
#include "core/version.hpp"
#include "core/vf_curve.hpp"
#include "hw/app_files.hpp"
#include "hw/boot_task.hpp"
#include "hw/gpu_control.hpp"
#include "hw/nvapi.hpp"
#include "hw/nvidia_app.hpp"
#include "hw/nvml.hpp"
#include <filesystem>
#include <string>
#include <cstdio>

namespace gao::cli {

using gao::app::kGpu;

int probe() {
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

// Shared by --set-core and --set-mem. Prints "requested X, read back Y" and
// OK/MISMATCH. Always prints the read-back, even on success -- the number
// being visible is the point, not just the verdict.
int set_offset(const char* label, int mhz, bool core) {
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
int curve() {
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
// top that is exactly flat). With <core_mhz> the flat top sits on that core
// offset, which is set first and stays on the points below the anchor. For
// the hardware checks; `gao --reset` undoes it.
int curve_flatten(int mv, int mhz, int tail_drop_mhz, int core_mhz) {
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
    if (core_mhz != 0 && !gpu.set_core_offset(core_mhz)) {
        std::printf("core offset %+d MHz: %s\n", core_mhz, nvapi.Error().c_str());
        return 1;
    }
    const auto before = gpu.read_vf_curve();
    if (!before) { std::printf("curve: unavailable (%s)\n", nvapi.Error().c_str()); return 1; }
    int anchor = -1;
    for (const gao::VfPoint& p : *before)   // lowest voltage first
        if (p.volt_uv >= mv * 1000) { anchor = p.index; break; }
    if (anchor < 0) { std::printf("no curve point at or above %d mV\n", mv); return 1; }
    const gao::VfApplyResult r = gao::apply_flat_top(gpu, anchor, mhz * 1000, tail_drop_mhz * 1000, core_mhz * 1000);
    if (const auto after = gpu.read_vf_curve()) print_curve(*after);
    if (!r.ok) { std::printf("not applied: %s\n", r.why.c_str()); return 1; }
    std::printf("flat top applied: slot %d runs %d MHz, in %d write rounds -- OK\n", anchor, mhz, r.passes);
    return 0;
}

int reset() {
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

// Exit 0: updated, or already the latest. 1: the check or the update failed.
int update() {
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

int apply() {
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

int boot(bool on) {
    std::string message;
    const bool ok = on ? gao::app::enable_boot(&message) : gao::app::disable_boot(&message);
    std::printf("%s\n", message.c_str());
    return ok ? 0 : 1;
}

int status() {
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
            switch (gao::curve_state(*points, cfg.profile->undervolt->volt_uv, cfg.profile->core_mhz * 1000)) {
                case gao::CurveState::FlatTop: state = "applied"; break;
                case gao::CurveState::Stock: state = "not applied (the curve is at stock)"; break;
                case gao::CurveState::Other: state = "not applied (the curve carries other offsets)"; break;
            }
        }
        std::printf("undervolt:  %s\n", state);
    }
    if (const auto fan = nvml.ReadFan(kGpu))
        std::printf("fans:       %s\n", fan->manual ? ("manual " + std::to_string(fan->target_pct) + " %").c_str() : "driver control");
    if (const auto target = nvml.TempTarget(kGpu)) {
        const auto usual = nvapi.TempTargetDefaultC(kGpu);
        const std::string note = !usual ? std::string() : *usual == *target ? ", its default" : ", NOT its default of " + std::to_string(*usual) + " C";
        std::printf("temp target: %d C (the card's own: from there it slows itself down%s)\n", *target, note.c_str());
    } else {
        std::printf("temp target: not reported by this card\n");
    }
    const auto curve = gao::active_fan_curve(cfg);
    std::printf("fan curve:  %s", cfg.fan_control && curve ? "on" : "off");
    if (curve) {
        if (curve->stop_below_c) std::printf(", driver below %d C", *curve->stop_below_c);
        for (const gao::FanPoint& p : curve->points) std::printf(", %d C %d %%", p.temp_c, p.pct);
    }
    std::printf("\n");
    if (cfg.fan_min_pct > 0) std::printf("fan minimum: %d %% (learned: the fans stall below it)\n", cfg.fan_min_pct);
    std::printf("boot-apply: %s\n", gao::boot_task_exists() ? "on (logon task registered)" : "off");
    // No run starts while it is on (app/common.cpp).
    const std::optional<bool> nvidia = gao::nvidia_auto_tuning();
    std::printf("nvidia app: Automatic Tuning %s\n",
                !nvidia ? "unknown (its setting could not be read)" : *nvidia ? "ON -- switch it off before a run" : "off");
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
int fan_auto() {
    gao::Nvml nvml;
    if (!nvml.Init()) { std::printf("NVML init failed: %s\n", nvml.Error().c_str()); return 1; }
    const bool ok = nvml.SetFanAuto(kGpu);
    if (!ok) std::printf("%s\n", nvml.Error().c_str());
    std::printf("fans: %s\n", ok ? "driver control -- OK" : "MISMATCH");
    return ok ? 0 : 1;
}

}  // namespace gao::cli
