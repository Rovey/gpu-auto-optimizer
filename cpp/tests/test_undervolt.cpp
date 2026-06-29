#include <doctest/doctest.h>
#include "core/undervolt.hpp"

static gao::VfCurve BaseCurve() {
    gao::VfCurve c;
    c.header = "000000000000000000000000";
    c.points = {
        {700.f,  2400.f, 0.f},
        {800.f,  2600.f, 0.f},
        {900.f,  2800.f, 0.f},
        {1000.f, 2950.f, 0.f},
        {1050.f, 3000.f, 0.f},
    };
    c.ending = "000000000000000000000000";
    return c;
}

TEST_CASE("undervolt locks target point to target freq") {
    auto c = gao::ApplyUndervolt(BaseCurve(), 900.f, 2850.f);
    CHECK((c.points[2].frequency_mhz + c.points[2].offset_mhz) == doctest::Approx(2850.f));
}

TEST_CASE("undervolt flattens points above target voltage") {
    auto c = gao::ApplyUndervolt(BaseCurve(), 900.f, 2850.f);
    for (const auto& p : c.points) {
        if (p.voltage_mv > 900.f)
            CHECK((p.frequency_mhz + p.offset_mhz) == doctest::Approx(2850.f));
    }
}

TEST_CASE("undervolt leaves points below target unchanged") {
    auto c = gao::ApplyUndervolt(BaseCurve(), 900.f, 2850.f);
    CHECK(c.points[0].offset_mhz == doctest::Approx(0.f));
    CHECK(c.points[1].offset_mhz == doctest::Approx(0.f));
}
