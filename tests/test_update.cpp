#include "core/update.hpp"
#include "core/version.hpp"
#include <doctest/doctest.h>
#include <string>
#include <vector>

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

// ---- installing: a fake source instead of the network -------------------

namespace {
const std::string kOtherHash = "0000000000000000000000000000000000000000000000000000000000000000";

// Stands in for GitHub, the disk and the two executables. `installed` is the
// version that runs: only the replace step may change it.
struct FakeSource {
    std::string serves_hash = kHash;   // what the downloaded file hashes to; empty: it cannot be read back
    bool download_ok = true, unpack_ok = true, replace_ok = true;
    unsigned unpacked_version = *version_number("0.3.1");
    std::string installed = "0.3.0";
    std::vector<std::string> calls;

    UpdateSteps steps() {
        UpdateSteps s;
        s.download = [this](const std::string& url, std::string* error) {
            calls.push_back("download " + url);
            if (!download_ok && error) *error = "no route";
            return download_ok;
        };
        s.sha256 = [this] { calls.push_back("sha256"); return serves_hash; };
        s.unpack = [this](std::string* error) {
            calls.push_back("unpack");
            if (!unpack_ok && error) *error = "bad zip";
            return unpack_ok;
        };
        s.unpacked_is = [this](unsigned version) { calls.push_back("version"); return version == unpacked_version; };
        s.replace = [this](std::string* error) {
            calls.push_back("replace");
            if (!replace_ok) { if (error) *error = "access denied"; return false; }
            installed = "0.3.1";
            return true;
        };
        return s;
    }
    bool called(const std::string& what) const {
        for (const auto& c : calls) if (c.compare(0, what.size(), what) == 0) return true;
        return false;
    }
};
const ReleaseInfo kRelease{"0.3.1", kUrl, kHash};
}

TEST_CASE("an update whose download matches the published SHA-256 is installed, in order") {
    FakeSource src;
    std::string message;
    CHECK(install_release(kRelease, src.steps(), &message));
    CHECK(src.calls == std::vector<std::string>{"download " + kUrl, "sha256", "unpack", "version", "replace"});
    CHECK(src.installed == "0.3.1");
    CHECK(message == "updated to version 0.3.1");
}

TEST_CASE("a download with the wrong SHA-256 is not installed; the old version stays") {
    FakeSource src;
    src.serves_hash = kOtherHash;   // the source serves something else than GitHub published
    std::string message;
    CHECK_FALSE(install_release(kRelease, src.steps(), &message));
    CHECK(src.installed == "0.3.0");
    // Nothing of the download is touched after the check: not unpacked, not copied.
    CHECK_FALSE(src.called("unpack"));
    CHECK_FALSE(src.called("version"));
    CHECK_FALSE(src.called("replace"));
    CHECK(message == "the download does not match the published SHA-256; nothing was changed");
}

TEST_CASE("one wrong digit in the SHA-256 is enough to refuse") {
    FakeSource src;
    src.serves_hash = kHash;
    src.serves_hash.back() = src.serves_hash.back() == '0' ? '1' : '0';
    CHECK_FALSE(install_release(kRelease, src.steps(), nullptr));
    CHECK(src.installed == "0.3.0");
    CHECK_FALSE(src.called("replace"));
}

TEST_CASE("a download that cannot be read back to hash it is not installed") {
    FakeSource src;
    src.serves_hash = "";   // the hash of an unreadable file
    std::string message;
    CHECK_FALSE(install_release(kRelease, src.steps(), &message));
    CHECK(src.installed == "0.3.0");
    CHECK_FALSE(src.called("unpack"));
    CHECK_FALSE(src.called("replace"));
    CHECK(message == "the download could not be read back to check its SHA-256; nothing was changed");
}

TEST_CASE("a release without a published SHA-256 is refused before anything is downloaded") {
    // parse_latest_release never produces one, but the install must not rely
    // on that: an empty expectation would equal the hash of an unreadable file.
    for (const std::string& published : {std::string(), kHash.substr(1), kHash + "0", "sha256:" + kHash,
                                         std::string(64, 'G'), std::string("7253E1D82926DD0A8E82C4D2BDF06556823EEE3FFE8D47404603220369BA3803")}) {
        FakeSource src;
        src.serves_hash = published;   // even a source that serves exactly that
        std::string message;
        CHECK_FALSE(install_release(ReleaseInfo{"0.3.1", kUrl, published}, src.steps(), &message));
        CHECK(src.installed == "0.3.0");
        CHECK(src.calls.empty());
        CHECK(message == "the release has no usable SHA-256; nothing was downloaded");
    }
}

TEST_CASE("a release that is not from this project's releases, or names no version, is refused before anything is downloaded") {
    for (const ReleaseInfo& bad : {ReleaseInfo{"0.3.1", "https://example.com/GpuAutoOptimizer-v0.3.1-win-x64.zip", kHash},
                                   ReleaseInfo{"0.3.1", "http://github.com/Rovey/gpu-auto-optimizer/releases/download/v0.3.1/x.zip", kHash},
                                   ReleaseInfo{"latest", kUrl, kHash}, ReleaseInfo{"", kUrl, kHash}}) {
        FakeSource src;
        CHECK_FALSE(install_release(bad, src.steps(), nullptr));
        CHECK(src.calls.empty());
        CHECK(src.installed == "0.3.0");
    }
}

TEST_CASE("a failed download, a bad zip or another version inside it: nothing is replaced") {
    {
        FakeSource src;
        src.download_ok = false;
        std::string message;
        CHECK_FALSE(install_release(kRelease, src.steps(), &message));
        CHECK(src.calls == std::vector<std::string>{"download " + kUrl});
        CHECK(message == "the download failed: no route");
    }
    {
        FakeSource src;
        src.unpack_ok = false;
        std::string message;
        CHECK_FALSE(install_release(kRelease, src.steps(), &message));
        CHECK_FALSE(src.called("replace"));
        CHECK(message == "bad zip");
    }
    {
        FakeSource src;
        src.unpacked_version = *version_number("0.3.0");   // the right hash, the wrong contents
        std::string message;
        CHECK_FALSE(install_release(kRelease, src.steps(), &message));
        CHECK_FALSE(src.called("replace"));
        CHECK(src.installed == "0.3.0");
        CHECK(message == "the download does not contain version 0.3.1; nothing was changed");
    }
}

TEST_CASE("a replace that fails is reported as it is") {
    FakeSource src;
    src.replace_ok = false;
    std::string message;
    CHECK_FALSE(install_release(kRelease, src.steps(), &message));
    CHECK(src.installed == "0.3.0");
    CHECK(message == "access denied");
}
