#include "core/step_down.hpp"
#include <algorithm>

namespace gao {

namespace {
constexpr int kCoreGridMhz = 15;
constexpr int kMemGridMhz = 50;

// Integer division truncates toward zero, so offsets move towards zero.
int lowered_offset(int orig, int step, int grid) {
    const long long scaled = static_cast<long long>(orig) * (100 - kStepPct * step) / 100;
    return static_cast<int>(scaled / grid * grid);
}

UndervoltTune lowered_undervolt(const UndervoltTune& orig, int step, int grid_khz) {
    const int grid = std::max(grid_khz, 1);
    const int drop = static_cast<int>(static_cast<long long>(orig.raise_khz) * kStepPct * step / 100);
    if (drop == 0) return orig;   // nothing to give back: no snap to the grid either
    const int target = orig.freq_khz - drop;
    int units = target / grid;
    if (target % grid != 0 && target < 0) --units;   // floor
    UndervoltTune u = orig;
    // Never below the built-in curve: the flat top keeps a raise of 0 or more.
    u.freq_khz = std::max(units * grid, orig.freq_khz - std::max(orig.raise_khz, 0));
    u.raise_khz = orig.raise_khz - (orig.freq_khz - u.freq_khz);
    return u;
}
}

int current_step(const Profile& p) {
    return p.step_down ? p.step_down->step : 0;
}

FoundTune found_tune(const Profile& p) {
    if (p.step_down) return p.step_down->found;
    FoundTune f;
    f.core_mhz = p.core_mhz;
    f.mem_mhz = p.mem_mhz;
    f.undervolt = p.undervolt;
    return f;
}

Profile at_step(const Profile& p, int step, int grid_khz) {
    const FoundTune found = found_tune(p);
    step = std::clamp(step, 0, kMaxStep);
    Profile r = p;
    if (step == 0) {
        r.core_mhz = found.core_mhz;
        r.mem_mhz = found.mem_mhz;
        r.undervolt = found.undervolt;
        r.step_down.reset();
        return r;
    }
    r.core_mhz = lowered_offset(found.core_mhz, step, kCoreGridMhz);
    r.mem_mhz = lowered_offset(found.mem_mhz, step, kMemGridMhz);
    r.undervolt.reset();
    if (found.undervolt) r.undervolt = lowered_undervolt(*found.undervolt, step, grid_khz);
    StepDown sd;
    if (p.step_down) sd = *p.step_down;
    sd.step = step;
    sd.tested = false;
    sd.found = found;
    r.step_down = sd;
    return r;
}

bool can_step_down(const Profile& p, int grid_khz) {
    const int step = current_step(p);
    if (step >= kMaxStep) return false;
    const Profile next = at_step(p, step + 1, grid_khz);
    const Profile cur = at_step(p, step, grid_khz);
    return next.core_mhz != cur.core_mhz || next.mem_mhz != cur.mem_mhz || next.undervolt != cur.undervolt;
}

bool can_step_up(const Profile& p) {
    if (!p.step_down || p.step_down->step <= 0) return false;
    const auto& failed = p.step_down->failed_step;
    return !failed || p.step_down->step - 1 > *failed;
}

bool failed_its_test(const Profile& p) {
    return p.step_down && p.step_down->failed_step && *p.step_down->failed_step >= p.step_down->step;
}

std::optional<Profile> put_back_target(const std::optional<Profile>& before, const std::optional<Profile>& saved) {
    if (!before || !saved || failed_its_test(*saved)) return std::nullopt;
    return saved;   // `before` itself when nothing changed it
}

std::string step_text(int step) {
    if (step <= 0) return {};
    return "-" + std::to_string(kStepPct * step) + " % (step " + std::to_string(step) + ")";
}

}
