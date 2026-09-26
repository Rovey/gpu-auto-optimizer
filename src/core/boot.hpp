#pragma once
#include "core/config.hpp"
#include "core/types.hpp"
#include <string>

namespace gao {

// Logons that crashed within 2 minutes of applying before boot-apply gives up.
inline constexpr int kMaxBootStrikes = 3;

enum class BootDecision { Apply, NoProfile, TooManyStrikes, DriverChanged };

// Order: no profile, then strikes, then driver. An empty (unknown) driver
// version never matches, so a failed NVML query cannot apply offsets that
// were tested on some other driver.
BootDecision decide_boot(const Config& c, const std::string& driver);

// Sets power, core and memory from the profile; each GpuControl setter
// verifies by read-back. A setter the card lacks is fine only when the
// profile asks for stock on that dimension. Any failure resets to stock and
// returns false with the reason in *why.
bool apply_profile(const GpuControl& gpu, const Profile& p, std::string* why);

}
