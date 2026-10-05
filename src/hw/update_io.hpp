#pragma once
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>

namespace gao {

// What an update needs from Windows: an HTTPS download, a hash, an unzip and
// a file's version. Nothing here decides anything; see app/common.

// GET of an https:// URL (redirects are followed, but never down to http).
// The answer must be 200 and at most max_bytes. Nothing + *error otherwise.
std::optional<std::string> https_get(const std::string& url, std::size_t max_bytes, std::string* error);
// The same, written to a file. A failed download leaves no file behind.
bool https_download(const std::string& url, const std::filesystem::path& to, std::size_t max_bytes, std::string* error);

// SHA-256 of a file as 64 lower-case hex digits; empty when it cannot be read.
std::string sha256_hex(const std::filesystem::path& file);

// Unpacks a zip with the tar.exe that ships in System32 (Windows 10 1803 and
// later). `into` must exist.
bool extract_zip(const std::filesystem::path& zip, const std::filesystem::path& into, std::string* error);

// The version in an executable's version resource, as version_number() packs
// it. Nothing when the file has none.
std::optional<unsigned> file_version_number(const std::filesystem::path& exe);

}
