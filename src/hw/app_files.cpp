#include "hw/app_files.hpp"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <shlobj.h>
#include <fstream>
#include <sstream>

namespace gao {

// Known folders come from the registry under HKLM, not from environment
// variables: the elevated process must not let the user's own environment
// (HKCU\Environment) decide where it installs or what it trusts.
static std::filesystem::path known_folder(REFKNOWNFOLDERID id) {
    PWSTR raw = nullptr;
    std::filesystem::path out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &raw))) out = raw;
    CoTaskMemFree(raw);
    return out;
}

std::filesystem::path program_files_dir() { return known_folder(FOLDERID_ProgramFiles); }

std::filesystem::path app_dir() {
    const auto base = known_folder(FOLDERID_ProgramData);
    return base.empty() ? base : base / L"GpuAutoOptimizer";
}

static std::filesystem::path in_app_dir(const wchar_t* name) {
    const auto dir = app_dir();
    return dir.empty() ? dir : dir / name;
}
std::filesystem::path journal_path() { return in_app_dir(L"journal.jsonl"); }
std::filesystem::path config_path() { return in_app_dir(L"gao.json"); }
std::filesystem::path boot_log_path() { return in_app_dir(L"boot.log"); }

// Owner Administrators; protected DACL: SYSTEM and Administrators full
// control, Users read & execute, inherited by everything inside.
static constexpr wchar_t kAppDirSddl[] = L"O:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1200a9;;;BU)";

bool ensure_app_dir(std::string* why) {
    auto fail = [&](const std::string& w) { if (why) *why = w; return false; };
    const auto dir = app_dir();
    if (dir.empty()) return fail("the ProgramData folder could not be resolved");
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(kAppDirSddl, SDDL_REVISION_1, &sd, nullptr))
        return fail("could not build the folder's security descriptor");
    SECURITY_ATTRIBUTES sa{sizeof(sa), sd, FALSE};
    if (CreateDirectoryW(dir.c_str(), &sa)) {   // new: created with our descriptor, nothing to check
        LocalFree(sd);
        return true;
    }
    if (GetLastError() != ERROR_ALREADY_EXISTS) {
        LocalFree(sd);
        return fail("could not create " + dir.string());
    }
    // It already exists. A user can create folders in ProgramData, so it may
    // have been planted -- possibly as a junction to some other admin-owned
    // tree. Open the folder object itself (never a reparse target) and do
    // every check and change through that one handle.
    const HANDLE h = CreateFileW(dir.c_str(), READ_CONTROL | WRITE_DAC,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                 FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        LocalFree(sd);
        return fail("could not open " + dir.string());
    }
    bool ok = true;
    BY_HANDLE_FILE_INFORMATION info{};
    PSID owner = nullptr;
    PSECURITY_DESCRIPTOR existing = nullptr;
    if (!GetFileInformationByHandle(h, &info) || !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        ok = fail(dir.string() + " is a link or not a folder; delete it and try again");
    } else if (GetSecurityInfo(h, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION, &owner, nullptr, nullptr, nullptr,
                               &existing) != ERROR_SUCCESS) {
        ok = fail("could not read the owner of " + dir.string());
    } else if (!IsWellKnownSid(owner, WinBuiltinAdministratorsSid) && !IsWellKnownSid(owner, WinLocalSystemSid)) {
        ok = fail(dir.string() + " exists but is not owned by Administrators or SYSTEM; delete it and try again");
    } else {
        PACL dacl = nullptr;
        BOOL present = FALSE, defaulted = FALSE;
        GetSecurityDescriptorDacl(sd, &present, &dacl, &defaulted);
        if (SetSecurityInfo(h, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                            nullptr, nullptr, dacl, nullptr) != ERROR_SUCCESS)
            ok = fail("could not secure " + dir.string());
    }
    if (existing) LocalFree(existing);
    CloseHandle(h);
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
