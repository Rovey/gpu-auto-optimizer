#include "core/update.hpp"

#include <algorithm>
#include <charconv>
#include <nlohmann/json.hpp>

namespace gao {

namespace {
// A SHA-256 as GitHub writes it: exactly 64 lower-case hex digits.
bool is_sha256(std::string_view text) {
    return text.size() == 64 &&
           std::all_of(text.begin(), text.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}
}

std::optional<unsigned> version_number(std::string_view text) {
    unsigned parts[3] = {};
    const char* at = text.data();
    const char* const end = text.data() + text.size();
    for (int i = 0; i < 3; ++i) {
        if (at == end || *at < '0' || *at > '9') return std::nullopt;   // from_chars would accept nothing else, but say it
        const auto [next, ec] = std::from_chars(at, end, parts[i]);
        if (ec != std::errc() || parts[i] > 255) return std::nullopt;
        at = next;
        if (i < 2) {
            if (at == end || *at != '.') return std::nullopt;
            ++at;
        }
    }
    if (at != end) return std::nullopt;
    return (parts[0] << 16) | (parts[1] << 8) | parts[2];
}

std::optional<ReleaseInfo> parse_latest_release(const std::string& json) {
    const auto j = nlohmann::json::parse(json, nullptr, /*allow_exceptions=*/false);
    if (!j.is_object()) return std::nullopt;
    const auto text = [](const nlohmann::json& o, const char* key) -> std::string {
        const auto it = o.find(key);
        return it != o.end() && it->is_string() ? it->get<std::string>() : std::string();
    };
    const auto is_true = [](const nlohmann::json& o, const char* key) {
        const auto it = o.find(key);
        return it != o.end() && it->is_boolean() && it->get<bool>();
    };
    if (is_true(j, "draft") || is_true(j, "prerelease")) return std::nullopt;
    const std::string tag = text(j, "tag_name");
    if (tag.size() < 2 || tag[0] != 'v' || !version_number(std::string_view(tag).substr(1))) return std::nullopt;
    const auto assets = j.find("assets");
    if (assets == j.end() || !assets->is_array()) return std::nullopt;

    const std::string zip_name = "GpuAutoOptimizer-" + tag + "-win-x64.zip";
    const std::string url = std::string(kReleaseUrlPrefix) + tag + "/" + zip_name;
    for (const auto& a : *assets) {
        if (!a.is_object() || text(a, "name") != zip_name) continue;
        if (text(a, "browser_download_url") != url) return std::nullopt;
        const std::string digest = text(a, "digest");
        constexpr std::string_view kind = "sha256:";
        if (digest.compare(0, kind.size(), kind) != 0) return std::nullopt;
        const std::string hash = digest.substr(kind.size());
        if (!is_sha256(hash)) return std::nullopt;
        return ReleaseInfo{tag.substr(1), url, hash};
    }
    return std::nullopt;
}

bool install_release(const ReleaseInfo& release, const UpdateSteps& steps, std::string* message) {
    auto say = [&](const std::string& m, bool ok) { if (message) *message = m; return ok; };
    // parse_latest_release only hands out releases that pass these three, but
    // the install does not lean on its caller: without a real expectation the
    // comparison below proves nothing (an empty one equals the hash of a file
    // that cannot be read).
    const auto version = version_number(release.version);
    if (!version) return say("the release names no version; nothing was downloaded", false);
    if (release.zip_url.compare(0, kReleaseUrlPrefix.size(), kReleaseUrlPrefix) != 0)
        return say("the release is not from this project's downloads; nothing was downloaded", false);
    if (!is_sha256(release.sha256)) return say("the release has no usable SHA-256; nothing was downloaded", false);

    std::string why;
    if (!steps.download(release.zip_url, &why)) return say("the download failed: " + why, false);
    const std::string hash = steps.sha256();
    if (hash.empty()) return say("the download could not be read back to check its SHA-256; nothing was changed", false);
    if (hash != release.sha256) return say("the download does not match the published SHA-256; nothing was changed", false);
    if (!steps.unpack(&why)) return say(why, false);
    // The zip must be the version it claims to be.
    if (!steps.unpacked_is(*version))
        return say("the download does not contain version " + release.version + "; nothing was changed", false);
    if (!steps.replace(&why)) return say(why, false);
    return say("updated to version " + release.version, true);
}

}
