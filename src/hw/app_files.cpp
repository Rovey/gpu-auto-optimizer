#include "hw/app_files.hpp"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace gao {

std::filesystem::path app_dir() {
    const wchar_t* base = _wgetenv(L"LOCALAPPDATA");
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
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    const HANDLE h = CreateFileW(p.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    const bool ok = write_all(h, line + "\n");
    CloseHandle(h);
    return ok;
}

bool write_file_atomic(const std::filesystem::path& p, const std::string& text) {
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    std::filesystem::path tmp = p;
    tmp += L".tmp";
    const HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    const bool ok = write_all(h, text);
    CloseHandle(h);
    return ok && MoveFileExW(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

}
