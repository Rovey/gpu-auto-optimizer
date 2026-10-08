#include "core/boot.hpp"
#include "core/search.hpp"
#include "core/undervolt.hpp"
#include "core/vf_curve.hpp"

#include <algorithm>

namespace gao {

BootDecision decide_boot(const Config& c, const std::string& driver, const std::string& gpu) {
    if (!c.profile) return BootDecision::NoProfile;
    if (c.boot_strikes >= kMaxBootStrikes) return BootDecision::TooManyStrikes;
    if (driver.empty() || driver != c.profile->driver) return BootDecision::DriverChanged;
    if (gpu.empty() || gpu != c.profile->gpu) return BootDecision::GpuChanged;
    return BootDecision::Apply;
}

namespace {
// Whether the profile's flat top fits this card's curve, judged on the
// built-in curve: every offset goes first. `raise_khz` in the profile is how
// far the anchor ran above its built-in frequency when the undervolt was
// tested, a core offset under it included. Empty when it fits, otherwise why
// not.
std::string undervolt_fits(const GpuControl& gpu, const UndervoltTune& u) {
    std::string why;
    if (!clear_vf_curve(gpu, &why)) return why;
    const auto curve = gpu.read_vf_curve();
    if (!curve) return "the curve could not be read";
    const auto anchor = std::find_if(curve->begin(), curve->end(), [&](const VfPoint& pt) { return pt.volt_uv == u.volt_uv; });
    if (anchor == curve->end()) return "the card's curve has no point at " + std::to_string(u.volt_uv / 1000) + " mV";
    const int raise_khz = u.freq_khz - anchor->freq_khz;
    if (raise_khz > u.raise_khz + kUvClockSlackKhz)
        return "the curve point would be raised by " + std::to_string(raise_khz / 1000) + " MHz; the undervolt was tested with " +
               std::to_string(u.raise_khz / 1000) + " MHz";
    return {};
}

// The profile's flat top, over the core offset that was just set (base_raw:
// that offset as the curve stores it). Empty on success, otherwise why not.
std::string apply_undervolt(const GpuControl& gpu, const UndervoltTune& u, int base_raw) {
    const auto curve = gpu.read_vf_curve();
    if (!curve) return "the curve could not be read";
    const auto anchor = std::find_if(curve->begin(), curve->end(), [&](const VfPoint& pt) { return pt.volt_uv == u.volt_uv; });
    if (anchor == curve->end()) return "the card's curve has no point at " + std::to_string(u.volt_uv / 1000) + " mV";
    const VfApplyResult written = apply_flat_top(gpu, anchor->index, u.freq_khz, kVfTailDropKhz, base_raw);
    // Its reason ends with where the curve was left; the caller resets the whole card and says so itself.
    return written.ok ? std::string() : written.why.substr(0, written.why.find(" -- "));
}
}

bool apply_profile(const GpuControl& gpu, const Profile& p, std::string* why, bool* left_clean) {
    if (left_clean) *left_clean = false;
    // A reported range may only widen what a profile may apply, never narrow it
    // below the built-in limits: it comes from an undocumented buffer, and a
    // low reading must not refuse a profile saved before it was read (every
    // refusal at logon is a boot strike). The driver still refuses a write it
    // does not accept, and the read-back catches that.
    const SearchBounds bounds = search_bounds(gpu);
    const int core_limit = std::max(bounds.core_max_mhz, kCoreFallbackMaxMhz);
    const int mem_limit = std::max(bounds.mem_max_mhz, kMemFallbackMaxMhz);
    if (p.power_pct < 50 || p.power_pct > 150 || p.core_mhz < 0 || p.core_mhz > core_limit || p.mem_mhz < 0 ||
        p.mem_mhz > mem_limit) {
        if (why) *why = "profile values out of range; nothing applied";
        if (left_clean) *left_clean = true;
        return false;
    }
    if (p.undervolt) {
        const UndervoltTune& u = *p.undervolt;
        const char* refused = u.volt_uv <= 0 || u.freq_khz <= 0 || u.raise_khz <= 0 || u.raise_khz > kVfMaxRaiseKhz
                                  ? "profile values out of range; nothing applied"
                              : !gpu.read_vf_curve || !gpu.write_vf_offsets
                                  ? "this card or driver does not offer the voltage/frequency curve; nothing applied"
                              : nullptr;
        if (refused) {
            if (why) *why = refused;
            if (left_clean) *left_clean = true;
            return false;
        }
    }
    auto fail = [&](const std::string& reason) {
        const bool reset = gpu.reset_to_stock && gpu.reset_to_stock();
        if (left_clean) *left_clean = reset;
        if (why) *why = reason + (reset ? " -- card at stock" : " -- reset to stock FAILED, run `gao --reset`");
        return false;
    };
    // A missing setter is only acceptable when the profile wants stock there.
    auto set = [](const std::function<bool(int)>& setter, int value, int stock) {
        return setter ? setter(value) : value == stock;
    };
    // An offset write that asks for what the driver already reports may
    // change nothing, and would then leave a flat top where it is.
    std::string curve_why;
    if (!remove_vf_shape(gpu, &curve_why)) return fail(curve_why);
    if (p.undervolt) {   // before the clocks are written: a flat top that does not fit ends the apply here
        const std::string unfit = undervolt_fits(gpu, *p.undervolt);
        if (!unfit.empty()) return fail("the undervolt was not applied: " + unfit);
    }
    if (!set(gpu.set_power_limit, p.power_pct, 100))
        return fail("setting power " + std::to_string(p.power_pct) + " % failed" +
                    (gpu.set_power_limit ? "" : " (no power control on this card)"));
    if (!set(gpu.set_core_offset, p.core_mhz, 0)) return fail("setting core +" + std::to_string(p.core_mhz) + " MHz failed");
    if (!set(gpu.set_mem_offset, p.mem_mhz, 0)) return fail("setting mem +" + std::to_string(p.mem_mhz) + " MHz failed");
    if (p.undervolt) {
        // The core offset and the curve are one table (hardware check 65):
        // the offset that was just set is every point's offset now, and the
        // flat top is written over it, not next to it.
        const std::string failed = apply_undervolt(gpu, *p.undervolt, p.core_mhz * 1000);
        if (!failed.empty()) return fail("the undervolt was not applied: " + failed);
    }
    return true;
}

}
