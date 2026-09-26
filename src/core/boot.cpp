#include "core/boot.hpp"

namespace gao {

BootDecision decide_boot(const Config& c, const std::string& driver) {
    if (!c.profile) return BootDecision::NoProfile;
    if (c.boot_strikes >= kMaxBootStrikes) return BootDecision::TooManyStrikes;
    if (driver.empty() || driver != c.profile->driver) return BootDecision::DriverChanged;
    return BootDecision::Apply;
}

bool apply_profile(const GpuControl& gpu, const Profile& p, std::string* why) {
    auto fail = [&](const std::string& reason) {
        if (gpu.reset_to_stock) gpu.reset_to_stock();
        if (why) *why = reason;
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
