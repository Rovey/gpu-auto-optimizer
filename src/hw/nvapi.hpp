#pragma once
#include "core/types.hpp"
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
class Nvapi {
public:
    bool Init();
    ~Nvapi();
    bool SetCoreOffsetMhz(unsigned gpu, int mhz);
    bool SetMemOffsetMhz(unsigned gpu, int mhz);
    bool ResetOffsets(unsigned gpu);
    std::optional<std::pair<int, int>> ReadOffsetsMhz(unsigned gpu);  // {core, mem}, MHz
    // The range the driver accepts for each offset, from the same buffer.
    std::optional<ClockOffsetRanges> ReadOffsetRangesMhz(unsigned gpu);

    // The voltage/frequency curve of the graphics clock (core/vf_curve.hpp),
    // through three more undocumented calls. Reading may need administrator
    // rights. nullopt when a call fails or the card reports no points.
    std::optional<std::vector<VfPoint>> ReadVfCurve(unsigned gpu);
    // Sets the raw offsets of the named slots and leaves the others as they
    // are. True when the driver accepted the call; what the card did with it
    // is for the caller to read back (apply_flat_top does).
    bool WriteVfRawOffsets(unsigned gpu, const std::vector<VfOffset>& offsets);

    const std::string& Error() const { return error_; }

private:
    // Writes one int32 kHz delta at offset_bytes (kOffCoreDelta or
    // kOffMemDelta, see nvapi.cpp), then reads the buffer back and fails
    // unless the driver actually applied it within tolerance.
    bool SetDeltaKhz(unsigned gpu, int offset_bytes, int khz);
    // Fills buf (kBufferSize bytes) via NvAPI_GPU_GetPstates20.
    bool GetPstates20(unsigned gpu, unsigned char* buf);
    // One of the curve calls: `buf` must carry its version word already.
    bool VfCall(unsigned gpu, unsigned id, const char* name, unsigned char* buf);
    // Fills the three curve buffers. fill_mask: ask for every slot instead of
    // letting the driver say which exist.
    bool ReadVfBuffers(unsigned gpu, bool fill_mask, unsigned char* mask, unsigned char* status, unsigned char* control);
    // Resolves gpu to a physical GPU handle from Init()'s enumeration, or
    // returns nullptr with error_ set (out of range, or Init() never ran).
    void* GpuHandle(unsigned gpu);
    // Looks up one NVAPI interface id through nvapi_QueryInterface. Returns
    // nullptr (never calls anything) if the id doesn't resolve.
    void* QueryFn(unsigned id);

    void* lib_ = nullptr;
    void* query_ = nullptr;
    std::vector<void*> gpus_;
    bool inited_ = false;
    std::string error_;
};

}
