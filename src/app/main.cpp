#include "core/types.hpp"
#include "core/version.hpp"
#include "hw/nvapi.hpp"
#include "hw/nvml.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>

static int probe() {
    gao::Nvml nvml;
    if (!nvml.Init()) {
        std::printf("NVML init failed: %s\n", nvml.Error().c_str());
        return 1;
    }
    const int count = nvml.DeviceCount();
    if (count < 0) {
        std::printf("NVML device count failed: %s\n", nvml.Error().c_str());
        return 1;
    }
    std::printf("GPUs: %d\n", count);
    for (int i = 0; i < count; ++i) {
        const gao::Telemetry t = nvml.Read(static_cast<unsigned>(i));
        if (!t.ok) { std::printf("  [%d] read failed\n", i); continue; }
        // fan_pct is -1 when the driver didn't report it; print that as an
        // unambiguous "n/a" rather than a number that looks like a reading.
        char fan[8];
        if (t.fan_pct < 0) std::snprintf(fan, sizeof(fan), "n/a");
        else std::snprintf(fan, sizeof(fan), "%d%%", t.fan_pct);
        std::printf("  [%d] core=%d MHz  mem=%d MHz  temp=%d C  fan=%s  power=%d/%d W\n",
                    i, t.core_mhz, t.mem_mhz, t.temp_c, fan, t.power_w, t.power_limit_w);
    }
    return 0;
}

// GPU index is fixed at 0: none of --set-core/--set-mem/--reset take a GPU
// selector yet (see task-5-brief.md's own example commands), and this
// machine has exactly one NVIDIA GPU.
static constexpr unsigned kGpu = 0;

// Shared by --set-core and --set-mem. Prints "requested X, read back Y" and
// OK/MISMATCH. Always prints the read-back, even on success -- the number
// being visible is the point, not just the verdict.
static int set_offset(const char* label, int mhz, bool core) {
    gao::Nvapi nvapi;
    if (!nvapi.Init()) { std::printf("NVAPI init failed: %s\n", nvapi.Error().c_str()); return 1; }
    const bool ok = core ? nvapi.SetCoreOffsetMhz(kGpu, mhz) : nvapi.SetMemOffsetMhz(kGpu, mhz);
    const auto readback = nvapi.ReadOffsetsMhz(kGpu);
    std::printf("%s: requested %d MHz, read back ", label, mhz);
    if (readback) std::printf("%d MHz\n", core ? readback->first : readback->second);
    else std::printf("unavailable (%s)\n", nvapi.Error().c_str());
    std::printf("%s\n", ok ? "OK" : "MISMATCH");
    return ok ? 0 : 1;
}

static int reset() {
    gao::Nvapi nvapi;
    if (!nvapi.Init()) { std::printf("NVAPI init failed: %s\n", nvapi.Error().c_str()); return 1; }
    const bool ok = nvapi.ResetOffsets(kGpu);
    const auto readback = nvapi.ReadOffsetsMhz(kGpu);
    std::printf("reset: requested core 0 MHz, mem 0 MHz\n");
    if (readback) std::printf("read back core %d MHz, mem %d MHz\n", readback->first, readback->second);
    else std::printf("read back unavailable (%s)\n", nvapi.Error().c_str());
    std::printf("%s\n", ok ? "OK" : "MISMATCH");
    return ok ? 0 : 1;
}

// pct is 0-100; -1 restores automatic control. Prints the same OK/MISMATCH
// verdict as set_offset() -- SetFanPct() already verified the change through
// NVML before returning, so the return value alone is trustworthy here.
static int set_fan(int pct) {
    gao::Nvapi nvapi;
    if (!nvapi.Init()) { std::printf("NVAPI init failed: %s\n", nvapi.Error().c_str()); return 1; }
    if (!nvapi.FanControlAvailable()) {
        std::printf("fan control unavailable: %s\n", nvapi.Error().c_str());
        return 1;
    }
    if (pct < 0) std::printf("fan: restoring automatic control\n");
    else std::printf("fan: requested %d%%\n", pct);
    const bool ok = nvapi.SetFanPct(kGpu, pct);
    if (!ok) std::printf("%s\n", nvapi.Error().c_str());
    std::printf("%s\n", ok ? "OK" : "MISMATCH");
    return ok ? 0 : 1;
}

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--version") == 0) {
        std::printf("%s %s\n", gao::kProductName.data(), gao::kVersion.data());
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "--probe") == 0) return probe();
    if (argc > 2 && std::strcmp(argv[1], "--set-core") == 0) return set_offset("core offset", std::atoi(argv[2]), true);
    if (argc > 2 && std::strcmp(argv[1], "--set-mem") == 0) return set_offset("mem offset", std::atoi(argv[2]), false);
    if (argc > 1 && std::strcmp(argv[1], "--reset") == 0) return reset();
    if (argc > 2 && std::strcmp(argv[1], "--set-fan") == 0) return set_fan(std::atoi(argv[2]));
    std::printf("usage: gao [--version | --probe | --set-core <mhz> | --set-mem <mhz> | --reset | --set-fan <pct>]\n");
    return argc > 1 ? 1 : 0;
}
