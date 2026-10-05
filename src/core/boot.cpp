#include "core/boot.hpp"
#include "core/search.hpp"

#include <algorithm>

namespace gao {

BootDecision decide_boot(const Config& c, const std::string& driver, const std::string& gpu) {
    if (!c.profile) return BootDecision::NoProfile;
    if (c.boot_strikes >= kMaxBootStrikes) return BootDecision::TooManyStrikes;
    if (driver.empty() || driver != c.profile->driver) return BootDecision::DriverChanged;
    if (gpu.empty() || gpu != c.profile->gpu) return BootDecision::GpuChanged;
    return BootDecision::Apply;
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
    if (!set(gpu.set_power_limit, p.power_pct, 100))
        return fail("setting power " + std::to_string(p.power_pct) + " % failed" +
                    (gpu.set_power_limit ? "" : " (no power control on this card)"));
    if (!set(gpu.set_core_offset, p.core_mhz, 0)) return fail("setting core +" + std::to_string(p.core_mhz) + " MHz failed");
    if (!set(gpu.set_mem_offset, p.mem_mhz, 0)) return fail("setting mem +" + std::to_string(p.mem_mhz) + " MHz failed");
    return true;
}

}
