#pragma once
#include "core/types.hpp"
#include <string>

namespace gao {

class Nvml {
public:
    bool Init();
    ~Nvml();
    // -1 when NVML isn't initialized or the count could not be read (see
    // Error() for why); a genuine "no GPUs" result is 0. Never conflate the
    // two, for the same reason Telemetry::fan_pct never conflates "unknown"
    // with a real zero reading.
    int DeviceCount();
    Telemetry Read(unsigned index);
    const std::string& Error() const { return error_; }

private:
    void* lib_ = nullptr;
    bool inited_ = false;
    std::string error_;
};

}
