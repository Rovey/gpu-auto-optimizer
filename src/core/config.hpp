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
// counted_s > 0: score, clocks and power are the averages of the run's last
// counted_s seconds (StabilityResult::counted), the part an undervolt was
// judged on, and avg_temp_c and avg_fan_pct are the averages over it too.
// 0: they are of the whole run, and there are no averages of temperature
// and fan speed. peak_temp_c and fan_pct are of the whole run either way.
struct MeasuredRun {
    double score = 0;   // iterations per second
    int core_mhz = -1;
    int mem_mhz = -1;
    int peak_temp_c = -1;
    int fan_pct = -1;   // at the end of the run
    int power_w = -1;
    double counted_s = 0;
    int avg_temp_c = -1;
    int avg_fan_pct = -1;
    bool operator==(const MeasuredRun&) const = default;
};

// What the run that saved the tune measured: the card at stock, and with the tune.
struct AgainstStock {
    MeasuredRun stock;
    MeasuredRun tuned;
    bool operator==(const AgainstStock&) const = default;
};

// The tune as the run found it, before any step lowered it.
struct FoundTune {
    int core_mhz = 0;
    int mem_mhz = 0;
    std::optional<UndervoltTune> undervolt;
    bool operator==(const FoundTune&) const = default;
};

inline constexpr int kMaxStep = 10;   // the largest step a profile can be lowered to

// A saved tune that was lowered in 5 % steps from what the run found.
struct StepDown {
    int step = 0;                     // 1..kMaxStep (0 is never stored)
    bool tested = false;              // this step held a 5-minute test
    std::optional<int> failed_step;   // the largest step that failed a test; steps <= it are never applied
    FoundTune found;
    bool operator==(const StepDown&) const = default;
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
    std::optional<StepDown> step_down;           // empty: the profile is the tune the run found
    bool operator==(const Profile&) const = default;
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

// A run that did not finish (stopped, failed, or ended at stock) keeps nothing:
// gao.json goes back to what it held before the run. An All in one run saves
// its overclock and undervolt as its steps end; this takes them back. The
// run's own fields (the profile, the fan curve and whether it drives the
// fans, the strikes that belong to the profile) come from `before`; what was
// learned or set meanwhile (the fans' minimum, the update check) stays as in
// `now`. nullopt: `now` already holds the tune from before the run.
std::optional<Config> undo_unfinished_run(const Config& before, const Config& now);

}
