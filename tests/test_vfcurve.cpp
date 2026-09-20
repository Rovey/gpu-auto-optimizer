#include <doctest/doctest.h>
#include "core/vfcurve.hpp"
#include "core/vfcurve_codec.hpp"
#include <fstream>
#include <string>

static std::string MakeHex(float v, float f, float o) {
    return gao::FloatToHex8(v) + gao::FloatToHex8(f) + gao::FloatToHex8(o);
}

TEST_CASE("parse + encode round-trips a synthetic curve") {
    std::string header = gao::FloatToHex8(1) + gao::FloatToHex8(2) + gao::FloatToHex8(3);
    std::string body = MakeHex(700, 300, 0) + MakeHex(800, 1500, 0);
    std::string terminator = MakeHex(0, 0, 0);
    std::string ending = "DEADBEEF";
    std::string full = header + body + terminator + ending;

    gao::VfCurve c = gao::ParseVfCurve(full);
    CHECK(c.points.size() == 2);
    CHECK(c.points[0].voltage_mv == doctest::Approx(700));
    CHECK(c.points[1].frequency_mhz == doctest::Approx(1500));
    CHECK(gao::EncodeVfCurve(c) == full);
}

// Real-profile byte-for-byte round-trip. The fixture file does not exist yet
// (it comes from a manual Afterburner-setup task). GUARD: skip cleanly when absent
// so the suite stays green now and this test activates once the fixture is added.
TEST_CASE("round-trips the real Afterburner VFCurve fixture") {
    std::ifstream in("tests/fixtures/profile_sample.cfg");
    if (!in.good()) { MESSAGE("fixture profile_sample.cfg absent - skipping (add it after Afterburner setup)"); return; }
    std::string line, vfcurve;
    while (std::getline(in, line)) {
        if (line.rfind("VFCurve=", 0) == 0) { vfcurve = line.substr(8); break; }
    }
    while (!vfcurve.empty() && (vfcurve.back() == '\r' || vfcurve.back() == '\n')) vfcurve.pop_back();
    if (vfcurve.empty()) { MESSAGE("no VFCurve= line in fixture - skipping"); return; }

    gao::VfCurve c = gao::ParseVfCurve(vfcurve);
    CHECK(c.points.size() > 0);
    CHECK(gao::EncodeVfCurve(c) == vfcurve);   // byte-for-byte
}
