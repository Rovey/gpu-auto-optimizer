#include "hw/gpu_control.hpp"

namespace gao {

// Wires the two drivers into the single struct core code sees. Callbacks the
// hardware cannot support are left empty on purpose, so callers can tell.
GpuControl make_gpu_control(Nvml& nvml, Nvapi& nvapi, unsigned gpu) {
    GpuControl c;
    c.read = [&nvml, gpu] { return nvml.Read(gpu); };
    c.set_core_offset = [&nvapi, gpu](int mhz) { return nvapi.SetCoreOffsetMhz(gpu, mhz); };
    c.set_mem_offset = [&nvapi, gpu](int mhz) { return nvapi.SetMemOffsetMhz(gpu, mhz); };
    c.reset_to_stock = [&nvapi, gpu] { return nvapi.ResetOffsets(gpu); };
    // set_power_limit stays empty: power limits are a later phase, and an
    // empty callback is the honest representation of "not implemented yet".
    if (nvapi.FanControlAvailable()) {
        c.set_fan_pct = [&nvapi, gpu](int pct) { return nvapi.SetFanPct(gpu, pct); };
    }
    return c;
}

}
