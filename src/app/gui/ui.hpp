#pragma once
#include "app/gui/worker.hpp"
#include "core/fan_curve.hpp"
#include "core/config.hpp"
#include "core/types.hpp"
#include "core/vf_curve.hpp"
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace gao::gui {

enum class Page { Dashboard, Optimize, Fan, About };
enum class Screen { Choose, Run, Results };   // the Optimize page's state

// Something the app did or was told this session, shown in the log with the
// boot.log lines.
struct Note {
    std::string time;   // "YYYY-MM-DD HH:MM", as in boot.log
    std::string text;
    bool warn = false;
};

// Everything the window shows, refreshed by the main loop about once a second.
struct UiState {
    Page page = Page::Dashboard;
    Screen screen = Screen::Choose;
    Preset preset = Preset::AllInOne;   // what the one button does when nothing else is picked
    bool elevated = false;

    std::string gpu_name;
    std::string driver;
    Telemetry telemetry;
    std::deque<float> temp_history;    // last minute, one sample per second
    std::deque<float> power_history;

    std::optional<Profile> profile;
    bool profile_driver_ok = false;
    bool profile_gpu_ok = false;
    std::optional<AppliedState> applied;
    // The voltage/frequency curve seen from the saved undervolt. Empty: no
    // undervolt is saved, or the curve could not be read.
    std::optional<CurveState> curve;
    bool boot_on = false;
    int strikes = 0;
    std::vector<std::string> boot_log;   // the last lines of boot.log, oldest first
    std::vector<Note> notes;             // this session, oldest first

    bool fan_available = false;           // elevated and the card has fan control
    bool fan_control = false;
    std::optional<FanCurve> fan_curve;    // the active curve
    std::optional<FanCurve> fan_tested;   // what the tune was tested with
    int fan_min_pct = 0;
    int fan_max_temp_c = 75;      // the limit the fans run at 100 % from: the profile's, or this card's when it is higher
    int fan_limit_raised_c = 0;   // by how much it is above the profile's own; the presets move up with it
    FanState fan_state;
    // While a run from this window is under way the curve driver above is
    // released and the run has the fans: what the card reports then (a
    // speed set by hand or not; empty when it could not be read), and
    // whether the run is measuring the fans right now.
    bool run_active = false;
    bool run_tunes_fans = false;
    std::optional<bool> fan_manual;
    std::optional<FanPreset> optimize_fan;   // fan curve for the next run; empty: the profile's own
    // The voltage/frequency curve during a run from this window, read back
    // once a second: as the run last left it, and as it last read at stock.
    // Both stay after the run, for its result screen. Empty: no run yet, or
    // the card does not offer the curve.
    std::vector<VfPoint> run_curve;
    std::vector<VfPoint> run_stock;

    std::string update_version;   // a newer release that can be installed ("0.3.2"); empty: none known
    bool update_busy = false;     // it is being downloaded and installed
    std::string update_error;     // why the last attempt failed
    bool update_check = true;     // look for a new release when the app starts
    // One click is to be all of it: a run that ends saved also switches
    // apply-at-logon on, unless this is unticked on the Optimize page.
    bool keep_at_logon = true;
    bool aborting = false;   // Abort was clicked; the run is putting the card back
    unsigned seen_run = 0;   // the last run the Optimize page followed (OptimizeWorker::Snapshot::run_id)
};

// The Optimize page's screen for this frame. A run that started since the
// page last looked is followed, also one that ended before any frame saw it
// running: a refusal (another tuning tool open, more than one NVIDIA GPU)
// ends at once, and then shows as "Not started" with its reason instead of
// leaving the page where it was. The run screen moves on to the results
// when the run has ended.
inline Screen follow_run(UiState& s, const OptimizeWorker::Snapshot& run) {
    if (run.run_id != s.seen_run) {
        s.seen_run = run.run_id;
        s.screen = Screen::Run;
    }
    if (run.running) s.screen = Screen::Run;
    else if (s.screen == Screen::Run && run.ended()) s.screen = Screen::Results;
    return s.screen;
}

struct UiActions {
    std::function<void(Preset)> optimize;
    std::function<void()> abort;
    std::function<void(BusyAnswer)> answer_busy;          // to the run's question about other programs on the card
    // With a profile: the elevated copy opens on the Optimize page with it
    // selected, so the choice made here is not lost.
    std::function<void(std::optional<Preset>)> restart_elevated;
    std::function<void()> apply_profile;
    std::function<void()> retest;                         // the saved tune, five minutes, on the driver that is installed now
    std::function<void()> revert_to_stock;
    std::function<void(bool)> set_boot;
    std::function<void()> detect_gpu;
    std::function<void(const FanCurve&)> set_fan_curve;   // saves it as the active curve
    std::function<void(bool)> set_fan_control;
    std::function<void()> reset_fan_curve;                // back to the tested/default curve
    std::function<void()> install_update;                 // download the new release, replace the app, restart
    std::function<void(bool)> set_update_check;
};

// Fonts (Segoe UI with Segoe Fluent Icons merged in) and the colour scheme.
// Call load_fonts() once before the first frame, apply_style() on every DPI change.
void load_fonts(const std::string& fonts_dir);
void apply_style(float dpi_scale);

void push_history(std::deque<float>& h, float v);
void draw_ui(UiState& s, const OptimizeWorker::Snapshot& run, const UiActions& act);

}
