#include "core/watchdog.hpp"
#include <cstdlib>

namespace gao {

namespace {
// Offsets read back exactly; the power limit comes back in watts and is
// rounded to a percentage, so allow 1 %.
bool same(const AppliedState& a, int core, int mem, int power) {
    return a.core_mhz == core && a.mem_mhz == mem && std::abs(a.power_pct - power) <= 1;
}
}

WatchAction Watchdog::check(const Profile& p, const std::optional<AppliedState>& seen, bool driver_matches,
                            bool gpu_matches, Clock::time_point now) {
    if (!driver_matches || !gpu_matches) {
        if (notified_driver_) return WatchAction::None;
        notified_driver_ = true;
        return WatchAction::NotifyDriverChanged;
    }
    if (!seen) return WatchAction::None;
    if (same(*seen, p.core_mhz, p.mem_mhz, p.power_pct)) {
        backed_off_ = false;
        return WatchAction::None;
    }
    // Every field either ours or stock: a TDR or driver reset undid (part of)
    // the tune -- it may reset the offsets and keep the power limit, or the
    // reverse. Re-apply, unless it keeps happening -- then the tune itself is
    // the likely cause.
    const bool core_ok = seen->core_mhz == p.core_mhz || seen->core_mhz == 0;
    const bool mem_ok = seen->mem_mhz == p.mem_mhz || seen->mem_mhz == 0;
    const bool power_ok = std::abs(seen->power_pct - p.power_pct) <= 1 || std::abs(seen->power_pct - 100) <= 1;
    if (core_ok && mem_ok && power_ok) {
        if (gave_up_) return WatchAction::None;
        while (!reapplies_.empty() && now - reapplies_.front() >= std::chrono::hours(1)) reapplies_.pop_front();
        if (static_cast<int>(reapplies_.size()) >= kMaxReappliesPerHour) {
            gave_up_ = true;
            return WatchAction::GiveUpUnstable;
        }
        reapplies_.push_back(now);
        return WatchAction::Reapply;
    }
    // Neither ours nor stock: another tool is tuning this card. Fighting it
    // means a write war; step aside and say so once.
    if (backed_off_) return WatchAction::None;
    backed_off_ = true;
    return WatchAction::BackOffForeign;
}

}
