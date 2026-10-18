#pragma once
// The gao.exe commands as main.cpp dispatches them (cli_hardware.cpp,
// cli_runs.cpp); CLI only, not part of the interface of common.hpp.
#include "app/common.hpp"
#include "hw/stress.hpp"
#include <atomic>
#include <cstdio>
#include <optional>
#include <string>

namespace gao::cli {

// cli_hardware.cpp
int probe();
int set_offset(const char* label, int mhz, bool core);
int curve();
int curve_flatten(int mv, int mhz, int tail_drop_mhz, int core_mhz);
int reset();
int update();
int apply();
int boot(bool on);
int status();
int fan_auto();

// cli_runs.cpp
int stress(int seconds, int max_temp_c, gao::StressSelftest selftest, int duty_pct);
int optimize(gao::Preset preset, const std::optional<gao::FanCurve>& fan_curve);
int undervolt(bool on_overclock);
int retest();
int all_in_one();
int bandwidth(int after_load_s);
int stress_recreate();

// Defined in cli_runs.cpp: the Ctrl+C handler sets g_abort; main() sets
// g_busy_ok for --busy-ok.
extern std::atomic<bool> g_abort;
extern bool g_busy_ok;


// A run Ctrl+C stopped, with nothing of it kept (exit 1), puts back the tune
// it found on the card: a tray app's watchdog would otherwise take the stock
// it left for a driver reset. The window's runs do the same (gui/worker.cpp).
template <class Run> int putting_back(Run run) {
    const auto before = gao::app::tune_before_run();
    const int code = run();
    if (g_abort && code == 1) {
        gao::app::OptimizeHooks hooks;
        hooks.log = gao::app::logged([](const std::string& msg) { std::printf("  %s\n", msg.c_str()); });
        gao::app::put_back_after_stop(before, hooks);
    }
    return code;
}

}  // namespace gao::cli
