#include "core/stability.hpp"
#include <string>
#include <cstdio>
#include <cmath>
#include <algorithm>
#include <deque>

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
                              double seconds, int max_temp_c,
                              const std::function<bool()>& should_stop,
                              double stall_below) {
    StabilityResult r;
    long long iterations = 0;
    Avg power, core, mem;
    // The counted part (StabilityResult::counted): the batches that begin in
    // the last kCountedShare of the asked-for length.
    const double counted_from = seconds * (1 - kCountedShare);
    long long counted_its = 0;
    Avg c_power, c_core, c_mem, c_temp, c_fan;
    struct Recent { long long iterations; double seconds; };
    std::deque<Recent> recent;   // the batches covering the last kStallWindowS
    long long recent_its = 0;
    double recent_s = 0;
    do {
        const StressBatch b = batch();
        // A batch that claims no time would never advance the loop; count it
        // as 1 ms so a broken timer ends the run instead of hanging it.
        const double batch_s = std::max(b.elapsed_ms, 1.0) / 1000.0;
        const bool counts = r.seconds >= counted_from;
        r.seconds += batch_s;
        // Device lost first: after a TDR the error counter is garbage.
        if (b.device_lost) { r.verdict = Verdict::DeviceLost; break; }
        if (b.wrong_values > 0) { r.verdict = Verdict::WrongResult; break; }
        iterations += b.iterations;
        const Telemetry t = read();
        if (!t.ok) { r.verdict = Verdict::NoTelemetry; break; }
        r.peak_temp_c = std::max(r.peak_temp_c, t.temp_c);
        r.end_fan_pct = t.fan_pct;
        r.end_temp_c = t.temp_c;
        power.add(t.power_w, batch_s);
        core.add(t.core_mhz, batch_s);
        mem.add(t.mem_mhz, batch_s);
        if (counts) {
            r.counted.seconds += batch_s;
            counted_its += b.iterations;
            c_power.add(t.power_w, batch_s);
            c_core.add(t.core_mhz, batch_s);
            c_mem.add(t.mem_mhz, batch_s);
            c_temp.add(t.temp_c, batch_s);
            c_fan.add(t.fan_pct, batch_s);
        }
        if (t.temp_c > max_temp_c) { r.verdict = Verdict::TooHot; break; }
        recent.push_back({b.iterations, batch_s});
        recent_its += b.iterations;
        recent_s += batch_s;
        while (recent.size() > 1 && recent_s - recent.front().seconds >= kStallWindowS) {
            recent_its -= recent.front().iterations;
            recent_s -= recent.front().seconds;
            recent.pop_front();
        }
        if (stall_below > 0 && recent_s >= kStallWindowS &&
            static_cast<double>(recent_its) / recent_s < stall_below) {
            r.verdict = Verdict::Stalled;
            break;
        }
        if (should_stop && should_stop()) { r.verdict = Verdict::Aborted; break; }
    } while (r.seconds < seconds);
    r.score = static_cast<double>(iterations) / r.seconds;
    r.avg_power_w = power.get();
    r.avg_core_mhz = core.get();
    r.avg_mem_mhz = mem.get();
    if (r.verdict == Verdict::Stable && r.counted.seconds > 0) {
        r.counted.score = static_cast<double>(counted_its) / r.counted.seconds;
        r.counted.avg_power_w = c_power.get();
        r.counted.avg_core_mhz = c_core.get();
        r.counted.avg_mem_mhz = c_mem.get();
        r.counted.avg_temp_c = c_temp.get();
        r.counted.avg_fan_pct = c_fan.get();
    } else {
        r.counted = {};
    }
    return r;
}

const char* verdict_name(Verdict v) {
    switch (v) {
        case Verdict::Stable: return "STABLE";
        case Verdict::WrongResult: return "WRONG RESULT";
        case Verdict::DeviceLost: return "DEVICE LOST";
        case Verdict::TooHot: return "TOO HOT";
        case Verdict::NoTelemetry: return "NO TELEMETRY";
        case Verdict::Aborted: return "ABORTED";
        case Verdict::Stalled: return "STALLED";
    }
    return "UNKNOWN";
}

std::string measured_line(const char* name, const StabilityResult& s, bool with_mem) {
    char mem[32] = "";
    if (with_mem) std::snprintf(mem, sizeof(mem), "  mem=%d MHz", s.avg_mem_mhz);
    char line[256];
    std::snprintf(line, sizeof(line), "  %s score=%.0f it/s  core=%d MHz%s  peak=%d C  fan=%s  power=%d W  (%.0f s)", name, s.score,
                  s.avg_core_mhz, mem, s.peak_temp_c, reading(s.end_fan_pct, " %").c_str(), s.avg_power_w, s.seconds);
    return line;
}

std::string counted_line(const char* name, const StabilityResult& s) {
    const StabilityResult::Counted& c = s.counted;
    char line[320];
    std::snprintf(line, sizeof(line),
                  "  %s averages of the last %.0f s: score=%.0f it/s  core=%d MHz  power=%d W  temp=%s  fan=%s;  whole %.0f s: peak=%d C  "
                  "fan at the end=%s",
                  name, c.seconds, c.score, c.avg_core_mhz, c.avg_power_w, reading(c.avg_temp_c, " C").c_str(),
                  reading(c.avg_fan_pct, " %").c_str(), s.seconds, s.peak_temp_c, reading(s.end_fan_pct, " %").c_str());
    return line;
}

std::string trace_line(const double t_s, const Telemetry& tel) {
    const int t = static_cast<int>(t_s);
    char when[24];
    std::snprintf(when, sizeof(when), "    %d:%02d  ", t / 60, t % 60);
    return when + reading(tel.core_mhz, " MHz") + "  " + reading(tel.temp_c, " C") + "  " + reading(tel.power_w, " W") + "  fan " +
           reading(tel.fan_pct, " %");
}

bool TraceClock::due(const double t_s) {
    if (t_s < next_) return false;
    // The next one at the next whole interval after this reading, wherever this one fell.
    next_ = (std::floor(t_s / kTraceEveryS) + 1) * kTraceEveryS;
    return true;
}

std::string temp_target_text(const int target_c, const int default_c) {
    if (target_c <= 0) return "  the card reports no temperature target of its own";
    const std::string target = std::to_string(target_c) + " C";
    if (default_c <= 0) return "  the card's own temperature target: " + target;
    if (target_c == default_c) return "  the card's own temperature target: " + target + " (its default)";
    return "  the card's own temperature target: " + target + ", NOT its default of " + std::to_string(default_c) +
           " C: another tool has set it (MSI Afterburner's temp limit does). The card slows itself down from " + target +
           " on, so this is not the card at stock";
}

}
