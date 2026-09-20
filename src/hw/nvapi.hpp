#pragma once
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace gao {

// Clock offsets through NVAPI's PState20 V2 buffer. Every setter verifies by
// reading the buffer back: NVAPI returns success for changes it does not
// apply, so a caller that trusted the return code alone could report an
// offset that was never actually written to the GPU.
//
// The buffer layout below is not documented by NVIDIA; it was reverse
// engineered and proved against this exact hardware by the project's earlier
// Python implementation (src/backends/nvapi.py, tag v0.9-python). See
// nvapi.cpp for the byte offsets.
//
// Fan control is different: unlike the offsets above, its ids are not
// proven against this hardware by any prior implementation of this project.
// See the block comment in nvapi.cpp for where the ids came from and why
// only the older SetCoolerLevels API is ever actually called.
class Nvapi {
public:
    bool Init();
    ~Nvapi();
    bool SetCoreOffsetMhz(unsigned gpu, int mhz);
    bool SetMemOffsetMhz(unsigned gpu, int mhz);
    bool ResetOffsets(unsigned gpu);
    std::optional<std::pair<int, int>> ReadOffsetsMhz(unsigned gpu);  // {core, mem}, MHz

    // Fan control through NvAPI_GPU_SetCoolerLevels (the older, fully
    // specified API -- see nvapi.cpp for why the newer client fan-cooler ids
    // are known but never called). pct is 0-100; -1 restores automatic
    // (driver) control. Verified by reading the fan percentage back through
    // NVML after a settle delay -- never trust NVAPI's return code alone.
    bool SetFanPct(unsigned gpu, int pct);
    // False until Init() has confirmed, read-only, that this GPU answers to
    // the fan-control calls; also flips to false if a later SetFanPct() call
    // resolves fine but fails to verify. Mirrors the offsets' "a resolved id
    // is not proof it works" discipline.
    bool FanControlAvailable() const { return fan_available_; }

    const std::string& Error() const { return error_; }

private:
    // Writes one int32 kHz delta at offset_bytes (kOffCoreDelta or
    // kOffMemDelta, see nvapi.cpp), then reads the buffer back and fails
    // unless the driver actually applied it within tolerance.
    bool SetDeltaKhz(unsigned gpu, int offset_bytes, int khz);
    // Fills buf (kBufferSize bytes) via NvAPI_GPU_GetPstates20.
    bool GetPstates20(unsigned gpu, unsigned char* buf);
    // Resolves gpu to a physical GPU handle from Init()'s enumeration, or
    // returns nullptr with error_ set (out of range, or Init() never ran).
    void* GpuHandle(unsigned gpu);
    // Looks up one NVAPI interface id through nvapi_QueryInterface. Returns
    // nullptr (never calls anything) if the id doesn't resolve.
    void* QueryFn(unsigned id);

    // Read-only, side-effect-free check run once from Init(): resolves the
    // fan-control calls and queries current cooler settings to confirm this
    // GPU actually answers, before make_gpu_control() ever offers the
    // callback to core code.
    void DetectFanControl();
    // Fills buf (kCoolerSettingsSize bytes) via NvAPI_GPU_GetCoolerSettings.
    bool GetCoolerSettings(unsigned gpu, unsigned char* buf);
    // Sets every cooler NvAPI_GPU_GetCoolerSettings reports to pct, manual
    // policy, via NvAPI_GPU_SetCoolerLevels.
    bool ApplyCoolerLevels(unsigned gpu, int pct);
    // Hands fan control back to the driver via NvAPI_GPU_RestoreCoolerSettings.
    bool RestoreCoolerLevels(unsigned gpu);
    // Settles, then reads fan_pct back through NVML and compares to
    // target_pct (skipped for target_pct < 0, which has no fixed target).
    bool VerifyFanPct(unsigned gpu, int target_pct);

    void* lib_ = nullptr;
    void* query_ = nullptr;
    std::vector<void*> gpus_;
    bool inited_ = false;
    bool fan_available_ = false;
    std::string error_;
};

}
