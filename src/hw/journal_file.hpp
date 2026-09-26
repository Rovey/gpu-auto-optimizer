#pragma once
#include <filesystem>
#include <string>
#include <vector>

namespace gao {

// %LOCALAPPDATA%\GpuAutoOptimizer\journal.jsonl, or empty when the variable
// is not set.
std::filesystem::path journal_path();
std::vector<std::string> read_lines(const std::filesystem::path& p);
// Appends line + '\n' and forces it to disk (FlushFileBuffers) before
// returning, so the line survives a freeze that follows immediately after.
bool append_line_durable(const std::filesystem::path& p, const std::string& line);

}
