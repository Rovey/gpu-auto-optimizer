#include "doctest/doctest.h"

#include "app/gui/ui.hpp"
#include "core/boot.hpp"
#include "core/search.hpp"
#include "core/step_down.hpp"
#include <string>

using gao::app::is_refusal;
using gao::gui::split_link;

namespace {
// A tune without an undervolt: the clock grid does not matter for it.
gao::Profile clocks_tune() {
    gao::Profile p;
    p.preset = gao::Preset::BestOfMyGpu;
    p.power_pct = 110;
    p.core_mhz = 150;
    p.mem_mhz = 1000;
    p.driver = "617.14";
    p.gpu = "GPU-1";
    p.saved_at = "2026-11-05 20:00";
    return p;
}

// The minus sign (U+2212) of the -5 % button and its tip.
const std::string kMinus = "\xE2\x88\x92";
}

TEST_CASE("the step-down tip names the step and the tune it sets") {
    const gao::Profile p = clocks_tune();
    CHECK(gao::gui::step_down_tip(p, gao::kDefaultGridKhz) ==
          kMinus + "5 %: to -5 % (step 1) (" + gao::app::profile_text(gao::at_step(p, 1, gao::kDefaultGridKhz)) + ")");
    const gao::Profile s = gao::at_step(p, 2, gao::kDefaultGridKhz);
    CHECK(gao::gui::step_down_tip(s, gao::kDefaultGridKhz) ==
          kMinus + "5 %: to -15 % (step 3) (" + gao::app::profile_text(gao::at_step(p, 3, gao::kDefaultGridKhz)) + ")");
}

TEST_CASE("the step-down tip at the limit and for a step that changes nothing") {
    CHECK(gao::gui::step_down_tip(gao::at_step(clocks_tune(), gao::kMaxStep, gao::kDefaultGridKhz), gao::kDefaultGridKhz) ==
          "Lower than this: optimize again");
    gao::Profile flat;
    flat.preset = gao::Preset::Undervolt;
    flat.undervolt = gao::UndervoltTune{960000, 2932000, 0};
    CHECK(gao::gui::step_down_tip(flat, gao::kDefaultGridKhz) ==
          "This tune cannot be lowered further in 5 % steps; optimize again");
}

TEST_CASE("the step-up tip names where it goes back to") {
    const gao::Profile p = clocks_tune();
    const gao::Profile two = gao::at_step(p, 2, gao::kDefaultGridKhz);
    CHECK(gao::gui::step_up_tip(two, gao::kDefaultGridKhz) ==
          "+5 %: back to -5 % (step 1) (" + gao::app::profile_text(gao::at_step(p, 1, gao::kDefaultGridKhz)) + ")");
    const gao::Profile one = gao::at_step(p, 1, gao::kDefaultGridKhz);
    CHECK(gao::gui::step_up_tip(one, gao::kDefaultGridKhz) ==
          "+5 %: back to the tune the run found (" + gao::app::profile_text(p) + ")");
    gao::Profile failed = two;
    failed.step_down->failed_step = 1;
    CHECK(gao::gui::step_up_tip(failed, gao::kDefaultGridKhz) == "+5 %: -5 % (step 1) failed a test");
    CHECK(gao::gui::step_up_tip(p, gao::kDefaultGridKhz) == "This is the tune the run found");
}

TEST_CASE("the tray's step items name the step they go to") {
    const gao::Profile p = clocks_tune();
    CHECK(gao::gui::step_down_item(gao::at_step(p, 1, gao::kDefaultGridKhz), gao::kDefaultGridKhz) == "Lower tune 5 % (to -10 %)");
    CHECK(gao::gui::step_up_item(gao::at_step(p, 2, gao::kDefaultGridKhz)) == "Raise tune 5 % (back to -5 %)");
    CHECK(gao::gui::step_up_item(gao::at_step(p, 1, gao::kDefaultGridKhz)) == "Raise tune 5 % (back to the tune the run found)");
    CHECK(gao::gui::step_up_item(p) == "Raise tune 5 %");
    gao::Profile failed = gao::at_step(p, 2, gao::kDefaultGridKhz);
    failed.step_down->failed_step = 1;
    CHECK(gao::gui::step_up_item(failed) == "Raise tune 5 %");
    CHECK(gao::gui::step_down_item(gao::at_step(p, gao::kMaxStep, gao::kDefaultGridKhz), gao::kDefaultGridKhz) == "Lower tune 5 %");
}

TEST_CASE("the saved tune's step says whether it was tested") {
    const gao::Profile p = clocks_tune();
    CHECK(gao::gui::step_status(p).empty());
    gao::Profile s = gao::at_step(p, 2, gao::kDefaultGridKhz);
    CHECK(gao::gui::step_status(s) == "-10 % (step 2), not tested on its own; from a tested tune");
    s.step_down->tested = true;
    CHECK(gao::gui::step_status(s) == "-10 % (step 2), tested");
    gao::Profile failed = gao::at_step(p, 2, gao::kDefaultGridKhz);
    failed.step_down->failed_step = 2;
    CHECK(gao::gui::step_status(failed) == "-10 % (step 2), failed a test");
    failed.step_down->failed_step = 1;
    CHECK(gao::gui::step_status(failed) == "-10 % (step 2), not tested on its own; from a tested tune");
}

TEST_CASE("a tune that failed its test can be lowered, not raised or tested as it is") {
    gao::gui::UiState s;
    s.elevated = true;
    s.profile_driver_ok = s.profile_gpu_ok = true;
    s.profile = gao::at_step(clocks_tune(), 2, gao::kDefaultGridKhz);
    CHECK(gao::gui::test_step_shown(s));
    s.profile->step_down->failed_step = 2;
    CHECK_FALSE(gao::gui::test_step_shown(s));
    CHECK(gao::gui::step_usable(s));
    CHECK(gao::can_step_down(*s.profile, gao::kDefaultGridKhz));
    CHECK_FALSE(gao::can_step_up(*s.profile));
    // A lower step is a new tune: it no longer failed.
    CHECK_FALSE(gao::failed_its_test(gao::at_step(*s.profile, 3, gao::kDefaultGridKhz)));
    s.profile->step_down->failed_step.reset();
    s.profile->step_down->tested = true;
    CHECK_FALSE(gao::gui::test_step_shown(s));   // tested already
    s.profile = clocks_tune();
    CHECK_FALSE(gao::gui::test_step_shown(s));   // the tune the run found: "Test the tune" is the button for it
}

TEST_CASE("the step buttons are off during a run and when the saved tune does not apply here") {
    gao::gui::UiState s;
    s.elevated = true;
    CHECK_FALSE(gao::gui::step_usable(s));
    s.profile = clocks_tune();
    s.profile_driver_ok = s.profile_gpu_ok = true;
    CHECK(gao::gui::step_usable(s));
    s.run_active = true;
    CHECK_FALSE(gao::gui::step_usable(s));
    s.run_active = false;
    s.profile_driver_ok = false;
    CHECK_FALSE(gao::gui::step_usable(s));
    s.profile_driver_ok = true;
    s.profile_gpu_ok = false;
    CHECK_FALSE(gao::gui::step_usable(s));
    s.profile_gpu_ok = true;
    s.elevated = false;
    CHECK_FALSE(gao::gui::step_usable(s));
}

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
