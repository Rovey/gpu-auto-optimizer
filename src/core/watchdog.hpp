#pragma once
#include "core/config.hpp"
#include "core/types.hpp"
#include "core/vf_curve.hpp"
#include <chrono>
#include <deque>
#include <optional>

namespace gao {

enum class WatchAction {
    None,
    Reapply,              // back at stock with the same driver and card: re-apply the profile
    GiveUpUnstable,       // too many resets in an hour: set the card to stock, stop re-applying, tell the user
    BackOffForeign,       // someone else's settings: leave them alone, tell the user once
    NotifyDriverChanged,  // the profile was tuned on another driver or card: tell the user once
};

// True when what the card reports is this profile. A profile with an
// undervolt is judged by the curve (`curve`: its state seen from the
// profile's anchor; nullopt when it could not be read), not by the core
// offset the driver reports next to it.
bool tune_applied(const Profile& p, const AppliedState& seen, const std::optional<CurveState>& curve);

// Decides what the resident tray process does about the tune it is supposed
// to keep applied. Pure: the caller reads the hardware and acts on the answer.
class Watchdog {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr int kMaxReappliesPerHour = 3;

    // curve: as for tune_applied; only read for a profile with an undervolt.
    // driver_reset: the driver was reset since the last call. A reset that
    // undid the tune and one that left it applied count the same, once:
    // more than kMaxReappliesPerHour of them within an hour and the tune
    // is given up.
    WatchAction check(const Profile& p, const std::optional<AppliedState>& seen, bool driver_matches,
                      bool gpu_matches, Clock::time_point now, const std::optional<CurveState>& curve = std::nullopt,
                      bool driver_reset = false);
    bool gave_up() const { return gave_up_; }
    // For a process that restarts itself and goes on keeping the tune applied:
    // the resets counted in the last hour, and that count handed to the
    // watchdog of the next process (never more than a watchdog can hold).
    // There they count as if they had just happened, so the tune is given up
    // no later than it would have been without the restart.
    int recent(Clock::time_point now);
    void seed(int resets, Clock::time_point now);

private:
    bool count(Clock::time_point now);   // false: one too many within the hour
    std::deque<Clock::time_point> reapplies_;   // the resets of the last hour
    bool gave_up_ = false;
    bool backed_off_ = false;
    bool notified_driver_ = false;
};

}
