#include "app/gui/ui_widgets.hpp"
#include <cstdio>

namespace gao::gui {

namespace {

// A before/after table; `rows` fills it through `row(name, before, after)`.
// The two runs it compares differ in length, and it says so: a card is not
// heated through after half a minute, so the temperature and the fan speed
// of the short run read lower than they would on a warm card.
template <typename Rows>
void comparison(const StabilityResult& before, const StabilityResult& after, Rows rows) {
    if (!ImGui::BeginTable("cmp", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_BordersOuter |
                                         ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_PadOuterX))
        return;
    ImGui::TableSetupColumn("");
    ImGui::TableSetupColumn(("Before (stock, " + duration_text(before.seconds) + ")").c_str());
    ImGui::TableSetupColumn(("After (" + duration_text(after.seconds) + ")").c_str());
    ImGui::TableHeadersRow();
    rows([](const char* name, const std::string& a, const std::string& b) {
        ImGui::TableNextRow(ImGuiTableRowFlags_None, em() * 1.6f);
        ImGui::TableNextColumn(); dim(name);
        ImGui::TableNextColumn(); ImGui::TextUnformatted(a.c_str());
        ImGui::TableNextColumn(); ImGui::TextUnformatted(b.c_str());
    });
    ImGui::EndTable();
    if (before.verdict == Verdict::TooHot)
        wrapped(kDim, "The stock run was stopped at " + reading(before.peak_temp_c, " \xC2\xB0""C") + " after " + duration_text(before.seconds) +
                          ": with the NVIDIA driver's own fan control (fans at " + reading(before.end_fan_pct, " %") +
                          ") the card gets that hot under full load. From there the run kept the fans on a curve.");
    else if (after.seconds > 3 * before.seconds)
        wrapped(kDim, "The stock run lasted " + duration_text(before.seconds) +
                          ", too short to heat the card through: its temperature and fan speed read lower than they would after " +
                          duration_text(after.seconds) + ". Score, clock and power compare directly.");
}

// What a finished run found. True when a result is applied.
bool optimize_result(const app::OptimizeOutcome& o, bool put_back) {
    if (!o.ran) {
        heading("Not started");
        wrapped(kBad, o.error);
        return false;
    }
    if (!o.result.ok) {
        heading("Not applied");
        wrapped(kBad, o.result.reason);
        if (!put_back)
            wrapped(kText, o.result.stock_restored ? "The card is back at stock." : "Reset to stock FAILED -- run `gao --reset`.");
        return false;
    }
    const OptimizeResult& r = o.result;
    heading("Done");
    char line[160];
    std::snprintf(line, sizeof(line), "Applied: power %d %%, core +%d MHz, memory +%d MHz", r.power_pct, r.core_mhz, r.mem_mhz);
    wrapped(kGood, line);
    std::snprintf(line, sizeof(line), "Confirmed edges: core +%d MHz, memory +%d MHz. The difference is the safety margin.",
                  r.core_confirmed, r.mem_confirmed);
    wrapped(kDim, line);
    ImGui::Spacing();
    const StabilityResult& before = o.stock;   // five minutes, like the soak
    comparison(before, r.soak, [&](auto row) {
        row("Score", score_text(before.score), against(before.score, r.soak.score, "it/s"));
        row("Core clock", reading(before.avg_core_mhz, " MHz"), reading(r.soak.avg_core_mhz, " MHz"));
        row("Memory clock", reading(before.avg_mem_mhz, " MHz"), reading(r.soak.avg_mem_mhz, " MHz"));
        row("Peak temperature", reading(before.peak_temp_c, " \xC2\xB0""C"), reading(r.soak.peak_temp_c, " \xC2\xB0""C"));
        row("Fan speed at the end", reading(before.end_fan_pct, " %"), reading(r.soak.end_fan_pct, " %"));
        row("Power", reading(before.avg_power_w, " W"), reading(r.soak.avg_power_w, " W"));
    });
    return true;
}

bool undervolt_result(const app::UndervoltOutcome& o, bool put_back) {
    if (!o.ran) {
        heading("Not started");
        wrapped(kBad, o.error);
        return false;
    }
    if (!o.result.ok) {
        heading("Not applied");
        wrapped(kBad, o.result.reason);
        if (!put_back)
            wrapped(kText, o.result.stock_restored ? "The card is back at stock." : "Reset to stock FAILED -- run `gao --reset`.");
        return false;
    }
    const UndervoltResult& r = o.result;
    heading("Done");
    char line[200];
    std::snprintf(line, sizeof(line), "Applied: %d MHz at %d mV instead of %d mV", r.freq_khz / 1000, r.applied_uv / 1000,
                  r.stock_uv / 1000);
    wrapped(kGood, line);
    std::snprintf(line, sizeof(line), "Lowest stable voltage: %d mV, confirmed %d mV. The difference is the safety margin.",
                  r.edge_uv / 1000, r.confirmed_uv / 1000);
    wrapped(kDim, line);
    ImGui::Spacing();
    comparison(r.baseline, r.after, [&](auto row) {
        row("Score", score_text(r.baseline.score), against(r.baseline.score, r.after.score, "it/s"));
        row("Core clock", reading(r.baseline.avg_core_mhz, " MHz"), reading(r.after.avg_core_mhz, " MHz"));
        row("Voltage for that clock", std::to_string(r.stock_uv / 1000) + " mV", std::to_string(r.applied_uv / 1000) + " mV");
        row("Peak temperature", reading(r.baseline.peak_temp_c, " \xC2\xB0""C"), reading(r.after.peak_temp_c, " \xC2\xB0""C"));
        row("Fan speed at the end", reading(r.baseline.end_fan_pct, " %"), reading(r.after.end_fan_pct, " %"));
        row("Power", reading(r.baseline.avg_power_w, " W"),
            r.baseline.avg_power_w > 0 && r.after.avg_power_w >= 0 ? against(r.baseline.avg_power_w, r.after.avg_power_w, "W")
                                                                   : reading(r.after.avg_power_w, " W"));
    });
    return true;
}

// The all-in-one run: what the card runs now, next to what it ran at stock.
bool all_in_one_result(const app::AllInOneOutcome& o, bool put_back) {
    if (!o.ok) {
        heading(o.ran ? "Not applied" : "Not started");
        wrapped(kBad, o.error);
        if (o.ran && !put_back) wrapped(kText, "The card is at stock.");
        return false;
    }
    const Profile& p = o.profile;
    heading("Done");
    wrapped(kGood, "Applied: core " + offset(p.core_mhz) + ", memory " + offset(p.mem_mhz) + ", power limit " + std::to_string(p.power_pct) +
                       " %" + (p.undervolt ? ", " + undervolt_text(*p.undervolt) : std::string()));
    if (o.fan.ok)
        wrapped(kGood, "Fans: " + std::to_string(o.fan.hold_pct) + " % keeps the card at " + std::to_string(o.fan.hold_temp_c) +
                           " \xC2\xB0""C under full load; the fan curve goes through that point.");
    if (!o.undervolt_note.empty()) wrapped(kWarn, "No undervolt on top of the overclock: " + o.undervolt_note + ".");
    if (!o.fan_note.empty()) wrapped(kWarn, "Fans: " + o.fan_note + ".");
    if (!o.disturbed.empty()) wrapped(kWarn, "The numbers below are not clean: " + o.disturbed + ".");
    ImGui::Spacing();
    comparison(o.stock, o.now, [&](auto row) {
        row("Score", score_text(o.stock.score), against(o.stock.score, o.now.score, "it/s"));
        row("Core clock", reading(o.stock.avg_core_mhz, " MHz"), reading(o.now.avg_core_mhz, " MHz"));
        row("Memory clock", reading(o.stock.avg_mem_mhz, " MHz"), reading(o.now.avg_mem_mhz, " MHz"));
        row("Peak temperature", reading(o.stock.peak_temp_c, " \xC2\xB0""C"), reading(o.now.peak_temp_c, " \xC2\xB0""C"));
        row("Fan speed at the end", reading(o.stock.end_fan_pct, " %"), reading(o.now.end_fan_pct, " %"));
        row("Power", reading(o.stock.avg_power_w, " W"),
            o.stock.avg_power_w > 0 && o.now.avg_power_w >= 0 ? against(o.stock.avg_power_w, o.now.avg_power_w, "W")
                                                              : reading(o.now.avg_power_w, " W"));
    });
    return true;
}

// The saved tune after its five minutes on a new driver. True: it is applied.
bool retest_result(const app::RetestOutcome& o) {
    if (!o.ok) {
        heading("Not applied");
        wrapped(kBad, o.error);
        if (o.result.test.seconds > 0)
            wrapped(kText, "The saved tune is kept as it is and stays off on this driver. Optimize again to find one that holds.");
        return false;
    }
    heading("The tune holds");
    wrapped(kGood, "Five minutes of full load on driver " + o.driver + " (it was tested on " + o.driver_was +
                       "): stable. It is applied again: core " + offset(o.profile.core_mhz) + ", memory " + offset(o.profile.mem_mhz) +
                       ", power limit " + std::to_string(o.profile.power_pct) + " %" +
                       (o.profile.undervolt ? ", " + undervolt_text(*o.profile.undervolt) : std::string()));
    const StabilityResult& t = o.result.test;
    wrapped(kDim, "Score " + score_text(t.score) + ", core " + reading(t.avg_core_mhz, " MHz") + ", memory " +
                      reading(t.avg_mem_mhz, " MHz") + ", peak " + reading(t.peak_temp_c, " \xC2\xB0""C") + ", fans " +
                      reading(t.end_fan_pct, " %") + " at the end, " + reading(t.avg_power_w, " W") + ".");
    return true;
}

// After a driver reset the run recovers, but Windows and the other programs
// may not: on an RTX 5070 the desktop stayed black but for this window
// (hardware check 99). "Nothing left to do" would not be true then.
std::string reset_note(int resets) {
    return "The graphics driver was reset during this run" +
           (resets > 1 ? " (" + std::to_string(resets) + " times)" : std::string()) +
           ". The run recovered, but Windows and other programs may not have: if windows stay black or do not respond, "
           "sign out and back in, or restart the computer.";
}

}

void results(UiState& s, const OptimizeWorker::Snapshot& run, const UiActions& act) {
    if (!run.ended()) {
        s.screen = Screen::Choose;
        return;
    }
    begin_card("results");
    const bool applied = run.retest       ? retest_result(*run.retest)
                         : run.all_in_one ? all_in_one_result(*run.all_in_one, run.put_back)
                         : run.undervolt  ? undervolt_result(*run.undervolt, run.put_back)
                                          : optimize_result(*run.outcome, run.put_back);
    // Stopped, and the saved tune that was on the card before the run is on it again (worker.cpp).
    if (!applied && run.put_back) wrapped(kText, "Your saved tune is on the card again, as it was before the run.");
    const int resets = run.all_in_one ? app::driver_resets(*run.all_in_one)
                       : run.undervolt ? app::driver_resets(*run.undervolt)
                       : run.outcome   ? app::driver_resets(*run.outcome)
                                       : 0;
    if (!applied && resets > 0) wrapped(kWarn, reset_note(resets));
    if (applied) {
        const bool saved = run.retest ? true : run.all_in_one ? run.all_in_one->ok : run.undervolt ? run.undervolt->saved : run.outcome->saved;
        ImGui::Spacing();
        if (saved && s.boot_on)
            wrapped(kText, resets > 0 ? "Saved, and applied at every logon from now on."
                                      : "Saved, and applied at every logon from now on. There is nothing left to do.");
        else if (saved) wrapped(kText, "Saved. It stays applied until reboot; turn on apply at logon to keep it.");
        else wrapped(kWarn, "Not saved: " + (run.undervolt ? run.undervolt->save_note : run.outcome ? run.outcome->save_note : std::string()));
        if (run.outcome && !run.outcome->busy_note.empty())
            wrapped(kWarn, run.outcome->busy_note + ". The numbers of this run and the ones at stock were not measured on the same card, "
                           "so the comparison can be off by several percent.");
        if (resets > 0) wrapped(kWarn, reset_note(resets));
        if (saved && !s.boot_on && primary_button("Apply at every logon")) act.set_boot(true);
        if (saved && !s.boot_on) ImGui::SameLine();
        if (ImGui::Button(with_icon(kIconUndo, "Revert to stock").c_str())) act.revert_to_stock();
        ImGui::SameLine();
    }
    if (ImGui::Button(with_icon(kIconBack, "Back").c_str())) s.screen = Screen::Choose;
    end_card();
    if (applied) curve_card(s, "As the run left it. Higher is faster at that voltage.", false);
}

}
