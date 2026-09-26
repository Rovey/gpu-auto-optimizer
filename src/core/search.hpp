#pragma once
#include "core/journal.hpp"
#include "core/objectives.hpp"
#include "core/stability.hpp"
#include "core/types.hpp"
#include <functional>
#include <string>

namespace gao {

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

// One stress run: seconds of load, aborting above max_temp_c.
using Probe = std::function<StabilityResult(double seconds, int max_temp_c)>;

struct OptimizeIo {
    Probe probe;
    std::function<bool()> aborted;                      // polled between probes
    std::function<void(const std::string&)> log;
};

struct OptimizeResult {
    bool ok = false;
    std::string reason;        // why it stopped, when !ok
    int power_pct = 100;
    int core_mhz = 0;
    int mem_mhz = 0;
    int core_max_stable = 0;
    int mem_max_stable = 0;
    StabilityResult baseline;
    StabilityResult soak;
};

// The whole tuning run (spec §3): baseline, power, core, memory, soak. Leaves
// the result applied when ok; any !ok return leaves the card at stock.
OptimizeResult optimize(const GpuControl& gpu, const Objectives& obj, Journal& journal, const OptimizeIo& io);

}
