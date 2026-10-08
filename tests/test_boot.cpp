#include "doctest/doctest.h"
#include "core/boot.hpp"
#include "core/types.hpp"
#include <algorithm>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

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

TEST_CASE("a failed apply says whether it left the card clean") {
    // Clean: nothing of the profile is on the card, so a failed logon apply is
    // no crash strike. Only a reset that failed leaves the card unknown.
    std::string why;
    {
        Card card;   // refused before any write
        Profile p = *with_profile("x").profile;
        p.core_mhz = 99999;
        bool clean = false;
        CHECK_FALSE(apply_profile(card.gpu(), p, &why, &clean));
        CHECK(clean);
        CHECK(card.writes == 0);
    }
    {
        Card card;   // a write failed, the reset worked
        card.fail_mem = true;
        bool clean = false;
        CHECK_FALSE(apply_profile(card.gpu(), *with_profile("x").profile, &why, &clean));
        CHECK(clean);
        CHECK(card.core == 0);
    }
    {
        Card card;   // a write failed and so did the reset
        card.fail_mem = true;
        card.reset_ok = false;
        bool clean = true;
        CHECK_FALSE(apply_profile(card.gpu(), *with_profile("x").profile, &why, &clean));
        CHECK_FALSE(clean);
    }
    {
        Card card;   // applied: the profile is on the card
        bool clean = true;
        CHECK(apply_profile(card.gpu(), *with_profile("x").profile, &why, &clean));
        CHECK_FALSE(clean);
    }
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

namespace {
// A card with a voltage/frequency curve. As on the reference RTX 4070, the
// core offset and the curve are one table: an offset write puts the same
// value on every point, and writing the offset the driver already reports
// changes nothing.
struct CurveCard {
    std::vector<int> volt_uv, base_khz, raw;
    int power = 100, core = 0, mem = 0;
    bool curve_write_ok = true;
    int curve_writes = 0, offset_writes = 0;

    CurveCard() {
        for (int i = 0; i < 40; ++i) {   // 700 .. 1090 mV, 1800 .. 2970 MHz on a 15 MHz grid
            volt_uv.push_back(700000 + i * 10000);
            base_khz.push_back(1800000 + i * 30000);
            raw.push_back(0);
        }
    }
    int freq(std::size_t i) const { return base_khz[i] + raw[i]; }
    bool at_stock() const { return std::all_of(raw.begin(), raw.end(), [](int v) { return v == 0; }); }
    GpuControl gpu() {
        GpuControl g;
        g.set_power_limit = [this](int p) { ++offset_writes; power = p; return true; };
        g.set_core_offset = [this](int v) {
            ++offset_writes;
            if (v != core) std::fill(raw.begin(), raw.end(), v * 1000);
            core = v;
            return true;
        };
        g.set_mem_offset = [this](int v) { ++offset_writes; mem = v; return true; };
        g.read_vf_curve = [this]() -> std::optional<std::vector<VfPoint>> {
            std::vector<VfPoint> out;
            int most = 0;   // every point reads the most that it or any point below it runs
            for (std::size_t i = 0; i < raw.size(); ++i) {
                most = std::max(most, freq(i));
                out.push_back({static_cast<int>(i), volt_uv[i], most, raw[i]});
            }
            return out;
        };
        g.write_vf_offsets = [this](const std::vector<VfOffset>& offsets) {
            if (!curve_write_ok) return false;
            ++curve_writes;
            for (const VfOffset& o : offsets) raw[static_cast<std::size_t>(o.index)] = o.raw;
            return true;
        };
        g.reset_to_stock = [this] {
            std::fill(raw.begin(), raw.end(), 0);
            power = 100; core = 0; mem = 0;
            return true;
        };
        return g;
    }
    // The flat top of `undervolt_profile()` as another run left it.
    void flat_top(std::size_t anchor, int freq_khz) {
        for (std::size_t i = anchor; i < raw.size(); ++i) raw[i] = freq_khz - base_khz[i];
    }
};

// 2400 MHz (the card's own clock at 900 mV) at 850 mV: a raise of 150 MHz at slot 15.
Profile undervolt_profile() {
    Profile p;
    p.preset = Preset::Undervolt;
    p.driver = "x";
    p.gpu = "GPU-1";
    p.undervolt = UndervoltTune{850000, 2400000, 150000};
    return p;
}
}

TEST_CASE("an undervolt profile writes its flat top") {
    CurveCard card;
    std::string why;
    bool clean = true;
    REQUIRE(apply_profile(card.gpu(), undervolt_profile(), &why, &clean));
    CHECK_FALSE(clean);   // the profile is on the card
    CHECK(card.freq(15) == 2400000);
    for (std::size_t i = 0; i < card.raw.size(); ++i) {
        if (i < 15) CHECK(card.raw[i] == 0);          // below the anchor: the built-in curve
        else CHECK(card.freq(i) <= 2400000);          // from the anchor up: nothing runs more
    }
    CHECK(card.power == 100);
    CHECK(card.core == 0);
}

TEST_CASE("an undervolt is applied again over what is on the card") {
    CurveCard card;
    card.flat_top(10, 2250000);   // an older flat top, anchored elsewhere
    std::string why;
    REQUIRE(apply_profile(card.gpu(), undervolt_profile(), &why));
    for (std::size_t i = 0; i < 15; ++i) CHECK(card.raw[i] == 0);
    CHECK(card.freq(15) == 2400000);
}

TEST_CASE("an undervolt that does not fit this card's curve is refused and ends at stock") {
    std::string why;
    {
        CurveCard card;   // no point at that voltage
        Profile p = undervolt_profile();
        p.undervolt->volt_uv = 855000;
        bool clean = false;
        CHECK_FALSE(apply_profile(card.gpu(), p, &why, &clean));
        CHECK(why.find("no point at 855 mV") != std::string::npos);
        CHECK(clean);
        CHECK(card.at_stock());
        CHECK(card.curve_writes == 0);
    }
    {
        CurveCard card;   // the built-in curve reads 60 MHz lower than when it was tested: the raise would be 210, tested 150
        for (int& f : card.base_khz) f -= 60000;
        bool clean = false;
        CHECK_FALSE(apply_profile(card.gpu(), undervolt_profile(), &why, &clean));
        CHECK(why.find("tested with 150 MHz") != std::string::npos);
        CHECK(clean);
        CHECK(card.at_stock());
        CHECK(card.curve_writes == 0);
    }
    {
        CurveCard card;   // 30 MHz lower is what temperature does: applied
        for (int& f : card.base_khz) f -= 30000;
        CHECK(apply_profile(card.gpu(), undervolt_profile(), &why));
        CHECK(card.freq(15) == 2400000);
    }
}

TEST_CASE("an undervolt profile is refused before anything is written when it cannot be what the search saved") {
    std::string why;
    auto refused = [&](const Profile& p, GpuControl gpu, CurveCard& card) {
        bool clean = false;
        CHECK_FALSE(apply_profile(gpu, p, &why, &clean));
        CHECK(clean);
        CHECK(card.offset_writes == 0);
        CHECK(card.curve_writes == 0);
    };
    for (const UndervoltTune bad : {UndervoltTune{850000, 2400000, 0}, UndervoltTune{850000, 2400000, 900000},
                                    UndervoltTune{0, 2400000, 150000}, UndervoltTune{850000, -1, 150000}}) {
        CurveCard card;
        Profile p = undervolt_profile();
        p.undervolt = bad;
        refused(p, card.gpu(), card);
        CHECK(why.find("out of range") != std::string::npos);
    }
    {
        CurveCard card;   // a card or driver without the curve calls
        GpuControl gpu = card.gpu();
        gpu.read_vf_curve = nullptr;
        gpu.write_vf_offsets = nullptr;
        refused(undervolt_profile(), gpu, card);
        CHECK(why.find("voltage/frequency curve") != std::string::npos);
    }
}

TEST_CASE("an undervolt whose curve write fails ends at stock and says so") {
    CurveCard card;
    card.curve_write_ok = false;
    std::string why;
    bool clean = false;
    CHECK_FALSE(apply_profile(card.gpu(), undervolt_profile(), &why, &clean));
    CHECK(why == "the undervolt was not applied: writing the curve failed -- card at stock");
    CHECK(clean);
    CHECK(card.at_stock());
}

TEST_CASE("a profile without an undervolt removes a flat top that is on the card") {
    CurveCard card;
    card.flat_top(15, 2400000);
    Profile p = *with_profile("x").profile;
    p.core_mhz = 0;   // the driver reports core +0 already: the offset write alone would change nothing
    std::string why;
    REQUIRE(apply_profile(card.gpu(), p, &why));
    CHECK(card.at_stock());
    CHECK(card.mem == 1050);
}

TEST_CASE("an overclock and an undervolt in one profile: the flat top is written over the core offset") {
    // As the overclock-then-undervolt run saves it: core +60, and 2460 MHz at
    // 850 mV. That point runs 2250 built in and 2310 with the offset: the flat
    // top lifts it another 150, 210 over the built-in curve.
    Profile p = undervolt_profile();
    p.core_mhz = 60;
    p.mem_mhz = 500;
    p.power_pct = 105;
    p.undervolt = UndervoltTune{850000, 2460000, 210000};
    CurveCard card;
    std::string why;
    REQUIRE(apply_profile(card.gpu(), p, &why));
    CHECK(card.core == 60);
    CHECK(card.mem == 500);
    CHECK(card.power == 105);
    for (std::size_t i = 0; i < 15; ++i) CHECK(card.raw[i] == 60000);   // the overclock, below the anchor
    CHECK(card.freq(15) == 2460000);
    for (std::size_t i = 16; i < card.raw.size(); ++i) CHECK(card.freq(i) < 2460000);
    // Applied again over itself, and over a card that kept the offset but lost the flat top.
    REQUIRE(apply_profile(card.gpu(), p, &why));
    CHECK(card.freq(15) == 2460000);
    std::fill(card.raw.begin(), card.raw.end(), 60000);
    REQUIRE(apply_profile(card.gpu(), p, &why));
    CHECK(card.freq(15) == 2460000);
    for (std::size_t i = 0; i < 15; ++i) CHECK(card.raw[i] == 60000);
}

TEST_CASE("the raise of an undervolt over a core offset is checked against the built-in curve before a clock is written") {
    Profile p = undervolt_profile();
    p.core_mhz = 60;
    p.undervolt = UndervoltTune{850000, 2460000, 210000};
    CurveCard card;
    for (int& f : card.base_khz) f -= 60000;   // the built-in curve reads 60 MHz lower: the point would run 270 over it, tested 210
    std::string why;
    bool clean = false;
    CHECK_FALSE(apply_profile(card.gpu(), p, &why, &clean));
    CHECK(why.find("would be raised by 270 MHz; the undervolt was tested with 210 MHz") != std::string::npos);
    CHECK(clean);
    CHECK(card.at_stock());
    CHECK(card.core == 0);   // the reset; the overclock was never written
}
