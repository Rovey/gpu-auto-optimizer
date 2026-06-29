#pragma once
#include "core/vfcurve.hpp"
namespace gao {
VfCurve ApplyUndervolt(const VfCurve& base, float target_voltage_mv, float target_freq_mhz);
}
