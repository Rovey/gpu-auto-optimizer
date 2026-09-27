#pragma once
#include "core/fan_curve.hpp"
#include "core/objectives.hpp"
#include <optional>
#include <string>

namespace gao {

// What --optimize found, as --apply and --boot-apply re-apply it.
struct Profile {
    Preset preset = Preset::BestOfMyGpu;
    int power_pct = 100;
    int core_mhz = 0;
    int mem_mhz = 0;
    std::string driver;     // driver version the profile was tested on
    std::string gpu;        // NVML UUID of the card it was tested on
    std::string saved_at;   // local time, "YYYY-MM-DD HH:MM"
    std::optional<FanCurve> fan_curve;   // the curve the tune was tested with; nullopt: driver control
};

// Everything that has to survive a reboot. Freeze ceilings are not here:
// they live in the journal.
struct Config {
    std::optional<Profile> profile;
    int boot_strikes = 0;   // logons that applied the profile and have not yet run 2 minutes
    std::optional<FanCurve> fan_curve;   // the active curve, once edited; otherwise active_fan_curve() picks one
    bool fan_control = false;            // drive the fans with the active curve
    int fan_min_pct = 0;                 // the lowest speed the fans hold, learned (0: none yet)
    std::string fan_min_gpu;             // the card it was learned on (NVML UUID)
};

std::string to_json(const Config& c);
// Never throws. Bad input yields defaults; a profile with any field missing
// or mistyped is treated as no profile rather than half a profile.
Config from_json(const std::string& text);

// The curve the fans follow: the edited one, else the one the tune was tested
// with, else the profile's default. nullopt without a profile.
std::optional<FanCurve> active_fan_curve(const Config& c);
// The minimum manual fan speed for this card: the learned one when it was
// learned on this card, never below what NVML reports.
int fan_min_for(const Config& c, const std::string& gpu, int nvml_min);

}
