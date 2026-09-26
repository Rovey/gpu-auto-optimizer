#include "hw/journal_file.hpp"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdlib>
#include <fstream>

namespace gao {

std::filesystem::path journal_path() {
    const wchar_t* base = _wgetenv(L"LOCALAPPDATA");
    if (!base || !*base) return {};
    return std::filesystem::path(base) / L"GpuAutoOptimizer" / L"journal.jsonl";
}

std::vector<std::string> read_lines(const std::filesystem::path& p) {
    std::vector<std::string> lines;
    std::ifstream in(p, std::ios::binary);
    for (std::string line; std::getline(in, line);) lines.push_back(line);
    return lines;
}

bool append_line_durable(const std::filesystem::path& p, const std::string& line) {
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    const HANDLE h = CreateFileW(p.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    const std::string data = line + "\n";
    DWORD written = 0;
    const bool ok = WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) &&
                    written == data.size() && FlushFileBuffers(h);
    CloseHandle(h);
    return ok;
}

}
