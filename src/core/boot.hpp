#pragma once
#include "core/config.hpp"
#include "core/types.hpp"
#include <string>

namespace gao {

// Logons that crashed within 2 minutes of applying before boot-apply gives up.
inline constexpr int kMaxBootStrikes = 3;

enum class BootDecision { Apply, NoProfile, TooManyStrikes, DriverChanged, GpuChanged };

// Order: no profile, strikes, driver, GPU. An empty (unknown) driver or GPU
// id never matches, so a failed NVML query cannot apply offsets that were
// tested on some other driver or card.
BootDecision decide_boot(const Config& c, const std::string& driver, const std::string& gpu);

// Sets power, core and memory from the profile; each GpuControl setter
// verifies by read-back. A setter the card lacks is fine only when the
// profile asks for stock on that dimension. Values outside the search
// bounds are refused before anything is written (gao.json is user-writable
// and boot-apply runs elevated). Any failure resets to stock and returns
// false with the reason in *why, including whether that reset worked.
// *left_clean (optional) says whether a failed apply left nothing of the
// profile on the card: refused before any write, or reset to stock after a
// failed one. False after a reset that failed, and after a successful apply.
bool apply_profile(const GpuControl& gpu, const Profile& p, std::string* why, bool* left_clean = nullptr);

}
