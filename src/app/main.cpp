#include "core/types.hpp"
#include "core/version.hpp"
#include "hw/nvml.hpp"
#include <cstdio>
#include <cstring>

static int probe() {
    gao::Nvml nvml;
    if (!nvml.Init()) {
        std::printf("NVML init failed: %s\n", nvml.Error().c_str());
        return 1;
    }
    const int count = nvml.DeviceCount();
    if (count < 0) {
        std::printf("NVML device count failed: %s\n", nvml.Error().c_str());
        return 1;
    }
    std::printf("GPUs: %d\n", count);
    for (int i = 0; i < count; ++i) {
        const gao::Telemetry t = nvml.Read(static_cast<unsigned>(i));
        if (!t.ok) { std::printf("  [%d] read failed\n", i); continue; }
        // fan_pct is -1 when the driver didn't report it; print that as an
        // unambiguous "n/a" rather than a number that looks like a reading.
        char fan[8];
        if (t.fan_pct < 0) std::snprintf(fan, sizeof(fan), "n/a");
        else std::snprintf(fan, sizeof(fan), "%d%%", t.fan_pct);
        std::printf("  [%d] core=%d MHz  mem=%d MHz  temp=%d C  fan=%s  power=%d/%d W\n",
                    i, t.core_mhz, t.mem_mhz, t.temp_c, fan, t.power_w, t.power_limit_w);
    }
    return 0;
}

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--version") == 0) {
        std::printf("%s %s\n", gao::kProductName.data(), gao::kVersion.data());
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "--probe") == 0) return probe();
    std::printf("usage: gao [--version | --probe]\n");
    return argc > 1 ? 1 : 0;
}
