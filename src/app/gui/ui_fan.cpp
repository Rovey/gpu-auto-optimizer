#include "app/gui/ui_widgets.hpp"
#include <algorithm>
#include <cmath>
#include <vector>

namespace gao::gui {

namespace {

// The preset whose curve equals c, if any. raised_c: by how much this card's
// limit is above the profile's; the presets are offered moved up by it.
std::optional<FanPreset> preset_of(const FanCurve& c, int raised_c = 0) {
    for (const FanPreset p : kFanPresets)
        if (shifted_curve(fan_preset_curve(p), raised_c) == c) return p;
    return std::nullopt;
}

// Temperature 20-100 C on x, fan 0-100 % on y. Points drag with the mouse and
// stay ordered: a point cannot pass its neighbours in temperature, and fan %
// never goes down from one point to the next. Returns true when a drag ended,
// so the caller saves once per edit rather than every frame.
bool curve_editor(FanCurve& c, int min_pct, int max_temp_c, int now_temp_c, int* selected) {
    const float w = ImGui::GetContentRegionAvail().x, h = em() * 16;
    const ImVec2 o = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float pad = em() * 2.2f;
    const ImVec2 a(o.x + pad, o.y + em() * 0.5f), b(o.x + w - em() * 0.5f, o.y + h - pad);
    auto to_screen = [&](float t, float p) {
        return ImVec2(a.x + (t - 20) / 80 * (b.x - a.x), b.y - p / 100 * (b.y - a.y));
    };
    dl->AddRectFilled(a, b, ImGui::GetColorU32(kInner), em() * 0.3f);
    for (int t = 20; t <= 100; t += 10) {
        const ImVec2 p = to_screen(static_cast<float>(t), 0);
        dl->AddLine(ImVec2(p.x, a.y), ImVec2(p.x, b.y), ImGui::GetColorU32(kBorder));
        dl->AddText(ImVec2(p.x - em() * 0.6f, b.y + em() * 0.3f), ImGui::GetColorU32(kDim), (std::to_string(t) + "\xC2\xB0").c_str());
    }
    for (int p = 0; p <= 100; p += 25) {
        const ImVec2 q = to_screen(20, static_cast<float>(p));
        dl->AddLine(ImVec2(a.x, q.y), ImVec2(b.x, q.y), ImGui::GetColorU32(kBorder));
        dl->AddText(ImVec2(o.x, q.y - em() * 0.5f), ImGui::GetColorU32(kDim), (std::to_string(p) + "%").c_str());
    }
    // Fan-stop zone, the card's minimum, the profile's limit and the current temperature.
    if (c.stop_below_c) {
        const ImVec2 z = to_screen(static_cast<float>(*c.stop_below_c), 0);
        dl->AddRectFilled(a, ImVec2(z.x, b.y), ImGui::GetColorU32(ImVec4(kGood.x, kGood.y, kGood.z, 0.10f)));
    }
    const ImVec2 m = to_screen(20, static_cast<float>(min_pct));
    dl->AddLine(ImVec2(a.x, m.y), ImVec2(b.x, m.y), ImGui::GetColorU32(kWarn), 1.0f);
    const ImVec2 lim = to_screen(static_cast<float>(max_temp_c), 100);
    dl->AddLine(ImVec2(lim.x, a.y), ImVec2(lim.x, b.y), ImGui::GetColorU32(kBad), 1.5f);
    if (now_temp_c >= 20) {
        const ImVec2 n = to_screen(static_cast<float>(std::min(now_temp_c, 100)), 0);
        dl->AddLine(ImVec2(n.x, a.y), ImVec2(n.x, b.y), ImGui::GetColorU32(kText), 1.0f);
    }
    // The curve as it was set: straight from point to point, flat outside them.
    auto as_set = [&](float t) {
        const auto& pts = c.points;
        if (t <= static_cast<float>(pts.front().temp_c)) return static_cast<float>(pts.front().pct);
        for (size_t k = 1; k < pts.size(); ++k) {
            if (t > static_cast<float>(pts[k].temp_c)) continue;
            const float f = (t - static_cast<float>(pts[k - 1].temp_c)) / static_cast<float>(pts[k].temp_c - pts[k - 1].temp_c);
            return static_cast<float>(pts[k - 1].pct) + f * static_cast<float>(pts[k].pct - pts[k - 1].pct);
        }
        return static_cast<float>(pts.back().pct);
    };
    // What the fans actually do, guard rails included: exact, not rounded to
    // whole percents, so straight segments stay straight and the jumps at the
    // stop threshold and the limit are vertical.
    auto effective = [&](float t) {
        if (t >= static_cast<float>(max_temp_c)) return 100.0f;
        if (c.stop_below_c && t < static_cast<float>(*c.stop_below_c)) return 0.0f;
        return std::max(as_set(t), static_cast<float>(min_pct));
    };
    // Thin and dimmed through the points, under the line the fans follow: where
    // the two part, a guard rail is why, and the points there are not ignored
    // by mistake.
    std::vector<ImVec2> set_line{to_screen(20, as_set(20))};
    for (const FanPoint& pt : c.points) set_line.push_back(to_screen(static_cast<float>(pt.temp_c), static_cast<float>(pt.pct)));
    set_line.push_back(to_screen(100, as_set(100)));
    dl->AddPolyline(set_line.data(), static_cast<int>(set_line.size()),
                    ImGui::GetColorU32(ImVec4(kAccentBright.x, kAccentBright.y, kAccentBright.z, 0.40f)), ImDrawFlags_None, 1.2f);
    std::vector<ImVec2> line;
    for (int i = 0; i <= 80 * 8; ++i) {
        const float t = 20 + static_cast<float>(i) / 8;
        line.push_back(to_screen(t, effective(t)));
    }
    dl->AddPolyline(line.data(), static_cast<int>(line.size()), ImGui::GetColorU32(kAccentBright), ImDrawFlags_None, 2.5f);
    bool released = false;
    for (int i = 0; i < static_cast<int>(c.points.size()); ++i) {
        FanPoint& pt = c.points[static_cast<size_t>(i)];
        const ImVec2 p = to_screen(static_cast<float>(pt.temp_c), static_cast<float>(pt.pct));
        const float r = em() * 0.45f;
        ImGui::SetCursorScreenPos(ImVec2(p.x - r * 1.5f, p.y - r * 1.5f));
        ImGui::PushID(i);
        ImGui::InvisibleButton("pt", ImVec2(r * 3, r * 3));
        if (ImGui::IsItemActivated()) *selected = i;
        if (ImGui::IsItemActive()) {
            const ImVec2 mouse = ImGui::GetIO().MousePos;
            const int lo_t = i > 0 ? c.points[static_cast<size_t>(i) - 1].temp_c + 1 : 20;
            const int hi_t = i + 1 < static_cast<int>(c.points.size()) ? c.points[static_cast<size_t>(i) + 1].temp_c - 1 : 100;
            const int lo_p = i > 0 ? c.points[static_cast<size_t>(i) - 1].pct : 0;
            const int hi_p = i + 1 < static_cast<int>(c.points.size()) ? c.points[static_cast<size_t>(i) + 1].pct : 100;
            pt.temp_c = std::clamp(static_cast<int>(std::lround(20 + (mouse.x - a.x) / (b.x - a.x) * 80)), lo_t, hi_t);
            pt.pct = std::clamp(static_cast<int>(std::lround((b.y - mouse.y) / (b.y - a.y) * 100)), lo_p, hi_p);
        }
        if (ImGui::IsItemDeactivated()) released = true;
        const bool hot = ImGui::IsItemHovered() || ImGui::IsItemActive() || *selected == i;
        // A point a guard rail overrides is drawn hollow, off the line, with the reason.
        const char* why = pt.temp_c >= max_temp_c ? "the profile's limit: the fans run at 100 % here"
                          : c.stop_below_c && pt.temp_c < *c.stop_below_c ? "the fan-stop zone: the driver has the fans here"
                          : pt.pct < min_pct ? "raised to the card's minimum"
                                             : nullptr;
        const ImU32 col = ImGui::GetColorU32(hot ? kText : kAccentBright);
        if (why) {
            dl->AddCircleFilled(p, r, ImGui::GetColorU32(kInner));
            dl->AddCircle(p, r, col, 0, 2.0f);
        } else {
            dl->AddCircleFilled(p, r, col);
        }
        if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
            if (why) ImGui::SetTooltip("%d \xC2\xB0""C  %d %%\nOverridden by %s", pt.temp_c, pt.pct, why);
            else ImGui::SetTooltip("%d \xC2\xB0""C  %d %%", pt.temp_c, pt.pct);
        }
        ImGui::PopID();
    }
    ImGui::SetCursorScreenPos(ImVec2(o.x, o.y + h));
    ImGui::Dummy(ImVec2(w, 0));
    return released;
}

}

void fan_page(UiState& s, const UiActions& act) {
    static FanCurve edit;          // the curve being edited; saved when a drag ends
    static bool editing = false;
    static int selected = -1;
    begin_card("fan");
    heading("Fan curve");
    if (!s.fan_available) {
        wrapped(kDim, s.elevated ? "This card's fans cannot be controlled through the NVIDIA driver."
                                 : "Fan control needs administrator rights.");
        if (!s.elevated && primary_button("Restart as administrator")) act.restart_elevated(std::nullopt);
        end_card();
        return;
    }
    if (!s.fan_curve) {
        wrapped(kDim, "Optimize first: the fan curve belongs to a saved tune.");
        if (primary_button(with_icon(kIconPulse, "Optimize..."))) s.page = Page::Optimize;
        end_card();
        return;
    }
    if (!editing) edit = *s.fan_curve;

    const std::string label = "Fan control";
    ImGui::TextUnformatted(label.c_str());
    ImGui::SameLine();
    if (toggle("##fanon", s.fan_control)) act.set_fan_control(!s.fan_control);
    ImGui::SameLine(0, em() * 2);
    switch (s.fan_state.mode) {
        case FanMode::Curve: dim(("Curve: " + std::to_string(s.fan_state.pct) + " %").c_str()); break;
        case FanMode::Foreign: ImGui::TextColored(kWarn, "Another program controls the fans."); break;
        case FanMode::Failed: ImGui::TextColored(kBad, "A fan speed did not verify; the driver controls the fans."); break;
        case FanMode::Driver: dim(s.fan_control ? "The NVIDIA driver controls the fans right now." : "Off: the NVIDIA driver controls the fans."); break;
    }
    ImGui::Spacing();

    const bool released = curve_editor(edit, s.fan_min_pct, s.fan_max_temp_c, s.telemetry.temp_c, &selected);
    editing = ImGui::IsAnyItemActive();
    if (released && valid(edit)) act.set_fan_curve(edit);

    bool stop = edit.stop_below_c.has_value();
    if (ImGui::Checkbox("Stop the fans below", &stop)) {
        edit.stop_below_c = stop ? std::optional<int>(50) : std::nullopt;
        act.set_fan_curve(edit);
    }
    if (stop) {
        ImGui::SameLine();
        int v = *edit.stop_below_c;
        ImGui::SetNextItemWidth(em() * 12);
        ImGui::SliderInt("##stop", &v, 30, 70, "%d \xC2\xB0""C");
        edit.stop_below_c = v;
        if (ImGui::IsItemDeactivatedAfterEdit()) act.set_fan_curve(edit);
    }
    ImGui::SameLine(0, em() * 2);
    ImGui::BeginDisabled(edit.points.size() >= 6);
    if (ImGui::Button("Add point")) {
        // Halfway along the widest temperature gap.
        size_t at = 1;
        for (size_t i = 1; i < edit.points.size(); ++i)
            if (edit.points[i].temp_c - edit.points[i - 1].temp_c > edit.points[at].temp_c - edit.points[at - 1].temp_c) at = i;
        const FanPoint& l = edit.points[at - 1];
        const FanPoint& r = edit.points[at];
        edit.points.insert(edit.points.begin() + static_cast<std::ptrdiff_t>(at), FanPoint{(l.temp_c + r.temp_c) / 2, (l.pct + r.pct) / 2});
        if (valid(edit)) act.set_fan_curve(edit);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(edit.points.size() <= 2 || selected < 0 || selected >= static_cast<int>(edit.points.size()));
    if (ImGui::Button("Remove point")) {
        edit.points.erase(edit.points.begin() + selected);
        selected = -1;
        act.set_fan_curve(edit);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Back to the tested curve")) {
        selected = -1;
        act.reset_fan_curve();
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Presets");
    ImGui::SameLine(0, em());
    if (const auto p = fan_preset_buttons("fan", preset_of(edit, s.fan_limit_raised_c))) {
        selected = -1;
        edit = shifted_curve(fan_preset_curve(*p), s.fan_limit_raised_c);
        act.set_fan_curve(edit);
    }

    ImGui::Spacing();
    if (s.fan_tested && quieter_than(edit, *s.fan_tested, s.fan_max_temp_c))
        wrapped(kWarn, "This curve is quieter than the one the tune was tested with, so the card runs warmer than during the "
                       "test. Optimize again to be sure the tune holds.");
    wrapped(kDim, "Drag the points. Yellow line: the slowest your fans can spin. Red line: from here the fans always run at "
                  "100 %. Shaded area: the fans may stop while the card is cool and idle. White line: the card now.");
    if (s.fan_limit_raised_c > 0)
        wrapped(kDim, "On this card the limit is " + std::to_string(s.fan_max_temp_c) + " \xC2\xB0""C, not the profile's " +
                      std::to_string(s.fan_max_temp_c - s.fan_limit_raised_c) + " \xC2\xB0""C: it reached that by itself at stock. "
                      "The presets are moved up by the difference.");
    end_card();
}

}
