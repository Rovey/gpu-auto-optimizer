#include "hw/nvapi.hpp"
#include "hw/nvml.hpp"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstring>

namespace gao {

namespace {

// QueryInterface ids. Proven against this hardware by the project's Python
// implementation (src/backends/nvapi.py, tag v0.9-python) -- see task-5-brief.md.
constexpr unsigned kInitializeId       = 0x0150E828;
// Was 0x0D22BDD7 (a digit-shifted typo the prior Python implementation
// carried for months without noticing, because a failed NvAPI_Unload is
// silent -- nothing reads its return value). The task brief that flagged
// this bug named 0xD22BDD7F as the fix; a standalone read-only probe
// (nvapi_QueryInterface with no other calls, then a harmless
// Initialize->Unload cycle) showed that value ALSO resolves to null, while
// 0xD22BDD7E resolves to a real function pointer that returns NVAPI_OK when
// called right after NvAPI_Initialize(). 0xD22BDD7E also matches
// arcnmx/nvapi-rs's id table (sys/src/nvid.rs) independently of the probe,
// so it is used here instead of the brief's stated value.
constexpr unsigned kUnloadId           = 0xD22BDD7E;
constexpr unsigned kEnumPhysicalGpusId = 0xE5AC921F;
constexpr unsigned kGetPstates20Id     = 0x6FF81213;
constexpr unsigned kSetPstates20Id     = 0x0F4DAE6B;

// -- Fan control ------------------------------------------------------------
//
// Unlike the ids above, none of this is proven against this hardware by any
// prior implementation of this project. What follows is sourced from
// arcnmx/nvapi-rs (github.com/arcnmx/nvapi-rs, MIT), fetched at
// implementation time:
//
//   - kGetCoolerSettingsId, kSetCoolerLevelsId, kRestoreCoolerSettingsId, and
//     their NV_GPU_COOLER_SETTINGS*/NV_GPU_SETCOOLER_LEVEL* struct layouts
//     below, come from sys/src/gpu/cooler.rs (structs/fn signatures) and
//     sys/src/nvid.rs (the ids themselves -- cooler.rs's `nvapi!` macro
//     resolves ids through that file, it does not embed them). Cross-checked
//     against the four ids already proven on this hardware in this file
//     (Initialize, EnumPhysicalGPUs, GetPstates20, SetPstates20): all four
//     match nvid.rs exactly, which is why this table is trusted for the rest.
//     kSetCoolerLevelsId (0x891FA0AE) also matches the brief's own citation
//     (Open Hardware Monitor / nvapi-rs).
//
//   - kClientFanCoolersGetStatusId/GetControlId/SetControlId, the newer
//     "client fan cooler" API this task was asked to try first, also come
//     from sys/src/nvid.rs -- but that is *all* that exists for them
//     anywhere in the crate (checked every branch: master, v0.1.x, v0.2.x,
//     v0.2.x-macros, v0.2.x-refactor, vfp, serde-rpc). No parameter struct,
//     no field layout, no caller. The ids were added in commit ca75f787
//     ("updated api names") under the comment "source: gpu-z" and are never
//     invoked by any wrapper function in the crate.
//
//     A wrong id is safe here: nvapi_QueryInterface does a lookup in the
//     driver's own dispatch table and simply returns null for an id it does
//     not recognize. But nvapi_QueryInterface resolving a *correct* id only
//     proves the installed driver knows that id -- it says nothing about
//     whether this physical GPU answers to it, and it says nothing at all
//     about the buffer such a resolved function pointer expects. Calling a
//     real function pointer with an invented buffer size/layout is not a
//     safe "unsupported" result the way a null pointer is; it is undefined
//     behavior. That is a materially different risk than every other id in
//     this file, all of which come with a fully specified struct from a real
//     source. So these three ids are recorded for diagnostics only -- see
//     DetectFanControl() -- and never used to build a call.
//
//     Empirically, on this project's own RTX 4070: NvAPI_GPU_GetCoolerSettings
//     (the fallback's own read call, fully specified, called correctly)
//     returns NVAPI_NOT_SUPPORTED (-104), not a resolution failure. That
//     matches the well-known fact that NVIDIA dropped the legacy per-cooler
//     API on Turing-and-later GPUs. In other words, FanControlAvailable()
//     is expected to read false on THIS card specifically -- fan control is
//     safe and correctly reports itself unavailable, but does not actually
//     work here until the newer API's struct is found. See task-6-report.md.
constexpr unsigned kGetCoolerSettingsId          = 0xDA141340;
constexpr unsigned kSetCoolerLevelsId            = 0x891FA0AE;
constexpr unsigned kRestoreCoolerSettingsId      = 0x8F6ED0FB;
constexpr unsigned kClientFanCoolersGetStatusId  = 0x35AED5E8;
constexpr unsigned kClientFanCoolersGetControlId = 0x814B209F;
constexpr unsigned kClientFanCoolersSetControlId = 0xA58971A5;

// NV_GPU_COOLER_SETTINGS_V1: version(u32) + count(u32) +
// cooler[NVAPI_MAX_COOLERS_PER_GPU], each cooler entry 12 u32 fields (48
// bytes). Only `count` and each entry's `currentLevel`/`currentPolicy` are
// read by this file; the rest of the entry is skipped over, not decoded.
constexpr int kMaxCoolersPerGpu = 3;       // NVAPI_MAX_COOLERS_PER_GPU
constexpr int kCoolerSettingsSize = 4 * 2 + (4 * 12) * kMaxCoolersPerGpu;  // 152
constexpr unsigned kCoolerSettingsVerV1 = kCoolerSettingsSize | (1u << 16);
constexpr int kCoolerOffVersion = 0;
constexpr int kCoolerOffCount = 4;

// NV_GPU_SETCOOLER_LEVEL_V1: version(u32) + cooler[kMaxCoolersPerGpu], each
// entry {currentLevel: u32, currentPolicy: u32} (8 bytes).
constexpr int kSetCoolerEntrySize = 8;
constexpr int kSetCoolerLevelSize = 4 + kSetCoolerEntrySize * kMaxCoolersPerGpu;  // 28
constexpr unsigned kSetCoolerLevelVerV1 = kSetCoolerLevelSize | (1u << 16);
constexpr int kSetCoolerOffVersion = 0;
constexpr int kSetCoolerOffCoolerArray = 4;
constexpr int kSetCoolerEntryOffLevel = 0;
constexpr int kSetCoolerEntryOffPolicy = 4;

constexpr unsigned kCoolerTargetAll = 7;     // NVAPI_COOLER_TARGET_ALL
constexpr unsigned kCoolerPolicyManual = 1;  // NVAPI_COOLER_POLICY_MANUAL

constexpr int kFanTolerancePct = 5;   // NVML read-back tolerance
constexpr int kFanSettleMs = 2000;    // time given the fan to physically move

// NV_GPU_PERF_PSTATES20_INFO, V2. Whole-buffer size and the version tag that
// must be written into the buffer before every GET and SET call.
constexpr int kBufferSize = 7416;
constexpr unsigned kVersionV2 = kBufferSize | (2u << 16);  // 0x00021CF8

// Byte offsets into the buffer. version/editable/numPstates are the header;
// the two deltas sit inside the P0 entry. All of this is undocumented by
// NVIDIA and comes from the same proven Python source as the ids above.
constexpr int kOffVersion = 0;      // uint32
constexpr int kOffEditable = 4;     // uint32 -- must be 1 or SET is a no-op
constexpr int kOffNumPstates = 8;   // uint32 -- 1 restricts the SET to P0
constexpr int kOffCoreDelta = 40;   // int32, kHz
constexpr int kOffMemDelta = 84;    // int32, kHz

constexpr int kToleranceKhz = 1000;
constexpr unsigned kMaxPhysicalGpus = 64;  // NVAPI_MAX_PHYSICAL_GPUS

using nvapi_status_t = int;
constexpr nvapi_status_t kNvapiOk = 0;

typedef void* (*fn_query)(unsigned);
typedef nvapi_status_t (*fn_initialize)();
typedef nvapi_status_t (*fn_unload)();
typedef nvapi_status_t (*fn_enum)(void**, unsigned*);
typedef nvapi_status_t (*fn_get_pstates20)(void*, void*);
typedef nvapi_status_t (*fn_set_pstates20)(void*, void*);
// hPhysicalGPU, coolerIndex, pCoolerInfo -- signatures from
// arcnmx/nvapi-rs sys/src/gpu/cooler.rs.
typedef nvapi_status_t (*fn_get_cooler_settings)(void*, unsigned, void*);
typedef nvapi_status_t (*fn_set_cooler_levels)(void*, unsigned, const void*);
typedef nvapi_status_t (*fn_restore_cooler_settings)(void*, const unsigned*, unsigned);

void PutU32(unsigned char* buf, int offset, unsigned value) {
    std::memcpy(buf + offset, &value, sizeof(value));
}

int GetI32(const unsigned char* buf, int offset) {
    int value = 0;
    std::memcpy(&value, buf + offset, sizeof(value));
    return value;
}

unsigned GetU32(const unsigned char* buf, int offset) {
    unsigned value = 0;
    std::memcpy(&value, buf + offset, sizeof(value));
    return value;
}

}  // namespace

void* Nvapi::QueryFn(unsigned id) {
    if (!query_) return nullptr;
    return ((fn_query)query_)(id);
}

bool Nvapi::Init() {
    HMODULE h = LoadLibraryA("nvapi64.dll");
    if (!h) { error_ = "could not load nvapi64.dll"; return false; }
    lib_ = h;

    query_ = (void*)GetProcAddress(h, "nvapi_QueryInterface");
    if (!query_) { error_ = "nvapi_QueryInterface not found in nvapi64.dll"; return false; }

    auto initialize = (fn_initialize)QueryFn(kInitializeId);
    if (!initialize) { error_ = "NvAPI_Initialize: nvapi_QueryInterface returned null"; return false; }
    if (initialize() != kNvapiOk) { error_ = "NvAPI_Initialize failed"; return false; }
    inited_ = true;  // NvAPI_Unload is now required from the destructor, even if the rest of Init() fails below.

    auto enum_gpus = (fn_enum)QueryFn(kEnumPhysicalGpusId);
    if (!enum_gpus) { error_ = "NvAPI_EnumPhysicalGPUs: nvapi_QueryInterface returned null"; return false; }

    void* handles[kMaxPhysicalGpus] = {};
    unsigned count = 0;
    if (enum_gpus(handles, &count) != kNvapiOk) { error_ = "NvAPI_EnumPhysicalGPUs failed"; return false; }

    gpus_.assign(handles, handles + count);
    if (gpus_.empty()) { error_ = "NvAPI_EnumPhysicalGPUs returned no GPUs"; return false; }

    // Read-only. May overwrite error_ with a fan-specific diagnostic, but
    // that's fine: Init() has already succeeded by this point (the offsets
    // work independently of fan control), so Init() must not fail just
    // because this particular GPU can't be fan-controlled. Callers who care
    // read FanControlAvailable() and, on false, Error() for why.
    DetectFanControl();
    return true;
}

void* Nvapi::GpuHandle(unsigned gpu) {
    if (!inited_) { error_ = "NVAPI not initialized"; return nullptr; }
    if (gpu >= gpus_.size()) { error_ = "GPU index out of range"; return nullptr; }
    return gpus_[gpu];
}

bool Nvapi::GetPstates20(unsigned gpu, unsigned char* buf) {
    void* handle = GpuHandle(gpu);
    if (!handle) return false;  // error_ set by GpuHandle

    auto get_pstates20 = (fn_get_pstates20)QueryFn(kGetPstates20Id);
    if (!get_pstates20) { error_ = "NvAPI_GPU_GetPstates20: nvapi_QueryInterface returned null"; return false; }

    std::memset(buf, 0, kBufferSize);
    PutU32(buf, kOffVersion, kVersionV2);  // the driver requires the version tag pre-set on entry
    if (get_pstates20(handle, buf) != kNvapiOk) { error_ = "NvAPI_GPU_GetPstates20 failed"; return false; }
    return true;
}

std::optional<std::pair<int, int>> Nvapi::ReadOffsetsMhz(unsigned gpu) {
    unsigned char buf[kBufferSize];
    if (!GetPstates20(gpu, buf)) return std::nullopt;  // error_ set by GetPstates20
    return std::make_pair(GetI32(buf, kOffCoreDelta) / 1000, GetI32(buf, kOffMemDelta) / 1000);
}

bool Nvapi::SetDeltaKhz(unsigned gpu, int offset_bytes, int khz) {
    void* handle = GpuHandle(gpu);
    if (!handle) return false;  // error_ set by GpuHandle

    unsigned char buf[kBufferSize];
    if (!GetPstates20(gpu, buf)) return false;  // error_ set by GetPstates20; buf holds the current state

    auto set_pstates20 = (fn_set_pstates20)QueryFn(kSetPstates20Id);
    if (!set_pstates20) { error_ = "NvAPI_GPU_SetPstates20: nvapi_QueryInterface returned null"; return false; }

    // Minimal SET: restrict the edit to P0 and mark it editable. This is the
    // technique already proven on this hardware; without it the driver can
    // accept the call and report success while leaving the delta untouched.
    PutU32(buf, kOffVersion, kVersionV2);
    PutU32(buf, kOffEditable, 1);
    PutU32(buf, kOffNumPstates, 1);
    PutU32(buf, offset_bytes, static_cast<unsigned>(khz));  // int32 stored bit-for-bit

    if (set_pstates20(handle, buf) != kNvapiOk) { error_ = "NvAPI_GPU_SetPstates20 failed"; return false; }

    // NvAPI_GPU_SetPstates20 can return NVAPI_OK for a delta it did not
    // actually apply. This read-back is the only way to know whether the
    // write landed, and it is the entire reason this function exists.
    unsigned char verify_buf[kBufferSize];
    if (!GetPstates20(gpu, verify_buf)) {
        error_ = "NvAPI_GPU_SetPstates20 read-back failed: " + error_;
        return false;
    }
    const int actual_khz = GetI32(verify_buf, offset_bytes);
    const int diff = actual_khz > khz ? actual_khz - khz : khz - actual_khz;
    if (diff > kToleranceKhz) {
        error_ = "NvAPI_GPU_SetPstates20 read-back mismatch: requested " + std::to_string(khz) +
                 " kHz, driver reports " + std::to_string(actual_khz) + " kHz";
        return false;
    }
    return true;
}

bool Nvapi::SetCoreOffsetMhz(unsigned gpu, int mhz) { return SetDeltaKhz(gpu, kOffCoreDelta, mhz * 1000); }
bool Nvapi::SetMemOffsetMhz(unsigned gpu, int mhz) { return SetDeltaKhz(gpu, kOffMemDelta, mhz * 1000); }

bool Nvapi::ResetOffsets(unsigned gpu) {
    // Two independent verified writes rather than one combined buffer edit:
    // each still goes through SetDeltaKhz, so a driver that silently ignores
    // either half is caught exactly the same way a single-offset set is.
    //
    // Best-effort across both, deliberately: reset is the escape hatch a
    // user reaches for after something went wrong, so a failed core reset
    // must not skip the memory reset. Report failure if either failed, but
    // always attempt both and keep whichever error is more specific (naming
    // which one failed) if both do.
    const bool core_ok = SetDeltaKhz(gpu, kOffCoreDelta, 0);
    const std::string core_error = error_;
    const bool mem_ok = SetDeltaKhz(gpu, kOffMemDelta, 0);
    if (core_ok && mem_ok) return true;
    error_ = "core reset " + (core_ok ? std::string("OK") : "failed (" + core_error + ")") +
             "; mem reset " + (mem_ok ? std::string("OK") : "failed (" + error_ + ")");
    return false;
}

bool Nvapi::GetCoolerSettings(unsigned gpu, unsigned char* buf) {
    void* handle = GpuHandle(gpu);
    if (!handle) return false;  // error_ set by GpuHandle

    auto get_cooler = (fn_get_cooler_settings)QueryFn(kGetCoolerSettingsId);
    if (!get_cooler) { error_ = "NvAPI_GPU_GetCoolerSettings: nvapi_QueryInterface returned null"; return false; }

    std::memset(buf, 0, kCoolerSettingsSize);
    PutU32(buf, kCoolerOffVersion, kCoolerSettingsVerV1);  // version tag pre-set, same convention as GetPstates20
    const nvapi_status_t rc = get_cooler(handle, kCoolerTargetAll, buf);
    if (rc != kNvapiOk) {
        // The status code is worth keeping here (unlike the plain "failed"
        // used elsewhere in this file): -104 is NVAPI_NOT_SUPPORTED, which is
        // the actual, verified reason fan control answers unavailable on
        // some GPUs -- the legacy per-cooler API this fallback calls was
        // dropped on Turing-and-later architectures in favor of the newer
        // client fan-cooler API this file cannot safely call (see the block
        // comment at the top of this file).
        error_ = "NvAPI_GPU_GetCoolerSettings failed (status " + std::to_string(rc) + ")";
        return false;
    }
    return true;
}

void Nvapi::DetectFanControl() {
    fan_available_ = false;
    if (gpus_.empty()) return;

    // The newer client fan-cooler ids (see the block comment above) resolve
    // on drivers that recognize them, but that is not proof this GPU
    // answers to them, and there is no struct available to find out by
    // calling them. Checked here only so a failure message below can note
    // it; never used to decide availability.
    const bool new_ids_resolve = QueryFn(kClientFanCoolersGetStatusId) &&
                                  QueryFn(kClientFanCoolersGetControlId) &&
                                  QueryFn(kClientFanCoolersSetControlId);

    auto set_levels = (fn_set_cooler_levels)QueryFn(kSetCoolerLevelsId);
    auto restore = (fn_restore_cooler_settings)QueryFn(kRestoreCoolerSettingsId);
    if (!set_levels || !restore) {
        error_ = "fan control unavailable: NvAPI_GPU_SetCoolerLevels or NvAPI_GPU_RestoreCoolerSettings did not resolve";
        if (new_ids_resolve) {
            error_ += " (the newer ClientFanCoolers ids do resolve on this driver, but no parameter struct for "
                      "them is available from any source this project could reach, so they are never called)";
        }
        return;
    }

    // A resolved id is not proof either -- query real cooler settings,
    // read-only, before ever telling make_gpu_control() this GPU can be
    // fan-controlled. Probes GPU 0, matching the rest of this CLI's
    // single-GPU assumption (see kGpu in main.cpp).
    unsigned char settings[kCoolerSettingsSize];
    if (!GetCoolerSettings(0, settings)) return;  // error_ set by GetCoolerSettings

    const unsigned count = GetU32(settings, kCoolerOffCount);
    if (count == 0 || count > static_cast<unsigned>(kMaxCoolersPerGpu)) {
        error_ = "fan control unavailable: NvAPI_GPU_GetCoolerSettings reported no usable coolers";
        return;
    }
    fan_available_ = true;
}

bool Nvapi::ApplyCoolerLevels(unsigned gpu, int pct) {
    unsigned char settings[kCoolerSettingsSize];
    if (!GetCoolerSettings(gpu, settings)) return false;  // error_ set by GetCoolerSettings

    const unsigned count = GetU32(settings, kCoolerOffCount);
    if (count == 0 || count > static_cast<unsigned>(kMaxCoolersPerGpu)) {
        error_ = "NvAPI_GPU_GetCoolerSettings reported an unusable cooler count (" + std::to_string(count) + ")";
        return false;
    }

    void* handle = GpuHandle(gpu);
    if (!handle) return false;  // error_ set by GpuHandle

    auto set_levels = (fn_set_cooler_levels)QueryFn(kSetCoolerLevelsId);
    if (!set_levels) { error_ = "NvAPI_GPU_SetCoolerLevels: nvapi_QueryInterface returned null"; return false; }

    unsigned char levels[kSetCoolerLevelSize] = {};
    PutU32(levels, kSetCoolerOffVersion, kSetCoolerLevelVerV1);
    // One entry per cooler GetCoolerSettings actually reported; MANUAL
    // policy pins the level regardless of temperature until
    // RestoreCoolerLevels hands control back. Entries past `count` are left
    // zeroed (policy NONE), which NvAPI_GPU_SetCoolerLevels documents as
    // "every cooler level with non-zero currentpolicy gets applied" --
    // i.e. skipped, not zeroed out.
    for (unsigned i = 0; i < count; ++i) {
        const int entry_off = kSetCoolerOffCoolerArray + static_cast<int>(i) * kSetCoolerEntrySize;
        PutU32(levels, entry_off + kSetCoolerEntryOffLevel, static_cast<unsigned>(pct));
        PutU32(levels, entry_off + kSetCoolerEntryOffPolicy, kCoolerPolicyManual);
    }
    if (set_levels(handle, kCoolerTargetAll, levels) != kNvapiOk) {
        error_ = "NvAPI_GPU_SetCoolerLevels failed";
        return false;
    }
    return true;
}

bool Nvapi::RestoreCoolerLevels(unsigned gpu) {
    void* handle = GpuHandle(gpu);
    if (!handle) return false;  // error_ set by GpuHandle

    auto restore = (fn_restore_cooler_settings)QueryFn(kRestoreCoolerSettingsId);
    if (!restore) { error_ = "NvAPI_GPU_RestoreCoolerSettings: nvapi_QueryInterface returned null"; return false; }

    // nullptr/0 means "restore every cooler on this GPU to the driver's
    // default policy" per NvAPI_GPU_RestoreCoolerSettings' own
    // documentation -- this is how automatic control gets handed back.
    if (restore(handle, nullptr, 0) != kNvapiOk) { error_ = "NvAPI_GPU_RestoreCoolerSettings failed"; return false; }
    return true;
}

bool Nvapi::VerifyFanPct(unsigned gpu, int target_pct) {
    // The driver needs a moment to actually move the fan before NVML
    // reports the new speed. The source of truth here is NVML, not NVAPI's
    // own GetCoolerSettings.currentLevel, because NVML's fan_pct is the same
    // reading --probe and any later fan-curve search already trust.
    Sleep(static_cast<DWORD>(kFanSettleMs));

    Nvml nvml;
    if (!nvml.Init()) {
        error_ = "fan change applied but could not verify: NVML init failed (" + nvml.Error() + ")";
        return false;
    }
    const Telemetry t = nvml.Read(gpu);
    if (!t.ok || t.fan_pct < 0) {
        error_ = "fan change applied but could not verify: NVML did not report a fan percentage";
        return false;
    }
    if (target_pct < 0) {
        // Automatic control was restored -- there is no fixed target to
        // compare against. Success here means NVML still reports a real
        // reading, i.e. the card is still responding.
        return true;
    }
    const int diff = t.fan_pct > target_pct ? t.fan_pct - target_pct : target_pct - t.fan_pct;
    if (diff > kFanTolerancePct) {
        error_ = "fan change read-back mismatch: requested " + std::to_string(target_pct) +
                 "%, NVML reports " + std::to_string(t.fan_pct) + "%";
        return false;
    }
    return true;
}

bool Nvapi::SetFanPct(unsigned gpu, int pct) {
    if (!fan_available_) {
        // error_ already explains which resolve/probe failed, set by
        // DetectFanControl() during Init().
        return false;
    }

    // The newer client fan-cooler API would be tried here first if this
    // project had a verified struct for it (see the block comment at the
    // top of this file for why it does not); DetectFanControl() already
    // confirmed the fallback below is the one this GPU actually answers to.
    const bool ok = (pct < 0) ? RestoreCoolerLevels(gpu) : ApplyCoolerLevels(gpu, pct);
    if (!ok) {
        // Resolved fine at Init() time but failed on a real call: the same
        // "never offer a control that just failed for real" discipline the
        // offsets use. error_ is already set by the failing call.
        fan_available_ = false;
        return false;
    }

    if (!VerifyFanPct(gpu, pct)) {
        fan_available_ = false;  // error_ set by VerifyFanPct
        return false;
    }
    return true;
}

Nvapi::~Nvapi() {
    if (inited_ && query_) {
        auto unload = (fn_unload)QueryFn(kUnloadId);
        if (unload) unload();
    }
    if (lib_) FreeLibrary((HMODULE)lib_);
}

}
