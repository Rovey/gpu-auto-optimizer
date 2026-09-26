#pragma once
#include "app/gui/worker.hpp"
#include "core/config.hpp"
#include "core/types.hpp"
#include <deque>
#include <functional>
#include <optional>
#include <string>

namespace gao::gui {

enum class Screen { Home, Run, Results };

// Everything the window shows, refreshed by the main loop about once a second.
struct UiState {
    Screen screen = Screen::Home;
    Preset preset = Preset::BestOfMyGpu;
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
    bool boot_on = false;
    int strikes = 0;
    std::string last_boot;
    std::string watchdog_note;   // what the tune watchdog last did, if anything
    std::string message;         // result of the last button press
};

struct UiActions {
    std::function<void(Preset)> optimize;
    std::function<void()> abort;
    std::function<void()> restart_elevated;
    std::function<void()> apply_profile;
    std::function<void()> revert_to_stock;
    std::function<void(bool)> set_boot;
};

void push_history(std::deque<float>& h, float v);
void draw_ui(UiState& s, const OptimizeWorker::Snapshot& run, const UiActions& act);

}
