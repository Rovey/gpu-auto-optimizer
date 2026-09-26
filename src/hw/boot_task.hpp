#pragma once
#include <filesystem>
#include <string>

namespace gao {

// The logon task \GpuAutoOptimizer\BootApply runs an installed copy of gao.exe
// in %ProgramFiles%, which only administrators can replace. schtasks calls
// return its exit code (0 = success), or -1 when it could not be started.

std::filesystem::path installed_exe_path();   // %ProgramFiles%\GpuAutoOptimizer\gao.exe
// Copies `self` there (overwriting); a no-op when `self` already is that file.
bool install_exe(const std::filesystem::path& self, std::string* why);
void uninstall_exe();                          // removes the copy and, if empty, its folder
bool files_equal(const std::filesystem::path& a, const std::filesystem::path& b);

std::string current_user_sid();                // e.g. "S-1-5-21-..."; empty on failure
// Task Scheduler expects its XML as UTF-16LE with a byte-order mark.
bool write_utf16_file(const std::filesystem::path& p, const std::string& utf8);

int boot_task_create_xml(const std::filesystem::path& xml_file);
int boot_task_remove();          // \GpuAutoOptimizer\BootApply
int boot_task_remove_legacy();   // \GpuAutoOptimizer, the pre-P4b task
bool boot_task_exists();
bool boot_task_legacy_exists();

}
