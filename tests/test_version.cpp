#include "doctest/doctest.h"
#include "core/version.hpp"
#include <string>  // doctest forward-declares std::string; CHECK() below needs the real type

TEST_CASE("version string is the product name and a semver") {
    CHECK(gao::kProductName == std::string("gpu-auto-optimizer"));
    CHECK(gao::kVersion == std::string("0.4.0"));
}
