#include "hw/boot_task.hpp"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <string>

namespace gao {

static int run_schtasks(std::wstring args) {
    std::wstring cmd = L"schtasks.exe " + args;
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    // CREATE_NO_WINDOW: schtasks' own output is not ours to print.
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return -1;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return static_cast<int>(code);
}

int boot_task_create(const std::filesystem::path& exe) {
    // /TR "\"C:\path with spaces\gao.exe\" --boot-apply": the exe is quoted
    // inside the quoted action, so a path with spaces survives.
    return run_schtasks(L"/Create /F /TN GpuAutoOptimizer /SC ONLOGON /RL HIGHEST /TR \"\\\"" +
                        exe.wstring() + L"\\\" --boot-apply\"");
}

int boot_task_remove() { return run_schtasks(L"/Delete /F /TN GpuAutoOptimizer"); }

bool boot_task_exists() { return run_schtasks(L"/Query /TN GpuAutoOptimizer") == 0; }

}
