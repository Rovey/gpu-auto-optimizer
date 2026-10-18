#include "app/gui/ui_widgets.hpp"
#include "core/boot.hpp"
#include "core/watchdog.hpp"
#include <algorithm>
#include <cstdio>
#include <vector>

namespace gao::gui {

namespace {

constexpr size_t kLogShort = 6;   // log lines on the dashboard before "View full log"

bool g_full_log = false;

// ------------------------------------------------------------------ dashboard

void gpu_card(const UiState& s, const UiActions& act) {
    begin_card("gpu");
    const float top = ImGui::GetCursorPosY();
    ImGui::BeginGroup();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, em() * 0.15f));
    text_bold(s.gpu_name.empty() ? "No NVIDIA GPU found" : s.gpu_name.c_str(), 1.6f);
    dim(("Driver " + (s.driver.empty() ? std::string("unknown") : s.driver)).c_str());
    ImGui::PopStyleVar();
    ImGui::EndGroup();
    const std::string detect = with_icon(kIconRefresh, "Detect GPU");
    align_right(button_width(detect));
    ImGui::SetCursorPosY(top);
    if (ImGui::Button(detect.c_str())) act.detect_gpu();
    ImGui::Spacing();
    telemetry_tiles(s);
    end_card();
}

void tuning_row(const char* label, const std::string& value, const ImVec4& color = kText) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    dim(label);
    ImGui::TableNextColumn();
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(value.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

// "5922 -> 6067 it/s (+2.4 %)", drawn with an arrow: a stock value next to
// the tune's; the change in percent only where it says something (score, power).
std::string stock_to_tune(double stock, double tuned, const char* unit, bool percent) {
    char buf[96];
    if (stock < 0 || tuned < 0) return "Not measured";
    if (percent && stock > 0)
        std::snprintf(buf, sizeof(buf), "%.0f \xE2\x86\x92 %.0f%s (%+.1f %%)", stock, tuned, unit, (tuned / stock - 1) * 100);
    else
        std::snprintf(buf, sizeof(buf), "%.0f \xE2\x86\x92 %.0f%s", stock, tuned, unit);
    return buf;
}

void tuning_card(UiState& s, const UiActions& act) {
    begin_card("tuning");
    heading("Current tuning");
    const float gap = ImGui::GetStyle().ItemSpacing.x;
    // The rows first, and the card after them: a value that wraps takes more
    // than one line, and a card that counted rows cut the last one off.
    struct Row {
        const char* label;
        std::string value;
        ImVec4 color;
    };
    std::vector<Row> rows;
    auto row = [&rows](const char* label, const std::string& value, const ImVec4& color = kText) { rows.push_back({label, value, color}); };
    {
        if (s.applied) {
            const AppliedState& a = *s.applied;
            // With an undervolt saved the curve has its say too: the offsets read +0 under a flat top.
            const bool undervolt = s.profile && s.profile->undervolt;
            const bool stock = a.core_mhz == 0 && a.mem_mhz == 0 && a.power_pct >= 99 && a.power_pct <= 101 &&
                               (!undervolt || s.curve == CurveState::Stock);
            const bool ours = s.profile && tune_applied(*s.profile, a, s.curve);
            if (ours) row("Applied now", std::string(title_of(s.profile->preset)) + " (the saved tune)", kGood);
            else if (stock) row("Applied now", "Stock");
            else row("Applied now", "Set by another program", kWarn);
            if (undervolt)
                row("Undervolt", ours ? undervolt_text(*s.profile->undervolt)
                                         : s.curve == CurveState::Stock ? "Not applied"
                                         : s.curve ? "Not applied (the curve carries other offsets)"
                                                   : "Unknown (the curve could not be read)",
                           ours ? kText : kDim);
            row("Core clock offset", offset(a.core_mhz));
            row("Memory clock offset", offset(a.mem_mhz));
            row("Power limit", std::to_string(a.power_pct) + " %");
        } else {
            row("Applied now", "Unknown (the driver does not report it)", kDim);
        }
        if (s.profile) {
            const Profile& p = *s.profile;
            std::string tune = std::to_string(p.power_pct) + " %, core " + offset(p.core_mhz) + ", memory " + offset(p.mem_mhz);
            if (p.undervolt) {   // clocks and power are stock under an undervolt: name only what is not
                tune = undervolt_text(*p.undervolt);
                if (p.power_pct != 100) tune += ", " + std::to_string(p.power_pct) + " %";
                if (p.mem_mhz != 0) tune += ", memory " + offset(p.mem_mhz);
            }
            row("Saved tune", std::string(title_of(p.preset)) + ": " + tune + " (" + p.saved_at + ")");
            // What the run that saved it measured under full load, five minutes each.
            if (const auto& m = p.against_stock; m && s.profile_gpu_ok) {
                row("Speed vs stock", stock_to_tune(m->stock.score, m->tuned.score, " it/s", true));
                row("Power vs stock", stock_to_tune(m->stock.power_w, m->tuned.power_w, " W", true));
                row("Fans under load", stock_to_tune(m->stock.fan_pct, m->tuned.fan_pct, " %", false));
                row("Peak temperature", stock_to_tune(m->stock.peak_temp_c, m->tuned.peak_temp_c, " \xC2\xB0""C", false));
            } else if (s.profile_gpu_ok) {
                row("Against stock", "Measured by the next run: this tune was saved by an older version.", kDim);
            }
            if (!s.profile_gpu_ok)
                row("", "Tuned on " + std::string(s.profile_driver_ok ? "" : "driver " + p.driver + " and ") +
                                   "another card. Optimize again before relying on it.",
                           kWarn);
            else if (!s.profile_driver_ok)
                row("", "The driver was updated: this tune was tested on " + p.driver +
                                   (s.driver.empty() ? "" : ", and this is " + s.driver) +
                                   ". It is not applied until it has been tested again.",
                           kWarn);
        } else {
            row("Saved tune", "None yet. Pick a profile and optimize.", kDim);
        }
        if (s.strikes > 0)
            row("Crash strikes", std::to_string(s.strikes) + " of " + std::to_string(kMaxBootStrikes) +
                                            " (apply at logon stops at " + std::to_string(kMaxBootStrikes) + ")",
                       kWarn);
    }
    const ImGuiStyle& style = ImGui::GetStyle();
    const float right_w = std::max(em() * 17, ImGui::GetContentRegionAvail().x * 0.36f);
    const float left_w = ImGui::GetContentRegionAvail().x - right_w - gap;
    const float label_w = em() * 10;
    // What is left for a value: the card's padding and the table's cell padding off both columns.
    const float value_w = std::max(em() * 8, left_w - em() * 0.9f * 2 - label_w - style.CellPadding.x * 4);
    float rows_h = 0;
    for (const Row& r : rows)
        rows_h += std::max(em(), ImGui::CalcTextSize(r.value.c_str(), nullptr, false, value_w).y) + style.CellPadding.y * 2;
    // Four buttons in the actions column, with the spacing between them and the column's padding.
    const float buttons_h = em() * 2.3f * 4 + style.ItemSpacing.y * 3 + style.WindowPadding.y * 2;
    const float h = std::max(buttons_h, rows_h + em() * 1.8f);

    begin_card("values", h, kInner, left_w);
    if (ImGui::BeginTable("rows", 2, ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed, label_w);
        ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
        for (const Row& r : rows) tuning_row(r.label, r.value, r.color);
        ImGui::EndTable();
    }
    end_card();

    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kInner);
    ImGui::BeginChild("actions", ImVec2(0, h), ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleColor();
    const float bw = ImGui::GetContentRegionAvail().x, bh = em() * 2.3f;
    if (s.elevated && s.profile && s.profile_gpu_ok && !s.profile_driver_ok && !s.driver.empty()) {
        // After a driver update the tune is not applied as it is: five minutes show whether it still holds.
        if (primary_button("Test the tune (5 min)", ImVec2(bw, bh))) act.retest();
    } else if (s.elevated) {
        ImGui::BeginDisabled(!s.profile);
        if (primary_button("Apply saved tune", ImVec2(bw, bh))) act.apply_profile();
        ImGui::EndDisabled();
    } else {
        if (primary_button("Restart as administrator", ImVec2(bw, bh))) act.restart_elevated();
    }
    // Apply at logon: a framed row with a switch.
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kCard);
    ImGui::BeginChild("logon", ImVec2(bw, bh), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleColor();
    ImGui::SetCursorPos(ImVec2(em() * 0.8f, (bh - em()) / 2 - 1));
    ImGui::TextUnformatted("Apply at logon");
    ImGui::SameLine(bw - em() * 3.4f);
    ImGui::SetCursorPosY((bh - em() * 1.25f) / 2 - 1);
    // Turning it off never needs a profile; turning it on does.
    ImGui::BeginDisabled(!s.elevated || (!s.profile && !s.boot_on));
    if (toggle("##logon", s.boot_on)) act.set_boot(!s.boot_on);
    ImGui::EndDisabled();
    ImGui::EndChild();
    ImGui::BeginDisabled(!s.elevated);
    if (ImGui::Button(with_icon(kIconUndo, "Revert to stock").c_str(), ImVec2(bw, bh))) act.revert_to_stock();
    ImGui::EndDisabled();
    // The profiles are picked on the Optimize page; the dashboard only leads there.
    if (ImGui::Button(with_icon(kIconPulse, "Optimize...").c_str(), ImVec2(bw, bh))) s.page = Page::Optimize;
    ImGui::EndChild();
    end_card();
}

struct LogLine {
    std::string time, text;
    bool warn;
};

// boot.log lines and this session's notes, newest first.
std::vector<LogLine> log_lines(const UiState& s) {
    std::vector<LogLine> out;
    for (const std::string& line : s.boot_log) {
        const size_t sep = line.find("  ");
        LogLine l{sep == std::string::npos ? "" : line.substr(0, sep), sep == std::string::npos ? line : line.substr(sep + 2),
                  false};
        l.warn = l.text.find("not applied") != std::string::npos || l.text.find("failed") != std::string::npos ||
                 l.text.find("stopped") != std::string::npos || l.text.find("another program") != std::string::npos;
        out.push_back(std::move(l));
    }
    for (const Note& n : s.notes) out.push_back({n.time, n.text, n.warn});
    // Same-minute entries keep their order: boot.log first, then notes.
    std::stable_sort(out.begin(), out.end(), [](const LogLine& a, const LogLine& b) { return a.time > b.time; });
    return out;
}

void log_card(const UiState& s) {
    begin_card("log");
    heading("Log");
    const std::string more = with_icon(kIconList, g_full_log ? "Show recent" : "View full log");
    align_right(button_width(more));
    if (ImGui::Button(more.c_str())) g_full_log = !g_full_log;
    const auto lines = log_lines(s);
    if (lines.empty()) {
        dim("Nothing yet.");
    } else if (ImGui::BeginTable("log", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_BordersOuter |
                                               ImGuiTableFlags_PadOuterX)) {
        ImGui::TableSetupColumn("time", ImGuiTableColumnFlags_WidthFixed, em() * 8.5f);
        ImGui::TableSetupColumn("text", ImGuiTableColumnFlags_WidthStretch);
        const size_t n = g_full_log ? lines.size() : std::min(lines.size(), kLogShort);
        for (size_t i = 0; i < n; ++i) {
            ImGui::TableNextRow(ImGuiTableRowFlags_None, em() * 1.7f);
            ImGui::TableNextColumn();
            dim(lines[i].time.c_str());
            ImGui::TableNextColumn();
            wrapped(lines[i].warn ? kWarn : kText, lines[i].text);
        }
        ImGui::EndTable();
    }
    end_card();
}

}

void dashboard(UiState& s, const UiActions& act) {
    gpu_card(s, act);
    tuning_card(s, act);
    log_card(s);
}

}
