#pragma once
#include "core/types.hpp"
#include <string>

namespace gao {

class Nvml {
public:
    bool Init();
    ~Nvml();
    int DeviceCount();
    Telemetry Read(unsigned index);
    const std::string& Error() const { return error_; }

private:
    void* lib_ = nullptr;
    bool inited_ = false;
    std::string error_;
};

}
