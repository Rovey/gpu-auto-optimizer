#pragma once
#include "core/types.hpp"
#include <optional>
#include <string>
#include <utility>

namespace gao {

class Nvml {
public:
    bool Init();
    // For tests: these entry points in place of nvml.dll. The session and
    // Read work with them; nothing else of the object does.
    struct Fake {
        int (*init)() = nullptr;
        int (*shutdown)() = nullptr;
        int (*by_index)(unsigned, void**) = nullptr;
        int (*power_limit)(void*, unsigned*) = nullptr;
        int (*fan)(void*, unsigned*) = nullptr;
        int (*clock)(void*, int, unsigned*) = nullptr;
        int (*temperature)(void*, int, unsigned*) = nullptr;
        unsigned refresh_wait_ms = 2000;   // between a card that is gone and the new session
        int (*temp_threshold)(void*, int, unsigned*) = nullptr;
    };
    bool Init(const Fake& fake);
    bool Init(int (*init)(), int (*shutdown)()) { return Init(Fake{init, shutdown}); }
    ~Nvml();
    // A call into nvml.dll was left by an access violation that was caught
    // (app/guarded_gpu.hpp). NVML counts the calls that are inside it, and
    // that one never left: nvmlShutdown then waits for it for ever, holding
    // the lock nvmlInit needs (hardware check 80). From here on no Nvml in
    // this process shuts the library down or unloads it: it stays loaded
    // until the process ends. For the whole process, as NVML's own state is.
    static void Abandon();
    static bool Abandoned();
    // -1 when NVML isn't initialized or the count could not be read (see
    // Error() for why); a genuine "no GPUs" result is 0. Never conflate the
    // two, for the same reason Telemetry::fan_pct never conflates "unknown"
    // with a real zero reading.
    int DeviceCount();
    Telemetry Read(unsigned index);
    // e.g. "610.74"; empty when NVML cannot report it (see Error()).
    std::string DriverVersion();
    // The card's NVML UUID ("GPU-..."); empty when unavailable.
    std::string GpuUuid(unsigned index);
    // Power limit as percent of the driver default. Empty when NVML cannot
    // report the constraints (older cards, or a failed call; see Error()).
    std::optional<std::pair<int, int>> PowerLimitRangePct(unsigned index);
    // Sets the limit to pct of default and verifies by reading it back
    // (within 1 % of default). Needs administrator rights.
    bool SetPowerLimitPct(unsigned index, int pct);
    // Current limit as a (rounded) percent of the default; nullopt on failure.
    std::optional<int> PowerLimitPct(unsigned index);
    // The card's own temperature target in C (nvmlDeviceGetTemperatureThreshold,
    // the "acoustic" threshold in force): from there the card slows itself
    // down. Needs no administrator rights. Empty when the card or the driver
    // does not report one, or reports a number that cannot be one.
    std::optional<int> TempTarget(unsigned index);
    // Fans. NVML reports several per card; they are always set together.
    int FanCount(unsigned index);                                  // 0 when unknown or none
    std::optional<std::pair<int, int>> FanRangePct(unsigned index);   // {min, max} manual speed
    // Every fan to pct, then the target read back from each; needs elevation.
    bool SetFanPct(unsigned index, int pct);
    // Every fan back to the driver, then the policy read back from each.
    bool SetFanAuto(unsigned index);
    std::optional<FanReading> ReadFan(unsigned index);             // fan 0's policy and target
    const std::string& Error() const { return error_; }

private:
    void* lib_ = nullptr;
    bool inited_ = false;
    std::string error_;
};

}
