#pragma once
#include <optional>
#include <string>
namespace gao {
struct AbLocation { std::string exePath; std::string profileCfgPath; int slot = 5; };
std::optional<AbLocation> FindAfterburner(int slot);
bool BackupProfile(const AbLocation& loc);
std::string ReadProfileText(const AbLocation& loc);
bool WriteProfileText(const AbLocation& loc, const std::string& text);
bool ApplyProfile(const AbLocation& loc);
}
