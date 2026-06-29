#include "hw/nvml.hpp"
#include <cstdio>

int main() {
    gao::Nvml nvml;
    if (!nvml.Init()) { std::printf("NVML init FAILED\n"); return 1; }
    auto t = nvml.Read(0);
    if (!t.ok) { std::printf("read FAILED: %s\n", t.error.c_str()); return 2; }
    std::printf("core=%u MHz  mem=%u MHz  temp=%u C  power=%u W  limit=%u W  util=%u%%\n",
                t.coreClockMhz, t.memClockMhz, t.tempC, t.powerW, t.powerLimitW, t.utilPct);
    return 0;
}
