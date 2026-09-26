#pragma once
#include <filesystem>

namespace gao {

// The logon task that runs `gao.exe --boot-apply`, driven through
// schtasks.exe. Each call returns schtasks' exit code (0 = success), or -1
// when schtasks could not be started. Creating needs an elevated caller.
// ponytail: schtasks defaults to "start only on AC power"; fine for desktops,
// switch to the Task Scheduler COM API if laptops need boot-apply on battery.
int boot_task_create(const std::filesystem::path& exe);
int boot_task_remove();
bool boot_task_exists();

}
