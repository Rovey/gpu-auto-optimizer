#include "app/guarded_gpu.hpp"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "hw/gpu_control.hpp"
#include "hw/nvapi.hpp"
#include "hw/nvml.hpp"
#include <atomic>
#include <string>
#include <cstdio>
#include <optional>
#include <utility>

namespace gao::app {

namespace {
// The last caught fault, taken in the exception filter: the frames of the
// fault are still there then. Plain data, nothing allocated.
struct FaultSite {
    void* frames[32];
    unsigned short count = 0;
    ULONG_PTR access = 0;    // 0 a read, 1 a write, 8 an execute
    ULONG_PTR address = 0;   // of what could not be accessed
};
thread_local FaultSite t_fault;
std::atomic<void (*)(const std::string&)> g_fault_log{nullptr};

int fault_filter(const EXCEPTION_POINTERS* info) {
    const EXCEPTION_RECORD& record = *info->ExceptionRecord;
    if (record.ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    t_fault.count = RtlCaptureStackBackTrace(0, 32, t_fault.frames, nullptr);
    t_fault.access = record.NumberParameters >= 2 ? record.ExceptionInformation[0] : 0;
    t_fault.address = record.NumberParameters >= 2 ? record.ExceptionInformation[1] : 0;
    return EXCEPTION_EXECUTE_HANDLER;
}

void fault_log(const std::string& line) {
    if (const auto sink = g_fault_log.load()) sink(line);
}
}

bool guarded(void (*fn)(void*), void* ctx) {
    __try {
        fn(ctx);
        return true;
    } __except (fault_filter(GetExceptionInformation())) {
        return false;
    }
}

std::string last_fault() {
    if (t_fault.count == 0) return {};
    char what[48];
    std::snprintf(what, sizeof(what), "%s 0x%llx at ", t_fault.access == 1 ? "write to" : t_fault.access == 8 ? "execute of" : "read of",
                  static_cast<unsigned long long>(t_fault.address));
    std::string out;
    for (unsigned i = 0; i < t_fault.count; ++i) {
        HMODULE module = nullptr;
        char path[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               static_cast<const char*>(t_fault.frames[i]), &module))
            GetModuleFileNameA(module, path, MAX_PATH);
        std::string name = path;
        if (const auto slash = name.find_last_of("\\/"); slash != std::string::npos) name.erase(0, slash + 1);
        char site[32];
        std::snprintf(site, sizeof(site), "+0x%llx",
                      static_cast<unsigned long long>(static_cast<const char*>(t_fault.frames[i]) - reinterpret_cast<const char*>(module)));
        if (!out.empty()) out += " < ";
        out += name + site;
    }
    return what + out;
}

void set_fault_log(void (*sink)(const std::string&)) { g_fault_log = sink; }

bool guarded_driver(void (*fn)(void*), void* ctx) {
    if (guarded(fn, ctx)) return true;
    Nvml::Abandon();
    fault_log("a call into a driver library faulted: " + last_fault());
    return false;
}

namespace {
constexpr int kRecoverAttempts = 15;
constexpr DWORD kRecoverWaitMs = 2000;
}

GuardedGpu::GuardedGpu(unsigned gpu)
    : gpu_(gpu), nvml_(std::make_unique<Nvml>()), nvapi_(std::make_unique<Nvapi>()) {}

GuardedGpu::~GuardedGpu() {
    // Shutting down a library after a driver reset may fault too.
    inner_ = {};
    Nvml* nvml = nvml_.release();
    Nvapi* nvapi = nvapi_.release();
    guarded_driver([nvml] { delete nvml; });
    guarded_driver([nvapi] { delete nvapi; });
}

// Stops using the libraries and shuts them down. The only call into a library
// that may just have faulted is its destructor, under the guard: if that
// faults as well, the object and the loaded DLL are leaked, as in the tray's
// hw_lost(). After a fault NVML's destructor no longer calls into the library
// at all (Nvml::Abandon). Leaves fresh, uninitialised objects, which answer
// every call with a failure without reaching the driver.
void GuardedGpu::Disconnect() {
    live_ = false;
    inner_ = {};   // its callbacks refer to the objects deleted below
    Nvml* nvml = nvml_.release();
    Nvapi* nvapi = nvapi_.release();
    nvml_ = std::make_unique<Nvml>();
    nvapi_ = std::make_unique<Nvapi>();
    guarded_driver([nvml] { delete nvml; });
    guarded_driver([nvapi] { delete nvapi; });
}

// Initialises the fresh objects Disconnect (or the constructor) left and
// builds the inner control over them. On failure the caller disconnects.
bool GuardedGpu::Connect(std::string* why) {
    auto fail = [&](const std::string& text) {
        if (why) *why = text;
        return false;
    };
    bool ok = false;
    if (!guarded_driver([&] { ok = nvml_->Init(); })) return fail("NVML init failed: access violation inside the driver library");
    if (!ok) return fail("NVML init failed: " + nvml_->Error());
    ok = false;
    if (!guarded_driver([&] { ok = nvapi_->Init(); })) return fail("NVAPI init failed: access violation inside the driver library");
    if (!ok) return fail("NVAPI init failed: " + nvapi_->Error());
    // make_gpu_control asks NVML what the card supports: driver calls as well.
    GpuControl built;
    if (!guarded_driver([&] { built = make_gpu_control(*nvml_, *nvapi_, gpu_); }))
        return fail("NVML init failed: access violation while querying the card");
    inner_ = std::move(built);
    live_ = true;
    return true;
}

// One driver call: serialised, guarded, and the last one into these libraries
// if it faults. The lock is taken outside the guarded call, in a frame a
// fault does not skip, so it is always released.
template <class R, class Fn> R GuardedGpu::Call(R fail, Fn fn) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (!live_) return fail;
    R out = fail;
    if (guarded_driver([&] { out = fn(); })) return out;
    live_ = false;
    inner_ = {};   // nothing calls into the faulted libraries again; recover deletes them
    return fail;
}

bool GuardedGpu::Init(std::string* why) {
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (!Connect(why)) {
            Disconnect();
            return false;
        }
    }
    // The outer control: the same callbacks the inner one has now, each
    // forwarding to whatever the inner one is at the time of the call. After
    // a recover the card could in theory report less than before; a callback
    // that is gone then answers with its failure value.
    const GpuControl& in = inner_;
    outer_.fan_min_pct = in.fan_min_pct;
    if (in.read)
        outer_.read = [this] { return Call(Telemetry{}, [this] { return inner_.read ? inner_.read() : Telemetry{}; }); };
    if (in.set_core_offset)
        outer_.set_core_offset = [this](int mhz) {
            return Call(false, [this, mhz] { return inner_.set_core_offset && inner_.set_core_offset(mhz); });
        };
    if (in.set_mem_offset)
        outer_.set_mem_offset = [this](int mhz) {
            return Call(false, [this, mhz] { return inner_.set_mem_offset && inner_.set_mem_offset(mhz); });
        };
    if (in.set_power_limit)
        outer_.set_power_limit = [this](int pct) {
            return Call(false, [this, pct] { return inner_.set_power_limit && inner_.set_power_limit(pct); });
        };
    if (in.power_limit_range_pct)
        outer_.power_limit_range_pct = [this] {
            return Call(std::pair<int, int>{0, 0}, [this] {
                return inner_.power_limit_range_pct ? inner_.power_limit_range_pct() : std::pair<int, int>{0, 0};
            });
        };
    if (in.reset_to_stock)
        outer_.reset_to_stock = [this] {
            return Call(false, [this] { return inner_.reset_to_stock && inner_.reset_to_stock(); });
        };
    if (in.read_applied)
        outer_.read_applied = [this] {
            return Call(std::optional<AppliedState>(), [this] {
                return inner_.read_applied ? inner_.read_applied() : std::optional<AppliedState>();
            });
        };
    if (in.clock_offset_range_mhz)
        outer_.clock_offset_range_mhz = [this] {
            return Call(std::optional<ClockOffsetRanges>(), [this] {
                return inner_.clock_offset_range_mhz ? inner_.clock_offset_range_mhz() : std::optional<ClockOffsetRanges>();
            });
        };
    if (in.set_fan_pct)
        outer_.set_fan_pct = [this](int pct) {
            return Call(false, [this, pct] { return inner_.set_fan_pct && inner_.set_fan_pct(pct); });
        };
    if (in.set_fan_auto) outer_.set_fan_auto = [this] { return FanAuto(); };
    if (in.read_fan)
        outer_.read_fan = [this] {
            return Call(std::optional<FanReading>(), [this] {
                return inner_.read_fan ? inner_.read_fan() : std::optional<FanReading>();
            });
        };
    if (in.read_vf_curve)
        outer_.read_vf_curve = [this] {
            return Call(std::optional<std::vector<VfPoint>>(), [this] {
                return inner_.read_vf_curve ? inner_.read_vf_curve() : std::optional<std::vector<VfPoint>>();
            });
        };
    if (in.write_vf_offsets)
        outer_.write_vf_offsets = [this](const std::vector<VfOffset>& offsets) {
            return Call(false, [&] { return inner_.write_vf_offsets && inner_.write_vf_offsets(offsets); });
        };
    outer_.recover = [this] { return Recover(); };
    return true;
}

// set_fan_auto through the outer control. Like Call, but a hand-back that did
// not happen is remembered: FanDriver treats the fans as returned to the driver
// whatever this answers, and without a driver reset they would stay at the
// manual speed last written. The lock is taken in this frame, outside the
// guarded call, so a caught fault always releases it.
bool GuardedGpu::FanAuto() {
    const std::lock_guard<std::mutex> lock(mutex_);
    bool ok = false;
    if (live_ && !guarded_driver([&] { ok = inner_.set_fan_auto && inner_.set_fan_auto(); })) {
        ok = false;
        live_ = false;
        inner_ = {};   // as in Call: the faulted libraries are not called again
    }
    fan_auto_owed_ = !ok;
    return ok;
}

// The owed hand-back, on a fresh connection; the caller holds the lock and
// calls the inner control directly. Never writes a speed. False only when the
// call faulted: then this connection is not usable either.
bool GuardedGpu::DeliverFanAuto() {
    if (!fan_auto_owed_ || !inner_.set_fan_auto) return true;
    bool ok = false;
    if (!guarded_driver([&] { ok = inner_.set_fan_auto(); })) return false;
    if (ok) fan_auto_owed_ = false;   // still owed otherwise; the next recover tries again
    return true;
}

// New libraries, until the card answers again: after a driver reset, after a
// fault, or when a write keeps failing. Up to half a minute, during which the
// run cannot be aborted. The lock is free during the waits, so an emergency
// handler on another thread is not held up for the whole time; it then finds
// no connection and gets `false` without the driver having been reached. That
// `false` says nothing about the card: after a driver reset it is at stock,
// but when recover runs for another reason (a failed write, a fault outside
// the driver) the card may still be off stock, and the caller must treat it
// so.
bool GuardedGpu::Recover() {
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        Disconnect();
    }
    std::string why;   // of the last attempt
    for (int attempt = 0; attempt < kRecoverAttempts; ++attempt) {
        Sleep(kRecoverWaitMs);   // before the first attempt too: the driver needs a moment
        const std::lock_guard<std::mutex> lock(mutex_);
        Telemetry t;
        if (!Connect(&why)) {
        } else if (!inner_.read || !guarded_driver([&] { t = inner_.read(); })) {
            why = "the telemetry read faulted";
        } else if (!t.ok) {
            why = "NVML and NVAPI initialised, but the card's clock or temperature could not be read";
        } else if (!DeliverFanAuto()) {
            why = "handing the fans back to the driver faulted";
        } else {
            return true;
        }
        Disconnect();   // not back yet; never reuse a half-initialised library
    }
    fault_log("reconnecting to the driver failed " + std::to_string(kRecoverAttempts) + " times; the last time: " + why);
    return false;
}

// Only reads the flag under the lock: no driver call, no guard, and nothing
// else is called while the lock is held.
bool GuardedGpu::FanAutoOwed() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return fan_auto_owed_;
}

std::string GuardedGpu::GpuUuid() {
    return Call(std::string(), [this] { return nvml_->GpuUuid(gpu_); });
}

std::string GuardedGpu::DriverVersion() {
    return Call(std::string(), [this] { return nvml_->DriverVersion(); });
}

}
