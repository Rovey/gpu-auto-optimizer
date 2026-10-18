#include "app/gui/ui_widgets.hpp"
#include "core/objectives.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace gao::gui {

// ------------------------------------------------------------------ optimize

namespace {

// An icon in a square badge, centred by the glyph's own outline and not by
// its advance: the glyphs of the icon font differ in width and in where
// they sit in their cell, which next to a title reads as uneven spacing.
void icon_badge(Icon icon, float side, const ImVec4& fill, const ImVec4& edge, const ImVec4& ink) {
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(side, side));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 corner(at.x + side, at.y + side);
    dl->AddRectFilled(at, corner, ImGui::GetColorU32(fill), side * 0.28f);
    dl->AddRect(at, corner, ImGui::GetColorU32(edge), side * 0.28f);
    ImGui::PushFont(nullptr, side * 0.5f);
    ImVec2 pen(at.x + side * 0.25f, at.y + side * 0.25f);   // a glyph the font does not have: roughly centred
    if (const ImFontGlyph* glyph = ImGui::GetFontBaked()->FindGlyph(static_cast<ImWchar>(icon)))
        pen = ImVec2(at.x + (side - glyph->X0 - glyph->X1) / 2, at.y + (side - glyph->Y0 - glyph->Y1) / 2);
    dl->AddText(ImVec2(std::floor(pen.x + 0.5f), std::floor(pen.y + 0.5f)), ImGui::GetColorU32(ink), ic(icon).c_str());
    ImGui::PopFont();
}

// What a profile card says. A no-break space keeps the temperature and its unit on one line.
std::string card_text(const PresetInfo& p) {
    return std::string(p.summary) + " Up to " + std::to_string(objectives_for(p.preset).max_temp_c) + "\xC2\xA0\xC2\xB0""C.";
}

// One profile as a card; clicking it selects it. h 0: as tall as its contents.
// The selected card says so with its border and its badge; there is no
// radio button next to them saying it a third time.
void preset_card(UiState& s, const PresetInfo& p, float w, float h) {
    ImGui::PushID(static_cast<int>(p.preset));
    const bool selected = s.preset == p.preset;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, selected ? kSelectedBg : kInner);
    ImGui::PushStyleColor(ImGuiCol_Border, selected ? kAccentBright : kBorder);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, selected ? 2.0f : 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(em() * 0.8f, em() * 0.7f));
    ImGui::BeginChild("preset", ImVec2(w, h),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_NavFlattened |
                          (h == 0 ? ImGuiChildFlags_AutoResizeY : ImGuiChildFlags_None),
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
    const float badge = em() * 2.1f;
    icon_badge(p.icon, badge, selected ? kAccent : kCard, selected ? kAccentBright : kBorder, selected ? kText : kDim);
    ImGui::SameLine(0, em() * 0.7f);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (badge - em() * 1.05f) / 2 - em() * 0.08f);   // level with the badge
    // A title wider than a narrow card is drawn smaller, not cut off.
    ImGui::PushFont(g_bold, base() * 1.05f);
    const float need = ImGui::CalcTextSize(p.title).x, room = ImGui::GetContentRegionAvail().x;
    ImGui::PopFont();
    text_bold(p.title, need > room && need > 0 ? std::max(0.8f, 1.05f * room / need) : 1.05f);
    wrapped(kDim, card_text(p));
    // The card as one button over its contents, for the keyboard: Tab
    // reaches it, Enter or Space selects it. No higher than the contents, or
    // a card as tall as its contents would grow by it every frame.
    const ImVec2 start = ImGui::GetCursorStartPos();
    const float bottom = ImGui::GetCursorPosY() - ImGui::GetStyle().ItemSpacing.y;
    ImGui::SetCursorPos(start);
    if (ImGui::InvisibleButton("select", ImVec2(ImGui::GetContentRegionAvail().x, std::max(1.0f, bottom - start.y))))
        s.preset = p.preset;
    ImGui::EndChild();
    if (ImGui::IsItemClicked()) s.preset = p.preset;
    ImGui::PopID();
}

// All in one first, across the full width; the four clock profiles side by
// side; and the undervolt below them, the other kind of tune.
void preset_cards(UiState& s) {
    preset_card(s, kAllInOne, ImGui::GetContentRegionAvail().x, 0);
    const float gap = ImGui::GetStyle().ItemSpacing.x;
    const float w = (ImGui::GetContentRegionAvail().x - gap * 3) / 4;
    // Side by side the four are as tall as each other: as the one with the
    // most text needs, in a narrow window too.
    float h = em() * 8.3f;
    for (const PresetInfo& p : kPresets)
        h = std::max(h, em() * (0.7f * 2 + 2.1f) + ImGui::GetStyle().ItemSpacing.y +
                            ImGui::CalcTextSize(card_text(p).c_str(), nullptr, false, w - em() * 0.8f * 2).y + em() * 0.2f);
    for (size_t i = 0; i < std::size(kPresets); ++i) {
        preset_card(s, kPresets[i], w, h);
        if (i + 1 < std::size(kPresets)) ImGui::SameLine();
    }
    preset_card(s, kUndervolt, ImGui::GetContentRegionAvail().x, 0);   // as tall as its text, at any width
}

void plot(const char* label, const std::deque<float>& h, float lo, float hi, const char* overlay, float w) {
    std::vector<float> v(h.begin(), h.end());
    if (v.empty()) v.push_back(0);
    ImGui::PlotLines(label, v.data(), static_cast<int>(v.size()), 0, overlay, lo, hi, ImVec2(w, em() * 5));
}

void choose(UiState& s, const UiActions& act) {
    begin_card("choose");
    heading("Choose a profile");
    dim("Each profile searches for the highest stable settings and applies a safety margin.");
    const bool undervolt = s.preset == Preset::Undervolt, all = s.preset == Preset::AllInOne;
    ImGui::Spacing();
    preset_cards(s);
    ImGui::Spacing();
    // The undervolt search keeps the fans on the curve in use; the
    // all-in-one run measures its own. Said, instead of a row that only
    // looks a little dimmer and does nothing.
    if (all) {
        dim("Fan curve during the run: All in one measures its own fan curve.");
    } else if (undervolt) {
        dim("Fan curve during the run: Undervolt keeps the fan curve that is in use.");
    } else {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Fan curve during the run");
        ImGui::SameLine(0, em());
        if (ImGui::RadioButton("Profile's own", !s.optimize_fan)) s.optimize_fan.reset();
        ImGui::SameLine(0, em());
        if (const auto p = fan_preset_buttons("run", s.optimize_fan)) s.optimize_fan = p;
    }
    ImGui::Spacing();
    const ImVec2 big(em() * 13, em() * 2.6f);
    ImGui::PushFont(g_bold, base() * 1.15f);
    // Without administrator rights the same button asks for them first; the
    // elevated copy opens here with this profile selected.
    const bool go = primary_button(with_icon(kIconPlay, undervolt ? "Undervolt GPU" : "Optimize GPU"), big);
    ImGui::PopFont();
    if (go) s.elevated ? act.optimize(s.preset) : act.restart_elevated(s.preset);
    ImGui::SameLine(0, em() * 1.5f);
    ImGui::BeginGroup();
    ImGui::TextUnformatted((ic(kIconClock) + (all ? "  About 40 minutes of full GPU load."
                                              : undervolt ? "  About 13 minutes of full GPU load."
                                                          : "  About 15 minutes of full GPU load."))
                               .c_str());
    if (!s.elevated) wrapped(kText, "Windows asks for administrator rights first; the app then opens here again.");
    wrapped(kDim, "The screen can go black for a few seconds while the run looks for the card's limit. That is expected. Save "
                  "your work first: rarely, Windows needs a sign-out or a restart afterwards.");
    wrapped(kDim, "Abort stops at any time: the card goes back to stock, or to your saved tune when that was on it.");
    wrapped(kDim, "A setting that crashes the machine is never tried again.");
    wrapped(kDim, "Close games and other programs that use the graphics card first: they change what the run measures.");
    ImGui::EndGroup();
    // One click is all of it: the result is kept at every logon without
    // another one. Shown while that is still off, so that it can be declined.
    if (s.elevated && !s.boot_on) {
        ImGui::Spacing();
        ImGui::Checkbox("Keep the result applied at every logon", &s.keep_at_logon);
        ImGui::SameLine(0, em());
        wrapped(kDim, "Copies the app to Program Files and starts it in the tray at logon.");
    }
    end_card();
}

}

// The voltage/frequency curve a run is writing (UiState::run_curve) over the
// card's own (run_stock): higher means faster at that voltage. An overclock
// lifts the whole line; an undervolt lifts the part left of where the card
// ran and cuts the top off flat. The dot (live: during the run) is where the
// card runs now: the lowest voltage at which the curve reaches the clock it
// reports.
void curve_card(const UiState& s, const char* caption, bool live) {
    if (s.run_curve.size() < 2) return;   // no run yet, or this card or driver does not offer the curve
    begin_card("curve");
    heading("Voltage/frequency curve");
    wrapped(kDim, caption);
    const float w = ImGui::GetContentRegionAvail().x, h = em() * 13;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(w, h));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), ImGui::GetColorU32(kInner), em() * 0.35f);

    float v_lo = s.run_curve.front().volt_uv / 1000.0f, v_hi = v_lo;
    float f_lo = s.run_curve.front().freq_khz / 1000.0f, f_hi = f_lo;
    for (const std::vector<VfPoint>* curve : {&s.run_stock, &s.run_curve})
        for (const VfPoint& pt : *curve) {
            v_lo = std::min(v_lo, pt.volt_uv / 1000.0f);
            v_hi = std::max(v_hi, pt.volt_uv / 1000.0f);
            f_lo = std::min(f_lo, pt.freq_khz / 1000.0f);
            f_hi = std::max(f_hi, pt.freq_khz / 1000.0f);
        }
    if (v_hi <= v_lo || f_hi <= f_lo) {
        end_card();
        return;
    }
    const float pad = (f_hi - f_lo) * 0.06f;
    f_lo -= pad;
    f_hi += pad;
    // Room for the axis labels on the left and below.
    const ImVec2 a(p0.x + em() * 4.6f, p0.y + em() * 0.7f), b(p0.x + w - em() * 1.0f, p0.y + h - em() * 1.9f);
    auto at = [&](float mv, float mhz) {
        return ImVec2(a.x + (mv - v_lo) / (v_hi - v_lo) * (b.x - a.x), b.y - (mhz - f_lo) / (f_hi - f_lo) * (b.y - a.y));
    };
    const ImU32 grid = ImGui::GetColorU32(kBorder), label = ImGui::GetColorU32(kDim);
    char text[48];
    for (int mhz = (static_cast<int>(f_lo) / 500 + 1) * 500; mhz < f_hi; mhz += 500) {
        const float y = at(v_lo, static_cast<float>(mhz)).y;
        dl->AddLine(ImVec2(a.x, y), ImVec2(b.x, y), grid);
        std::snprintf(text, sizeof(text), "%d MHz", mhz);
        dl->AddText(ImVec2(a.x - ImGui::CalcTextSize(text).x - em() * 0.5f, y - em() * 0.5f), label, text);
    }
    for (int mv = (static_cast<int>(v_lo) / 100 + 1) * 100; mv < v_hi; mv += 100) {
        const float x = at(static_cast<float>(mv), f_lo).x;
        dl->AddLine(ImVec2(x, a.y), ImVec2(x, b.y), grid);
        std::snprintf(text, sizeof(text), "%d mV", mv);
        dl->AddText(ImVec2(x - ImGui::CalcTextSize(text).x / 2, b.y + em() * 0.35f), label, text);
    }
    auto line = [&](const std::vector<VfPoint>& curve, const ImVec4& colour, float thickness) {
        std::vector<ImVec2> points;
        for (const VfPoint& pt : curve) points.push_back(at(pt.volt_uv / 1000.0f, pt.freq_khz / 1000.0f));
        if (points.size() > 1)
            dl->AddPolyline(points.data(), static_cast<int>(points.size()), ImGui::GetColorU32(colour), ImDrawFlags_None, thickness);
    };
    line(s.run_stock, kDim, em() * 0.09f);
    line(s.run_curve, kAccentBright, em() * 0.16f);

    // Where the card is now. Nothing while it idles below the curve.
    std::string now;
    const int now_khz = s.telemetry.core_mhz * 1000;
    if (live && now_khz >= s.run_curve.front().freq_khz - kVfToleranceKhz) {
        for (const VfPoint& pt : s.run_curve) {
            if (pt.freq_khz < now_khz - kVfToleranceKhz) continue;
            const ImVec2 dot = at(pt.volt_uv / 1000.0f, pt.freq_khz / 1000.0f);
            dl->AddCircleFilled(dot, em() * 0.38f, ImGui::GetColorU32(kGood));
            dl->AddCircle(dot, em() * 0.38f, ImGui::GetColorU32(kBg), 0, em() * 0.1f);
            now = "Now: " + std::to_string(s.telemetry.core_mhz) + " MHz, which this curve reaches at " +
                  std::to_string(pt.volt_uv / 1000) + " mV";
            break;
        }
    }
    // The legend, top left, where neither curve runs.
    float y = a.y + em() * 0.2f;
    auto legend = [&](const ImVec4& colour, const std::string& name, bool is_dot) {
        const float x = a.x + em() * 0.8f;
        if (is_dot) dl->AddCircleFilled(ImVec2(x + em() * 0.7f, y + em() * 0.55f), em() * 0.3f, ImGui::GetColorU32(colour));
        else dl->AddLine(ImVec2(x, y + em() * 0.55f), ImVec2(x + em() * 1.4f, y + em() * 0.55f), ImGui::GetColorU32(colour), em() * 0.16f);
        dl->AddText(ImVec2(x + em() * 2.0f, y), ImGui::GetColorU32(kText), name.c_str());
        y += em() * 1.35f;
    };
    legend(kAccentBright, "The curve on the card", false);
    if (!s.run_stock.empty()) legend(kDim, "The card's own curve (stock)", false);
    if (!now.empty()) legend(kGood, now, true);
    end_card();
}

namespace {

void run_screen(UiState& s, const OptimizeWorker::Snapshot& run, const UiActions& act) {
    begin_card("run");
    text_bold(run.is_retest ? "Testing the saved tune"
              : s.preset == Preset::Undervolt ? "Finding the undervolt"
                                              : (std::string("Optimizing: ") + title_of(s.preset)).c_str(),
              1.4f);
    if (!run.running) s.aborting = false;
    if (run.running) {
        // How far the whole run is, against the time the choose screen gave it.
        const int about = run.is_retest ? 5 : s.preset == Preset::AllInOne ? 40 : s.preset == Preset::Undervolt ? 13 : 15;
        const int spent = std::max(1, static_cast<int>(std::ceil(run.run_spent / 60)));
        char so_far[64];
        if (spent <= about) std::snprintf(so_far, sizeof(so_far), "%d of about %d min", spent, about);
        else std::snprintf(so_far, sizeof(so_far), "%d min (about %d expected)", spent, about);
        const std::string abort = with_icon(kIconCancel, "Abort run");
        const char* stopping = "Stopping and putting the card back...";
        const float right = s.aborting ? ImGui::CalcTextSize(stopping).x : button_width(abort) + em();
        align_right(ImGui::CalcTextSize(so_far).x + em() * 1.5f + right);
        ImGui::AlignTextToFramePadding();
        dim(so_far);
        ImGui::SameLine(0, em() * 1.5f);
        if (s.aborting) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(kWarn, "%s", stopping);
        } else {
            ImGui::PushStyleColor(ImGuiCol_Border, kBad);
            if (ImGui::Button(abort.c_str(), ImVec2(right, 0))) {
                s.aborting = true;
                act.abort();
            }
            ImGui::PopStyleColor();
        }
    }
    wrapped(kDim, run.is_retest ? "Five minutes of full load with the tune applied. Abort stops the test and puts the card back to "
                                  "stock, or to your saved tune if it was on before."
                                : "The screen can go black for a moment while the run looks for the card's limit; do not switch "
                                  "the PC off, the run recovers by itself. Abort stops the run and puts the card back to stock, "
                                  "or to your saved tune if it was on before.");
    ImGui::Spacing();
    // The run looked at what else uses the card before it began, found
    // something, and waits for what to do about it.
    if (run.running && !run.busy_question.empty()) {
        text_bold((ic(kIconInfo) + "  Other programs are using the graphics card").c_str(), 1.1f);
        wrapped(kText, busy_text(run.busy_question) + ".");
        wrapped(kDim, "A run measures the card under full load. What these programs take from it makes every number lower and less "
                      "steady: the result, and its comparison with stock. Close them and check again, or go on as it is. "
                      "Nothing is closed for you.");
        ImGui::Spacing();
        if (primary_button("Check again")) act.answer_busy(BusyAnswer::CheckAgain);
        ImGui::SameLine();
        if (ImGui::Button("Go on anyway")) act.answer_busy(BusyAnswer::GoOn);
        ImGui::SameLine();
        if (ImGui::Button("Don't start")) act.answer_busy(BusyAnswer::Stop);
        ImGui::Spacing();
    }
    // The step the run is at. One that lasts counts down; one that takes
    // seconds, or as long as it takes, only says what it is. The row is as
    // high either way, so the page does not jump from step to step.
    if (run.running && !run.measuring.empty()) {
        text_bold((ic(kIconClock) + "  " + run.measuring).c_str(), 1.1f);
        const ImVec2 bar(-1, em() * 0.5f);
        if (run.measuring_seconds > 0) {
            const double left = std::max(0.0, run.measuring_seconds - run.measuring_spent);
            char time[48];
            if (run.measuring_seconds < 90) std::snprintf(time, sizeof(time), "%d s left", static_cast<int>(std::ceil(left)));
            else std::snprintf(time, sizeof(time), "%d:%02d left of %s", static_cast<int>(left) / 60, static_cast<int>(left) % 60,
                               duration_text(run.measuring_seconds).c_str());
            // At the right edge, over the end of the bar it belongs to: the
            // step's name changes length from step to step, the time does not.
            align_right(ImGui::CalcTextSize(time).x);
            ImGui::TextUnformatted(time);
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, kAccentBright);
            ImGui::PushStyleColor(ImGuiCol_FrameBg, kInner);
            ImGui::ProgressBar(static_cast<float>(std::min(1.0, run.measuring_spent / run.measuring_seconds)), bar, "");
            ImGui::PopStyleColor(2);
        } else {
            ImGui::Dummy(ImVec2(1, bar.y));
        }
        ImGui::Spacing();
    }
    telemetry_tiles(s);
    ImGui::Spacing();
    const float w = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) / 2;
    char overlay[64];
    std::snprintf(overlay, sizeof(overlay), "temperature %s", reading(s.telemetry.temp_c, " \xC2\xB0""C").c_str());
    plot("##temp", s.temp_history, 30, 90, overlay, w);
    ImGui::SameLine();
    std::snprintf(overlay, sizeof(overlay), "power %s W", reading(s.telemetry.power_w, "").c_str());
    plot("##power", s.power_history, 0, static_cast<float>(std::max(s.telemetry.power_limit_w, 1)), overlay, w);
    end_card();

    curve_card(s, "What the run writes to the card, read back every second. Higher is faster at that voltage.", true);

    // What is left of the page, and never less than a few lines: a card that
    // sizes itself has no height to hand on to the list inside it.
    begin_card("probes", std::max(em() * 14, ImGui::GetContentRegionAvail().y));
    heading("Details");
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kInner);
    ImGui::BeginChild("lines", ImVec2(0, ImGui::GetContentRegionAvail().y), ImGuiChildFlags_Borders);
    ImGui::PopStyleColor();
    for (const std::string& line : run.log) ImGui::TextUnformatted(for_window(line).c_str());
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
    end_card();
    if (!run.running && run.ended()) s.screen = Screen::Results;
}

}

void optimize_page(UiState& s, const OptimizeWorker::Snapshot& run, const UiActions& act) {
    if (run.running) s.screen = Screen::Run;
    switch (s.screen) {
        case Screen::Choose: choose(s, act); break;
        case Screen::Run: run_screen(s, run, act); break;
        case Screen::Results: results(s, run, act); break;
    }
}

}
