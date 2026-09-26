#include "app/gui/ui.hpp"
#include "imgui.h"
#include <algorithm>
#include <cstdio>
#include <vector>

namespace gao::gui {

namespace {

constexpr size_t kHistory = 60;
const ImVec4 kGood{0.45f, 0.85f, 0.45f, 1.0f};
const ImVec4 kWarn{0.95f, 0.75f, 0.30f, 1.0f};
const ImVec4 kBad{0.95f, 0.40f, 0.40f, 1.0f};
const ImVec4 kDim{0.60f, 0.60f, 0.65f, 1.0f};

struct PresetInfo {
    Preset preset;
    const char* title;
    const char* summary;
};
constexpr PresetInfo kPresets[] = {
    {Preset::BestOfMyGpu, "Best of my GPU",
     "Highest confirmed clocks with a safety margin and the most power the card allows, up to 75 C."},
    {Preset::Quiet, "Quiet",
     "Milder clocks and the lowest power limit that costs under 2 % speed, up to 80 C."},
    {Preset::CoolAndEfficient, "Cool & efficient",
     "Power only: the lowest power limit that costs under 2 % speed, clocks left stock, up to 65 C."},
    {Preset::MaxPerformance, "Max performance",
     "Highest confirmed clocks minus one step and the highest power limit, up to 83 C."},
};

std::string field(int v, const char* unit) {
    if (v < 0) return "n/a";
    return std::to_string(v) + unit;
}

void plot(const char* label, const std::deque<float>& h, float lo, float hi, const char* overlay) {
    std::vector<float> v(h.begin(), h.end());
    if (v.empty()) v.push_back(0);
    ImGui::PlotLines(label, v.data(), static_cast<int>(v.size()), 0, overlay, lo, hi, ImVec2(-1, 70));
}

void telemetry_row(const UiState& s) {
    const Telemetry& t = s.telemetry;
    ImGui::Text("Core %s   Memory %s   Temperature %s   Power %s / %d W   Fan %s", field(t.core_mhz, " MHz").c_str(),
                field(t.mem_mhz, " MHz").c_str(), field(t.temp_c, " C").c_str(), field(t.power_w, "").c_str(),
                t.power_limit_w, field(t.fan_pct, " %").c_str());
}

void status_block(const UiState& s, const UiActions& act) {
    ImGui::SeparatorText("Saved tune");
    if (!s.profile) {
        ImGui::TextColored(kDim, "None yet. Pick a preset and press Optimize.");
    } else {
        const Profile& p = *s.profile;
        ImGui::Text("%s: power %d %%, core +%d MHz, memory +%d MHz  (saved %s)", preset_name(p.preset), p.power_pct,
                    p.core_mhz, p.mem_mhz, p.saved_at.c_str());
        if (!s.profile_driver_ok || !s.profile_gpu_ok)
            ImGui::TextColored(kWarn, "Tuned on driver %s%s. Optimize again before relying on it.", p.driver.c_str(),
                               s.profile_gpu_ok ? "" : " and another card");
    }
    if (s.applied) {
        const AppliedState& a = *s.applied;
        const bool stock = a.core_mhz == 0 && a.mem_mhz == 0 && a.power_pct >= 99 && a.power_pct <= 101;
        const bool ours = s.profile && a.core_mhz == s.profile->core_mhz && a.mem_mhz == s.profile->mem_mhz &&
                          a.power_pct >= s.profile->power_pct - 1 && a.power_pct <= s.profile->power_pct + 1;
        ImGui::Text("Applied now: power %d %%, core %+d MHz, memory %+d MHz", a.power_pct, a.core_mhz, a.mem_mhz);
        ImGui::SameLine();
        if (ours) ImGui::TextColored(kGood, "(the saved tune)");
        else if (stock) ImGui::TextColored(kDim, "(stock)");
        else ImGui::TextColored(kWarn, "(set by something else)");
    }
    ImGui::Text("Apply at logon: %s", s.boot_on ? "on" : "off");
    if (s.strikes > 0) {
        ImGui::SameLine();
        ImGui::TextColored(kWarn, "  %d of 3 crash strikes", s.strikes);
    }
    if (!s.last_boot.empty()) ImGui::TextColored(kDim, "Last logon: %s", s.last_boot.c_str());
    if (!s.watchdog_note.empty()) ImGui::TextColored(kWarn, "%s", s.watchdog_note.c_str());

    if (s.elevated) {
        ImGui::BeginDisabled(!s.profile);
        if (ImGui::Button("Apply saved tune")) act.apply_profile();
        ImGui::SameLine();
        if (ImGui::Button(s.boot_on ? "Turn off apply at logon" : "Apply at every logon")) act.set_boot(!s.boot_on);
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Revert to stock")) act.revert_to_stock();
    }
}

void home(UiState& s, const UiActions& act) {
    ImGui::Text("%s", s.gpu_name.empty() ? "No NVIDIA GPU found" : s.gpu_name.c_str());
    ImGui::SameLine();
    ImGui::TextColored(kDim, "  driver %s", s.driver.empty() ? "unknown" : s.driver.c_str());
    telemetry_row(s);
    status_block(s, act);

    ImGui::SeparatorText("Optimize");
    const float w = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) / 2;
    int i = 0;
    for (const PresetInfo& p : kPresets) {
        ImGui::PushID(i);
        const bool selected = s.preset == p.preset;
        ImGui::PushStyleColor(ImGuiCol_Border, selected ? kGood : ImGui::GetStyleColorVec4(ImGuiCol_Border));
        ImGui::BeginChild("preset", ImVec2(w, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
        if (ImGui::RadioButton(p.title, selected)) s.preset = p.preset;
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(kDim, "%s", p.summary);
        ImGui::PopTextWrapPos();
        ImGui::EndChild();
        ImGui::PopStyleColor();
        if (i % 2 == 0) ImGui::SameLine();
        ImGui::PopID();
        ++i;
    }
    ImGui::Spacing();
    if (s.elevated) {
        if (ImGui::Button("Optimize", ImVec2(160, 0))) act.optimize(s.preset);
        ImGui::SameLine();
        ImGui::TextColored(kDim, "About 10 minutes of full GPU load. Abort restores stock.");
    } else {
        if (ImGui::Button("Restart as administrator", ImVec2(220, 0))) act.restart_elevated();
        ImGui::SameLine();
        ImGui::TextColored(kDim, "Optimizing and applying change clocks and power limits.");
    }
}

void run_screen(UiState& s, const OptimizeWorker::Snapshot& run, const UiActions& act) {
    ImGui::Text("Optimizing: %s", preset_name(s.preset));
    telemetry_row(s);
    char overlay[64];
    std::snprintf(overlay, sizeof(overlay), "temperature %s", field(s.telemetry.temp_c, " C").c_str());
    plot("##temp", s.temp_history, 30, 90, overlay);
    std::snprintf(overlay, sizeof(overlay), "power %s W", field(s.telemetry.power_w, "").c_str());
    plot("##power", s.power_history, 0, static_cast<float>(std::max(s.telemetry.power_limit_w, 1)), overlay);
    if (run.running) {
        if (ImGui::Button("Abort", ImVec2(120, 0))) act.abort();
        ImGui::SameLine();
        ImGui::TextColored(kDim, "Aborting finishes the current probe, then restores stock.");
    }
    ImGui::SeparatorText("Log");
    ImGui::BeginChild("log", ImVec2(0, 0), ImGuiChildFlags_Borders);
    for (const std::string& line : run.log) ImGui::TextUnformatted(line.c_str());
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
    if (!run.running && run.outcome) s.screen = Screen::Results;
}

void results(UiState& s, const OptimizeWorker::Snapshot& run, const UiActions& act) {
    if (!run.outcome) {
        s.screen = Screen::Home;
        return;
    }
    const app::OptimizeOutcome& o = *run.outcome;
    if (!o.ran) {
        ImGui::TextColored(kBad, "Not started: %s", o.error.c_str());
    } else if (!o.result.ok) {
        ImGui::TextColored(kBad, "Not applied: %s", o.result.reason.c_str());
        ImGui::Text("%s", o.result.stock_restored ? "The card is back at stock." : "Reset to stock FAILED -- run `gao --reset`.");
    } else {
        const OptimizeResult& r = o.result;
        ImGui::TextColored(kGood, "Applied: power %d %%, core +%d MHz, memory +%d MHz", r.power_pct, r.core_mhz, r.mem_mhz);
        ImGui::TextColored(kDim, "Confirmed edges: core +%d MHz, memory +%d MHz", r.core_confirmed, r.mem_confirmed);
        if (ImGui::BeginTable("cmp", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchSame)) {
            ImGui::TableSetupColumn("");
            ImGui::TableSetupColumn("Before (stock)");
            ImGui::TableSetupColumn("After");
            ImGui::TableHeadersRow();
            auto row = [](const char* name, const std::string& a, const std::string& b) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(name);
                ImGui::TableNextColumn(); ImGui::TextUnformatted(a.c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(b.c_str());
            };
            const double gain = r.baseline.score > 0 ? (r.soak.score / r.baseline.score - 1) * 100 : 0;
            char sb[32], sa[32];
            std::snprintf(sb, sizeof(sb), "%.0f it/s", r.baseline.score);
            std::snprintf(sa, sizeof(sa), "%.0f it/s (%+.1f %%)", r.soak.score, gain);
            row("Score", sb, sa);
            row("Core clock", field(r.baseline.avg_core_mhz, " MHz"), field(r.soak.avg_core_mhz, " MHz"));
            row("Memory clock", field(r.baseline.avg_mem_mhz, " MHz"), field(r.soak.avg_mem_mhz, " MHz"));
            row("Peak temperature", field(r.baseline.peak_temp_c, " C"), field(r.soak.peak_temp_c, " C"));
            row("Power", field(r.baseline.avg_power_w, " W"), field(r.soak.avg_power_w, " W"));
            ImGui::EndTable();
        }
        if (o.saved) ImGui::Text("Saved. It stays applied until reboot; turn on apply at logon to keep it.");
        else ImGui::TextColored(kWarn, "Not saved: %s", o.save_note.c_str());
        if (o.saved && ImGui::Button(s.boot_on ? "Apply at logon is on" : "Apply at every logon")) {
            if (!s.boot_on) act.set_boot(true);
        }
        ImGui::SameLine();
        if (ImGui::Button("Revert to stock")) act.revert_to_stock();
    }
    ImGui::Spacing();
    if (ImGui::Button("Back")) s.screen = Screen::Home;
}

}

void push_history(std::deque<float>& h, float v) {
    h.push_back(v);
    while (h.size() > kHistory) h.pop_front();
}

void draw_ui(UiState& s, const OptimizeWorker::Snapshot& run, const UiActions& act) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("GPU Auto Optimizer", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);
    if (run.running && s.screen != Screen::Run) s.screen = Screen::Run;
    switch (s.screen) {
        case Screen::Home: home(s, act); break;
        case Screen::Run: run_screen(s, run, act); break;
        case Screen::Results: results(s, run, act); break;
    }
    if (!s.message.empty()) {
        ImGui::Separator();
        ImGui::TextWrapped("%s", s.message.c_str());
    }
    ImGui::End();
}

}
