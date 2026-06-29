#include "hw/afterburner.hpp"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <fstream>
#include <sstream>
#include <filesystem>

namespace fs = std::filesystem;

namespace gao {

std::optional<AbLocation> FindAfterburner(int slot) {
    AbLocation loc; loc.slot = slot;
    const char* base = "C:\\Program Files (x86)\\MSI Afterburner";
    std::string exe = std::string(base) + "\\MSIAfterburner.exe";
    if (!fs::exists(exe)) return std::nullopt;
    loc.exePath = exe;
    fs::path profiles = fs::path(base) / "Profiles";
    if (fs::exists(profiles)) {
        for (auto& e : fs::directory_iterator(profiles)) {
            auto name = e.path().filename().string();
            if (name.find("DEV_2786") != std::string::npos && e.path().extension() == ".cfg") {
                loc.profileCfgPath = e.path().string();
                break;
            }
        }
    }
    if (loc.profileCfgPath.empty()) return std::nullopt;
    return loc;
}

bool BackupProfile(const AbLocation& loc) {
    std::string bak = loc.profileCfgPath + ".gao-backup";
    if (fs::exists(bak)) return true;
    std::error_code ec;
    fs::copy_file(loc.profileCfgPath, bak, ec);
    return !ec;
}

std::string ReadProfileText(const AbLocation& loc) {
    std::ifstream in(loc.profileCfgPath, std::ios::binary);
    std::ostringstream ss; ss << in.rdbuf();
    return ss.str();
}

bool WriteProfileText(const AbLocation& loc, const std::string& text) {
    std::ofstream out(loc.profileCfgPath, std::ios::binary | std::ios::trunc);
    out << text;
    return out.good();
}

bool ApplyProfile(const AbLocation& loc) {
    std::string cmd = "\"" + loc.exePath + "\" -Profile" + std::to_string(loc.slot);
    STARTUPINFOA si{ sizeof(si) };
    PROCESS_INFORMATION pi{};
    std::string mutableCmd = cmd;
    BOOL ok = CreateProcessA(nullptr, mutableCmd.data(), nullptr, nullptr, FALSE,
                             0, nullptr, nullptr, &si, &pi);
    if (!ok) return false;
    WaitForSingleObject(pi.hProcess, 4000);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

}
