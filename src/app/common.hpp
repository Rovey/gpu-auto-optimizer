#pragma once
// Application logic shared by gao.exe (CLI) and GpuAutoOptimizer.exe (window
// and tray): everything between the core/hw layers and a user interface.
// Nothing here prints; callers decide how to show messages.
#include "core/boot.hpp"
#include "core/config.hpp"
#include "core/objectives.hpp"
#include "core/search.hpp"
#include "core/types.hpp"
#include <functional>
#include <string>

namespace gao::app {

// GPU index is fixed at 0: this machine, like the CLI, handles one NVIDIA GPU.
inline constexpr unsigned kGpu = 0;

bool is_elevated();
std::string now_text();   // local time, "YYYY-MM-DD HH:MM"
Config load_config();
bool save_config(const Config& c);
void boot_log(const std::string& msg);
// Creates, or verifies and re-secures, the admin-only state folder. Every
// elevated command that reads or writes state calls this first.
bool prepare_state(std::string* why);
std::string profile_text(const Profile& p);
std::string decision_text(BootDecision d, const Config& c, const std::string& driver);

struct OptimizeHooks {
    std::function<bool()> aborted;                        // polled between probes
    std::function<void(const std::string&)> log;          // one line per event
    // The GPU being tuned while a run is active (nullptr before and after), so
    // an emergency handler can reset it.
    std::function<void(const GpuControl*)> active_gpu;
};

struct OptimizeOutcome {
    bool ran = false;          // false: stopped before tuning; `error` says why
    std::string error;
    OptimizeResult result;     // valid when ran
    bool saved = false;        // the profile was saved for --apply / boot-apply
    std::string save_note;     // why it was not saved, when !saved
};

// --optimize from start to end: state folder, drivers, journal checks, the
// search, and saving the profile. Needs elevation.
OptimizeOutcome run_optimize(Preset preset, const OptimizeHooks& hooks);

struct BootApplyOutcome {
    bool applied = false;
    std::string message;       // what happened, as written to boot.log
    Profile profile;           // the profile that was applied, when applied
};

// The logon half of boot-apply: decision (strikes, driver, GPU), strike
// recorded durably, profile applied. Writes one boot.log line.
BootApplyOutcome apply_at_logon();
// After the grace period: clears the strike without touching the profile.
void clear_boot_strike();

// Installs the exes into Program Files and registers the logon task (true),
// or removes both. `message` describes the outcome either way.
bool enable_boot(std::string* message);
bool disable_boot(std::string* message);

}
