#include "doctest/doctest.h"
#include "core/watchdog.hpp"

using namespace gao;
using namespace std::chrono_literals;

namespace {
Profile tuned() {
    Profile p;
    p.power_pct = 105;
    p.core_mhz = 135;
    p.mem_mhz = 1050;
    return p;
}
const AppliedState kOurs{135, 1050, 105};
const AppliedState kStock{0, 0, 100};
const Watchdog::Clock::time_point t0{};
}

TEST_CASE("an applied tune needs nothing") {
    Watchdog w;
    CHECK(w.check(tuned(), kOurs, true, true, t0) == WatchAction::None);
    CHECK(w.check(tuned(), AppliedState{135, 1050, 106}, true, true, t0) == WatchAction::None);   // power within 1 %
}

TEST_CASE("a tune reset to stock is re-applied") {
    Watchdog w;
    CHECK(w.check(tuned(), kStock, true, true, t0) == WatchAction::Reapply);
    CHECK(w.check(tuned(), kOurs, true, true, t0 + 30s) == WatchAction::None);
}

TEST_CASE("a fourth reset within an hour gives up, once") {
    // Repeated resets are an instability signal (likely TDRs): stop fighting.
    Watchdog w;
    for (int i = 0; i < 3; ++i)
        CHECK(w.check(tuned(), kStock, true, true, t0 + i * 10min) == WatchAction::Reapply);
    CHECK(w.check(tuned(), kStock, true, true, t0 + 35min) == WatchAction::GiveUpUnstable);
    CHECK(w.gave_up());
    CHECK(w.check(tuned(), kStock, true, true, t0 + 36min) == WatchAction::None);
}

TEST_CASE("resets spread over more than an hour keep being re-applied") {
    Watchdog w;
    for (int i = 0; i < 6; ++i)
        CHECK(w.check(tuned(), kStock, true, true, t0 + i * 25min) == WatchAction::Reapply);
    CHECK_FALSE(w.gave_up());
}

TEST_CASE("another tool's settings are left alone after one notice") {
    Watchdog w;
    const AppliedState foreign{200, 500, 110};
    CHECK(w.check(tuned(), foreign, true, true, t0) == WatchAction::BackOffForeign);
    CHECK(w.check(tuned(), foreign, true, true, t0 + 30s) == WatchAction::None);
    // Once ours again, a later foreign change is reported again.
    CHECK(w.check(tuned(), kOurs, true, true, t0 + 60s) == WatchAction::None);
    CHECK(w.check(tuned(), foreign, true, true, t0 + 90s) == WatchAction::BackOffForeign);
}

TEST_CASE("a changed driver or card is reported once and never re-applied") {
    Watchdog w;
    CHECK(w.check(tuned(), kStock, false, true, t0) == WatchAction::NotifyDriverChanged);
    CHECK(w.check(tuned(), kStock, false, true, t0 + 30s) == WatchAction::None);
    Watchdog g;
    CHECK(g.check(tuned(), kStock, true, false, t0) == WatchAction::NotifyDriverChanged);
}

TEST_CASE("a failed read-back does nothing") {
    Watchdog w;
    CHECK(w.check(tuned(), std::nullopt, true, true, t0) == WatchAction::None);
}
