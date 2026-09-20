#include "core/types.hpp"
#include "core/version.hpp"
#include "hw/nvapi.hpp"
#include "hw/nvml.hpp"
#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// std::atoi silently returns 0 for anything it cannot parse, so a typo like
// "--set-fan off" would parse to 0 and pin the fan there -- 0 is in range,
// so the CLI's own range check cannot catch it. strtol plus this end-pointer
// check rejects any argument that is not fully numeric, instead of guessing.
static bool ParseIntArg(const char* text, int* out) {
    if (!text || *text == '\0') return false;
    char* end = nullptr;
    errno = 0;
    const long value = std::strtol(text, &end, 10);
    if (end == text || *end != '\0') return false;   // trailing/leading junk, or nothing consumed
    if (errno == ERANGE || value < INT_MIN || value > INT_MAX) return false;
    *out = static_cast<int>(value);
    return true;
}

// Telemetry fields that use the -1-means-unknown sentinel print as "n/a"
// instead of a number that would look like a real reading.
static void FormatField(char* buf, std::size_t size, int value, const char* suffix) {
    if (value < 0) std::snprintf(buf, size, "n/a");
    else std::snprintf(buf, size, "%d%s", value, suffix);
}

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
        // core_mhz/mem_mhz/temp_c/power_w/fan_pct are each -1 when the
        // driver didn't report that particular value, even though t.ok is
        // true overall; print "n/a" for those rather than a number that
        // would look like a real reading.
        char core[8], mem[8], temp[8], fan[8], power[8];
        FormatField(core, sizeof(core), t.core_mhz, "");
        FormatField(mem, sizeof(mem), t.mem_mhz, "");
        FormatField(temp, sizeof(temp), t.temp_c, "");
        FormatField(fan, sizeof(fan), t.fan_pct, "%");
        FormatField(power, sizeof(power), t.power_w, "");
        std::printf("  [%d] core=%s MHz  mem=%s MHz  temp=%s C  fan=%s  power=%s/%d W\n",
                    i, core, mem, temp, fan, power, t.power_limit_w);
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
    // Print the diagnostic before the verdict, same as set_fan(): "clamped to
    // 210 MHz" and "the call never reached an unelevated driver" both show
    // MISMATCH here, and without this line they are indistinguishable.
    if (!ok) std::printf("%s\n", nvapi.Error().c_str());
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
    // Print the diagnostic before the verdict, same as set_fan(): a refused
    // write and an unelevated call both show MISMATCH here otherwise.
    if (!ok) std::printf("%s\n", nvapi.Error().c_str());
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
    if (argc > 2 && std::strcmp(argv[1], "--set-core") == 0) {
        int mhz = 0;
        if (!ParseIntArg(argv[2], &mhz)) { std::printf("--set-core expects an integer MHz value, got '%s'\n", argv[2]); return 1; }
        return set_offset("core offset", mhz, true);
    }
    if (argc > 2 && std::strcmp(argv[1], "--set-mem") == 0) {
        int mhz = 0;
        if (!ParseIntArg(argv[2], &mhz)) { std::printf("--set-mem expects an integer MHz value, got '%s'\n", argv[2]); return 1; }
        return set_offset("mem offset", mhz, false);
    }
    if (argc > 1 && std::strcmp(argv[1], "--reset") == 0) return reset();
    if (argc > 2 && std::strcmp(argv[1], "--set-fan") == 0) {
        int pct = 0;
        if (!ParseIntArg(argv[2], &pct)) { std::printf("--set-fan expects an integer percentage (or -1), got '%s'\n", argv[2]); return 1; }
        return set_fan(pct);
    }
    std::printf("usage: gao [--version | --probe | --set-core <mhz> | --set-mem <mhz> | --reset | --set-fan <pct>]\n");
    return argc > 1 ? 1 : 0;
}
