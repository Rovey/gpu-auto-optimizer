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
typedef nvmlReturn_t (*fn_pl_default)(nvmlDevice_t, unsigned*);
typedef nvmlReturn_t (*fn_pl_constraints)(nvmlDevice_t, unsigned*, unsigned*);
typedef nvmlReturn_t (*fn_pl_set)(nvmlDevice_t, unsigned);
static fn_pl_default     p_pl_default = nullptr;
static fn_pl_constraints p_pl_constraints = nullptr;
static fn_pl_set         p_pl_set = nullptr;

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
    p_pl_default     = (fn_pl_default)GetProcAddress(h, "nvmlDeviceGetPowerManagementDefaultLimit");
    p_pl_constraints = (fn_pl_constraints)GetProcAddress(h, "nvmlDeviceGetPowerManagementLimitConstraints");
    p_pl_set         = (fn_pl_set)GetProcAddress(h, "nvmlDeviceSetPowerManagementLimit");
    if (!p_init || !p_byIndex) { error_ = "required NVML entry points not found"; return false; }
    inited_ = (p_init() == NVML_SUCCESS);
    if (!inited_) error_ = "nvmlInit_v2 failed";
    return inited_;
}

int Nvml::DeviceCount() {
    // -1 means "not ready" (not initialized, or the query failed) and is
    // never conflated with a genuine zero-device result, mirroring the
    // fan_pct sentinel convention in Telemetry. Error() carries the reason.
    if (!inited_) { error_ = "NVML not initialized"; return -1; }
    if (!p_count) { error_ = "nvmlDeviceGetCount_v2 not available"; return -1; }
    unsigned n = 0;
    if (p_count(&n) != NVML_SUCCESS) { error_ = "nvmlDeviceGetCount_v2 failed"; return -1; }
    return static_cast<int>(n);
}

Telemetry Nvml::Read(unsigned index) {
    Telemetry t;
    if (!inited_) return t;
    nvmlDevice_t dev = nullptr;
    if (p_byIndex(index, &dev) != NVML_SUCCESS) return t;
    unsigned v = 0;
    // Every field below stays at its -1 default (its "unknown" sentinel)
    // unless the driver actually reports a reading: a missing symbol or a
    // non-zero status must never be read as 0, since 0 is itself a real
    // value for several of these (an idle card's fan, a card at its thermal
    // floor). core_ok/temp_ok track the two readings the search depends on,
    // for t.ok below.
    bool core_ok = false;
    bool temp_ok = false;
    if (p_clock && p_clock(dev, /*GRAPHICS*/0, &v) == NVML_SUCCESS) { t.core_mhz = static_cast<int>(v); core_ok = true; }
    if (p_clock && p_clock(dev, /*MEM*/2, &v) == NVML_SUCCESS)      t.mem_mhz = static_cast<int>(v);
    if (p_temp && p_temp(dev, /*GPU*/0, &v) == NVML_SUCCESS)        { t.temp_c = static_cast<int>(v); temp_ok = true; }
    if (p_power && p_power(dev, &v) == NVML_SUCCESS)                t.power_w = static_cast<int>(v / 1000);
    if (p_powerlimit && p_powerlimit(dev, &v) == NVML_SUCCESS)      t.power_limit_w = static_cast<int>(v / 1000);
    if (p_fan && p_fan(dev, &v) == NVML_SUCCESS) t.fan_pct = static_cast<int>(v);
    // A device handle alone is not proof the readings this app relies on
    // (the thermal abort, the fan-curve search) are real, rather than a
    // failed call masquerading as a plausible zero. mem/power/fan may still
    // be -1 (unknown) with ok == true; callers check those individually.
    t.ok = core_ok && temp_ok;
    return t;
}

std::optional<std::pair<int, int>> Nvml::PowerLimitRangePct(unsigned index) {
    nvmlDevice_t dev = nullptr;
    unsigned def = 0, lo = 0, hi = 0;
    if (!inited_ || p_byIndex(index, &dev) != NVML_SUCCESS) { error_ = "NVML device not available"; return std::nullopt; }
    if (!p_pl_default || p_pl_default(dev, &def) != NVML_SUCCESS || def == 0) {
        error_ = "nvmlDeviceGetPowerManagementDefaultLimit failed"; return std::nullopt;
    }
    if (!p_pl_constraints || p_pl_constraints(dev, &lo, &hi) != NVML_SUCCESS) {
        error_ = "nvmlDeviceGetPowerManagementLimitConstraints failed"; return std::nullopt;
    }
    // Round inward so every percent in the range is actually settable.
    const int min_pct = static_cast<int>((static_cast<unsigned long long>(lo) * 100 + def - 1) / def);
    const int max_pct = static_cast<int>(static_cast<unsigned long long>(hi) * 100 / def);
    return std::make_pair(min_pct, max_pct);
}

bool Nvml::SetPowerLimitPct(unsigned index, int pct) {
    nvmlDevice_t dev = nullptr;
    unsigned def = 0, now = 0;
    if (!inited_ || p_byIndex(index, &dev) != NVML_SUCCESS) { error_ = "NVML device not available"; return false; }
    if (!p_pl_default || p_pl_default(dev, &def) != NVML_SUCCESS || def == 0) {
        error_ = "nvmlDeviceGetPowerManagementDefaultLimit failed"; return false;
    }
    const unsigned target = static_cast<unsigned>(static_cast<unsigned long long>(def) * pct / 100);
    auto close_enough = [&](unsigned mw) {
        const long long diff = static_cast<long long>(mw) - static_cast<long long>(target);
        return (diff < 0 ? -diff : diff) <= def / 100;
    };
    // Already there: no write. Some cards report constraints but refuse the
    // set call; without this, resetting to a default they already have
    // would fail and block every run.
    if (p_powerlimit && p_powerlimit(dev, &now) == NVML_SUCCESS && close_enough(now)) return true;
    if (!p_pl_set || p_pl_set(dev, target) != NVML_SUCCESS) {
        error_ = "nvmlDeviceSetPowerManagementLimit failed (elevated?)"; return false;
    }
    // Never trust the return code: read the limit back.
    if (!p_powerlimit || p_powerlimit(dev, &now) != NVML_SUCCESS) {
        error_ = "nvmlDeviceGetPowerManagementLimit failed after set"; return false;
    }
    if (!close_enough(now)) {
        error_ = "power limit read back " + std::to_string(now / 1000) + " W, requested " + std::to_string(target / 1000) + " W";
        return false;
    }
    return true;
}

Nvml::~Nvml() {
    if (inited_ && p_shutdown) p_shutdown();
    if (lib_) FreeLibrary((HMODULE)lib_);
}

}
