#pragma once
#include "core/fan_curve.hpp"
#include "core/objectives.hpp"
#include <optional>
#include <string>

namespace gao {

// A flat top on the voltage/frequency curve (core/vf_curve.hpp), as the
// undervolt search found it. The point is named by its voltage: that is the
// card's own, where a slot number is the driver's.
struct UndervoltTune {
    int volt_uv = 0;     // the curve point the flat top is anchored at
    int freq_khz = 0;    // the clock that point and every point above it run
    int raise_khz = 0;   // how far the anchor ran above its built-in frequency when it was tested, a core offset under it included
    bool operator==(const UndervoltTune&) const = default;
};

// A five-minute run under full load, as the dashboard compares the tune with
// stock: the run's own numbers, -1 (0 for the score) where it had none.
struct MeasuredRun {
    double score = 0;   // iterations per second
    int core_mhz = -1;
    int mem_mhz = -1;
    int peak_temp_c = -1;
    int fan_pct = -1;   // at the end of the run
    int power_w = -1;
    bool operator==(const MeasuredRun&) const = default;
};

// What the run that saved the tune measured: the card at stock, and with the tune.
struct AgainstStock {
    MeasuredRun stock;
    MeasuredRun tuned;
    bool operator==(const AgainstStock&) const = default;
};

// What --optimize or --undervolt found, as --apply and --boot-apply re-apply it.
struct Profile {
    Preset preset = Preset::BestOfMyGpu;
    int power_pct = 100;
    int core_mhz = 0;
    int mem_mhz = 0;
    std::string driver;     // driver version the profile was tested on
    std::string gpu;        // NVML UUID of the card it was tested on
    std::string saved_at;   // local time, "YYYY-MM-DD HH:MM"
    std::optional<FanCurve> fan_curve;   // the curve the tune was tested with; nullopt: tuned before fan control
    // The temperature limit the tune was found with, when the card reached
    // more than the preset's own at stock (temp_ceiling). Empty: the preset's.
    // The fan curve above is drawn against it.
    std::optional<int> temp_ceiling_c;
    std::optional<UndervoltTune> undervolt;   // set: a flat top on the curve, over core_mhz when that is not 0
    std::optional<AgainstStock> against_stock;   // empty: saved by a build before this, or not measured
};

// Everything that has to survive a reboot. Freeze ceilings are not here:
// they live in the journal.
struct Config {
    std::optional<Profile> profile;
    int boot_strikes = 0;   // logons that applied the profile and have not yet run 2 minutes
    std::optional<FanCurve> fan_curve;   // the active curve, once edited; otherwise active_fan_curve() picks one
    bool fan_control = false;            // drive the fans with the active curve
    bool update_check = true;            // ask GitHub for a newer release when the app starts
    int fan_min_pct = 0;                 // the lowest speed the fans hold, learned (0: none yet)
    std::string fan_min_gpu;             // the card it was learned on (NVML UUID)
};

std::string to_json(const Config& c);
// Never throws. Bad input yields defaults; a profile with any field missing
// or mistyped is treated as no profile rather than half a profile. That
// includes its undervolt: one that cannot be read whole, or the undervolt
// preset without one, would otherwise apply as plain stock and look applied.
Config from_json(const std::string& text);

// The temperature limit of a saved profile on its card: the fans run at 100 %
// from here. The preset's own, or the ceiling the tune was found with; never
// below the first, never above kStockTempC.
int profile_ceiling(const Profile& p);

// The curve the fans follow: the edited one, else the one the tune was tested
// with, else the profile's default. nullopt without a profile.
std::optional<FanCurve> active_fan_curve(const Config& c);
// The minimum manual fan speed for this card: the learned one when it was
// learned on this card, never below what NVML reports.
int fan_min_for(const Config& c, const std::string& gpu, int nvml_min);

}
