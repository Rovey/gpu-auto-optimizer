#pragma once
#include <optional>
#include <string>
#include <string_view>

namespace gao {

// "0.3.1" as one number that orders like the version. Nothing unless the text
// is exactly three numbers, each 0 to 255.
std::optional<unsigned> version_number(std::string_view text);

struct ReleaseInfo {
    std::string version;   // "0.3.1"
    std::string zip_url;   // the win-x64 zip of that release
    std::string sha256;    // of the zip, as GitHub computed it: 64 lower-case hex digits
};

// The only place an update is ever downloaded from.
inline constexpr std::string_view kReleaseUrlPrefix = "https://github.com/Rovey/gpu-auto-optimizer/releases/download/";

// Reads GitHub's answer to "the latest release of this project". Nothing
// unless it is a published release that names a version, the zip of that
// version under kReleaseUrlPrefix, and that zip's SHA-256.
std::optional<ReleaseInfo> parse_latest_release(const std::string& json);

}
