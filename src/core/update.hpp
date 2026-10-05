#pragma once
#include <functional>
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

// What installing a release needs done. The order and every decision are
// install_release's; these only do. Each returns false (and says why) when
// its step failed.
struct UpdateSteps {
    std::function<bool(const std::string& url, std::string* error)> download;   // fetch the zip
    std::function<std::string()> sha256;                // of the fetched zip: 64 lower-case hex digits; empty when it cannot be read
    std::function<bool(std::string* error)> unpack;     // the fetched zip
    std::function<bool(unsigned version)> unpacked_is;  // do the unpacked executables carry this version (version_number())?
    std::function<bool(std::string* error)> replace;    // put the unpacked executables in place of the installed ones
};

// Installs a release: download, check the SHA-256, unpack, check the version,
// replace. Stops at the first step that does not hold; only `replace` changes
// what is installed. `message` describes the outcome either way.
bool install_release(const ReleaseInfo& release, const UpdateSteps& steps, std::string* message);

}
