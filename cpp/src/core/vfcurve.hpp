#pragma once
#include <string>
#include <string_view>
#include <vector>
namespace gao {
struct CurvePoint { float voltage_mv; float frequency_mhz; float offset_mhz; };
struct VfCurve { std::string header; std::vector<CurvePoint> points; std::string ending; };
VfCurve ParseVfCurve(std::string_view hex);
std::string EncodeVfCurve(const VfCurve& curve);
}
