#include "app/common.hpp"
#include "core/stability.hpp"
#include "core/step_down.hpp"
#include <cstdio>

namespace gao::app {

int driver_resets(const OptimizeOutcome& o) { return o.ran ? o.result.driver_resets : 0; }
int driver_resets(const UndervoltOutcome& o) { return o.ran ? o.result.driver_resets : 0; }
int driver_resets(const AllInOneOutcome& o) { return driver_resets(o.overclock) + driver_resets(o.undervolt); }

namespace {

std::string not_applied(const std::string& reason, bool stock_restored) {
    return "RESULT: not applied -- " + reason + " (" + (stock_restored ? "card at stock" : "reset to stock FAILED, run `gao --reset`") + ")";
}

void add_resets(std::vector<std::string>& lines, int resets) {
    if (resets > 0) lines.push_back("  driver resets during this run: " + std::to_string(resets));
}

void add_saved(std::vector<std::string>& lines, bool saved, const std::string& save_note) {
    if (!saved) lines.push_back("  not saved: " + save_note);
}

}

std::vector<std::string> result_lines(const OptimizeOutcome& o) {
    if (!o.ran) return {"RESULT: not started -- " + o.error};
    const OptimizeResult& r = o.result;
    std::vector<std::string> lines;
    if (!r.ok) {
        lines.push_back(not_applied(r.reason, r.stock_restored));
        add_resets(lines, driver_resets(o));
        return lines;
    }
    char line[256];
    std::snprintf(line, sizeof(line), "RESULT: power %d %%, core +%d MHz (confirmed +%d), mem +%d MHz (confirmed +%d)", r.power_pct,
                  r.core_mhz, r.core_confirmed, r.mem_mhz, r.mem_confirmed);
    lines.push_back(line);
    add_resets(lines, driver_resets(o));
    // Both runs last as long, so the temperature and the fan speed compare; at stock the driver had the fans.
    lines.push_back(measured_line("before:", o.stock, true));
    lines.push_back(measured_line("after: ", r.soak, true));
    add_saved(lines, o.saved, o.save_note);
    return lines;
}

std::vector<std::string> result_lines(const UndervoltOutcome& o) {
    if (!o.ran) return {"RESULT: not started -- " + o.error};
    const UndervoltResult& r = o.result;
    std::vector<std::string> lines;
    if (!r.ok) {
        lines.push_back(not_applied(r.reason, r.stock_restored));
        add_resets(lines, driver_resets(o));
        return lines;
    }
    char line[256];
    std::snprintf(line, sizeof(line), "RESULT: %d MHz at %d mV instead of %d mV (lowest stable %d mV, confirmed %d mV)", r.freq_khz / 1000,
                  r.applied_uv / 1000, r.stock_uv / 1000, r.edge_uv / 1000, r.confirmed_uv / 1000);
    lines.push_back(line);
    add_resets(lines, driver_resets(o));
    lines.push_back(measured_line("before:", r.baseline, false));
    lines.push_back(measured_line("after: ", r.after, false));
    add_saved(lines, o.saved, o.save_note);
    return lines;
}

std::vector<std::string> result_lines(const AllInOneOutcome& o) {
    std::vector<std::string> lines;
    if (!o.ok) {
        lines.push_back("RESULT: not applied -- " + o.error);
        add_resets(lines, driver_resets(o));
        return lines;
    }
    const Profile& p = o.profile;
    char line[256];
    int n = std::snprintf(line, sizeof(line), "RESULT: power %d %%, core +%d MHz, mem +%d MHz", p.power_pct, p.core_mhz, p.mem_mhz);
    if (p.undervolt && n > 0 && n < static_cast<int>(sizeof(line)))
        n += std::snprintf(line + n, sizeof(line) - n, ", %d MHz at %d mV", p.undervolt->freq_khz / 1000, p.undervolt->volt_uv / 1000);
    if (o.fan.ok && n > 0 && n < static_cast<int>(sizeof(line)))
        std::snprintf(line + n, sizeof(line) - n, ", fans %d %% at %d C", o.fan.hold_pct, o.fan.hold_temp_c);
    lines.push_back(line);
    add_resets(lines, driver_resets(o));
    if (!o.undervolt_note.empty()) lines.push_back("  no undervolt: " + o.undervolt_note);
    if (!o.fan_note.empty()) lines.push_back("  fans: " + o.fan_note);
    if (!o.disturbed.empty()) lines.push_back("  not clean: " + o.disturbed);
    lines.push_back(measured_line("stock:", o.stock, true));
    lines.push_back(measured_line("now:  ", o.now, true));
    if (o.stock.score > 0 && o.stock.avg_power_w > 0) {
        std::snprintf(line, sizeof(line), "  that is %+.1f %% score and %+.1f %% power against stock", (o.now.score / o.stock.score - 1) * 100,
                      (static_cast<double>(o.now.avg_power_w) / o.stock.avg_power_w - 1) * 100);
        lines.push_back(line);
    }
    return lines;
}

namespace {
// "driver 621.30 (it was tested on 621.14)"; just "driver 621.14" when that is the one it was tested on.
std::string retest_driver(const RetestOutcome& o) {
    return "driver " + o.driver + (o.driver == o.driver_was ? std::string() : " (it was tested on " + o.driver_was + ")");
}
// Step 0 is the tune the run found.
std::string step_name(int step) { return step == 0 ? std::string("the tune the run found") : step_text(step); }
}

std::vector<std::string> result_lines(const RetestOutcome& o) {
    std::vector<std::string> lines;
    if (o.result.test.seconds > 0) lines.push_back(measured_line(verdict_name(o.result.test.verdict), o.result.test, true));
    if (!o.ok) {
        // Every step from the first to the last one tested failed; a stopped test failed nothing.
        const bool named = o.result.test.seconds > 0 && o.result.test.verdict != Verdict::Aborted &&
                           (o.final_step != o.first_step || o.first_step > 0);
        const std::string failed =
            !named ? std::string()
                   : step_name(o.first_step) + (o.final_step != o.first_step ? " to " + step_name(o.final_step) : std::string()) +
                         " failed: ";
        lines.push_back("RESULT: not applied -- " + failed + o.error);
    } else if (o.final_step != o.first_step) {
        // Every step from the first to the one below the step that held failed.
        const int last_failed = o.final_step - 1;
        const std::string failed =
            step_name(o.first_step) + (last_failed > o.first_step ? " to " + step_name(last_failed) : std::string());
        lines.push_back("RESULT: the tune held at " + step_text(o.final_step) + " after " + failed + " failed, on " + retest_driver(o) +
                        ", and is applied: " + profile_text(o.profile));
    } else
        lines.push_back("RESULT: the tune holds on " + retest_driver(o) + " and is applied: " + profile_text(o.profile));
    return lines;
}

bool retest_failed(const RetestOutcome& o) {
    const Verdict v = o.result.test.verdict;
    return o.ran && !o.ok && o.result.test.seconds > 0 && v != Verdict::Stable && v != Verdict::Aborted;
}

std::string retest_boot_line(const RetestOutcome& o) {
    return std::string(o.driver == o.driver_was ? "tested on " : "tested again on ") + retest_driver(o) + ": applied " +
           profile_text(o.profile);
}
}
