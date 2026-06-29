#include "core/vfcurve.hpp"
#include "core/vfcurve_codec.hpp"

namespace gao {

static constexpr size_t kVal = 8;     // hex chars per float
static constexpr size_t kPt  = 24;    // hex chars per point (3 floats)
static constexpr size_t kHdr = 24;    // header = 3 floats

VfCurve ParseVfCurve(std::string_view hex) {
    VfCurve c;
    if (hex.size() < kHdr) { c.header = std::string(hex); return c; }
    c.header = std::string(hex.substr(0, kHdr));
    size_t pos = kHdr;
    while (pos + kPt <= hex.size()) {
        CurvePoint p{
            Hex8ToFloat(hex.substr(pos,          kVal)),
            Hex8ToFloat(hex.substr(pos + kVal,   kVal)),
            Hex8ToFloat(hex.substr(pos + 2*kVal, kVal)),
        };
        if (p.voltage_mv == 0 && p.frequency_mhz == 0 && p.offset_mhz == 0) {
            c.ending = std::string(hex.substr(pos));   // includes the zero terminator
            return c;
        }
        c.points.push_back(p);
        pos += kPt;
    }
    c.ending = std::string(hex.substr(pos));
    return c;
}

std::string EncodeVfCurve(const VfCurve& c) {
    std::string out = c.header;
    for (const auto& p : c.points) {
        out += FloatToHex8(p.voltage_mv);
        out += FloatToHex8(p.frequency_mhz);
        out += FloatToHex8(p.offset_mhz);
    }
    out += c.ending;
    return out;
}

}
