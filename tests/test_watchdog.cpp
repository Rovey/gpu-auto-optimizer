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

TEST_CASE("a partial reset is still a reset, not another tool") {
    // A TDR may reset the offsets but leave the power limit (or the reverse):
    // every field being either ours or stock means our tune was undone.
    Watchdog w;
    CHECK(w.check(tuned(), AppliedState{0, 0, 105}, true, true, t0) == WatchAction::Reapply);
    Watchdog v;
    CHECK(v.check(tuned(), AppliedState{135, 1050, 100}, true, true, t0) == WatchAction::Reapply);
    Watchdog u;
    CHECK(u.check(tuned(), AppliedState{0, 900, 105}, true, true, t0) == WatchAction::BackOffForeign);
}

namespace {
Profile undervolted() {
    Profile p;
    p.preset = Preset::Undervolt;
    p.undervolt = UndervoltTune{960000, 2745000, 210000};
    return p;
}
}

TEST_CASE("an undervolt is judged by the curve, not by the core offset the driver reports") {
    // What the offset read-out says while a flat top is on the card is the
    // driver's business; the curve says whether the flat top is there.
    Watchdog w;
    CHECK(w.check(undervolted(), AppliedState{0, 0, 100}, true, true, t0, CurveState::FlatTop) == WatchAction::None);
    CHECK(w.check(undervolted(), AppliedState{210, 0, 100}, true, true, t0, CurveState::FlatTop) == WatchAction::None);
    CHECK(tune_applied(undervolted(), AppliedState{210, 0, 100}, CurveState::FlatTop));
    CHECK_FALSE(tune_applied(undervolted(), kStock, CurveState::Stock));
    CHECK_FALSE(tune_applied(undervolted(), kStock, std::nullopt));
    CHECK(tune_applied(tuned(), kOurs, std::nullopt));   // a profile without one needs no curve
}

TEST_CASE("an undervolt that was reset to stock is re-applied, and given up on the fourth time") {
    Watchdog w;
    for (int i = 0; i < 3; ++i)
        CHECK(w.check(undervolted(), kStock, true, true, t0 + i * 10min, CurveState::Stock) == WatchAction::Reapply);
    CHECK(w.check(undervolted(), kStock, true, true, t0 + 35min, CurveState::Stock) == WatchAction::GiveUpUnstable);
    CHECK(w.check(undervolted(), kStock, true, true, t0 + 36min, CurveState::Stock) == WatchAction::None);
}

TEST_CASE("a curve that is neither the flat top nor stock is another program's") {
    Watchdog w;
    CHECK(w.check(undervolted(), kStock, true, true, t0, CurveState::Other) == WatchAction::BackOffForeign);
    CHECK(w.check(undervolted(), kStock, true, true, t0 + 30s, CurveState::Other) == WatchAction::None);
}

TEST_CASE("an undervolt whose curve cannot be read is left alone") {
    Watchdog w;
    CHECK(w.check(undervolted(), kStock, true, true, t0, std::nullopt) == WatchAction::None);
}

TEST_CASE("driver resets that leave the tune applied count against it too") {
    // A tune the driver keeps across a reset is never found at stock; it is
    // the resets themselves that say it is not stable.
    Watchdog w;
    for (int i = 0; i < 3; ++i)
        CHECK(w.check(undervolted(), kStock, true, true, t0 + i * 10min, CurveState::FlatTop, true) == WatchAction::None);
    CHECK_FALSE(w.gave_up());
    CHECK(w.check(undervolted(), kStock, true, true, t0 + 35min, CurveState::FlatTop, true) == WatchAction::GiveUpUnstable);
    CHECK(w.gave_up());
    // The same for an overclock, and mixed with resets that did undo it: one count.
    Watchdog v;
    CHECK(v.check(tuned(), kOurs, true, true, t0, std::nullopt, true) == WatchAction::None);
    CHECK(v.check(tuned(), kStock, true, true, t0 + 5min, std::nullopt, true) == WatchAction::Reapply);
    CHECK(v.check(tuned(), kOurs, true, true, t0 + 10min, std::nullopt, true) == WatchAction::None);
    CHECK(v.check(tuned(), kOurs, true, true, t0 + 15min, std::nullopt, true) == WatchAction::GiveUpUnstable);
}

TEST_CASE("a tune that stays applied without a driver reset is never counted") {
    Watchdog w;
    for (int i = 0; i < 10; ++i) CHECK(w.check(tuned(), kOurs, true, true, t0 + i * 1min) == WatchAction::None);
    CHECK_FALSE(w.gave_up());
}

TEST_CASE("the resets counted so far go with a process that restarts itself") {
    Watchdog before;
    CHECK(before.recent(t0) == 0);
    CHECK(before.check(tuned(), kStock, true, true, t0) == WatchAction::Reapply);
    CHECK(before.check(tuned(), kStock, true, true, t0 + 10min) == WatchAction::Reapply);
    CHECK(before.recent(t0 + 20min) == 2);
    CHECK(before.recent(t0 + 65min) == 1);   // the first one is over an hour old
    // The next process starts from that count instead of from nothing.
    Watchdog after;
    after.seed(2, t0 + 20min);
    CHECK(after.check(tuned(), kStock, true, true, t0 + 21min) == WatchAction::Reapply);
    CHECK(after.check(tuned(), kStock, true, true, t0 + 22min) == WatchAction::GiveUpUnstable);
    // A count from a command line is not trusted beyond what a watchdog can hold.
    Watchdog full, none;
    full.seed(1000, t0);
    CHECK(full.recent(t0) == Watchdog::kMaxReappliesPerHour);
    CHECK(full.check(tuned(), kStock, true, true, t0 + 1min) == WatchAction::GiveUpUnstable);
    none.seed(-5, t0);
    CHECK(none.recent(t0) == 0);
}
