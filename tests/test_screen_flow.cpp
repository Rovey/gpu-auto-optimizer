#include "doctest/doctest.h"

#include "app/gui/ui.hpp"
#include <string>

using gao::gui::follow_run;
using gao::gui::OptimizeWorker;
using gao::gui::Screen;
using gao::gui::UiState;

namespace {
// A run that has ended without starting its search, as a refusal does.
OptimizeWorker::Snapshot refused(unsigned run_id, const std::string& why) {
    OptimizeWorker::Snapshot run;
    run.run_id = run_id;
    run.outcome = gao::app::OptimizeOutcome{};
    run.outcome->error = why;
    return run;
}
}

TEST_CASE("a run refused before any frame saw it running shows as not started") {
    UiState s;   // on the choose screen, no run yet
    const auto run = refused(1, "this PC has more than one NVIDIA GPU");
    CHECK(follow_run(s, run) == Screen::Results);
    CHECK_FALSE(run.ran());   // the results screen says "Not started" for it, with the reason
    CHECK(run.outcome->error == "this PC has more than one NVIDIA GPU");
}

TEST_CASE("the page leaves the screen alone until a run starts") {
    UiState s;
    CHECK(follow_run(s, OptimizeWorker::Snapshot{}) == Screen::Choose);
}

TEST_CASE("a run that is under way shows the run screen, then its results") {
    UiState s;
    OptimizeWorker::Snapshot run;
    run.run_id = 1;
    run.running = true;
    CHECK(follow_run(s, run) == Screen::Run);
    CHECK(follow_run(s, run) == Screen::Run);
    run.running = false;
    run.outcome = gao::app::OptimizeOutcome{};
    CHECK(follow_run(s, run) == Screen::Results);
}

TEST_CASE("after its results are closed, the same run does not come back") {
    UiState s;
    const auto run = refused(1, "MSI Afterburner is running");
    CHECK(follow_run(s, run) == Screen::Results);
    s.screen = Screen::Choose;   // Back on the results screen
    CHECK(follow_run(s, run) == Screen::Choose);
    // The next refused run is followed again.
    CHECK(follow_run(s, refused(2, "MSI Afterburner is running")) == Screen::Results);
}
