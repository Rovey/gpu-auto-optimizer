#include "doctest/doctest.h"

#include "app/gui/ui.hpp"
#include "core/boot.hpp"
#include <string>

using gao::gui::split_link;

TEST_CASE("a reason's link is split off for a line of its own") {
    const auto [text, link] = split_link(std::string("Not applied. Open an issue at https://github.com/Rovey/gpu-auto-optimizer/issues"));
    CHECK(text == "Not applied. Open an issue at");
    CHECK(link == "https://github.com/Rovey/gpu-auto-optimizer/issues");
}

TEST_CASE("the several-GPU refusal keeps all of its text, with the link apart") {
    const auto [text, link] = split_link(gao::kSeveralGpusRefusal);
    CHECK(link == "https://github.com/Rovey/gpu-auto-optimizer/issues");
    CHECK(text + " " + link == gao::kSeveralGpusRefusal);
}

TEST_CASE("text after a link stays with the text") {
    const auto [text, link] = split_link(std::string("See https://example.com/a for more."));
    CHECK(text == "See for more.");
    CHECK(link == "https://example.com/a");
}

TEST_CASE("a reason without a link is left whole") {
    const auto [text, link] = split_link(std::string("MSI Afterburner is running"));
    CHECK(text == "MSI Afterburner is running");
    CHECK(link.empty());
}
