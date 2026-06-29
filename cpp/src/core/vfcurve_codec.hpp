#pragma once
#include <string>
#include <string_view>
namespace gao {
std::string FloatToHex8(float value);
float Hex8ToFloat(std::string_view hex8);
}
