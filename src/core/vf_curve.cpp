#include "core/vf_curve.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <numeric>

namespace gao {

namespace {

unsigned get_u32(const unsigned char* buf, std::size_t at) {
    unsigned v = 0;
    std::memcpy(&v, buf + at, sizeof(v));
    return v;
}

int get_i32(const unsigned char* buf, std::size_t at) {
    int v = 0;
    std::memcpy(&v, buf + at, sizeof(v));
    return v;
}

const VfPoint* find(const std::vector<VfPoint>& curve, int index) {
    for (const VfPoint& p : curve)
        if (p.index == index) return &p;
    return nullptr;
}

bool at_stock(const std::vector<VfPoint>& curve) {
    return std::all_of(curve.begin(), curve.end(), [](const VfPoint& p) { return p.raw_offset == 0; });
}

}

std::vector<VfPoint> parse_vf_curve(const unsigned char* mask, const unsigned char* status, const unsigned char* control) {
    std::vector<VfPoint> points;
    for (int i = 0; i < kVfSlots; ++i) {
        const std::size_t slot = static_cast<std::size_t>(i);
        const std::size_t m = kVfEntries + slot * kVfMaskEntry;
        const std::size_t s = kVfEntries + slot * kVfStatusEntry;
        const std::size_t c = kVfEntries + slot * kVfControlEntry;
        if (m + kVfMaskEntry > kVfMaskSize || s + kVfStatusEntry > kVfStatusSize || c + kVfControlEntry > kVfControlSize) break;
        if (get_u32(mask, m) != kVfDomainGraphics || mask[m + 4] == 0) continue;
        VfPoint p;
        p.index = i;
        p.freq_khz = static_cast<int>(get_u32(status, s + 4));
        p.volt_uv = static_cast<int>(get_u32(status, s + 8));
        p.raw_offset = get_i32(control, c + kVfControlOffset);
        if (p.freq_khz <= 0 || p.volt_uv <= 0) continue;   // an enabled slot the card does not use
        points.push_back(p);
    }
    std::stable_sort(points.begin(), points.end(), [](const VfPoint& a, const VfPoint& b) { return a.volt_uv < b.volt_uv; });
    return points;
}

void put_vf_raw_offset(unsigned char* control, int index, int raw) {
    std::memcpy(control + kVfEntries + static_cast<std::size_t>(index) * kVfControlEntry + kVfControlOffset, &raw, sizeof(raw));
}

CurveState curve_state(const std::vector<VfPoint>& curve, int anchor_uv) {
    if (curve.empty()) return CurveState::Other;
    if (at_stock(curve)) return CurveState::Stock;
    bool anchored = false;
    for (const VfPoint& p : curve) {
        if (p.volt_uv < anchor_uv && p.raw_offset != 0) return CurveState::Other;
        if (p.volt_uv == anchor_uv) anchored = p.raw_offset > 0;
    }
    return anchored ? CurveState::FlatTop : CurveState::Other;
}

int stock_point_for(const std::vector<VfPoint>& curve, int freq_khz) {
    for (const VfPoint& p : curve)   // lowest voltage first
        if (p.freq_khz >= freq_khz) return p.index;
    return -1;
}

namespace {
// Writes zero to every point. The return code of the write is not what
// counts: the caller reads back.
bool zero_curve(const GpuControl& gpu, const std::vector<VfPoint>& curve) {
    std::vector<VfOffset> zero;
    for (const VfPoint& p : curve) zero.push_back({p.index, 0});
    return gpu.write_vf_offsets(zero);
}

bool has_shape(const std::vector<VfPoint>& curve) {
    return std::any_of(curve.begin(), curve.end(), [&](const VfPoint& p) { return p.raw_offset != curve.front().raw_offset; });
}
}

bool clear_vf_curve(const GpuControl& gpu, std::string* why) {
    auto fail = [&](const char* m) { if (why) *why = m; return false; };
    if (!gpu.read_vf_curve || !gpu.write_vf_offsets) return true;   // nothing this program could have written
    auto curve = gpu.read_vf_curve();
    if (!curve) return fail("the curve could not be read");
    if (at_stock(*curve)) return true;
    const bool accepted = zero_curve(gpu, *curve);
    curve = gpu.read_vf_curve();
    if (!curve) return fail(accepted ? "the curve could not be read back" : "writing the curve failed");
    if (!at_stock(*curve)) return fail(accepted ? "the curve did not go back to stock" : "writing the curve failed");
    return true;
}

bool remove_vf_shape(const GpuControl& gpu, std::string* why) {
    if (!gpu.read_vf_curve || !gpu.write_vf_offsets) return true;
    auto curve = gpu.read_vf_curve();
    if (!curve || curve->empty() || !has_shape(*curve)) return true;
    zero_curve(gpu, *curve);
    curve = gpu.read_vf_curve();
    if (curve && !curve->empty() && !has_shape(*curve)) return true;
    if (why) *why = "the shape on the voltage/frequency curve could not be removed";
    return false;
}

VfApplyResult apply_flat_top(const GpuControl& gpu, int anchor_index, int freq_khz, int tail_drop_khz) {
    VfApplyResult r;
    auto refuse = [&](const std::string& why) { r.why = why; r.at_stock = true; return r; };   // nothing was written
    // After a write: the card must not keep half a curve.
    auto fail = [&](const std::string& why) {
        std::string ignored;
        r.at_stock = clear_vf_curve(gpu, &ignored);
        r.why = why + (r.at_stock ? " -- curve at stock" : " -- reset to stock FAILED, run `gao --reset`");
        return r;
    };
    if (!gpu.read_vf_curve || !gpu.write_vf_offsets) return refuse("this card or driver does not offer the voltage/frequency curve");

    std::string why;
    if (!clear_vf_curve(gpu, &why)) return refuse(why + "; nothing was written");
    auto curve = gpu.read_vf_curve();
    if (!curve || curve->empty()) return refuse("the curve could not be read; nothing was written");
    const VfPoint* anchor = find(*curve, anchor_index);
    if (!anchor) return refuse("that point is not on the curve; nothing was written");
    const int top_khz = std::max_element(curve->begin(), curve->end(), [](const VfPoint& a, const VfPoint& b) {
                            return a.freq_khz < b.freq_khz;
                        })->freq_khz;
    if (freq_khz < anchor->freq_khz) return refuse("the point already runs more than the target; nothing was written");
    if (freq_khz > top_khz) return refuse("the target is above anything the card runs by itself; nothing was written");
    if (freq_khz - anchor->freq_khz > kVfMaxRaiseKhz) return refuse("the raise is larger than this program writes; nothing was written");

    // The plan: the slots from the anchor's voltage up, each to the target.
    const int anchor_uv = anchor->volt_uv;
    std::vector<int> tail;
    for (const VfPoint& p : *curve)
        if (p.volt_uv >= anchor_uv) tail.push_back(p.index);
    // The points below the anchor, to see that they stay untouched.
    std::vector<int> below;
    for (const VfPoint& p : *curve)
        if (p.volt_uv < anchor_uv) below.push_back(p.index);

    // What the plan asks of each point, from this read at stock, and with it
    // the most that will ever be written there.
    auto limit_raw = [&](int index) {
        const VfPoint* built_in = find(*curve, index);
        return std::max(0, freq_khz - (built_in ? built_in->freq_khz : freq_khz)) + kVfSlackKhz;
    };
    // The step the card's clock moves in (15 MHz on the reference card), read
    // off the curve itself. The half step of the first round is kept on it,
    // so that no offset is ever one the card has to round.
    int grid_khz = 0;
    for (std::size_t i = 1; i < curve->size(); ++i)
        grid_khz = std::gcd(grid_khz, std::abs((*curve)[i].freq_khz - (*curve)[i - 1].freq_khz));
    if (grid_khz <= 0) grid_khz = 1;
    std::vector<VfPoint> before = *curve;
    for (r.passes = 1; r.passes <= kVfPasses; ++r.passes) {
        std::vector<VfOffset> offsets;
        for (int index : tail) {
            const VfPoint* now = find(before, index);
            if (!now) return fail("a point disappeared from the curve");
            // The first round raises a point only half of the way: a card
            // that moves further than asked lands on the target at worst.
            // Lowering a point needs no such care.
            const int gap_khz = freq_khz - now->freq_khz;
            const int step_khz = r.passes == 1 && gap_khz > 0 ? gap_khz / 2 / grid_khz * grid_khz : gap_khz;
            offsets.push_back({index, std::min(now->raw_offset + step_khz, limit_raw(index))});
        }
        if (!gpu.write_vf_offsets(offsets)) return fail("writing the curve failed");
        const auto after = gpu.read_vf_curve();
        if (!after) return fail("the curve could not be read back");

        long long asked = 0, moved = 0;
        bool reached = true;
        for (const VfOffset& o : offsets) {
            const VfPoint* was = find(before, o.index);
            const VfPoint* is = find(*after, o.index);
            if (!was || !is) return fail("a point disappeared from the curve");
            asked += std::llabs(static_cast<long long>(o.raw) - was->raw_offset);
            moved += std::llabs(static_cast<long long>(is->freq_khz) - was->freq_khz);
            // The anchor must be on the target; a point above it only must
            // not run more (lower is harmless: the card then simply has no
            // use for that voltage).
            const int error = is->freq_khz - freq_khz;
            if (o.index == anchor_index ? std::abs(error) > kVfToleranceKhz : error > kVfToleranceKhz) reached = false;
        }
        // Untouched means: still without an offset. Their frequencies are not
        // compared: the built-in curve itself moves with temperature (45 MHz
        // between cold and warm on the reference card, up at low voltages and
        // down at high ones).
        for (int index : below) {
            const VfPoint* is = find(*after, index);
            if (!is) return fail("a point disappeared from the curve");
            if (is->raw_offset != 0)
                return fail("a point below the anchor got an offset (slot " + std::to_string(index) + ": " +
                            std::to_string(is->raw_offset) + ")");
        }
        if (reached) {
            if (tail_drop_khz <= 0) {
                r.ok = true;
                return r;
            }
            // The top is flat, as read back. Now the points above the
            // anchor go lower than it, in one write of their offsets. What
            // the card reads back for them does not follow: it reports
            // for every point the most that it or any point below it
            // runs (hardware check 72), so they keep reading the anchor's
            // frequency. What can be checked is that the offsets were
            // stored, that the anchor still is on the target and that
            // nothing reads above it.
            std::vector<VfOffset> lowered;
            for (int index : tail) {
                const VfPoint* is = find(*after, index);
                if (!is) return fail("a point disappeared from the curve");
                if (index != anchor_index) lowered.push_back({index, is->raw_offset - tail_drop_khz});
            }
            if (lowered.empty()) {   // the anchor is the top point
                r.ok = true;
                return r;
            }
            if (!gpu.write_vf_offsets(lowered)) return fail("writing the curve failed");
            const auto final_read = gpu.read_vf_curve();
            if (!final_read) return fail("the curve could not be read back");
            for (const VfPoint& p : *final_read) {
                if (p.volt_uv < anchor_uv) {
                    if (p.raw_offset != 0) return fail("a point below the anchor got an offset (slot " + std::to_string(p.index) + ")");
                    continue;
                }
                if (p.index == anchor_index ? std::abs(p.freq_khz - freq_khz) > kVfToleranceKhz
                                            : p.freq_khz - freq_khz > kVfToleranceKhz)
                    return fail("lowering the points above the anchor moved the top off the target");
            }
            for (const VfOffset& o : lowered) {
                const VfPoint* is = find(*final_read, o.index);
                if (!is || std::abs(is->raw_offset - o.raw) > kVfToleranceKhz)
                    return fail("the card did not store the lowered points above the anchor");
            }
            r.ok = true;
            return r;
        }
        if (r.passes == 1 && asked >= 4 * kVfToleranceKhz && moved * 20 < asked)
            return fail("the card does not show the curve change in its read-out");
        before = *after;
    }
    const VfPoint* anchor_now = find(before, anchor_index);
    return fail("the curve did not settle on the target (the anchor reads " +
                std::to_string(anchor_now ? anchor_now->freq_khz / 1000 : 0) + " MHz, the target is " +
                std::to_string(freq_khz / 1000) + ")");
}

}
