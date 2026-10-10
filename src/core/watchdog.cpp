#include "core/watchdog.hpp"
#include <algorithm>
#include <cstdlib>

namespace gao {

namespace {
// The power limit comes back in watts and is rounded to a percentage, so allow 1 %.
bool power_is(const AppliedState& a, int pct) { return std::abs(a.power_pct - pct) <= 1; }
// The clock part of the tune: a core offset, read back exactly, or a flat top.
bool core_is_ours(const Profile& p, const AppliedState& a, const std::optional<CurveState>& curve) {
    return p.undervolt ? curve == CurveState::FlatTop : a.core_mhz == p.core_mhz;
}
bool core_is_stock(const Profile& p, const AppliedState& a, const std::optional<CurveState>& curve) {
    return p.undervolt ? curve == CurveState::Stock : a.core_mhz == 0;
}
}

bool tune_applied(const Profile& p, const AppliedState& seen, const std::optional<CurveState>& curve) {
    return core_is_ours(p, seen, curve) && seen.mem_mhz == p.mem_mhz && power_is(seen, p.power_pct);
}

int Watchdog::recent(Clock::time_point now) {
    while (!reapplies_.empty() && now - reapplies_.front() >= std::chrono::hours(1)) reapplies_.pop_front();
    return static_cast<int>(reapplies_.size());
}

void Watchdog::seed(int resets, Clock::time_point now) {
    reapplies_.assign(static_cast<std::size_t>(std::clamp(resets, 0, kMaxReappliesPerHour)), now);
}

bool Watchdog::count(Clock::time_point now) {
    if (recent(now) >= kMaxReappliesPerHour) {
        gave_up_ = true;
        return false;
    }
    reapplies_.push_back(now);
    return true;
}

WatchAction Watchdog::check(const Profile& p, const std::optional<AppliedState>& seen, bool driver_matches,
                            bool gpu_matches, Clock::time_point now, const std::optional<CurveState>& curve,
                            bool driver_reset) {
    if (!driver_matches || !gpu_matches) {
        if (notified_driver_) return WatchAction::None;
        notified_driver_ = true;
        return WatchAction::NotifyDriverChanged;
    }
    if (!seen || (p.undervolt && !curve)) return WatchAction::None;
    if (tune_applied(p, *seen, curve)) {
        backed_off_ = false;
        // Still applied after a driver reset: nothing to write, but the
        // reset counts. A tune the driver keeps across its resets would
        // otherwise never be found unstable.
        if (!driver_reset || gave_up_ || count(now)) return WatchAction::None;
        return WatchAction::GiveUpUnstable;
    }
    // Every field either ours or stock: a TDR or driver reset undid (part of)
    // the tune -- it may reset the offsets and keep the power limit, or the
    // reverse. Re-apply, unless it keeps happening -- then the tune itself is
    // the likely cause.
    const bool core_ok = core_is_ours(p, *seen, curve) || core_is_stock(p, *seen, curve);
    const bool mem_ok = seen->mem_mhz == p.mem_mhz || seen->mem_mhz == 0;
    const bool power_ok = power_is(*seen, p.power_pct) || power_is(*seen, 100);
    if (core_ok && mem_ok && power_ok) {
        if (gave_up_) return WatchAction::None;
        return count(now) ? WatchAction::Reapply : WatchAction::GiveUpUnstable;
    }
    // Neither ours nor stock: another tool is tuning this card. Fighting it
    // means a write war; step aside and say so once.
    if (backed_off_) return WatchAction::None;
    backed_off_ = true;
    return WatchAction::BackOffForeign;
}

}
