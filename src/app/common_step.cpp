#include "app/common_internal.hpp"
#include "hw/app_files.hpp"
#include "core/step_down.hpp"
#include "core/vf_curve.hpp"

namespace gao::app {

int grid_khz_of(const GpuControl& gpu) {
    if (gpu.read_vf_curve) {
        if (const auto curve = gpu.read_vf_curve()) {
            const int grid = clock_grid_khz(*curve);
            if (grid > 0) return grid;
        }
    }
    return kDefaultGridKhz;
}

std::optional<Profile> step_base(const Config& c, int delta, const std::string& driver, const std::string& gpu_id,
                                 int nvidia_gpus, std::string* refusal) {
    Config cfg = c;
    cfg.boot_strikes = 0;   // as saved_tune_for: strikes only gate the logon apply
    const BootDecision d = decide_boot(cfg, driver, gpu_id, nvidia_gpus);
    if (d == BootDecision::Apply || (d == BootDecision::FailedTest && delta < 0)) return cfg.profile;
    if (refusal) *refusal = decision_text(d, cfg, driver);
    return std::nullopt;
}

std::string step_not_applied(const std::string& why, bool left_clean) {
    // apply_profile's reasons after a write end with what the reset to stock did
    // (" -- card at stock" or " -- reset to stock FAILED ..."); a refusal before any write does not.
    const bool untouched = left_clean && why.find(" -- ") == std::string::npos;
    return std::string(untouched ? "not applied, the card was not changed: " : "not applied: ") + why;
}

StepOutcome step_tune(const GpuControl& gpu, int delta, const std::string& driver, const std::string& gpu_id, int nvidia_gpus) {
    StepOutcome out;
    auto refuse = [&](const std::string& why) { out.message = why; return out; };
    if (delta != 1 && delta != -1) return refuse("the step changes by one at a time (+1 or -1)");
    const TuningLock lock;
    if (!lock.owned()) return refuse(kAlreadyRunning);
    if (!is_elevated()) return refuse("changing the step sets clocks and power limits and needs administrator rights");
    std::string why;
    if (!prepare_state(&why)) return refuse(why);
    const auto tune = step_base(load_config(), delta, driver, gpu_id, nvidia_gpus, &why);
    if (!tune) return refuse(why);
    const int grid = grid_khz_of(gpu);
    if (delta < 0 && !can_step_down(*tune, grid)) return refuse("the tune cannot be lowered any further");
    if (delta > 0 && !can_step_up(*tune)) return refuse("the tune cannot be raised any further");
    const int step = current_step(*tune) + (delta < 0 ? 1 : -1);
    const Profile next = at_step(*tune, step, grid);
    bool left_clean = false;
    if (!apply_profile(gpu, next, &why, &left_clean)) return refuse(step_not_applied(why, left_clean));
    Config latest = load_config();
    latest.profile = next;
    latest.boot_strikes = 0;
    const std::string what = step == 0 ? std::string("stepped up to the tune the run found")
                                       : std::string(delta < 0 ? "stepped down to " : "stepped up to ") + step_text(step);
    out.message = what + ": " + profile_text(next);
    if (!save_config(latest)) {
        out.message += " -- but that could not be saved (" + config_path().string() + "); the card keeps the applied step";
        out.applied_not_saved = true;
        return out;
    }
    out.ok = true;
    out.profile = next;
    boot_log(out.message);
    return out;
}

}
