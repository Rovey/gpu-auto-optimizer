#pragma once
#include <string>
namespace gao {
struct GpuTelemetry {
    unsigned coreClockMhz = 0, memClockMhz = 0, tempC = 0, powerW = 0, powerLimitW = 0, utilPct = 0;
    unsigned long long eccErrors = 0;
    bool ok = false;
    std::string error;
};
class Nvml {
public:
    bool Init();
    GpuTelemetry Read(unsigned index);
    ~Nvml();
private:
    void* lib_ = nullptr;
    bool inited_ = false;
};
}
