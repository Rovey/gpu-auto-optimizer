#include "app/gui/ui.hpp"
#include "app/gui/ui_widgets.hpp"
#include "core/version.hpp"

namespace gao::gui {

namespace {

constexpr size_t kHistory = 60;

// ------------------------------------------------------------------ frame

void nav_item(UiState& s, Page page, Icon icon, const char* label) {
    const bool selected = s.page == page;
    const ImVec2 size(ImGui::GetContentRegionAvail().x, em() * 2.4f);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    if (ImGui::InvisibleButton(label, size)) s.page = page;
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (selected || hovered)
        dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), ImGui::GetColorU32(selected ? kAccent : kInner), em() * 0.35f);
    const float y = p.y + (size.y - em()) / 2;
    if (icon == kIconFan) draw_fan(dl, ImVec2(p.x + em() * 1.4f, y + em() * 0.5f), em() * 0.55f, ImGui::GetColorU32(kText));
    else dl->AddText(ImVec2(p.x + em() * 0.9f, y), ImGui::GetColorU32(kText), ic(icon).c_str());
    dl->AddText(ImVec2(p.x + em() * 2.6f, y), ImGui::GetColorU32(kText), label);
}

// One quiet line above the footer: a new release, installed on a click. Never
// a dialog, never in the way.
void update_line(const UiState& s, const UiActions& act) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 size(ImGui::GetContentRegionAvail().x, em() * 1.5f);
    const bool clicked = ImGui::InvisibleButton("##update", size) && !s.update_busy;
    const bool hovered = ImGui::IsItemHovered();
    const ImVec4 colour = s.update_busy ? kDim : hovered ? kText : kAccentBright;
    const std::string text = ic(kIconDownload) + (s.update_busy            ? "  Updating..."
                                                   : !s.update_error.empty() ? "  Update failed, click to retry"
                                                                             : "  Update available");
    ImGui::GetWindowDrawList()->AddText(ImVec2(p.x, p.y + (size.y - em()) / 2), ImGui::GetColorU32(colour), text.c_str());
    if (hovered && !s.update_busy) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImGui::SetTooltip("%s", ("Version " + s.update_version + " is available (this is " + std::string(kVersion) +
                                 ").\nClick to download it, check it and restart the app." +
                                 (s.update_error.empty() ? "" : "\n\nThe last attempt failed: " + s.update_error))
                                    .c_str());
    }
    if (clicked) act.install_update();
}

void sidebar(UiState& s, const UiActions& act) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kNav);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(em() * 0.6f, em() * 0.8f));
    ImGui::BeginChild("nav", ImVec2(em() * 13, 0), ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_NavFlattened,
                      ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    nav_item(s, Page::Dashboard, kIconHome, "Dashboard");
    nav_item(s, Page::Optimize, kIconPulse, "Optimize");
    nav_item(s, Page::Fan, kIconFan, "Fan");
    nav_item(s, Page::About, kIconInfo, "About");
    const bool update = !s.update_version.empty() || s.update_busy;
    ImGui::SetCursorPosY(ImGui::GetWindowHeight() - em() * (update ? 3.9f : 2.2f));
    if (update) update_line(s, act);
    ImGui::SetCursorPosY(ImGui::GetWindowHeight() - em() * 2.2f);
    ImGui::PushStyleColor(ImGuiCol_Text, s.elevated ? kGood : kDim);
    ImGui::TextUnformatted((ic(kIconShield) + (s.elevated ? "  Administrator" : "  Not administrator")).c_str());
    ImGui::PopStyleColor();
    ImGui::EndChild();
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
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("GPU Auto Optimizer", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::PopStyleVar();
    sidebar(s, act);
    ImGui::SameLine(0, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(em() * 0.9f, em() * 0.9f));
    // The scrollbar's room always kept: a page that grows past the window
    // (the question about other programs) does not move what is at its right edge.
    ImGui::BeginChild("content", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_NavFlattened,
                      ImGuiWindowFlags_AlwaysVerticalScrollbar);
    ImGui::PopStyleVar();
    switch (s.page) {
        case Page::Dashboard: dashboard(s, act); break;
        case Page::Optimize: optimize_page(s, run, act); break;
        case Page::Fan: fan_page(s, act); break;
        case Page::About: about(s, act); break;
    }
    ImGui::EndChild();
    ImGui::End();
}

}
