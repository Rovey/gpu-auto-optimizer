#include "core/fan_tune.hpp"

#include <algorithm>

namespace gao {

FanCurve tuned_curve(int target_c, int limit_c, int hold_pct, int min_pct) {
    hold_pct = std::clamp(hold_pct, 0, 100);
    target_c = std::clamp(target_c, 40, 95);
    limit_c = std::clamp(limit_c, target_c + 1, 100);
    // Where the fans start: the presets' 50 C, lower for a low target, and
    // not more than 20 % under what the target needs.
    const int start_c = std::min(50, target_c - 10);
    const int start_pct = std::clamp(hold_pct - 20, std::min(min_pct, hold_pct), hold_pct);
    FanCurve c;
    c.stop_below_c = start_c;
    c.points = {{start_c, start_pct}, {target_c, hold_pct}, {limit_c, 100}};
    return c;
}

FanTuneResult tune_fan(const GpuControl& gpu, const FanTuneIo& io, int limit_temp_c, int start_pct, int min_pct) {
    FanTuneResult r;
    r.target_c = limit_temp_c - kFanTuneMarginC;
    auto log = [&](const std::string& m) { if (io.log) io.log("  " + m); };
    if (!gpu.set_fan_pct || !gpu.set_fan_auto) {
        r.reason = "the fans of this card cannot be set";
        return r;   // nothing was touched
    }
    auto give_back = [&](const std::string& why) {
        const bool handed = gpu.set_fan_auto();
        r.reason = why;
        log("fan tune stopped: " + why + (handed ? " -- the driver has the fans again" : " -- handing the fans back FAILED, run `gao --fan auto`"));
        return r;
    };

    const int floor_pct = std::clamp(std::max(min_pct, gpu.fan_min_pct), 0, 100);
    // On the step grid, rounded up: a start that is too quiet costs a round.
    int speed = std::clamp((start_pct + kFanTuneStepPct - 1) / kFanTuneStepPct * kFanTuneStepPct, floor_pct, 100);
    log("fan tune: the lowest fan speed that keeps the card at " + std::to_string(r.target_c) + " C under this load, from " +
        std::to_string(speed) + " %");
    int best = -1, best_temp = 0;
    bool going_up = false;
    for (;;) {
        if (io.aborted && io.aborted()) return give_back("aborted");
        if (!gpu.set_fan_pct(speed)) return give_back("a fan speed did not verify");
        if (io.measuring) io.measuring("Fan speed " + std::to_string(speed) + " %: waiting for the temperature to settle", kFanTuneSliceS * kFanTuneMaxSlices);
        std::vector<int> seen;
        bool too_hot = false;
        std::string failed;
        for (int slice = 0; slice < kFanTuneMaxSlices; ++slice) {
            const StabilityResult s = io.probe(kFanTuneSliceS, limit_temp_c, 0.0);
            if (s.verdict == Verdict::Aborted) { failed = "aborted"; break; }
            if (s.verdict == Verdict::TooHot) { too_hot = true; break; }
            if (s.verdict != Verdict::Stable) {
                failed = std::string("the load failed while the fans were tuned (") + verdict_name(s.verdict) + ")";
                break;
            }
            seen.push_back(s.end_temp_c >= 0 ? s.end_temp_c : s.peak_temp_c);
            if (seen.back() > r.target_c + 2) break;   // clearly above the target already: no need to wait for it to settle there
            if (static_cast<int>(seen.size()) >= kFanTuneSettleSlices) {
                const auto last = seen.end() - kFanTuneSettleSlices;
                if (*std::max_element(last, seen.end()) - *std::min_element(last, seen.end()) <= 1) break;
            }
        }
        if (io.measuring) io.measuring("", 0);
        if (!failed.empty()) return give_back(failed);
        const int temp = too_hot || seen.empty() ? limit_temp_c + 1 : seen.back();
        const bool held = !too_hot && temp <= r.target_c;
        r.steps.push_back({speed, temp, held});
        log("fan " + std::to_string(speed) + " %: " + (too_hot ? "above " + std::to_string(limit_temp_c) : std::to_string(temp)) + " C" +
            (held ? "" : " -- too warm"));
        if (held) {
            best = speed;
            best_temp = temp;
            if (going_up || speed <= floor_pct) break;   // going up, the first speed that holds is the answer
            speed = std::max(floor_pct, speed - kFanTuneStepPct);
        } else {
            if (best >= 0) break;                        // the speed before this one was the answer
            if (speed >= 100) return give_back("the fans cannot keep the card at " + std::to_string(r.target_c) + " C under this load");
            going_up = true;
            speed = std::min(100, speed + kFanTuneStepPct);
        }
    }
    // The fans stay on the answer; the caller's curve takes over from there.
    if (!gpu.set_fan_pct(best)) return give_back("a fan speed did not verify");
    r.ok = true;
    r.hold_pct = best;
    r.hold_temp_c = best_temp;
    r.curve = tuned_curve(r.target_c, limit_temp_c, best, floor_pct);
    log("fan tune: " + std::to_string(best) + " % holds " + std::to_string(best_temp) + " C");
    return r;
}

}
