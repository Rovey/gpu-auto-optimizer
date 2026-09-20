#include "core/vfcurve_codec.hpp"
#include <cstdint>
#include <cstring>
#include <cstdio>

namespace gao {

std::string FloatToHex8(float value) {
    uint8_t bytes[4];
    std::memcpy(bytes, &value, 4);              // little-endian memory order on x64
    char out[9];
    std::snprintf(out, sizeof(out), "%02X%02X%02X%02X",
                  bytes[0], bytes[1], bytes[2], bytes[3]);
    return std::string(out, 8);
}

float Hex8ToFloat(std::string_view hex8) {
    uint8_t bytes[4]{};
    for (int i = 0; i < 4; ++i) {
        auto hexNibble = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return 0;
        };
        bytes[i] = static_cast<uint8_t>(hexNibble(hex8[i*2]) * 16 + hexNibble(hex8[i*2+1]));
    }
    float value;
    std::memcpy(&value, bytes, 4);
    return value;
}

}
