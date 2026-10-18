#include "app/common.hpp"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "hw/app_files.hpp"
#include "core/version.hpp"
#include <cstdio>
#include <ctime>
#include <filesystem>

namespace gao::app {

namespace {
constexpr wchar_t kTuningMutex[] = L"Local\\GpuAutoOptimizer.Tuning";
constexpr wchar_t kTrayWindowClass[] = L"GpuAutoOptimizerWindow";
}

TuningLock::TuningLock() {
    handle_ = CreateMutexW(nullptr, FALSE, kTuningMutex);
    if (!handle_) return;
    // Not 0: tuning_in_progress() holds the mutex for an instant while it looks.
    const DWORD r = WaitForSingleObject(handle_, 100);
    owned_ = r == WAIT_OBJECT_0 || r == WAIT_ABANDONED;   // abandoned: the last owner died mid-run
}

TuningLock::~TuningLock() {
    if (owned_) ReleaseMutex(handle_);
    if (handle_) CloseHandle(handle_);
}

bool tuning_in_progress() {
    const HANDLE h = OpenMutexW(SYNCHRONIZE, FALSE, kTuningMutex);
    if (!h) return false;
    const DWORD r = WaitForSingleObject(h, 0);
    if (r == WAIT_OBJECT_0 || r == WAIT_ABANDONED) ReleaseMutex(h);
    CloseHandle(h);
    return r == WAIT_TIMEOUT;
}

unsigned tray_notice_message() {
    static const UINT msg = RegisterWindowMessageW(L"GpuAutoOptimizer.TrayNotice");
    return msg;
}

void tell_tray(TrayNotice notice) {
    if (const HWND tray = FindWindowW(kTrayWindowClass, nullptr))
        PostMessageW(tray, tray_notice_message(), static_cast<WPARAM>(notice), 0);
}

bool is_elevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    const bool ok = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size);
    CloseHandle(token);
    return ok && elevation.TokenIsElevated;
}

std::string now_text() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm);
    return buf;
}

Config load_config() {
    const auto text = read_file(config_path());
    return text ? from_json(*text) : Config{};
}

bool save_config(const Config& c) {
    return !config_path().empty() && write_file_atomic(config_path(), to_json(c));
}

void boot_log(const std::string& msg) {
    append_line_durable(boot_log_path(), now_text() + "  " + msg);
}

bool prepare_state(std::string* why) { return ensure_app_dir(why); }

std::function<void(const std::string&)> logged_to(const std::filesystem::path& file, std::function<void(const std::string&)> show) {
    std::error_code ec;
    if (std::filesystem::file_size(file, ec) > kRunLogMaxBytes && !ec) {
        std::filesystem::path old = file;
        old += L".old";
        std::filesystem::rename(file, old, ec);   // replaces an older one
    }
    auto write = [file](const std::string& line) {
        SYSTEMTIME t{};
        GetLocalTime(&t);
        char stamp[32];
        std::snprintf(stamp, sizeof(stamp), "%04d-%02d-%02d %02d:%02d:%02d  ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
        append_line_durable(file, stamp + line);
    };
    write("=== " + std::string(kProductName) + " " + std::string(kVersion));
    return [write, show = std::move(show)](const std::string& line) {
        write(line);   // on disk before anything acts on it
        if (show) show(line);
    };
}

std::function<void(const std::string&)> logged(std::function<void(const std::string&)> show) {
    // Only into a folder that was verified: an elevated process does not
    // append to a file somebody else may have put there.
    std::string why;
    if (!is_elevated() || run_log_path().empty() || !prepare_state(&why)) return show;
    return logged_to(run_log_path(), std::move(show));
}

std::string profile_text(const Profile& p) {
    const std::string tail = " (driver " + p.driver + ", saved " + p.saved_at + ")";
    const std::string clocks = "power " + std::to_string(p.power_pct) + " %, core +" + std::to_string(p.core_mhz) +
                               " MHz, mem +" + std::to_string(p.mem_mhz) + " MHz";
    if (!p.undervolt) return std::string(preset_name(p.preset)) + ": " + clocks + tail;
    return std::string(preset_name(p.preset)) + ": " + std::to_string(p.undervolt->freq_khz / 1000) + " MHz at " +
           std::to_string(p.undervolt->volt_uv / 1000) + " mV, " + clocks + tail;
}

std::string decision_text(BootDecision d, const Config& c, const std::string& driver) {
    switch (d) {
        case BootDecision::NoProfile: return "no saved profile; run an optimize first";
        case BootDecision::TooManyStrikes:
            return "disabled after " + std::to_string(kMaxBootStrikes) + " crashes; turn boot-apply on again to retry";
        case BootDecision::DriverChanged:
            if (driver.empty()) return "driver version unknown (NVML did not report it); not applied";
            return "the driver was updated (" + c.profile->driver + " -> " + driver +
                   "); test the tune again in the app (5 minutes, or `gao --retest`), or optimize again";
        case BootDecision::GpuChanged: return "this is not the card the profile was tested on; optimize again";
        case BootDecision::Apply: return "apply";
    }
    return "unknown";
}

}
