#include "hw/boot_task.hpp"
#include "hw/app_files.hpp"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <sddl.h>
#include <taskschd.h>
#include <wrl/client.h>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace gao {

std::filesystem::path installed_dir() {
    const auto pf = program_files_dir();
    return pf.empty() ? pf : pf / L"GpuAutoOptimizer";
}
std::filesystem::path installed_exe_path() { return installed_dir() / L"gao.exe"; }
std::filesystem::path installed_tray_path() { return installed_dir() / L"GpuAutoOptimizer.exe"; }

namespace {
// A running exe cannot be deleted or overwritten, but it can be renamed.
// Moving it aside frees its name at once; the renamed file goes at the next
// restart. (Deleting it under its own name at restart would also delete a
// copy installed there in the meantime.)
bool move_aside(const std::filesystem::path& p) {
    const auto aside = std::filesystem::path(p).concat(L".old-" + std::to_wstring(GetTickCount64()));
    if (!MoveFileExW(p.c_str(), aside.c_str(), 0)) return false;
    MoveFileExW(aside.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
    return true;
}
}

bool install_app(const std::filesystem::path& from_dir, std::string* why) {
    const auto dst = installed_dir();
    if (dst.empty()) { if (why) *why = "the Program Files folder could not be resolved"; return false; }
    return copy_app(from_dir, dst, why);
}

bool copy_app(const std::filesystem::path& from_dir, const std::filesystem::path& dst, std::string* why) {
    std::error_code ec;
    if (std::filesystem::equivalent(from_dir, dst, ec)) return true;   // already there
    for (const wchar_t* name : kAppExes) {   // all or nothing: never a half-installed pair
        if (!std::filesystem::is_regular_file(from_dir / name, ec)) {
            if (why) *why = (from_dir / name).string() + " is missing; both executables must be in the same folder";
            return false;
        }
    }
    std::filesystem::create_directories(dst, ec);
    // Copies renamed aside by an earlier run (see move_aside) that are no
    // longer running: gone now. Deleting them at restart needs administrator
    // rights, which an update in a user's own folder does not have.
    for (std::filesystem::directory_iterator it(dst, ec), end; !ec && it != end; it.increment(ec)) {
        const std::wstring file = it->path().filename().wstring();
        for (const wchar_t* name : kAppExes)
            if (file.rfind(std::wstring(name) + L".old-", 0) == 0) DeleteFileW(it->path().c_str());
    }
    for (const wchar_t* name : kAppExes) {
        const auto src = from_dir / name;
        const auto to = dst / name;
        bool copied = CopyFileW(src.c_str(), to.c_str(), FALSE);
        if (!copied && (GetLastError() == ERROR_SHARING_VIOLATION || GetLastError() == ERROR_ACCESS_DENIED))
            copied = move_aside(to) && CopyFileW(src.c_str(), to.c_str(), FALSE);   // a running copy
        if (!copied) {
            if (why) *why = "could not copy " + src.string() + " to " + dst.string() + " (error " + std::to_string(GetLastError()) + ")";
            return false;
        }
    }
    return true;
}

bool uninstall_app() {
    const auto dst = installed_dir();
    if (dst.empty()) return true;
    bool all = true;
    for (const wchar_t* name : kAppExes) {
        const auto p = dst / name;
        if (DeleteFileW(p.c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND) continue;
        move_aside(p);
        all = false;
    }
    if (all) RemoveDirectoryW(dst.c_str());   // only succeeds when empty
    return all;
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

namespace {

// The task \GpuAutoOptimizer\BootApply, through the Task Scheduler's own
// interface. (Until 0.4.0 this started schtasks.exe; an unsigned program that
// starts schtasks to create a task with the highest rights is what malware
// does too, and reads like it to an antivirus.)
constexpr wchar_t kTaskFolder[] = L"\\GpuAutoOptimizer";
constexpr wchar_t kTaskName[] = L"BootApply";

struct Bstr {
    BSTR text;
    explicit Bstr(const wchar_t* s) : text(SysAllocString(s)) {}
    ~Bstr() { SysFreeString(text); }
    Bstr(const Bstr&) = delete;
    Bstr& operator=(const Bstr&) = delete;
};

// COM on this thread for one call. A thread that already runs COM in the
// other mode keeps it: the Task Scheduler works in both.
struct Com {
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ~Com() { if (SUCCEEDED(hr)) CoUninitialize(); }
    bool usable() const { return SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE; }
};

// The root folder of the local Task Scheduler.
HRESULT task_root(Microsoft::WRL::ComPtr<ITaskFolder>& root) {
    Microsoft::WRL::ComPtr<ITaskService> service;
    HRESULT hr = CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&service));
    if (FAILED(hr)) return hr;
    VARIANT none;
    VariantInit(&none);
    hr = service->Connect(none, none, none, none);
    if (FAILED(hr)) return hr;
    const Bstr path(L"\\");
    return service->GetFolder(path.text, &root);
}

}

long boot_task_create(const std::string& xml) {
    const Com com;
    if (!com.usable()) return com.hr;
    Microsoft::WRL::ComPtr<ITaskFolder> root, folder;
    HRESULT hr = task_root(root);
    if (FAILED(hr)) return hr;
    VARIANT none;
    VariantInit(&none);
    const Bstr folder_path(kTaskFolder);
    if (FAILED(root->GetFolder(folder_path.text, &folder))) {
        hr = root->CreateFolder(folder_path.text, none, &folder);
        if (FAILED(hr)) return hr;
    }
    // Who the task runs as, and with which rights, is in the XML.
    const Bstr name(kTaskName), text(widen(xml).c_str());
    Microsoft::WRL::ComPtr<IRegisteredTask> task;
    return folder->RegisterTask(name.text, text.text, TASK_CREATE_OR_UPDATE, none, none, TASK_LOGON_INTERACTIVE_TOKEN, none, &task);
}

long boot_task_remove() {
    const Com com;
    if (!com.usable()) return com.hr;
    Microsoft::WRL::ComPtr<ITaskFolder> root, folder;
    HRESULT hr = task_root(root);
    if (FAILED(hr)) return hr;
    const Bstr folder_path(kTaskFolder), name(kTaskName);
    hr = root->GetFolder(folder_path.text, &folder);
    if (FAILED(hr)) return hr;
    return folder->DeleteTask(name.text, 0);
}

bool boot_task_exists() {
    const Com com;
    if (!com.usable()) return false;
    Microsoft::WRL::ComPtr<ITaskFolder> root, folder;
    if (FAILED(task_root(root))) return false;
    const Bstr folder_path(kTaskFolder), name(kTaskName);
    if (FAILED(root->GetFolder(folder_path.text, &folder))) return false;
    Microsoft::WRL::ComPtr<IRegisteredTask> task;
    return SUCCEEDED(folder->GetTask(name.text, &task));
}

}
