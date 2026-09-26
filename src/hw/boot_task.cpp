#include "hw/boot_task.hpp"
#include "hw/app_files.hpp"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <sddl.h>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace gao {

static int run_schtasks(const std::wstring& args) {
    // Full path: --boot runs elevated, and a bare name would let the current
    // or the exe's folder supply a different schtasks.exe.
    wchar_t sys[MAX_PATH];
    const UINT n = GetSystemDirectoryW(sys, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return -1;
    const std::wstring app = std::wstring(sys) + L"\\schtasks.exe";
    std::wstring cmd = L"\"" + app + L"\" " + args;
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(app.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return -1;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return static_cast<int>(code);
}

std::filesystem::path installed_exe_path() {
    const auto pf = program_files_dir();
    return pf.empty() ? pf : pf / L"GpuAutoOptimizer" / L"gao.exe";
}

bool install_exe(const std::filesystem::path& self, std::string* why) {
    const auto dst = installed_exe_path();
    if (dst.empty()) { if (why) *why = "the Program Files folder could not be resolved"; return false; }
    std::error_code ec;
    if (std::filesystem::equivalent(self, dst, ec)) return true;   // running the installed copy already
    std::filesystem::create_directories(dst.parent_path(), ec);
    if (!CopyFileW(self.c_str(), dst.c_str(), FALSE)) {
        if (why) *why = "could not copy gao.exe to " + dst.string() + " (error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    return true;
}

void uninstall_exe() {
    const auto dst = installed_exe_path();
    if (dst.empty()) return;
    std::error_code ec;
    std::filesystem::remove(dst, ec);
    std::filesystem::remove(dst.parent_path(), ec);   // only succeeds when empty
}

bool files_equal(const std::filesystem::path& a, const std::filesystem::path& b) {
    std::ifstream fa(a, std::ios::binary), fb(b, std::ios::binary);
    if (!fa || !fb) return false;
    return std::vector<char>(std::istreambuf_iterator<char>(fa), {}) ==
           std::vector<char>(std::istreambuf_iterator<char>(fb), {});
}

std::string current_user_sid() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return {};
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<unsigned char> buf(size);
    std::string out;
    if (size && GetTokenInformation(token, TokenUser, buf.data(), size, &size)) {
        LPSTR text = nullptr;
        if (ConvertSidToStringSidA(reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid, &text)) {
            out = text;
            LocalFree(text);
        }
    }
    CloseHandle(token);
    return out;
}

bool write_utf16_file(const std::filesystem::path& p, const std::string& utf8) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), w.data(), n);
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    const unsigned char bom[] = {0xFF, 0xFE};
    out.write(reinterpret_cast<const char*>(bom), 2);
    out.write(reinterpret_cast<const char*>(w.data()), static_cast<std::streamsize>(w.size() * sizeof(wchar_t)));
    return static_cast<bool>(out);
}

int boot_task_create_xml(const std::filesystem::path& xml_file) {
    return run_schtasks(L"/Create /F /TN \\GpuAutoOptimizer\\BootApply /XML \"" + xml_file.wstring() + L"\"");
}
int boot_task_remove() { return run_schtasks(L"/Delete /F /TN \\GpuAutoOptimizer\\BootApply"); }
int boot_task_remove_legacy() { return run_schtasks(L"/Delete /F /TN GpuAutoOptimizer"); }
bool boot_task_exists() { return run_schtasks(L"/Query /TN \\GpuAutoOptimizer\\BootApply") == 0; }
bool boot_task_legacy_exists() { return run_schtasks(L"/Query /TN GpuAutoOptimizer") == 0; }

}
