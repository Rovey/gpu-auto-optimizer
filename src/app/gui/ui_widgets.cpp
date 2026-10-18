#include "app/gui/ui_widgets.hpp"
#include "core/objectives.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace gao::gui {

namespace {

ImFont* g_regular = nullptr;

constexpr const char* kFanPresetTitles[] = {"Silent", "Normal", "Cool", "Aggressive"};

}

ImFont* g_bold = nullptr;

// UTF-8 for a code point in the Basic Multilingual Plane's private use area.
std::string ic(unsigned cp) {
    const char s[4] = {static_cast<char>(0xE0 | (cp >> 12)), static_cast<char>(0x80 | ((cp >> 6) & 0x3F)),
                       static_cast<char>(0x80 | (cp & 0x3F)), 0};
    return s;
}
std::string with_icon(unsigned cp, const char* text) { return ic(cp) + "   " + text; }

float em() { return ImGui::GetFontSize(); }
float base() { return ImGui::GetStyle().FontSizeBase; }

// The fan icon is drawn, not a glyph: the icon fonts of Windows have no fan
// (kIconFan's code point is a ring of dots). A casing ring, a hub and three
// swept blades inside a circle of `radius` around `c`.
void draw_fan(ImDrawList* dl, const ImVec2& c, float radius, ImU32 colour) {
    constexpr float kTurn = 6.2831853f;
    constexpr int kOutline = 20;   // points around one blade
    dl->AddCircle(c, radius * 0.92f, colour, 0, std::max(1.0f, radius * 0.12f));
    for (int k = 0; k < 3; ++k) {
        // A blade: an ellipse half-way out on its spoke, leaning 52 degrees into the turn.
        const float spoke = kTurn * static_cast<float>(k) / 3 - kTurn / 4, lean = spoke + 0.9076f;
        const ImVec2 mid(c.x + radius * 0.50f * std::cos(spoke), c.y + radius * 0.50f * std::sin(spoke));
        ImVec2 outline[kOutline];
        for (int i = 0; i < kOutline; ++i) {
            const float t = kTurn * static_cast<float>(i) / kOutline;
            const float x = radius * 0.34f * std::cos(t), y = radius * 0.21f * std::sin(t);
            outline[i] = ImVec2(mid.x + x * std::cos(lean) - y * std::sin(lean), mid.y + x * std::sin(lean) + y * std::cos(lean));
        }
        dl->AddConvexPolyFilled(outline, kOutline, colour);
    }
    dl->AddCircleFilled(c, radius * 0.20f, colour);
}

const char* title_of(Preset preset) {
    if (preset == kUndervolt.preset) return kUndervolt.title;
    if (preset == kAllInOne.preset) return kAllInOne.title;
    for (const PresetInfo& p : kPresets)
        if (p.preset == preset) return p.title;
    return preset_name(preset);
}

std::string offset(int mhz) { return (mhz >= 0 ? "+" : "") + std::to_string(mhz) + " MHz"; }
std::string duration_text(double seconds) {
    char buf[32];
    if (seconds < 90) std::snprintf(buf, sizeof(buf), "%.0f s", seconds);
    else std::snprintf(buf, sizeof(buf), "%.0f min", seconds / 60);
    return buf;
}
std::string undervolt_text(const UndervoltTune& u) {
    return std::to_string(u.freq_khz / 1000) + " MHz at " + std::to_string(u.volt_uv / 1000) + " mV";
}

std::string score_text(double score) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.0f it/s", score);
    return buf;
}
// "5582 it/s (-1.0 %)", or the same for watts.
std::string against(double before, double after, const char* unit) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.0f %s (%+.1f %%)", after, unit, before > 0 ? (after / before - 1) * 100 : 0.0);
    return buf;
}

// ------------------------------------------------------------------ widgets

void text_bold(const char* text, float scale) {
    ImGui::PushFont(g_bold, base() * scale);
    ImGui::TextUnformatted(text);
    ImGui::PopFont();
}
void heading(const char* text) { text_bold(text, 1.2f); }
void dim(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
}
void wrapped(const ImVec4& color, const std::string& text) {
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(text.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

// A rounded panel. height 0: as tall as its contents. Always pair with end_card().
// It never scrolls itself: the wheel goes to the page behind it. (A panel as
// tall as its contents can still be a pixel short of them, and then it
// kept the wheel: the page did not scroll with the mouse on such a card.)
void begin_card(const char* id, float height, const ImVec4& bg, float width) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, bg);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(em() * 0.9f, em() * 0.75f));
    ImGui::BeginChild(id, ImVec2(width, height),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding |
                          (height == 0 ? ImGuiChildFlags_AutoResizeY : ImGuiChildFlags_None),
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}
void end_card() { ImGui::EndChild(); }

// Puts the next item at the right edge of the current line, `width` wide.
void align_right(float width) {
    ImGui::SameLine(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - width);
}

float button_width(const std::string& label) {
    return ImGui::CalcTextSize(label.c_str()).x + ImGui::GetStyle().FramePadding.x * 2;
}

bool primary_button(const std::string& label, ImVec2 size) {
    ImGui::PushStyleColor(ImGuiCol_Button, kAccent);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kAccentHover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, kAccentBright);
    ImGui::PushStyleColor(ImGuiCol_Border, kAccent);
    const bool pressed = ImGui::Button(label.c_str(), size);
    ImGui::PopStyleColor(4);
    return pressed;
}

// An on/off switch; returns true when clicked.
bool toggle(const char* id, bool on) {
    const float h = em() * 1.25f, w = h * 1.9f;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(w, h));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec4 track = on ? kAccent : (ImGui::IsItemHovered() ? kInnerHover : kInner);
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), ImGui::GetColorU32(track), h / 2);
    dl->AddRect(p, ImVec2(p.x + w, p.y + h), ImGui::GetColorU32(on ? kAccent : kBorder), h / 2);
    const float r = h / 2 - h * 0.15f;
    dl->AddCircleFilled(ImVec2(on ? p.x + w - h / 2 : p.x + h / 2, p.y + h / 2), r, ImGui::GetColorU32(kText));
    return clicked;
}

// A telemetry tile: icon, label, value.
void tile(const char* id, Icon icon, const char* label, const std::string& value, float w, float h) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kInner);
    ImGui::BeginChild(id, ImVec2(w, h), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleColor();
    const float content_h = em() * 2.45f;
    ImGui::SetCursorPos(ImVec2(em() * 0.9f, (h - content_h) / 2 + em() * 0.3f));
    ImGui::PushFont(nullptr, base() * 1.8f);
    ImGui::PushStyleColor(ImGuiCol_Text, kAccentBright);
    if (icon == kIconFan) {   // drawn, in the box a glyph of this size would take
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const float size = ImGui::GetFontSize();
        ImGui::Dummy(ImVec2(size, size));
        draw_fan(ImGui::GetWindowDrawList(), ImVec2(at.x + size / 2, at.y + size / 2), size * 0.5f, ImGui::GetColorU32(kAccentBright));
    } else {
        ImGui::TextUnformatted(ic(icon).c_str());
    }
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::SameLine(0, em() * 0.8f);
    ImGui::SetCursorPosY((h - content_h) / 2);
    // Label and value as one block, as tall as content_h says: with the
    // style's own line spacing between them the value hung at the bottom.
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, em() * 0.15f));
    ImGui::BeginGroup();
    dim(label);
    // A value that is wider than the tile is drawn smaller, down to the
    // label's own size, so that it is never cut off.
    float scale = 1.3f;
    ImGui::PushFont(g_bold, base() * scale);
    const float need = ImGui::CalcTextSize(value.c_str()).x;
    ImGui::PopFont();
    const float room = ImGui::GetContentRegionAvail().x - em() * 0.3f;
    if (need > room && need > 0) scale = std::max(1.0f, scale * room / need);
    text_bold(value.c_str(), scale);
    ImGui::EndGroup();
    ImGui::PopStyleVar();
    ImGui::EndChild();
}

void telemetry_tiles(const UiState& s) {
    const Telemetry& t = s.telemetry;
    const float gap = ImGui::GetStyle().ItemSpacing.x;
    const float w = (ImGui::GetContentRegionAvail().x - gap * 4) / 5, h = em() * 4.2f;
    tile("core", kIconChip, "Core clock", reading(t.core_mhz, " MHz"), w, h);
    ImGui::SameLine();
    tile("mem", kIconMemory, "Memory clock", reading(t.mem_mhz, " MHz"), w, h);
    ImGui::SameLine();
    tile("temp", kIconTemp, "Temperature", reading(t.temp_c, " \xC2\xB0""C"), w, h);
    ImGui::SameLine();
    tile("power", kIconPower, "Power", reading(t.power_w, "") + " / " + reading(t.power_limit_w, " W"), w, h);
    ImGui::SameLine();
    // Who has the fans: the app's curve, the NVIDIA driver, another program,
    // or, during a run, the run itself (its curve, or its fan measurement).
    const char* mode = fan_owner_label(
        fan_owner(s.fan_state.mode, s.fan_control && s.fan_available, s.run_active, s.run_tunes_fans, s.fan_manual));
    tile("fan", kIconFan, "Fan speed", reading(t.fan_pct, " %") + mode, w, h);
}

// One button per fan preset; `current` is highlighted. Returns the clicked one.
std::optional<FanPreset> fan_preset_buttons(const char* id, const std::optional<FanPreset>& current) {
    std::optional<FanPreset> clicked;
    ImGui::PushID(id);
    for (size_t i = 0; i < std::size(kFanPresets); ++i) {
        if (i > 0) ImGui::SameLine();
        const bool on = current == kFanPresets[i];
        if (on) {
            ImGui::PushStyleColor(ImGuiCol_Button, kSelectedBg);
            ImGui::PushStyleColor(ImGuiCol_Border, kAccentBright);
        }
        if (ImGui::Button(kFanPresetTitles[i])) clicked = kFanPresets[i];
        if (on) ImGui::PopStyleColor(2);
    }
    ImGui::PopID();
    return clicked;
}

void load_fonts(const std::string& fonts_dir) {
    ImGuiIO& io = ImGui::GetIO();
    auto add_icons = [&] {
        ImFontConfig cfg;
        cfg.MergeMode = true;
        cfg.GlyphOffset.y = 3.0f;   // the icon fonts sit higher than Segoe UI's baseline
        if (!io.Fonts->AddFontFromFileTTF((fonts_dir + "\\SegoeIcons.ttf").c_str(), 17.0f, &cfg))
            io.Fonts->AddFontFromFileTTF((fonts_dir + "\\segmdl2.ttf").c_str(), 17.0f, &cfg);   // Windows 10
    };
    g_regular = fonts_dir.empty() ? nullptr : io.Fonts->AddFontFromFileTTF((fonts_dir + "\\segoeui.ttf").c_str(), 17.0f);
    if (!g_regular) {
        g_regular = g_bold = io.Fonts->AddFontDefault();
        return;
    }
    add_icons();
    g_bold = io.Fonts->AddFontFromFileTTF((fonts_dir + "\\seguisb.ttf").c_str(), 17.0f);
    if (g_bold) add_icons();
    else g_bold = g_regular;
}

void apply_style(float scale) {
    ImGuiStyle& st = ImGui::GetStyle();
    st = ImGuiStyle();
    ImGui::StyleColorsDark();
    ImVec4* c = st.Colors;
    c[ImGuiCol_WindowBg] = kBg;
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = kCard;
    c[ImGuiCol_Border] = kBorder;
    c[ImGuiCol_Text] = kText;
    c[ImGuiCol_TextDisabled] = kDim;
    c[ImGuiCol_FrameBg] = kInner;
    c[ImGuiCol_FrameBgHovered] = kInnerHover;
    c[ImGuiCol_FrameBgActive] = kInnerHover;
    c[ImGuiCol_Button] = kInner;
    c[ImGuiCol_ButtonHovered] = kInnerHover;
    c[ImGuiCol_ButtonActive] = kSelectedBg;
    c[ImGuiCol_CheckMark] = kAccentBright;
    c[ImGuiCol_Header] = kInner;
    c[ImGuiCol_HeaderHovered] = kInnerHover;
    c[ImGuiCol_HeaderActive] = kSelectedBg;
    c[ImGuiCol_TableHeaderBg] = kInner;
    c[ImGuiCol_TableBorderStrong] = kBorder;
    c[ImGuiCol_TableBorderLight] = kBorder;
    c[ImGuiCol_TableRowBg] = kInner;
    c[ImGuiCol_TableRowBgAlt] = ImVec4(0.100f, 0.114f, 0.137f, 1);
    c[ImGuiCol_PlotLines] = kAccentBright;
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    st.WindowRounding = 0;
    st.ChildRounding = 8;
    st.FrameRounding = 6;
    st.GrabRounding = 6;
    st.FrameBorderSize = 1;
    st.ChildBorderSize = 1;
    st.FramePadding = ImVec2(12, 7);
    st.ItemSpacing = ImVec2(12, 12);
    st.CellPadding = ImVec2(10, 4);
    st.ScaleAllSizes(scale);
    st.FontScaleDpi = scale;
}

}
