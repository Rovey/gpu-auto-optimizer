#include "core/stability.hpp"
#include <algorithm>

namespace gao {

namespace {
// Time-weighted average over the samples that were actually reported
// (-1 = unknown). Weighting by batch time keeps hw's short calibration
// batches at startup from dragging a full-load average down.
struct Avg {
    double sum = 0;
    double weight = 0;
    void add(int v, double w) { if (v >= 0) { sum += v * w; weight += w; } }
    int get() const { return weight > 0 ? static_cast<int>(sum / weight) : -1; }
};
}

StabilityResult run_stability(const std::function<StressBatch()>& batch,
                              const std::function<Telemetry()>& read,
                              double seconds, int max_temp_c) {
    StabilityResult r;
    long long iterations = 0;
    Avg power, core, mem;
    do {
        const StressBatch b = batch();
        // A batch that claims no time would never advance the loop; count it
        // as 1 ms so a broken timer ends the run instead of hanging it.
        const double batch_s = std::max(b.elapsed_ms, 1.0) / 1000.0;
        r.seconds += batch_s;
        // Device lost first: after a TDR the error counter is garbage.
        if (b.device_lost) { r.verdict = Verdict::DeviceLost; break; }
        if (b.wrong_values > 0) { r.verdict = Verdict::WrongResult; break; }
        iterations += b.iterations;
        const Telemetry t = read();
        if (!t.ok) { r.verdict = Verdict::NoTelemetry; break; }
        r.peak_temp_c = std::max(r.peak_temp_c, t.temp_c);
        power.add(t.power_w, batch_s);
        core.add(t.core_mhz, batch_s);
        mem.add(t.mem_mhz, batch_s);
        if (t.temp_c > max_temp_c) { r.verdict = Verdict::TooHot; break; }
    } while (r.seconds < seconds);
    r.score = static_cast<double>(iterations) / r.seconds;
    r.avg_power_w = power.get();
    r.avg_core_mhz = core.get();
    r.avg_mem_mhz = mem.get();
    return r;
}

const char* verdict_name(Verdict v) {
    switch (v) {
        case Verdict::Stable: return "STABLE";
        case Verdict::WrongResult: return "WRONG RESULT";
        case Verdict::DeviceLost: return "DEVICE LOST";
        case Verdict::TooHot: return "TOO HOT";
        case Verdict::NoTelemetry: return "NO TELEMETRY";
    }
    return "UNKNOWN";
}

}
