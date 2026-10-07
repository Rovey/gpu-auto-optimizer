#pragma once
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace gao {

// A reading that uses the -1-means-unknown sentinel, as text: "n/a" rather
// than a number that would look real.
inline std::string reading(int value, const char* unit = "") {
    return value < 0 ? "n/a" : std::to_string(value) + unit;
}

struct Telemetry {
    int core_mhz = -1;        // -1 when the driver does not report it
    int mem_mhz = -1;         // -1 when the driver does not report it
    int temp_c = -1;          // -1 when the driver does not report it
    int fan_pct = -1;         // -1 when the driver does not report it
    int power_w = -1;         // -1 when the driver does not report it
    int power_limit_w = 0;
    // True only when the two readings the search depends on -- core clock
    // and temperature -- both came back real. A device handle alone is not
    // enough: without this, a missing/failing sensor call reads back as a
    // plausible zero (a "cold card") instead of the unknown it actually is,
    // and the spec's 85 C thermal abort would never fire against a stuck
    // temp_c of 0. The other fields may still be -1 (unknown) even when ok
    // is true; check each field individually, the same way fan_pct works.
    bool ok = false;
};

// One point of the voltage/frequency curve of the graphics clock.
struct VfPoint {
    int index = 0;        // the slot in the driver's tables; what a write addresses
    int volt_uv = 0;
    int freq_khz = 0;     // what the card runs at this voltage now: its built-in curve plus the offset
    int raw_offset = 0;   // the offset as the driver stores it (see core/vf_curve.hpp for the units)
};
struct VfOffset {
    int index = 0;
    int raw = 0;
};

// What the GPU reports as applied right now (read back, not remembered).
struct AppliedState {
    int core_mhz = 0;
    int mem_mhz = 0;
    int power_pct = 100;
};

// The clock offsets the driver says it accepts for this card.
struct OffsetRange {
    int min_mhz = 0;
    int max_mhz = 0;
};

struct ClockOffsetRanges {
    OffsetRange core;
    OffsetRange mem;
};

// What the driver reports for the fans (fan 0 stands for all of them).
struct FanReading {
    bool manual = false;   // false: the driver controls the fans
    int target_pct = 0;
    int speed_pct = -1;    // the lowest measured speed of any fan; -1 when unknown
};

// The only way core code reaches hardware. hw/ fills these in production,
// tests fill them with lambdas. An empty callback means "not supported here".
struct GpuControl {
    std::function<Telemetry()> read;
    std::function<bool(int)> set_core_offset;   // MHz, verified by read-back
    std::function<bool(int)> set_mem_offset;    // MHz, verified by read-back
    std::function<bool(int)> set_power_limit;   // percent of default
    std::function<std::pair<int, int>()> power_limit_range_pct;   // {min, max}, percent of default
    std::function<bool()> reset_to_stock;
    std::function<std::optional<AppliedState>()> read_applied;   // nullopt when a read fails
    // The offset ranges the driver reports; nullopt when a read fails. Empty:
    // the search and apply_profile use their built-in limits.
    std::function<std::optional<ClockOffsetRanges>()> clock_offset_range_mhz;
    // Re-creates the driver connections after a driver reset (TDR); true when
    // the hardware answers again. May take many seconds. Empty: not supported.
    std::function<bool()> recover;
    std::function<bool(int)> set_fan_pct;                 // every fan to pct, verified by the target read-back
    std::function<bool()> set_fan_auto;                    // every fan back to the driver, verified by the policy
    std::function<std::optional<FanReading>()> read_fan;   // nullopt when a read fails
    int fan_min_pct = 0;                                   // the card's minimum manual speed
    // The voltage/frequency curve. Empty: the card or driver does not offer
    // it. A write is not verified here; core/vf_curve.hpp reads back and
    // corrects, because what a raw offset does is the card's to say.
    std::function<std::optional<std::vector<VfPoint>>()> read_vf_curve;   // lowest voltage first; nullopt when a read fails
    std::function<bool(const std::vector<VfOffset>&)> write_vf_offsets;   // the named slots; the others keep their offset
};

}
