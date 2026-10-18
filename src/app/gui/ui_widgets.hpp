#pragma once
#include "app/gui/ui.hpp"
#include "imgui.h"
#include <optional>
#include <string>

// Shared by the pages of the window (ui*.cpp): colours, icons, fonts,
// widgets and text helpers. Not part of ui.hpp's interface.
namespace gao::gui {

extern ImFont* g_bold;

inline const ImVec4 kBg{0.067f, 0.075f, 0.090f, 1};
inline const ImVec4 kNav{0.050f, 0.057f, 0.069f, 1};
inline const ImVec4 kCard{0.090f, 0.102f, 0.122f, 1};
inline const ImVec4 kInner{0.110f, 0.125f, 0.149f, 1};
inline const ImVec4 kInnerHover{0.140f, 0.158f, 0.188f, 1};
inline const ImVec4 kBorder{0.170f, 0.192f, 0.227f, 1};
// Dark enough for kText on it to read at 4.5:1 (primary buttons).
inline const ImVec4 kAccent{0.150f, 0.380f, 0.750f, 1};
inline const ImVec4 kAccentHover{0.190f, 0.440f, 0.830f, 1};
inline const ImVec4 kAccentBright{0.290f, 0.580f, 1.000f, 1};
inline const ImVec4 kSelectedBg{0.098f, 0.145f, 0.220f, 1};
inline const ImVec4 kText{0.900f, 0.910f, 0.930f, 1};
inline const ImVec4 kDim{0.600f, 0.640f, 0.690f, 1};
inline const ImVec4 kGood{0.360f, 0.780f, 0.500f, 1};
inline const ImVec4 kWarn{0.910f, 0.710f, 0.230f, 1};
inline const ImVec4 kBad{0.930f, 0.400f, 0.400f, 1};

// Segoe Fluent Icons (Windows 11) / Segoe MDL2 Assets (Windows 10) code points.
enum Icon : unsigned {
    kIconHome = 0xE80F, kIconPulse = 0xE9D9, kIconInfo = 0xE946, kIconChip = 0xEEA1, kIconMemory = 0xE964,
    kIconTemp = 0xE9CA, kIconPower = 0xE945, kIconFan = 0xF16A, kIconRefresh = 0xE72C, kIconUndo = 0xE777,
    kIconPlay = 0xF5B0, kIconStop = 0xE71A, kIconClock = 0xE823, kIconStar = 0xE734, kIconQuiet = 0xE992,
    kIconLeaf = 0xEC0A, kIconGauge = 0xEC4A, kIconShield = 0xEA18, kIconList = 0xE8FD, kIconBack = 0xE72B,
    kIconDownload = 0xE896, kIconStarFilled = 0xE735, kIconCancel = 0xE711,
};

struct PresetInfo {
    Preset preset;
    Icon icon;
    const char* title;
    const char* summary;
};
inline constexpr PresetInfo kPresets[] = {
    {Preset::BestOfMyGpu, kIconStar, "Best of my GPU",
     "Highest confirmed clocks with a safety margin and the most power the card allows."},
    {Preset::CoolAndEfficient, kIconLeaf, "Cool & efficient",
     "Power only: the lowest power limit that costs under 2\xC2\xA0% speed, clocks left stock."},
    {Preset::Quiet, kIconQuiet, "Quiet", "Milder clocks and the lowest power limit that costs under 2\xC2\xA0% speed."},
    {Preset::MaxPerformance, kIconGauge, "Max performance",
     "Highest confirmed clocks minus one step and the highest power limit."},
};
// The one that does it all, shown first and across the full width.
inline constexpr PresetInfo kAllInOne = {Preset::AllInOne, kIconStarFilled, "All in one",
                                         "Overclock, then an undervolt on top of it, then the quietest fan curve that holds the "
                                         "temperature: more speed than stock on less power, tested as a whole. One click, about 40 "
                                         "minutes."};
// Not a clock search, and shown apart from the four above: the speed the
// card has at stock, on the lowest voltage that holds it.
inline constexpr PresetInfo kUndervolt = {Preset::Undervolt, kIconPower, "Undervolt",
                                          "The same speed on less voltage: less power, less heat and quieter fans. Clocks and the "
                                          "power limit stay stock. It replaces a saved overclock."};

inline constexpr FanPreset kFanPresets[] = {FanPreset::Silent, FanPreset::Normal, FanPreset::Cool, FanPreset::Aggressive};

std::string ic(unsigned cp);
std::string with_icon(unsigned cp, const char* text);
float em();
float base();
void draw_fan(ImDrawList* dl, const ImVec2& c, float radius, ImU32 colour);

const char* title_of(Preset preset);
std::string offset(int mhz);
std::string duration_text(double seconds);
std::string undervolt_text(const UndervoltTune& u);
std::string score_text(double score);
std::string against(double before, double after, const char* unit);
// "Done: 4.6 % faster, 6 % more power": the result in one line, from the
// two runs a result page compares. Just "Done" when either has no score.
std::string done_heading(const StabilityResult& before, const StabilityResult& after);
// The engine's words, said for the window: its lines also go to the command
// line and boot.log, where `gao --reset` and boot-apply are the right names.
std::string for_window(std::string text);
// A run the user stopped (Abort) or did not start (the question about other
// programs): no failure, and not shown as one.
bool stopped_by_user(const std::string& reason);

void text_bold(const char* text, float scale = 1.0f);
void heading(const char* text);
void dim(const char* text);
void wrapped(const ImVec4& color, const std::string& text);
void begin_card(const char* id, float height = 0, const ImVec4& bg = kCard, float width = 0);
void end_card();
void align_right(float width);
float button_width(const std::string& label);
bool primary_button(const std::string& label, ImVec2 size = ImVec2(0, 0));
bool toggle(const char* id, bool on);
void tile(const char* id, Icon icon, const char* label, const std::string& value, float w, float h);
void telemetry_tiles(const UiState& s);
std::optional<FanPreset> fan_preset_buttons(const char* id, const std::optional<FanPreset>& current);

// The pages, drawn by draw_ui().
void dashboard(UiState& s, const UiActions& act);
void optimize_page(UiState& s, const OptimizeWorker::Snapshot& run, const UiActions& act);
void fan_page(UiState& s, const UiActions& act);
void about(const UiState& s, const UiActions& act);

// Parts of the Optimize page that live in their own file.
void curve_card(const UiState& s, const char* caption, bool live);
void results(UiState& s, const OptimizeWorker::Snapshot& run, const UiActions& act);

}
