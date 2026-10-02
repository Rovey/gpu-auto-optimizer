#pragma once
#include "core/journal.hpp"
#include "core/objectives.hpp"
#include "core/stability.hpp"
#include "core/types.hpp"
#include <functional>
#include <optional>
#include <string>

namespace gao {

// Search ranges. apply_profile refuses anything outside them, since a
// saved profile can only legitimately come from this search.
inline constexpr int kCoreMaxMhz = 300;
inline constexpr int kMemMaxMhz = 1500;

// Highest value in lo, lo+step, ... (<= hi, < ceiling) for which is_stable
// holds, assuming stability is monotonic: once a value fails, every higher
// one fails too. lo itself is assumed stable (it is stock) and never probed.
int highest_stable(int lo, int hi, int step, int ceiling, const std::function<bool(int)>& is_stable);

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
// steps down by step and retries, at most `tries` probes, never probing lo
// (stock). Returns the first value that holds, or lo.
int confirm_edge(int edge, int lo, int step, int tries, const std::function<bool(int)>& holds);

// One stress run: seconds of load, aborting above max_temp_c.
using Probe = std::function<StabilityResult(double seconds, int max_temp_c)>;

struct OptimizeIo {
    Probe probe;
    std::function<bool()> aborted;                      // polled between probes; the probe itself must honour it too
    std::function<void(const std::string&)> log;
    // GB/s at the currently applied settings; nullopt when the measurement
    // failed. Empty: the memory search falls back to stability only.
    std::function<std::optional<double>()> bandwidth;
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
};

// The whole tuning run (spec §3): baseline, power, core, memory, soak. Leaves
// the result applied when ok; any !ok return leaves the card at stock.
OptimizeResult optimize(const GpuControl& gpu, const Objectives& obj, Journal& journal, const OptimizeIo& io);

}
