#include "app/common.hpp"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "core/task_xml.hpp"
#include "core/version.hpp"
#include "hw/app_files.hpp"
#include "hw/boot_task.hpp"
#include "hw/gpu_control.hpp"
#include "hw/nvapi.hpp"
#include "hw/nvml.hpp"
#include "hw/update_io.hpp"
#include <cstdio>
#include <filesystem>

namespace gao::app {

BootApplyOutcome apply_at_logon() {
    BootApplyOutcome out;
    auto done = [&](const std::string& msg) { out.message = msg; boot_log(msg); return out; };
    Nvml nvml;
    if (!nvml.Init()) return done("NVML init failed: " + nvml.Error());
    Config cfg = load_config();
    const std::string driver = nvml.DriverVersion();
    const auto d = decide_boot(cfg, driver, nvml.GpuUuid(kGpu), nvml.DeviceCount());
    out.decision = d;
    if (d != BootDecision::Apply) return done("not applied: " + decision_text(d, cfg, driver));
    // The strike is on disk before the hardware is touched: a crash from here
    // on counts.
    ++cfg.boot_strikes;
    if (!save_config(cfg)) return done("could not record the strike; not applying");
    // A strike is for a logon that may have crashed the machine. An apply
    // that ended here, with nothing of the profile on the card, did not: the
    // strike is taken back, or three refused logons would read as "disabled
    // after 3 crashes".
    auto no_strike = [&] {
        --cfg.boot_strikes;
        if (!save_config(cfg)) boot_log("could not take the strike back; it stays");
    };
    Nvapi nvapi;
    if (!nvapi.Init()) { no_strike(); return done("NVAPI init failed: " + nvapi.Error()); }
    std::string why;
    bool left_clean = false;
    if (!apply_profile(make_gpu_control(nvml, nvapi, kGpu), *cfg.profile, &why, &left_clean)) {
        if (left_clean) no_strike();
        return done("apply failed: " + why);
    }
    out.applied = true;
    out.profile = *cfg.profile;
    return done("applied " + profile_text(*cfg.profile) + ", strike " + std::to_string(cfg.boot_strikes) +
                " clears in 2 minutes");
}

void clear_boot_strike() {
    // Reload: an optimize may have saved a new profile meanwhile; only the
    // counter is ours to change. If the file cannot be read, or reads without
    // a profile, writing would destroy it: leave the strike (fails safe).
    for (int attempt = 0; attempt < 3; ++attempt) {
        const auto text = read_file(config_path());
        Config latest = text ? from_json(*text) : Config{};
        if (!latest.profile) { boot_log("could not re-read gao.json to clear the strike; it stays"); return; }
        latest.boot_strikes = 0;
        if (save_config(latest)) { boot_log("ran 2 minutes without a crash; strike cleared"); return; }
        Sleep(1000);
    }
    boot_log("could not save gao.json to clear the strike; it stays");
}

bool enable_boot(std::string* message) {
    auto say = [&](const std::string& m, bool ok) { if (message) *message = m; return ok; };
    if (!is_elevated()) return say("boot-apply needs administrator rights", false);
    std::string why;
    if (!prepare_state(&why)) return say(why, false);
    const std::string sid = current_user_sid();
    if (sid.empty()) return say("could not determine the current user's SID", false);
    wchar_t self[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, self, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return say("could not find this program's own path", false);
    if (!install_app(std::filesystem::path(self).parent_path(), &why))
        return say(why, false);
    // The logon task starts the tray app, which applies the tune and stays
    // resident to keep it applied: no time limit.
    const auto exe = installed_tray_path();
    const auto u8 = exe.u8string();
    const std::string xml = boot_task_xml(std::string(u8.begin(), u8.end()), "--tray", sid, "PT0S");
    if (const long code = boot_task_create(xml); code < 0) {
        char hex[16];
        std::snprintf(hex, sizeof(hex), "%08lX", static_cast<unsigned long>(code));
        return say(std::string("could not create the task (Task Scheduler error 0x") + hex + ")", false);
    }
    // Not a reason to fail: the task is there, and the tray icon still opens the app.
    const bool in_start_menu = start_menu_entry_create() >= 0;
    Config cfg = load_config();
    cfg.boot_strikes = 0;
    if (!save_config(cfg)) return say("task created, but could not reset the strike counter", false);
    return say("boot-apply on: " + exe.string() + " --tray runs at every logon and keeps the tune applied (strikes reset)" +
               (in_start_menu ? "" : "; the Start menu entry could not be made") +
               (cfg.profile ? "" : "; note: there is no saved profile yet, optimize first"), true);
}

bool disable_boot(std::string* message) {
    auto say = [&](const std::string& m, bool ok) { if (message) *message = m; return ok; };
    if (!is_elevated()) return say("boot-apply needs administrator rights", false);
    std::string why;
    if (!prepare_state(&why)) return say(why, false);
    const bool task_removed = boot_task_remove() >= 0;
    start_menu_entry_remove();
    const bool removed_now = uninstall_app();
    return say(std::string("boot-apply off: task ") + (task_removed ? "removed" : "not removed") + ", installed copy " +
                   (removed_now ? "removed" : "in use by the running tray app; it is removed at the next restart"),
               task_removed);
}

namespace {
std::filesystem::path own_dir() {
    wchar_t self[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, self, MAX_PATH);
    return n == 0 || n >= MAX_PATH ? std::filesystem::path() : std::filesystem::path(self).parent_path();
}
}

bool logon_copy_is_this_build() {
    const auto dir = own_dir();
    if (dir.empty()) return false;
    return files_equal(dir / L"gao.exe", installed_exe_path()) && files_equal(dir / L"GpuAutoOptimizer.exe", installed_tray_path());
}

bool logon_copy_outdated() {
    if (!boot_task_exists()) return false;
    const auto dir = own_dir();
    std::error_code ec;
    if (dir.empty() || std::filesystem::equivalent(dir, installed_dir(), ec)) return false;
    if (!std::filesystem::exists(installed_exe_path(), ec) || logon_copy_is_this_build()) return false;
    // Never a downgrade: an old zip that is still lying around must not
    // replace a newer installed copy just because it was started.
    const auto installed = file_version_number(installed_exe_path());
    return !(installed && *installed > kVersionNumber);
}

void ensure_start_menu_entry() {
    std::error_code ec;
    if (is_elevated() && boot_task_exists() && !start_menu_entry_exists() && std::filesystem::exists(installed_tray_path(), ec))
        start_menu_entry_create();
}

std::string update_logon_copy() {
    if (!is_elevated() || !logon_copy_outdated()) return {};
    std::string message;
    if (!enable_boot(&message)) return "The copy that starts at logon could not be updated: " + message;
    return "The copy that starts at logon was updated to this version (" + std::string(kVersion) + ").";
}

UpdateCheck check_for_update() {
    UpdateCheck out;
    const auto body = https_get("https://api.github.com/repos/Rovey/gpu-auto-optimizer/releases/latest", 1024 * 1024, &out.error);
    if (!body) return out;
    auto release = parse_latest_release(*body);
    if (!release) { out.error = "the answer did not describe a release"; return out; }
    if (*version_number(release->version) > kVersionNumber) out.release = std::move(release);
    return out;
}

bool own_folder_writable() {
    const auto dir = own_dir();
    if (dir.empty()) return false;
    const auto probe = dir / (L"write-test-" + std::to_wstring(GetCurrentProcessId()) + L".tmp");
    const HANDLE file = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                    FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    CloseHandle(file);
    return true;
}

bool install_update(const ReleaseInfo& release, std::string* message) {
    auto say = [&](const std::string& m, bool ok) { if (message) *message = m; return ok; };
    const auto dir = own_dir();
    if (dir.empty()) return say("could not find this program's own folder", false);
    // Elevated, the download is unpacked where only administrators can write:
    // between the hash check and the copy, nobody else may swap the files.
    std::filesystem::path work;
    std::string why;
    if (is_elevated()) {
        if (!prepare_state(&why)) return say(why, false);
        work = app_dir() / L"update";
    } else {
        std::error_code ec;
        work = std::filesystem::temp_directory_path(ec) / L"GpuAutoOptimizer-update";
        if (ec) return say("no temporary folder", false);
    }
    std::error_code ec;
    std::filesystem::remove_all(work, ec);
    const auto unpacked = work / L"files";
    if (!std::filesystem::create_directories(unpacked, ec)) return say("could not create " + work.string(), false);
    struct Cleanup {
        std::filesystem::path p;
        ~Cleanup() { std::error_code e; std::filesystem::remove_all(p, e); }
    } cleanup{work};

    // What to do and in which order is install_release's (core, tested with a
    // fake source); here are only the real steps.
    const auto zip = work / L"update.zip";
    UpdateSteps steps;
    steps.download = [&](const std::string& url, std::string* error) { return https_download(url, zip, 64 * 1024 * 1024, error); };
    steps.sha256 = [&] { return sha256_hex(zip); };
    steps.unpack = [&](std::string* error) { return extract_zip(zip, unpacked, error); };
    steps.unpacked_is = [&](unsigned version) {
        for (const wchar_t* name : kAppExes)
            if (file_version_number(unpacked / name) != version) return false;
        return true;
    };
    steps.replace = [&](std::string* error) { return copy_app(unpacked, dir, error); };
    return install_release(release, steps, message);
}

}
