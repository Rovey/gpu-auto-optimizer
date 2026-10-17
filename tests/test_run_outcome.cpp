#include "doctest/doctest.h"

#include "app/common.hpp"

using gao::app::AllInOneOutcome;
using gao::app::driver_resets;
using gao::app::OptimizeOutcome;
using gao::app::UndervoltOutcome;

TEST_CASE("a run's driver resets are counted over its steps") {
    OptimizeOutcome overclock;
    overclock.ran = true;
    overclock.result.driver_resets = 1;
    CHECK(driver_resets(overclock) == 1);

    UndervoltOutcome undervolt;
    undervolt.ran = true;
    undervolt.result.driver_resets = 1;
    CHECK(driver_resets(undervolt) == 1);

    AllInOneOutcome all;
    all.overclock = overclock;
    CHECK(driver_resets(all) == 1);
    all.undervolt = undervolt;
    CHECK(driver_resets(all) == 2);
}

TEST_CASE("a step that did not run counts no driver reset") {
    OptimizeOutcome overclock;
    overclock.result.driver_resets = 1;   // never valid when the step did not run
    CHECK(driver_resets(overclock) == 0);

    UndervoltOutcome undervolt;
    undervolt.result.driver_resets = 1;
    CHECK(driver_resets(undervolt) == 0);

    CHECK(driver_resets(AllInOneOutcome{}) == 0);
}
