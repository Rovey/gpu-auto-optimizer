#pragma once
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
    std::string saved_at;   // local time, "YYYY-MM-DD HH:MM"
};

// Everything that has to survive a reboot. Freeze ceilings are not here:
// they live in the journal.
struct Config {
    std::optional<Profile> profile;
    int boot_strikes = 0;   // logons that applied the profile and have not yet run 2 minutes
};

std::string to_json(const Config& c);
// Never throws. Bad input yields defaults; a profile with any field missing
// or mistyped is treated as no profile rather than half a profile.
Config from_json(const std::string& text);

}
