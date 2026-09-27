#pragma once
#include <filesystem>
#include <string>

namespace gao {

// The logon task \GpuAutoOptimizer\BootApply runs an installed copy of gao.exe
// in %ProgramFiles%, which only administrators can replace. schtasks calls
// return its exit code (0 = success), or -1 when it could not be started.

// The executables that make up the app; both are installed together.
inline constexpr const wchar_t* kAppExes[] = {L"gao.exe", L"GpuAutoOptimizer.exe"};
std::filesystem::path installed_dir();        // %ProgramFiles%\GpuAutoOptimizer
std::filesystem::path installed_exe_path();   // ...\gao.exe
std::filesystem::path installed_tray_path();  // ...\GpuAutoOptimizer.exe
// Copies both executables from `from_dir` into installed_dir() (overwriting; a
// running copy is renamed aside first);
// a no-op when `from_dir` already is that folder.
bool install_app(const std::filesystem::path& from_dir, std::string* why);
// Removes both copies and, if empty, the folder. A copy that is running (the
// tray app) is renamed aside and deleted at the next restart; returns false then.
bool uninstall_app();
bool files_equal(const std::filesystem::path& a, const std::filesystem::path& b);

std::string current_user_sid();                // e.g. "S-1-5-21-..."; empty on failure
// Task Scheduler expects its XML as UTF-16LE with a byte-order mark.
bool write_utf16_file(const std::filesystem::path& p, const std::string& utf8);

int boot_task_create_xml(const std::filesystem::path& xml_file);
int boot_task_remove();          // \GpuAutoOptimizer\BootApply
bool boot_task_exists();

}
