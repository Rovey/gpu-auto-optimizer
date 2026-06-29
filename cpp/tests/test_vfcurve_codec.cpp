#include <doctest/doctest.h>
#include "core/vfcurve_codec.hpp"
#include <cctype>

TEST_CASE("float hex codec round-trips") {
    for (float v : {0.0f, 1.0f, 1000.0f, 1050.5f, -150.0f, 2730.0f}) {
        std::string h = gao::FloatToHex8(v);
        CHECK(h.size() == 8);
        CHECK(gao::Hex8ToFloat(h) == doctest::Approx(v));
    }
}

TEST_CASE("hex is 8 uppercase chars") {
    std::string h = gao::FloatToHex8(1050.0f);
    CHECK(h.size() == 8);
    for (char c : h) CHECK((std::isdigit((unsigned char)c) || (c >= 'A' && c <= 'F')));
}
