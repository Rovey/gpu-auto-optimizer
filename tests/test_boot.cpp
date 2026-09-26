#include "doctest/doctest.h"
#include "core/boot.hpp"
#include <string>

using namespace gao;

namespace {
Config with_profile(const std::string& driver, int strikes = 0) {
    Config c;
    Profile p;
    p.power_pct = 105;
    p.core_mhz = 135;
    p.mem_mhz = 1050;
    p.driver = driver;
    c.profile = p;
    c.boot_strikes = strikes;
    return c;
}

struct Card {
    int power = 100, core = 0, mem = 0;
    bool fail_mem = false;
    GpuControl gpu(bool with_power = true) {
        GpuControl g;
        g.set_core_offset = [this](int v) { core = v; return true; };
        g.set_mem_offset = [this](int v) { if (fail_mem) return false; mem = v; return true; };
        g.reset_to_stock = [this] { power = 100; core = 0; mem = 0; return true; };
        if (with_power) g.set_power_limit = [this](int p) { power = p; return true; };
        return g;
    }
};
}

TEST_CASE("decide_boot: each outcome") {
    CHECK(decide_boot(Config{}, "610.74") == BootDecision::NoProfile);
    CHECK(decide_boot(with_profile("610.74", 3), "610.74") == BootDecision::TooManyStrikes);
    CHECK(decide_boot(with_profile("610.74"), "615.20") == BootDecision::DriverChanged);
    CHECK(decide_boot(with_profile("610.74", 2), "610.74") == BootDecision::Apply);
}

TEST_CASE("decide_boot: strikes are checked before the driver") {
    CHECK(decide_boot(with_profile("610.74", 5), "615.20") == BootDecision::TooManyStrikes);
}

TEST_CASE("an unknown driver never matches") {
    CHECK(decide_boot(with_profile("610.74"), "") == BootDecision::DriverChanged);
    CHECK(decide_boot(with_profile(""), "") == BootDecision::DriverChanged);
}

TEST_CASE("apply_profile sets power, core and memory") {
    Card card;
    std::string why;
    CHECK(apply_profile(card.gpu(), *with_profile("x").profile, &why));
    CHECK(card.power == 105);
    CHECK(card.core == 135);
    CHECK(card.mem == 1050);
}

TEST_CASE("a failing setter resets to stock and names the setter") {
    Card card;
    card.fail_mem = true;
    std::string why;
    CHECK_FALSE(apply_profile(card.gpu(), *with_profile("x").profile, &why));
    CHECK(why.find("mem") != std::string::npos);
    CHECK(card.power == 100);
    CHECK(card.core == 0);
}

TEST_CASE("power other than 100 % without power control fails and resets") {
    Card card;
    std::string why;
    CHECK_FALSE(apply_profile(card.gpu(/*with_power=*/false), *with_profile("x").profile, &why));
    CHECK(why.find("power") != std::string::npos);
    CHECK(card.core == 0);

    Profile p = *with_profile("x").profile;
    p.power_pct = 100;
    CHECK(apply_profile(card.gpu(false), p, &why));
    CHECK(card.core == 135);
}
