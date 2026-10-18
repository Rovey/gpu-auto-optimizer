#include "app/common.hpp"
#include "core/stability.hpp"
#include <cstdio>

namespace gao::app {

int driver_resets(const OptimizeOutcome& o) { return o.ran ? o.result.driver_resets : 0; }
int driver_resets(const UndervoltOutcome& o) { return o.ran ? o.result.driver_resets : 0; }
int driver_resets(const AllInOneOutcome& o) { return driver_resets(o.overclock) + driver_resets(o.undervolt); }

namespace {

// One measurement, as the result lines show it. with_mem: the memory clock too.
std::string measured_line(const char* name, const StabilityResult& s, bool with_mem) {
    char mem[32] = "";
    if (with_mem) std::snprintf(mem, sizeof(mem), "  mem=%d MHz", s.avg_mem_mhz);
    char line[256];
    std::snprintf(line, sizeof(line), "  %s score=%.0f it/s  core=%d MHz%s  peak=%d C  fan=%s  power=%d W  (%.0f s)", name, s.score,
                  s.avg_core_mhz, mem, s.peak_temp_c, reading(s.end_fan_pct, " %").c_str(), s.avg_power_w, s.seconds);
    return line;
}

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

std::vector<std::string> result_lines(const RetestOutcome& o) {
    std::vector<std::string> lines;
    if (o.result.test.seconds > 0) lines.push_back(measured_line(verdict_name(o.result.test.verdict), o.result.test, true));
    if (!o.ok) lines.push_back("RESULT: not applied -- " + o.error);
    else
        lines.push_back("RESULT: the tune holds on driver " + o.driver + " (it was tested on " + o.driver_was + ") and is applied: " +
                        profile_text(o.profile));
    return lines;
}
}
