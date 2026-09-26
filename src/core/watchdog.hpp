#pragma once
#include "core/config.hpp"
#include "core/types.hpp"
#include <chrono>
#include <deque>
#include <optional>

namespace gao {

enum class WatchAction {
    None,
    Reapply,              // back at stock with the same driver and card: re-apply the profile
    GiveUpUnstable,       // too many resets in an hour: stop re-applying, tell the user
    BackOffForeign,       // someone else's settings: leave them alone, tell the user once
    NotifyDriverChanged,  // the profile was tuned on another driver or card: tell the user once
};

// Decides what the resident tray process does about the tune it is supposed
// to keep applied. Pure: the caller reads the hardware and acts on the answer.
class Watchdog {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr int kMaxReappliesPerHour = 3;

    WatchAction check(const Profile& p, const std::optional<AppliedState>& seen, bool driver_matches,
                      bool gpu_matches, Clock::time_point now);
    bool gave_up() const { return gave_up_; }

private:
    std::deque<Clock::time_point> reapplies_;
    bool gave_up_ = false;
    bool backed_off_ = false;
    bool notified_driver_ = false;
};

}
