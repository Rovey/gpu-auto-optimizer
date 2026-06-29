#include <doctest/doctest.h>
#include "core/ab_profile.hpp"

static const char* kCfg =
    "[Profile]\r\n"
    "Format=2\r\n"
    "PowerLimit=90\r\n"
    "CoreClkBoost=0\r\n"
    "VFCurve=ABCDEF0123\r\n";

TEST_CASE("reads a key value") {
    CHECK(gao::ReadProfileKey(kCfg, "PowerLimit").value() == "90");
    CHECK(gao::ReadProfileKey(kCfg, "VFCurve").value() == "ABCDEF0123");
    CHECK(!gao::ReadProfileKey(kCfg, "Missing").has_value());
}

TEST_CASE("sets a key value in place, preserving the rest") {
    std::string out = gao::SetProfileKey(kCfg, "PowerLimit", "75");
    CHECK(gao::ReadProfileKey(out, "PowerLimit").value() == "75");
    CHECK(gao::ReadProfileKey(out, "VFCurve").value() == "ABCDEF0123");
    CHECK(out.find("PowerLimit=75\r\n") != std::string::npos);
}

TEST_CASE("replaces a long VFCurve value") {
    std::string out = gao::SetProfileKey(kCfg, "VFCurve", "00112233");
    CHECK(gao::ReadProfileKey(out, "VFCurve").value() == "00112233");
}

TEST_CASE("absent key leaves text unchanged") {
    std::string out = gao::SetProfileKey(kCfg, "Missing", "1");
    CHECK(out == std::string(kCfg));
}
