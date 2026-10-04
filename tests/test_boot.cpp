#include "doctest/doctest.h"
#include "core/boot.hpp"
#include "core/types.hpp"
#include <optional>
#include <string>
#include <tuple>
#include <utility>

using namespace gao;

namespace {
Config with_profile(const std::string& driver, int strikes = 0, const std::string& gpu = "GPU-1") {
    Config c;
    Profile p;
    p.power_pct = 105;
    p.core_mhz = 135;
    p.mem_mhz = 1050;
    p.driver = driver;
    p.gpu = gpu;
    c.profile = p;
    c.boot_strikes = strikes;
    return c;
}

struct Card {
    int power = 100, core = 0, mem = 0;
    bool fail_mem = false;
    bool reset_ok = true;
    int writes = 0;
    std::optional<ClockOffsetRanges> ranges;   // what the card reports; empty = nothing
    GpuControl gpu(bool with_power = true) {
        GpuControl g;
        g.set_core_offset = [this](int v) { ++writes; core = v; return true; };
        g.set_mem_offset = [this](int v) { if (fail_mem) return false; mem = v; return true; };
        g.reset_to_stock = [this] { if (!reset_ok) return false; power = 100; core = 0; mem = 0; return true; };
        if (with_power) g.set_power_limit = [this](int p) { ++writes; power = p; return true; };
        if (ranges) {
            g.clock_offset_range_mhz = [this] { return ranges; };
            g.read_applied = [this] { return std::optional<AppliedState>(AppliedState{core, mem, power}); };
        }
        return g;
    }
};
}

TEST_CASE("decide_boot: each outcome") {
    CHECK(decide_boot(Config{}, "610.74", "GPU-1") == BootDecision::NoProfile);
    CHECK(decide_boot(with_profile("610.74", 3), "610.74", "GPU-1") == BootDecision::TooManyStrikes);
    CHECK(decide_boot(with_profile("610.74"), "615.20", "GPU-1") == BootDecision::DriverChanged);
    CHECK(decide_boot(with_profile("610.74", 2), "610.74", "GPU-1") == BootDecision::Apply);
}

TEST_CASE("decide_boot: strikes are checked before the driver") {
    CHECK(decide_boot(with_profile("610.74", 5), "615.20", "GPU-1") == BootDecision::TooManyStrikes);
}

TEST_CASE("an unknown driver never matches") {
    CHECK(decide_boot(with_profile("610.74"), "", "GPU-1") == BootDecision::DriverChanged);
    CHECK(decide_boot(with_profile(""), "", "GPU-1") == BootDecision::DriverChanged);
}

TEST_CASE("apply_profile sets power, core and memory") {
    Card card;
    std::string why;
    CHECK(apply_profile(card.gpu(), *with_profile("x").profile, &why));
    CHECK(card.power == 105);
    CHECK(card.core == 135);
    CHECK(card.mem == 1050);
}

TEST_CASE("a failing setter resets to stock and names the setter") {
    Card card;
    card.fail_mem = true;
    std::string why;
    CHECK_FALSE(apply_profile(card.gpu(), *with_profile("x").profile, &why));
    CHECK(why.find("mem") != std::string::npos);
    CHECK(card.power == 100);
    CHECK(card.core == 0);
}

TEST_CASE("power other than 100 % without power control fails and resets") {
    Card card;
    std::string why;
    CHECK_FALSE(apply_profile(card.gpu(/*with_power=*/false), *with_profile("x").profile, &why));
    CHECK(why.find("power") != std::string::npos);
    CHECK(card.core == 0);

    Profile p = *with_profile("x").profile;
    p.power_pct = 100;
    CHECK(apply_profile(card.gpu(false), p, &why));
    CHECK(card.core == 135);
}

TEST_CASE("a profile only applies to the GPU it was tested on") {
    CHECK(decide_boot(with_profile("610.74"), "610.74", "GPU-2") == BootDecision::GpuChanged);
    CHECK(decide_boot(with_profile("610.74"), "610.74", "") == BootDecision::GpuChanged);
    CHECK(decide_boot(with_profile("610.74", 0, ""), "610.74", "") == BootDecision::GpuChanged);
    CHECK(decide_boot(with_profile("610.74", 0, "GPU-1"), "615.20", "GPU-2") == BootDecision::DriverChanged);
}

TEST_CASE("a failed reset is reported, not claimed as stock") {
    Card card;
    card.fail_mem = true;
    card.reset_ok = false;
    std::string why;
    CHECK_FALSE(apply_profile(card.gpu(), *with_profile("x").profile, &why));
    CHECK(why.find("reset to stock FAILED") != std::string::npos);
}

TEST_CASE("values outside the search bounds are refused before anything is written") {
    // gao.json is user-writable, and boot-apply runs elevated: never apply
    // something --optimize could not have produced.
    for (auto bad : {std::make_tuple(105, 400, 0), std::make_tuple(105, 0, 2000), std::make_tuple(105, -15, 0),
                     std::make_tuple(300, 0, 0), std::make_tuple(10, 0, 0)}) {
        Card card;
        Profile p = *with_profile("x").profile;
        std::tie(p.power_pct, p.core_mhz, p.mem_mhz) = bad;
        std::string why;
        CHECK_FALSE(apply_profile(card.gpu(), p, &why));
        CHECK(why.find("out of range") != std::string::npos);
        CHECK(card.writes == 0);
    }
}

TEST_CASE("a profile above the old cap applies when the card's range allows it") {
    Card card;
    card.ranges = ClockOffsetRanges{{-500, 1000}, {-1000, 3000}};
    Profile p = *with_profile("x").profile;
    p.core_mhz = 405;
    p.mem_mhz = 2000;
    std::string why;
    CHECK(apply_profile(card.gpu(), p, &why));
    CHECK(card.core == 405);
    CHECK(card.mem == 2000);
}

TEST_CASE("a profile above the card's own range is refused before anything is written") {
    for (auto bad : {std::make_pair(1015, 0), std::make_pair(0, 3050)}) {
        Card card;
        card.ranges = ClockOffsetRanges{{-500, 1000}, {-1000, 3000}};
        Profile p = *with_profile("x").profile;
        std::tie(p.core_mhz, p.mem_mhz) = bad;
        std::string why;
        CHECK_FALSE(apply_profile(card.gpu(), p, &why));
        CHECK(why.find("out of range") != std::string::npos);
        CHECK(card.writes == 0);
    }
}

TEST_CASE("an implausible reported range does not widen what a profile may apply") {
    // gao.json is user-writable and boot-apply runs elevated: a misread range
    // must never be the reason a +900 profile gets applied.
    Card card;
    card.ranges = ClockOffsetRanges{{100, 9000}, {100, 9000}};
    Profile p = *with_profile("x").profile;
    p.core_mhz = 900;
    std::string why;
    CHECK_FALSE(apply_profile(card.gpu(), p, &why));
    CHECK(card.writes == 0);
}

TEST_CASE("a profile inside the built-in limits still applies when the card reports no range") {
    Card card;   // no ranges: every profile saved before this change
    std::string why;
    CHECK(apply_profile(card.gpu(), *with_profile("x").profile, &why));
    CHECK(card.core == 135);
    CHECK(card.mem == 1050);
}

TEST_CASE("a reported range below the built-in limits does not refuse a profile those limits allow") {
    // The range comes from an undocumented buffer, verified on one card. A
    // low reading must not turn every logon into a refused apply and a strike.
    Card card;
    card.ranges = ClockOffsetRanges{{-500, 150}, {-1000, 400}};
    std::string why;
    CHECK(apply_profile(card.gpu(), *with_profile("x").profile, &why));   // core +135, mem +1050
    CHECK(card.core == 135);
    CHECK(card.mem == 1050);

    Card over;
    over.ranges = ClockOffsetRanges{{-500, 150}, {-1000, 400}};
    Profile p = *with_profile("x").profile;
    p.core_mhz = 315;   // above both the reported and the built-in limit
    CHECK_FALSE(apply_profile(over.gpu(), p, &why));
    CHECK(over.writes == 0);
}
