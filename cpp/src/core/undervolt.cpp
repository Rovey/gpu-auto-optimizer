#include "core/undervolt.hpp"

namespace gao {

VfCurve ApplyUndervolt(const VfCurve& base, float target_voltage_mv, float target_freq_mhz) {
    VfCurve c = base;
    for (auto& p : c.points) {
        if (p.voltage_mv + 0.5f >= target_voltage_mv) {
            p.offset_mhz = target_freq_mhz - p.frequency_mhz;
        }
    }
    return c;
}

}
