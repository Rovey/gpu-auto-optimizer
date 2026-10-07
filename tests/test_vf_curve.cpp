#include "core/vf_curve.hpp"
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

using namespace gao;

namespace {

void put_u32(std::vector<unsigned char>& b, std::size_t at, unsigned v) { std::memcpy(b.data() + at, &v, 4); }
void put_i32(std::vector<unsigned char>& b, std::size_t at, int v) { std::memcpy(b.data() + at, &v, 4); }

// A card as the curve code sees it: a built-in curve, the raw offsets the
// driver stores, and the quirks the write has to cope with.
struct FakeCurve {
    std::vector<int> volt_uv;    // per slot
    std::vector<int> base_khz;   // the built-in curve
    std::vector<int> raw;        // offsets as stored
    double raw_per_khz = 1.0;    // 2.0: the driver stores offsets at twice their size
    int bin_khz = 0;             // > 0: frequencies snap to this grid
    bool round_nearest = false;  // snap to the nearest step instead of down
    int min_raw = -2000000000;   // the lowest offset the driver stores; lower ones are clamped
    bool report_failure = false; // writes go through, but the call says it failed
    bool live = true;            // false: the read-out never shows an offset
    bool write_ok = true;
    int writes_allowed = -1;     // >= 0: this many writes succeed, the rest fail
    bool read_ok = true;
    int drift_khz = 0;           // added to every built-in frequency from the first write on
    int writes = 0;
    int max_freq_seen = 0;       // the highest frequency any point ever read back as

    FakeCurve() {
        // 700 mV .. 1075 mV in 25 mV steps, 1500 .. 3000 MHz in 100 MHz steps.
        for (int i = 0; i < 16; ++i) {
            volt_uv.push_back(700000 + i * 25000);
            base_khz.push_back(1500000 + i * 100000);
            raw.push_back(0);
        }
    }
    int freq(std::size_t i) const {
        long long f = base_khz[i] + (writes ? drift_khz : 0);
        if (live) f += std::llround(raw[i] / raw_per_khz);
        if (bin_khz > 0) f = (f + (round_nearest ? bin_khz / 2 : 0)) / bin_khz * bin_khz;
        return static_cast<int>(f);
    }
    GpuControl gpu() {
        GpuControl g;
        g.read_vf_curve = [this]() -> std::optional<std::vector<VfPoint>> {
            if (!read_ok) return std::nullopt;
            std::vector<VfPoint> out;
            for (std::size_t i = 0; i < volt_uv.size(); ++i) {
                out.push_back({static_cast<int>(i), volt_uv[i], freq(i), raw[i]});
                max_freq_seen = std::max(max_freq_seen, freq(i));
            }
            return out;
        };
        g.write_vf_offsets = [this](const std::vector<VfOffset>& offsets) {
            if (!write_ok || (writes_allowed >= 0 && writes >= writes_allowed)) return false;
            ++writes;
            for (const VfOffset& o : offsets) raw[static_cast<std::size_t>(o.index)] = std::max(o.raw, min_raw);
            return !report_failure;
        };
        return g;
    }
    bool at_stock() const { return std::all_of(raw.begin(), raw.end(), [](int r) { return r == 0; }); }
};

}

TEST_CASE("parse_vf_curve reads the graphics points, lowest voltage first") {
    std::vector<unsigned char> mask(kVfMaskSize), status(kVfStatusSize), control(kVfControlSize);
    // Slot 0: graphics, enabled. Slot 1: memory domain. Slot 2: graphics, disabled. Slot 3: graphics, enabled, lower voltage.
    const unsigned domain[] = {0, 4, 0, 0};
    const unsigned char enabled[] = {1, 1, 0, 1};
    const unsigned freq[] = {2500000, 9000000, 2600000, 1800000};
    const unsigned volt[] = {950000, 0, 975000, 800000};
    const int offset[] = {30000, 0, 0, -15000};
    for (std::size_t i = 0; i < 4; ++i) {
        put_u32(mask, kVfEntries + i * kVfMaskEntry, domain[i]);
        mask[kVfEntries + i * kVfMaskEntry + 4] = enabled[i];
        put_u32(status, kVfEntries + i * kVfStatusEntry + 4, freq[i]);
        put_u32(status, kVfEntries + i * kVfStatusEntry + 8, volt[i]);
        put_i32(control, kVfEntries + i * kVfControlEntry + kVfControlOffset, offset[i]);
    }
    const auto points = parse_vf_curve(mask.data(), status.data(), control.data());
    REQUIRE(points.size() == 2);
    CHECK(points[0].index == 3);
    CHECK(points[0].volt_uv == 800000);
    CHECK(points[0].freq_khz == 1800000);
    CHECK(points[0].raw_offset == -15000);
    CHECK(points[1].index == 0);
    CHECK(points[1].raw_offset == 30000);
}

TEST_CASE("put_vf_raw_offset writes where parse_vf_curve reads") {
    std::vector<unsigned char> mask(kVfMaskSize), status(kVfStatusSize), control(kVfControlSize);
    mask[kVfEntries + 5 * kVfMaskEntry + 4] = 1;
    put_u32(status, kVfEntries + 5 * kVfStatusEntry + 4, 2000000);
    put_u32(status, kVfEntries + 5 * kVfStatusEntry + 8, 900000);
    put_vf_raw_offset(control.data(), 5, -123456);
    const auto points = parse_vf_curve(mask.data(), status.data(), control.data());
    REQUIRE(points.size() == 1);
    CHECK(points[0].raw_offset == -123456);
}

TEST_CASE("a flat top: the anchor and every point above it run the target, the points below stay stock") {
    FakeCurve card;
    // Anchor: slot 8 (900 mV, 2300 MHz built in). Target: 2700 MHz, which the card reaches at 1000 mV by itself.
    const auto r = apply_flat_top(card.gpu(), 8, 2700000);
    REQUIRE(r.ok);
    for (std::size_t i = 0; i < 8; ++i) CHECK(card.freq(i) == card.base_khz[i]);
    for (std::size_t i = 8; i < 16; ++i) CHECK(card.freq(i) == 2700000);
    CHECK(card.max_freq_seen == 3000000);   // nothing ever read above the built-in top
}

TEST_CASE("a card that stores offsets in other units is refused, never overshot") {
    // Twice the size: every round falls short, the rounds run out, the curve is cleared.
    FakeCurve half;
    half.raw_per_khz = 2.0;
    const auto r = apply_flat_top(half.gpu(), 8, 2700000);
    CHECK_FALSE(r.ok);
    CHECK(r.why == "the curve did not settle on the target -- curve at stock");
    CHECK(half.at_stock());

    // Half the size: the card moves twice as far as asked. The first round
    // only asks for half of the way, so the anchor lands on the target.
    FakeCurve twice;
    twice.raw_per_khz = 0.5;
    const auto t = apply_flat_top(twice.gpu(), 8, 2700000);
    REQUIRE(t.ok);
    CHECK(twice.freq(8) == 2700000);
    for (std::size_t i = 8; i < 16; ++i) CHECK(twice.freq(i) <= 2700000);
}

TEST_CASE("frequencies on a 15 MHz grid are accepted within half a step") {
    FakeCurve card;
    card.bin_khz = 15000;
    const auto r = apply_flat_top(card.gpu(), 8, 2700000);
    REQUIRE(r.ok);
    CHECK(std::abs(card.freq(8) - 2700000) <= kVfToleranceKhz);
}

TEST_CASE("a built-in curve that shifts after the first write is followed") {
    FakeCurve card;
    card.drift_khz = 30000;   // the card warmed up: every point moved two steps
    const auto r = apply_flat_top(card.gpu(), 8, 2700000);
    REQUIRE(r.ok);
    CHECK(std::abs(card.freq(8) - 2700000) <= kVfToleranceKhz);
    for (std::size_t i = 8; i < 16; ++i) CHECK(card.freq(i) <= 2700000 + kVfToleranceKhz);
}

TEST_CASE("a card whose read-out does not show the offset is refused and left at stock") {
    FakeCurve card;
    card.live = false;
    const auto r = apply_flat_top(card.gpu(), 8, 2700000);
    CHECK_FALSE(r.ok);
    CHECK(r.why == "the card does not show the curve change in its read-out -- curve at stock");
    CHECK(card.at_stock());
}

TEST_CASE("a failed write says whether the curve is at stock") {
    FakeCurve card;
    card.write_ok = false;   // nothing was ever written
    const auto r = apply_flat_top(card.gpu(), 8, 2700000);
    CHECK_FALSE(r.ok);
    CHECK(r.why == "writing the curve failed -- curve at stock");
    CHECK(card.at_stock());

    FakeCurve stuck;         // the plan went in, the card did not follow, and the clean-up write failed
    stuck.live = false;
    stuck.writes_allowed = 1;
    const auto s = apply_flat_top(stuck.gpu(), 8, 2700000);
    CHECK_FALSE(s.ok);
    CHECK(s.why == "the card does not show the curve change in its read-out -- reset to stock FAILED, run `gao --reset`");
    CHECK_FALSE(stuck.at_stock());
}

TEST_CASE("a flat top is refused before anything is written when it makes no sense") {
    {
        FakeCurve card;   // the anchor already runs more than the target: that is no undervolt
        const auto r = apply_flat_top(card.gpu(), 8, 2200000);
        CHECK_FALSE(r.ok);
        CHECK(card.writes == 0);
    }
    {
        FakeCurve card;   // more than the largest raise this code will ever write
        const auto r = apply_flat_top(card.gpu(), 2, 1700000 + kVfMaxRaiseKhz + 1000);
        CHECK_FALSE(r.ok);
        CHECK(card.writes == 0);
    }
    {
        FakeCurve card;   // a target above anything the card runs by itself: that is an overclock, not this function's job
        const auto r = apply_flat_top(card.gpu(), 14, 3100000);
        CHECK_FALSE(r.ok);
        CHECK(card.writes == 0);
    }
    {
        FakeCurve card;   // a slot that is not on the curve
        const auto r = apply_flat_top(card.gpu(), 99, 2700000);
        CHECK_FALSE(r.ok);
        CHECK(card.writes == 0);
    }
    {
        FakeCurve card;
        card.read_ok = false;
        const auto r = apply_flat_top(card.gpu(), 8, 2700000);
        CHECK_FALSE(r.ok);
        CHECK(card.writes == 0);
    }
    {
        GpuControl none;   // a card without the curve calls
        const auto r = apply_flat_top(none, 8, 2700000);
        CHECK_FALSE(r.ok);
        CHECK(r.why == "this card or driver does not offer the voltage/frequency curve");
    }
}

TEST_CASE("a curve that is not at stock is cleared first, so the target counts from the built-in curve") {
    FakeCurve card;
    for (std::size_t i = 10; i < 16; ++i) card.raw[i] = 50000;   // something left behind
    const auto r = apply_flat_top(card.gpu(), 8, 2700000);
    REQUIRE(r.ok);
    for (std::size_t i = 0; i < 8; ++i) CHECK(card.freq(i) == card.base_khz[i]);
    for (std::size_t i = 8; i < 16; ++i) CHECK(card.freq(i) == 2700000);
}

TEST_CASE("clear_vf_curve puts every offset back to zero and verifies it") {
    FakeCurve card;
    REQUIRE(apply_flat_top(card.gpu(), 8, 2700000).ok);
    std::string why;
    CHECK(clear_vf_curve(card.gpu(), &why));
    CHECK(card.at_stock());

    FakeCurve stock;   // already at stock: nothing is written
    CHECK(clear_vf_curve(stock.gpu(), &why));
    CHECK(stock.writes == 0);

    FakeCurve broken;
    broken.raw[3] = 1000;
    broken.write_ok = false;
    CHECK_FALSE(clear_vf_curve(broken.gpu(), &why));

    GpuControl none;   // nothing to clear on a card without the calls
    CHECK(clear_vf_curve(none, &why));
}

TEST_CASE("stock_point_for finds where the card reaches a frequency by itself") {
    FakeCurve card;
    const auto curve = *card.gpu().read_vf_curve();
    CHECK(stock_point_for(curve, 2700000) == 12);   // 1000 mV
    CHECK(stock_point_for(curve, 2650000) == 12);   // the first point that reaches it
    CHECK(stock_point_for(curve, 1500000) == 0);
    CHECK(stock_point_for(curve, 3100000) == -1);   // the card never runs that
}

TEST_CASE("no raised point ever reads above the target on the way, whatever the card does with the write") {
    struct Quirk { double units; int bin; bool nearest; int min_raw; };
    for (const Quirk& q : {Quirk{1.0, 0, false, -2000000000}, Quirk{0.5, 0, false, -2000000000}, Quirk{2.0, 0, false, -2000000000},
                           Quirk{1.0, 15000, false, -2000000000}, Quirk{1.0, 15000, true, -2000000000},
                           Quirk{1.0, 0, false, -100000}, Quirk{1.0, 15000, true, -100000}}) {
        for (int anchor = 2; anchor <= 11; ++anchor) {   // raises from 1000 MHz down to 100 MHz... all refused or exact
            FakeCurve card;
            card.raw_per_khz = q.units;
            card.bin_khz = q.bin;
            card.round_nearest = q.nearest;
            card.min_raw = q.min_raw;
            int highest = 0;   // of the points the plan raises
            GpuControl gpu = card.gpu();
            const auto read = gpu.read_vf_curve;
            gpu.read_vf_curve = [&]() {
                auto curve = read();
                if (curve)
                    for (const VfPoint& p : *curve)
                        if (p.index >= anchor && p.index <= 11) highest = std::max(highest, p.freq_khz);
                return curve;
            };
            const auto r = apply_flat_top(gpu, anchor, 2700000);
            CAPTURE(q.units);
            CAPTURE(q.bin);
            CAPTURE(q.min_raw);
            CAPTURE(anchor);
            CHECK(highest <= 2700000 + kVfToleranceKhz);
            if (r.ok) CHECK(std::abs(card.freq(static_cast<std::size_t>(anchor)) - 2700000) <= kVfToleranceKhz);
            else CHECK(card.at_stock());
        }
    }
}

TEST_CASE("on a card that keeps the units and a 15 MHz grid, every anchor settles") {
    for (bool nearest : {false, true}) {
        for (int anchor = 8; anchor <= 11; ++anchor) {   // raises of about 400 MHz and less
            FakeCurve card;
            card.bin_khz = 15000;
            card.round_nearest = nearest;
            const auto r = apply_flat_top(card.gpu(), anchor, 2700000);
            CAPTURE(nearest);
            CAPTURE(anchor);
            REQUIRE(r.ok);
            CHECK(card.freq(static_cast<std::size_t>(anchor)) == 2700000);
            CHECK(r.passes <= 2);
        }
    }
}

TEST_CASE("a driver that cannot lower the points above the anchor far enough: refused, at stock") {
    FakeCurve card;
    card.min_raw = -100000;   // slots 14 and 15 run 2900 and 3000 MHz and cannot come down to 2700
    const auto r = apply_flat_top(card.gpu(), 8, 2700000);
    CHECK_FALSE(r.ok);
    CHECK(card.at_stock());
}

TEST_CASE("clearing the curve is judged by the read-back, not by what the write call says") {
    FakeCurve card;
    REQUIRE(apply_flat_top(card.gpu(), 8, 2700000).ok);
    card.report_failure = true;   // the driver applies the write and reports an error, as the Python days saw
    std::string why;
    CHECK(clear_vf_curve(card.gpu(), &why));
    CHECK(card.at_stock());
}

TEST_CASE("remove_vf_shape: a flat top is zeroed, a plain core offset is left to the offset reset") {
    std::string why;
    FakeCurve offset;   // core +135 MHz: the same offset on every point
    for (int& r : offset.raw) r = 135000;
    CHECK(remove_vf_shape(offset.gpu(), &why));
    CHECK(offset.writes == 0);
    CHECK(offset.raw[0] == 135000);

    FakeCurve shaped;
    REQUIRE(apply_flat_top(shaped.gpu(), 8, 2700000).ok);
    const int writes = shaped.writes;
    CHECK(remove_vf_shape(shaped.gpu(), &why));
    CHECK(shaped.at_stock());
    CHECK(shaped.writes == writes + 1);

    FakeCurve stuck;    // a shape the driver will not let go of
    stuck.raw[10] = 50000;
    stuck.write_ok = false;
    CHECK_FALSE(remove_vf_shape(stuck.gpu(), &why));

    FakeCurve blind;    // a curve that cannot be read is not this program's doing
    blind.raw[10] = 50000;
    blind.read_ok = false;
    CHECK(remove_vf_shape(blind.gpu(), &why));

    GpuControl none;
    CHECK(remove_vf_shape(none, &why));
}

