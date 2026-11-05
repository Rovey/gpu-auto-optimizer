#pragma once
#include "core/config.hpp"
#include <optional>
#include <string>

namespace gao {

inline constexpr int kStepPct = 5;             // each step lowers the gain over stock by this much
inline constexpr int kDefaultGridKhz = 15000;  // the clock grid when the card's own is not known

int current_step(const Profile& p);       // 0 without step_down
FoundTune found_tune(const Profile& p);   // step_down->found, or the profile's own values

// The profile at `step` (0..kMaxStep, clamped): the main fields hold the lowered
// values and step_down is set (tested false, failed_step and found kept), or
// cleared at 0. Everything else is copied. Steps never compound: they always
// start from the found tune.
Profile at_step(const Profile& p, int step, int grid_khz);
// step < kMaxStep and the next step changes a value. A flat top is never
// lowered below the built-in curve (raise_khz stays >= 0), so a step that only
// that clamp holds back changes nothing.
bool can_step_down(const Profile& p, int grid_khz);
// step > 0 and (no failed_step or step - 1 > *failed_step).
bool can_step_up(const Profile& p);
// The saved tune failed its test: its step is at or below failed_step. Never
// applied (decide_boot, the watchdog, a stopped run); a step down leaves it.
bool failed_its_test(const Profile& p);
// What a run that was stopped and kept nothing puts back on the card. `before`:
// the tune that was on it when the run began (nullopt: none). `saved`: the
// saved tune now. The same tune: `before`. Another one (a stepped test that was
// stopped saved the step under test): that one, never `before`, which failed.
// Nullopt when there is nothing to put back or it failed its test.
std::optional<Profile> put_back_target(const std::optional<Profile>& before, const std::optional<Profile>& saved);
// "-10 % (step 2)"; "" at 0. ASCII hyphen-minus: also used in logs.
std::string step_text(int step);

}
