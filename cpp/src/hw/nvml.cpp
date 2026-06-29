#include "hw/nvml.hpp"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace gao {

using nvmlReturn_t = int;
static constexpr nvmlReturn_t NVML_SUCCESS = 0;
using nvmlDevice_t = void*;

typedef nvmlReturn_t (*fn_init)();
typedef nvmlReturn_t (*fn_shutdown)();
typedef nvmlReturn_t (*fn_byIndex)(unsigned, nvmlDevice_t*);
typedef nvmlReturn_t (*fn_clock)(nvmlDevice_t, int type, unsigned*);
typedef nvmlReturn_t (*fn_temp)(nvmlDevice_t, int sensor, unsigned*);
typedef nvmlReturn_t (*fn_power)(nvmlDevice_t, unsigned*);
typedef nvmlReturn_t (*fn_powerlimit)(nvmlDevice_t, unsigned*);
typedef struct { unsigned gpu; unsigned memory; } nvmlUtilization_t;
typedef nvmlReturn_t (*fn_util)(nvmlDevice_t, nvmlUtilization_t*);

static fn_init        p_init = nullptr;
static fn_shutdown    p_shutdown = nullptr;
static fn_byIndex     p_byIndex = nullptr;
static fn_clock       p_clock = nullptr;
static fn_temp        p_temp = nullptr;
static fn_power       p_power = nullptr;
static fn_powerlimit  p_powerlimit = nullptr;
static fn_util        p_util = nullptr;

bool Nvml::Init() {
    HMODULE h = LoadLibraryA("nvml.dll");
    if (!h) h = LoadLibraryA("C:\\Windows\\System32\\nvml.dll");
    if (!h) return false;
    lib_ = h;
    p_init       = (fn_init)GetProcAddress(h, "nvmlInit_v2");
    p_shutdown   = (fn_shutdown)GetProcAddress(h, "nvmlShutdown");
    p_byIndex    = (fn_byIndex)GetProcAddress(h, "nvmlDeviceGetHandleByIndex_v2");
    p_clock      = (fn_clock)GetProcAddress(h, "nvmlDeviceGetClockInfo");
    p_temp       = (fn_temp)GetProcAddress(h, "nvmlDeviceGetTemperature");
    p_power      = (fn_power)GetProcAddress(h, "nvmlDeviceGetPowerUsage");
    p_powerlimit = (fn_powerlimit)GetProcAddress(h, "nvmlDeviceGetPowerManagementLimit");
    p_util       = (fn_util)GetProcAddress(h, "nvmlDeviceGetUtilizationRates");
    if (!p_init || !p_byIndex) return false;
    inited_ = (p_init() == NVML_SUCCESS);
    return inited_;
}

GpuTelemetry Nvml::Read(unsigned index) {
    GpuTelemetry t;
    if (!inited_) { t.error = "NVML not initialized"; return t; }
    nvmlDevice_t dev = nullptr;
    if (p_byIndex(index, &dev) != NVML_SUCCESS) { t.error = "GetHandleByIndex failed"; return t; }
    unsigned v = 0;
    if (p_clock && p_clock(dev, /*GRAPHICS*/0, &v) == NVML_SUCCESS) t.coreClockMhz = v;
    if (p_clock && p_clock(dev, /*MEM*/2, &v) == NVML_SUCCESS)      t.memClockMhz = v;
    if (p_temp && p_temp(dev, /*GPU*/0, &v) == NVML_SUCCESS)        t.tempC = v;
    if (p_power && p_power(dev, &v) == NVML_SUCCESS)                t.powerW = v / 1000;
    if (p_powerlimit && p_powerlimit(dev, &v) == NVML_SUCCESS)      t.powerLimitW = v / 1000;
    nvmlUtilization_t u{};
    if (p_util && p_util(dev, &u) == NVML_SUCCESS)                  t.utilPct = u.gpu;
    t.ok = true;
    return t;
}

Nvml::~Nvml() {
    if (inited_ && p_shutdown) p_shutdown();
    if (lib_) FreeLibrary((HMODULE)lib_);
}

}
