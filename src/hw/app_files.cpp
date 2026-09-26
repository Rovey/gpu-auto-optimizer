#include "hw/app_files.hpp"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace gao {

std::filesystem::path app_dir() {
    const wchar_t* base = _wgetenv(L"ProgramData");
    if (!base || !*base) return {};
    return std::filesystem::path(base) / L"GpuAutoOptimizer";
}

static std::filesystem::path in_app_dir(const wchar_t* name) {
    const auto dir = app_dir();
    return dir.empty() ? dir : dir / name;
}
std::filesystem::path journal_path() { return in_app_dir(L"journal.jsonl"); }
std::filesystem::path config_path() { return in_app_dir(L"gao.json"); }
std::filesystem::path boot_log_path() { return in_app_dir(L"boot.log"); }

std::filesystem::path legacy_journal_path() {
    const wchar_t* base = _wgetenv(L"LOCALAPPDATA");
    if (!base || !*base) return {};
    return std::filesystem::path(base) / L"GpuAutoOptimizer" / L"journal.jsonl";
}

// Owner Administrators; protected DACL: SYSTEM and Administrators full
// control, Users read & execute, inherited by everything inside.
static constexpr wchar_t kAppDirSddl[] = L"O:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1200a9;;;BU)";

bool ensure_app_dir(std::string* why) {
    auto fail = [&](const std::string& w) { if (why) *why = w; return false; };
    const auto dir = app_dir();
    if (dir.empty()) return fail("%ProgramData% is not set");
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(kAppDirSddl, SDDL_REVISION_1, &sd, nullptr))
        return fail("could not build the folder's security descriptor");
    SECURITY_ATTRIBUTES sa{sizeof(sa), sd, FALSE};
    bool ok = true;
    if (!CreateDirectoryW(dir.c_str(), &sa)) {
        if (GetLastError() != ERROR_ALREADY_EXISTS) {
            ok = fail("could not create " + dir.string());
        } else {
            // Someone may have created it first to plant files the elevated
            // process would trust: only adopt a folder an admin owns.
            PSID owner = nullptr;
            PSECURITY_DESCRIPTOR existing = nullptr;
            if (GetNamedSecurityInfoW(dir.c_str(), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION, &owner, nullptr,
                                      nullptr, nullptr, &existing) != ERROR_SUCCESS) {
                ok = fail("could not read the owner of " + dir.string());
            } else {
                const bool trusted = IsWellKnownSid(owner, WinBuiltinAdministratorsSid) || IsWellKnownSid(owner, WinLocalSystemSid);
                LocalFree(existing);
                if (!trusted) {
                    ok = fail(dir.string() + " exists but is not owned by Administrators or SYSTEM; delete it and try again");
                } else {
                    PACL dacl = nullptr;
                    BOOL present = FALSE, defaulted = FALSE;
                    GetSecurityDescriptorDacl(sd, &present, &dacl, &defaulted);
                    std::wstring name = dir.wstring();
                    if (SetNamedSecurityInfoW(name.data(), SE_FILE_OBJECT,
                                              DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr,
                                              nullptr, dacl, nullptr) != ERROR_SUCCESS)
                        ok = fail("could not secure " + dir.string());
                }
            }
        }
    }
    LocalFree(sd);
    return ok;
}

std::vector<std::string> read_lines(const std::filesystem::path& p) {
    std::vector<std::string> lines;
    std::ifstream in(p, std::ios::binary);
    for (std::string line; std::getline(in, line);) lines.push_back(line);
    return lines;
}

std::optional<std::string> read_file(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

static bool write_all(HANDLE h, const std::string& data) {
    DWORD written = 0;
    return WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) &&
           written == data.size() && FlushFileBuffers(h);
}

bool append_line_durable(const std::filesystem::path& p, const std::string& line) {
    const HANDLE h = CreateFileW(p.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    const bool ok = write_all(h, line + "\n");
    CloseHandle(h);
    return ok;
}

bool write_file_atomic(const std::filesystem::path& p, const std::string& text) {
    std::filesystem::path tmp = p;
    tmp += L".tmp";
    const HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    const bool ok = write_all(h, text);
    CloseHandle(h);
    return ok && MoveFileExW(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

}
