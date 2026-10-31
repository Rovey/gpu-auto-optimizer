#include "doctest/doctest.h"

#include "app/gui/ui.hpp"
#include "core/boot.hpp"
#include "core/search.hpp"
#include <string>

using gao::app::is_refusal;
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

TEST_CASE("a refusal is told apart from a fault") {
    // Something on the PC the user can change: the window leaves run.log out.
    CHECK(is_refusal(gao::kSeveralGpusRefusal));
    CHECK(is_refusal(gao::other_tool_refusal({"MSIAfterburner.exe"})));
    CHECK(is_refusal(gao::other_tool_refusal({"RTSS.exe"})));
    CHECK(is_refusal(gao::app::kAutoTuningRefusal));
    CHECK(is_refusal(gao::app::kAlreadyRunning));
    // A fault: run.log stays, because a bug report needs it.
    CHECK_FALSE(is_refusal("NVML init failed: NVML_ERROR_DRIVER_NOT_LOADED"));
    CHECK_FALSE(is_refusal("stress init failed: no NVIDIA adapter found"));
    CHECK_FALSE(is_refusal("could not set the card to stock"));
    CHECK_FALSE(is_refusal(""));
}
