#include <doctest/doctest.h>
#include "core/version.hpp"
#include <string>

TEST_CASE("version string is set") {
    CHECK(std::string(gao::kVersion) == "0.1.0");
    CHECK(std::string(gao::VersionString()) == "0.1.0");
}
