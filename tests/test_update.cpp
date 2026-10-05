#include "core/update.hpp"
#include "core/version.hpp"
#include <doctest/doctest.h>

using namespace gao;

namespace {
// The fields of GitHub's "latest release" answer that matter, as it sends them.
std::string answer(const std::string& tag, const std::string& zip_name, const std::string& url, const std::string& digest) {
    return R"({"tag_name":")" + tag + R"(","draft":false,"prerelease":false,"assets":[)"
           R"({"name":"notes.md","browser_download_url":"https://github.com/Rovey/gpu-auto-optimizer/releases/download/)" + tag +
           R"(/notes.md","digest":"sha256:1111111111111111111111111111111111111111111111111111111111111111"},)"
           R"({"name":")" + zip_name + R"(","browser_download_url":")" + url + R"(","digest":")" + digest + R"("}]})";
}
const std::string kHash = "7253e1d82926dd0a8e82c4d2bdf06556823eee3ffe8d47404603220369ba3803";
const std::string kZip = "GpuAutoOptimizer-v0.3.1-win-x64.zip";
const std::string kUrl = "https://github.com/Rovey/gpu-auto-optimizer/releases/download/v0.3.1/" + kZip;
}

TEST_CASE("version_number orders like the version") {
    CHECK(version_number("0.3.1") == ((0u << 16) | (3u << 8) | 1u));
    CHECK(*version_number("0.3.1") > *version_number("0.3.0"));
    CHECK(*version_number("0.10.0") > *version_number("0.9.9"));
    CHECK(*version_number("1.0.0") > *version_number("0.255.255"));
    CHECK(version_number(kVersion) == kVersionNumber);
}

TEST_CASE("version_number refuses anything that is not three small numbers") {
    for (const char* bad : {"", "0.3", "0.3.1.0", "v0.3.1", "0.3.x", "0.3.1-rc1", "0..1", "0.3.256", "-1.0.0", " 0.3.1", "0.3.1 "})
        CHECK_FALSE(version_number(bad).has_value());
}

TEST_CASE("parse_latest_release reads the version, the zip and its hash") {
    const auto r = parse_latest_release(answer("v0.3.1", kZip, kUrl, "sha256:" + kHash));
    REQUIRE(r.has_value());
    CHECK(r->version == "0.3.1");
    CHECK(r->zip_url == kUrl);
    CHECK(r->sha256 == kHash);
}

TEST_CASE("parse_latest_release refuses what an update must never be installed from") {
    // No hash, a hash of another kind or length, upper case (GitHub sends lower case).
    CHECK_FALSE(parse_latest_release(answer("v0.3.1", kZip, kUrl, "")).has_value());
    CHECK_FALSE(parse_latest_release(answer("v0.3.1", kZip, kUrl, "md5:" + kHash)).has_value());
    CHECK_FALSE(parse_latest_release(answer("v0.3.1", kZip, kUrl, "sha256:" + kHash.substr(1))).has_value());
    CHECK_FALSE(parse_latest_release(answer("v0.3.1", kZip, kUrl, "sha256:" + kHash.substr(1) + "G")).has_value());
    // A download from anywhere but this project's releases.
    CHECK_FALSE(parse_latest_release(answer("v0.3.1", kZip, "https://example.com/" + kZip, "sha256:" + kHash)).has_value());
    CHECK_FALSE(parse_latest_release(answer("v0.3.1", kZip, "http://github.com/Rovey/gpu-auto-optimizer/releases/download/v0.3.1/" + kZip,
                                            "sha256:" + kHash)).has_value());
    // No zip with the expected name for that version.
    CHECK_FALSE(parse_latest_release(answer("v0.3.1", "GpuAutoOptimizer-v0.3.0-win-x64.zip", kUrl, "sha256:" + kHash)).has_value());
    // A tag that is no version.
    CHECK_FALSE(parse_latest_release(answer("nightly", kZip, kUrl, "sha256:" + kHash)).has_value());
    // Not JSON, or JSON of another shape.
    CHECK_FALSE(parse_latest_release("").has_value());
    CHECK_FALSE(parse_latest_release("<html>rate limited</html>").has_value());
    CHECK_FALSE(parse_latest_release(R"({"message":"Not Found"})").has_value());
    CHECK_FALSE(parse_latest_release(R"({"tag_name":5,"assets":"none"})").has_value());
    CHECK_FALSE(parse_latest_release(R"([1,2,3])").has_value());
}

TEST_CASE("a draft or a prerelease is never an update") {
    std::string a = answer("v0.3.1", kZip, kUrl, "sha256:" + kHash);
    const auto swap = [](std::string s, const std::string& from, const std::string& to) {
        s.replace(s.find(from), from.size(), to);
        return s;
    };
    CHECK_FALSE(parse_latest_release(swap(a, R"("draft":false)", R"("draft":true)")).has_value());
    CHECK_FALSE(parse_latest_release(swap(a, R"("prerelease":false)", R"("prerelease":true)")).has_value());
}
