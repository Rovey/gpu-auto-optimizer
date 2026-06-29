#pragma once
#include <optional>
#include <string>
#include <string_view>
namespace gao {
std::optional<std::string> ReadProfileKey(const std::string& cfgText, std::string_view key);
std::string SetProfileKey(const std::string& cfgText, std::string_view key, std::string_view value);
}
