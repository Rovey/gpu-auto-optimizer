#pragma once
#include "core/config.hpp"
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
// step < kMaxStep and the next step changes a value.
bool can_step_down(const Profile& p, int grid_khz);
// step > 0 and (no failed_step or step - 1 > *failed_step).
bool can_step_up(const Profile& p);
// "-10 % (step 2)"; "" at 0. ASCII hyphen-minus: also used in logs.
std::string step_text(int step);

}
