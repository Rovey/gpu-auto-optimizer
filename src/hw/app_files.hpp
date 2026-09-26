#pragma once
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace gao {

// Everything the app keeps lives in %ProgramData%\GpuAutoOptimizer: readable
// by users, writable only by administrators (see ensure_app_dir). Each path
// is empty when the known folder cannot be resolved.
std::filesystem::path app_dir();
std::filesystem::path journal_path();
std::filesystem::path config_path();
std::filesystem::path boot_log_path();
// %ProgramFiles%, resolved from the registry (never the environment).
std::filesystem::path program_files_dir();

// Creates the folder with a protected DACL (SYSTEM + Administrators full,
// Users read), or verifies an existing one is owned by Administrators or
// SYSTEM and re-applies that DACL. Every elevated writer calls this first:
// a folder a user created in advance could hold files the elevated process
// would trust. false + *why when the folder cannot be trusted.
bool ensure_app_dir(std::string* why);

std::vector<std::string> read_lines(const std::filesystem::path& p);
std::optional<std::string> read_file(const std::filesystem::path& p);
// Appends line + '\n' and forces it to disk before returning, so the line
// survives a freeze that follows immediately after.
bool append_line_durable(const std::filesystem::path& p, const std::string& line);
// Replaces the file atomically: a crash mid-write leaves the old or the new
// content, never a mix (the boot strike counter depends on it).
bool write_file_atomic(const std::filesystem::path& p, const std::string& text);

}
