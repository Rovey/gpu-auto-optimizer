#pragma once
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace gao {

// Everything the app keeps lives in %LOCALAPPDATA%\GpuAutoOptimizer. Each
// path is empty when %LOCALAPPDATA% is not set.
std::filesystem::path app_dir();
std::filesystem::path journal_path();
std::filesystem::path config_path();
std::filesystem::path boot_log_path();

std::vector<std::string> read_lines(const std::filesystem::path& p);
std::optional<std::string> read_file(const std::filesystem::path& p);
// Appends line + '\n' and forces it to disk before returning, so the line
// survives a freeze that follows immediately after.
bool append_line_durable(const std::filesystem::path& p, const std::string& line);
// Replaces the file atomically: a crash mid-write leaves the old or the new
// content, never a mix (the boot strike counter depends on it).
bool write_file_atomic(const std::filesystem::path& p, const std::string& text);

}
