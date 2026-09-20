#include "hw/nvml.hpp"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace gao {

using nvmlReturn_t = int;
static constexpr nvmlReturn_t NVML_SUCCESS = 0;
using nvmlDevice_t = void*;

typedef nvmlReturn_t (*fn_init)();
typedef nvmlReturn_t (*fn_shutdown)();
typedef nvmlReturn_t (*fn_count)(unsigned*);
typedef nvmlReturn_t (*fn_byIndex)(unsigned, nvmlDevice_t*);
typedef nvmlReturn_t (*fn_clock)(nvmlDevice_t, int type, unsigned*);
typedef nvmlReturn_t (*fn_temp)(nvmlDevice_t, int sensor, unsigned*);
typedef nvmlReturn_t (*fn_power)(nvmlDevice_t, unsigned*);
typedef nvmlReturn_t (*fn_powerlimit)(nvmlDevice_t, unsigned*);
typedef nvmlReturn_t (*fn_fan)(nvmlDevice_t, unsigned*);

static fn_init        p_init = nullptr;
static fn_shutdown    p_shutdown = nullptr;
static fn_count       p_count = nullptr;
static fn_byIndex     p_byIndex = nullptr;
static fn_clock       p_clock = nullptr;
static fn_temp        p_temp = nullptr;
static fn_power       p_power = nullptr;
static fn_powerlimit  p_powerlimit = nullptr;
static fn_fan         p_fan = nullptr;

bool Nvml::Init() {
    HMODULE h = LoadLibraryA("nvml.dll");
    if (!h) h = LoadLibraryA("C:\\Windows\\System32\\nvml.dll");
    if (!h) { error_ = "could not load nvml.dll"; return false; }
    lib_ = h;
    p_init       = (fn_init)GetProcAddress(h, "nvmlInit_v2");
    p_shutdown   = (fn_shutdown)GetProcAddress(h, "nvmlShutdown");
    p_count      = (fn_count)GetProcAddress(h, "nvmlDeviceGetCount_v2");
    p_byIndex    = (fn_byIndex)GetProcAddress(h, "nvmlDeviceGetHandleByIndex_v2");
    p_clock      = (fn_clock)GetProcAddress(h, "nvmlDeviceGetClockInfo");
    p_temp       = (fn_temp)GetProcAddress(h, "nvmlDeviceGetTemperature");
    p_power      = (fn_power)GetProcAddress(h, "nvmlDeviceGetPowerUsage");
    p_powerlimit = (fn_powerlimit)GetProcAddress(h, "nvmlDeviceGetPowerManagementLimit");
    p_fan        = (fn_fan)GetProcAddress(h, "nvmlDeviceGetFanSpeed");
    if (!p_init || !p_byIndex) { error_ = "required NVML entry points not found"; return false; }
    inited_ = (p_init() == NVML_SUCCESS);
    if (!inited_) error_ = "nvmlInit_v2 failed";
    return inited_;
}

int Nvml::DeviceCount() {
    if (!inited_ || !p_count) return 0;
    unsigned n = 0;
    if (p_count(&n) != NVML_SUCCESS) return 0;
    return static_cast<int>(n);
}

Telemetry Nvml::Read(unsigned index) {
    Telemetry t;
    if (!inited_) return t;
    nvmlDevice_t dev = nullptr;
    if (p_byIndex(index, &dev) != NVML_SUCCESS) return t;
    unsigned v = 0;
    if (p_clock && p_clock(dev, /*GRAPHICS*/0, &v) == NVML_SUCCESS) t.core_mhz = static_cast<int>(v);
    if (p_clock && p_clock(dev, /*MEM*/2, &v) == NVML_SUCCESS)      t.mem_mhz = static_cast<int>(v);
    if (p_temp && p_temp(dev, /*GPU*/0, &v) == NVML_SUCCESS)        t.temp_c = static_cast<int>(v);
    if (p_power && p_power(dev, &v) == NVML_SUCCESS)                t.power_w = static_cast<int>(v / 1000);
    if (p_powerlimit && p_powerlimit(dev, &v) == NVML_SUCCESS)      t.power_limit_w = static_cast<int>(v / 1000);
    // fan_pct stays -1 (its default) unless the driver actually reports a
    // reading: a missing symbol or a non-zero status must never be read as 0,
    // since 0 is itself a real fan speed (e.g. an idle card with the fan off).
    if (p_fan && p_fan(dev, &v) == NVML_SUCCESS) t.fan_pct = static_cast<int>(v);
    t.ok = true;
    return t;
}

Nvml::~Nvml() {
    if (inited_ && p_shutdown) p_shutdown();
    if (lib_) FreeLibrary((HMODULE)lib_);
}

}
