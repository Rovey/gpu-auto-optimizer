#pragma once
// Application logic shared by gao.exe (CLI) and GpuAutoOptimizer.exe (window
// and tray): everything between the core/hw layers and a user interface.
// Nothing here prints; callers decide how to show messages.
#include "core/boot.hpp"
#include "core/config.hpp"
#include "core/fan_curve.hpp"
#include "core/fan_tune.hpp"
#include "core/objectives.hpp"
#include "core/search.hpp"
#include "core/types.hpp"
#include "core/undervolt.hpp"
#include "core/update.hpp"
#include <functional>
#include <optional>
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

// Held (process-wide, across gao.exe and the tray app) for the whole of an
// optimize: the watchdog must not re-apply the saved tune under a running
// search, and two searches must not run at once.
class TuningLock {
public:
    TuningLock();
    ~TuningLock();
    TuningLock(const TuningLock&) = delete;
    TuningLock& operator=(const TuningLock&) = delete;
    bool owned() const { return owned_; }
private:
    void* handle_ = nullptr;
    bool owned_ = false;
};
// True while another thread or process holds the TuningLock.
bool tuning_in_progress();

// What the command line tells a running tray app, so its watchdog follows:
// stock on purpose (gao --reset) stops it; the tune applied (gao --apply)
// starts it again.
enum class TrayNotice : unsigned { StockByChoice = 0, TuneApplied = 1 };
void tell_tray(TrayNotice notice);
// The registered window message tell_tray() posts; the notice is its wParam.
unsigned tray_notice_message();

struct OptimizeHooks {
    std::function<bool()> aborted;                        // polled between probes and after every stress batch
    std::function<void(const std::string&)> log;          // one line per event
    // The GPU being tuned while a run is active (nullptr before and after), so
    // an emergency handler can reset it.
    std::function<void(const GpuControl*)> active_gpu;
    // A long measurement starts (what it is, its length in seconds) or has
    // ended ("", 0). The undervolt run reports them; see UndervoltIo.
    std::function<void(const std::string& what, double seconds)> measuring;
};

struct OptimizeOutcome {
    bool ran = false;          // false: stopped before tuning; `error` says why
    std::string error;
    OptimizeResult result;     // valid when ran
    // The card at stock for five minutes, as long as the result is soaked,
    // with the fans left to the NVIDIA driver: the "before" of the result.
    // The search's own 30 s baseline (result.baseline) is too short to heat
    // the card through; it stays what the search measures its probes against.
    StabilityResult stock;     // valid when ran
    bool saved = false;        // the profile was saved for --apply / boot-apply
    std::string save_note;     // why it was not saved, when !saved
};

// --optimize from start to end: state folder, drivers, journal checks, the
// card at stock for five minutes, the search, and saving the profile. Needs
// elevation.
// fan_curve: the curve to drive the fans with during the run and to save
// as the tested curve; empty means the profile's own (default_curve).
// known_stock: the card at stock as the caller has just measured it (the all-in-one
// run); empty means it is measured here, before the search.
// Not for Preset::Undervolt: that is run_undervolt.
OptimizeOutcome run_optimize(Preset preset, const OptimizeHooks& hooks, const std::optional<FanCurve>& fan_curve = {},
                             const std::optional<StabilityResult>& known_stock = {});

struct UndervoltOutcome {
    bool ran = false;          // false: stopped before the search; `error` says why
    std::string error;
    UndervoltResult result;    // valid when ran
    bool saved = false;        // the profile was saved for --apply / boot-apply
    std::string save_note;     // why it was not saved, when !saved
};

// The undervolt from start to end: state folder, drivers, journal, the
// search (core/undervolt.hpp), and saving the result as the profile, which
// replaces a saved overclock: a core offset and a flat top are one table on
// the card (hardware check 65). Needs elevation. The NVIDIA driver keeps the
// fans during the search.
// on_saved_tune: the undervolt is searched on top of the saved overclock
// (its power limit, core and memory offsets stay applied throughout) and
// saved into that profile, next to them. Without a saved tune that applies
// to this card and driver the run does not start.
// how: for a run on the saved tune that is one step of a longer run.
struct UndervoltOnTune {
    std::optional<StabilityResult> measured;   // the overclock's own long run: the reference, nothing is measured again
    int clock_khz = 0;                         // > 0: the clock to keep, below the overclock's (see UndervoltBase)
    std::optional<Preset> save_as;             // the preset the profile is saved under; empty: as it was
};
UndervoltOutcome run_undervolt(const OptimizeHooks& hooks, bool on_saved_tune = false, const UndervoltOnTune& how = {});

struct AllInOneOutcome {
    bool ran = false;            // false: stopped before anything was tuned; `error` says why
    std::string error;           // why the run ended without a result; the card is at stock then
    bool ok = false;             // a result is applied and saved
    StabilityResult stock;       // five minutes at stock: the "before"
    OptimizeOutcome overclock;   // step 2
    UndervoltOutcome undervolt;  // step 3; not ran when it was not reached
    std::string undervolt_note;  // why the result carries no undervolt, when it does not
    FanTuneResult fan;           // step 4; !ok: the profile's own fan curve is kept
    std::string fan_note;        // why, when it is
    StabilityResult now;         // the last five-minute run with the result applied: the "after"
    // Set when the final test scored clearly less than the same settings did
    // minutes before, at the same clock: something else was using the
    // graphics card or the processor, and `now` is not a clean number.
    std::string disturbed;
    Profile profile;             // what was saved, when ok
};

// One run that does all of it, for a card nobody has tuned before:
//   1. five minutes at stock, to compare with;
//   2. the overclock of Best of my GPU (power limit, memory, core);
//   3. an undervolt on top of it, at a clock half-way between what the card
//      ran at stock and what it runs with the overclock: the overclock and
//      the undervolt are two ends of one line (hardware check 74), and this
//      takes the middle: more speed than stock on less power than stock;
//   4. the quietest fan speed that holds the temperature target, measured,
//      and a fan curve through it;
//   5. five minutes with everything applied, fans on that curve: the result
//      is only kept whole when this passes.
// A step that fails keeps what the steps before it found: no undervolt
// leaves the overclock, no fan tune leaves the profile's own curve. A stop
// request ends at stock. Needs elevation.
AllInOneOutcome run_all_in_one(const OptimizeHooks& hooks);

struct BootApplyOutcome {
    bool applied = false;
    BootDecision decision = BootDecision::NoProfile;
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

// True when the copies in Program Files are byte for byte this build.
bool logon_copy_is_this_build();
// Apply-at-logon runs the copy in Program Files. When this build runs
// elevated from another folder and that copy is an older or equal version
// with other contents, it is replaced by this build, so that a profile saved
// here is one the logon copy accepts. Returns what happened for the log;
// empty when there was nothing to do.
std::string update_logon_copy();

struct UpdateCheck {
    std::optional<ReleaseInfo> release;   // set when a release newer than this build exists
    std::string error;                    // why the check failed, when it did
};
// Asks GitHub for the latest release. One HTTPS request; it sends nothing
// but the request itself.
UpdateCheck check_for_update();
// Downloads that release, checks the zip against the SHA-256 GitHub
// published for it, and replaces both executables in this program's folder.
// The running program keeps running its old code until it is restarted.
// `message` describes the outcome either way.
bool install_update(const ReleaseInfo& release, std::string* message);

}
