#pragma once
#include <filesystem>
#include <string>

namespace gao {

// The logon task \GpuAutoOptimizer\BootApply runs an installed copy of the
// app in %ProgramFiles%, which only administrators can replace. The task is
// registered through the Task Scheduler's own interface; nothing is started
// for it. The calls return its HRESULT: 0 or more is success.

// The executables that make up the app; both are installed together.
inline constexpr const wchar_t* kAppExes[] = {L"gao.exe", L"GpuAutoOptimizer.exe"};
std::filesystem::path installed_dir();        // %ProgramFiles%\GpuAutoOptimizer
std::filesystem::path installed_exe_path();   // ...\gao.exe
std::filesystem::path installed_tray_path();  // ...\GpuAutoOptimizer.exe
// Copies both executables from `from_dir` into installed_dir() (overwriting; a
// running copy is renamed aside first);
// a no-op when `from_dir` already is that folder.
bool install_app(const std::filesystem::path& from_dir, std::string* why);
// The same copy into any folder: an update replaces the executables where
// they run from.
bool copy_app(const std::filesystem::path& from_dir, const std::filesystem::path& to_dir, std::string* why);
// Removes both copies and, if empty, the folder. A copy that is running (the
// tray app) is renamed aside and deleted at the next restart; returns false then.
bool uninstall_app();
bool files_equal(const std::filesystem::path& a, const std::filesystem::path& b);

std::string current_user_sid();                // e.g. "S-1-5-21-..."; empty on failure
// Creates the task, or replaces it, from its XML (core/boot.hpp, UTF-8).
long boot_task_create(const std::string& xml);
long boot_task_remove();
bool boot_task_exists();

}
