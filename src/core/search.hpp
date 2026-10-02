#pragma once
#include "core/journal.hpp"
#include "core/objectives.hpp"
#include "core/stability.hpp"
#include "core/types.hpp"
#include <functional>
#include <optional>
#include <string>

namespace gao {

// Search limits used when the card reports no usable offset range.
inline constexpr int kCoreFallbackMaxMhz = 300;
inline constexpr int kMemFallbackMaxMhz = 1500;

// A reported maximum above these is taken for a misread buffer, not a real
// card. Wide on purpose: they exist to catch garbage (kHz read as MHz, a
// shifted layout), not to cap a card. First guesses, checked against the
// ranges read in docs/hardware-checks.md check 45.
inline constexpr int kCoreRangeSanityMhz = 2000;
inline constexpr int kMemRangeSanityMhz = 6000;

// True when a reported range can be trusted as an upper bound: it spans
// stock (min <= 0 < max), the offset applied right now lies inside it, and
// its maximum is at most sanity_max_mhz.
bool plausible_range(const OffsetRange& range, int applied_mhz, int sanity_max_mhz);

// The highest offsets the search may try and apply_profile may apply.
struct SearchBounds {
    int core_max_mhz = kCoreFallbackMaxMhz;
    int mem_max_mhz = kMemFallbackMaxMhz;
    bool core_from_card = false;   // false: the fallback is in use
    bool mem_from_card = false;
};

// The card's reported range where it is plausible, the fallback otherwise;
// core and memory are judged independently. Needs both
// gpu.clock_offset_range_mhz and gpu.read_applied; without either, or when
// either read fails, both bounds are the fallback.
SearchBounds search_bounds(const GpuControl& gpu);

// Highest value in lo, lo+step, ... (<= hi, < ceiling) for which is_stable
// holds, assuming stability is monotonic: once a value fails, every higher
// one fails too. lo itself is assumed stable (it is stock) and never probed.
int highest_stable(int lo, int hi, int step, int ceiling, const std::function<bool(int)>& is_stable);

// Like highest_stable, but for a range whose top may be far beyond the edge:
// bisecting such a range would start with a probe hundreds of MHz too high.
// Climbs lo+stride, lo+2*stride, ... until a probe fails or the top is
// reached, then bisects between the last passing and the first failing value
// on the step grid. The first failing probe is at most one stride above the
// highest stable value. No value is probed twice, lo is never probed, and
// nothing above hi or at or above ceiling is. With stride == step, as
// optimize uses it, it climbs one step at a time and never bisects.
int climb_to_edge(int lo, int hi, int step, int stride, int ceiling, const std::function<bool(int)>& is_stable);

// Lowest value in hi, hi-step, ... (>= lo) for which passes holds, assuming
// passing is monotonic upward. hi is the reference and is assumed to pass;
// it is never probed.
int lowest_passing(int lo, int hi, int step, const std::function<bool(int)>& passes);

// The offset to apply: perf_push * max, rounded down to a step, and always at
// least one step below max so a 3 s probe's blind spot has headroom.
int apply_margin(int max, int step, float perf_push);

// Memory overclocks fail by losing bandwidth before they fail by producing
// errors: GDDR6X/GDDR6 retry bad transfers. The memory search therefore looks
// for the bandwidth peak, not the stability ceiling.
inline constexpr double kBandwidthDrop = 0.01;   // stop once 1 % below the best
// Within 1 % of the best counts as the best: measured noise on the 4070 is
// ~0.4 % and one 50 MHz step is worth ~0.5 %, so a narrower band would let
// noise pick the offset.
inline constexpr double kBandwidthTie = 0.01;

// A probe judged STABLE that scores below this fraction of the baseline is
// STALLED: the card stopped computing without a wrong value or a lost device.
// Measured on an RTX 5070 past its edge: 167 it/s against a baseline of 5869
// (3 %). A working card stays far above a quarter: the lowest score recorded
// in docs/hardware-checks.md is 5486 against a reference of 5718 (96 %, RTX
// 4070 at a lowered power limit).
inline constexpr double kStalledScore = 0.25;

// After a driver reset the card is left alone, at stock, before it is loaded
// again, and must then prove it is back: a short probe at stock that is STABLE
// and scores at least kHealthyScore of the baseline. A card that computes but
// is not back gets another rest and another probe, kHealthTries probes in all.
// First choices, not measured: on an RTX 5070 a probe two seconds after a
// reset ran at 6 % of the baseline score.
inline constexpr double kRestAfterResetS = 20;
inline constexpr double kHealthProbeS = 5;
inline constexpr double kHealthyScore = 0.9;
inline constexpr int kHealthTries = 3;

// After a reset event the search explores nothing new, and the long probes
// keep their distance from where it happened: confirmation starts this many
// grid steps below the last value that passed (in a bandwidth scan: below the
// bandwidth peak of the values that passed), and a confirm try or a soak
// attempt that had a reset event is followed by one this many steps lower.
inline constexpr int kResetBackoffSteps = 4;

struct MemSample {
    bool stable = false;
    double gbps = 0;
};

// Scans lo, lo+step, ... (<= hi, < ceiling), sampling lo too. Stops at the
// first unstable sample or once gbps falls more than kBandwidthDrop below the
// best so far (a drop is re-measured once before it counts). Returns the
// lowest stable offset within kBandwidthTie of the best, so noise on a flat
// curve never drifts the result upward; lo when no sample was stable.
int best_bandwidth_offset(int lo, int hi, int step, int ceiling, const std::function<MemSample(int)>& sample);

// "Search fast, confirm long": re-checks edge with a long probe; on failure
// steps down and retries, at most `tries` probes, never probing lo (stock) or
// anything below it. Returns the first value that holds, or lo.
// step_down: asked after each failed try how many steps to go down; empty = one.
int confirm_edge(int edge, int lo, int step, int tries, const std::function<bool(int)>& holds,
                 const std::function<int()>& step_down = {});

// One stress run: seconds of load, aborting above max_temp_c.
using Probe = std::function<StabilityResult(double seconds, int max_temp_c, double stall_below)>;

struct OptimizeIo {
    Probe probe;
    std::function<bool()> aborted;                      // polled between probes; the probe itself must honour it too
    std::function<void(const std::string&)> log;
    // GB/s at the currently applied settings; nullopt when the measurement
    // failed. Empty: the memory search falls back to stability only.
    std::function<std::optional<double>()> bandwidth;
    // Waits without loading the card; returns false when the wait was cut short
    // by a stop request. Empty: the search does not wait, and does not log
    // that it did.
    std::function<bool(double seconds)> rest;
};

struct OptimizeResult {
    bool ok = false;
    std::string reason;        // why it stopped, when !ok
    bool stock_restored = false;   // when !ok: did the reset to stock succeed?
    int power_pct = 100;
    int core_mhz = 0;
    int mem_mhz = 0;
    int core_max_stable = 0;
    int mem_max_stable = 0;
    int core_confirmed = 0;   // edge that held a 30 s probe; the margin applies to this
    int mem_confirmed = 0;
    StabilityResult baseline;
    StabilityResult soak;
    // Reset events in this run, each counted once: a baseline, power, candidate,
    // soak or health probe that ended DEVICE LOST, STALLED or NO TELEMETRY (the
    // last two only suggest a reset); and, when the card can be reconnected
    // (gpu.recover), a candidate, power or soak write that failed and a
    // bandwidth measurement that failed on a stable candidate above memory +0.
    // With gpu.recover the run ends at the second one, so this is 0, 1 or 2.
    int driver_resets = 0;
};

// The whole tuning run (spec §3): baseline, power, memory, core, soak. Leaves
// the result applied when ok; any !ok return leaves the card at stock, or
// says in stock_restored that it could not.
OptimizeResult optimize(const GpuControl& gpu, const Objectives& obj, Journal& journal, const OptimizeIo& io);

}
