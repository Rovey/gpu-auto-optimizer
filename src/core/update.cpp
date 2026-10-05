#include "core/update.hpp"

#include <algorithm>
#include <charconv>
#include <nlohmann/json.hpp>

namespace gao {

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
        if (digest.size() != kind.size() + 64 || digest.compare(0, kind.size(), kind) != 0) return std::nullopt;
        const std::string hash = digest.substr(kind.size());
        if (!std::all_of(hash.begin(), hash.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }))
            return std::nullopt;
        return ReleaseInfo{tag.substr(1), url, hash};
    }
    return std::nullopt;
}

}
