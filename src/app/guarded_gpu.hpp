#pragma once
// Driver calls that cannot take the process down, and driver connections that
// can be re-created after a driver reset (TDR). Used by the optimize run; the
// tray app uses `guarded` for its own timers.
#include "core/types.hpp"
#include <memory>
#include <mutex>
#include <string>

namespace gao {
class Nvml;
class Nvapi;
}

namespace gao::app {

// Runs fn(ctx); false when it ended in an access violation, which is what a
// call into nvml.dll or nvapi64.dll does during or after a driver reset
// (hardware check 33). Structured exceptions, not C++ ones: a C++ exception
// passes through untouched. A function with __try may not hold destructible
// objects, hence the function pointer. Objects in the frames a fault skips are
// not destroyed (/EHsc): whatever they own is leaked, and that leak is the
// price of keeping the process alive. Other faults (a CFG or stack-cookie
// fast-fail, a stack overflow) still end the process.
bool guarded(void (*fn)(void*), void* ctx);
template <class F> bool guarded(F f) {
    return guarded([](void* p) { (*static_cast<F*>(p))(); }, &f);
}

// `guarded`, for calls into NVML or NVAPI. A fault there also marks NVML as
// never to be shut down in this process again (Nvml::Abandon): the call that
// faulted never left the library, and its shutdown would wait for it for
// ever. Which of the two libraries faulted is not known here; leaving NVML
// loaded when it was NVAPI costs nothing. The tray app then restarts itself.
bool guarded_driver(void (*fn)(void*), void* ctx);
// The access violation `guarded` last caught on this thread: what could not
// be read or written, then the faulting instruction and its callers, each as
// module+offset. Empty when it has caught none.
std::string last_fault();
// Where a fault in a driver call, and a reconnect that failed, are written
// down: one line each. Set once at startup (the apps use boot_log); without
// it nothing is written. A caught fault otherwise leaves no trace at all.
void set_fault_log(void (*sink)(const std::string&));
template <class F> bool guarded_driver(F f) {
    return guarded_driver([](void* p) { (*static_cast<F*>(p))(); }, &f);
}

// NVML and NVAPI for one GPU, behind a GpuControl whose callbacks run guarded
// and whose `recover` re-creates both libraries.
class GuardedGpu {
public:
    explicit GuardedGpu(unsigned gpu);
    ~GuardedGpu();
    GuardedGpu(const GuardedGpu&) = delete;              // control()'s callbacks point at this object
    GuardedGpu& operator=(const GuardedGpu&) = delete;

    // NVML + NVAPI; builds control(). Call once. False: `why` says what failed.
    bool Init(std::string* why);
    // Built once by Init and never reassigned, so references to it and copies
    // of its callbacks stay valid across a recover, for this object's
    // lifetime. It has the callbacks the card supported at Init, plus recover.
    // A call that faults returns its failure value (false, an empty Telemetry,
    // nullopt, {0, 0}), and so does every call after it until recover
    // succeeds: a library that just faulted is not called again. Such a
    // failure value means "not delivered", not "the card is at stock". A
    // set_fan_auto that did not succeed is remembered and delivered by the
    // next successful recover.
    const GpuControl& control() const { return outer_; }
    // From the current NVML instance, guarded; empty when it cannot be read.
    std::string GpuUuid();
    std::string DriverVersion();
    // True while a set_fan_auto is remembered as not delivered: the fans may
    // still be at a manual speed. No driver call. Takes the lock, so not for
    // use from inside a callback of control().
    bool FanAutoOwed() const;

private:
    bool Connect(std::string* why);
    void Disconnect();
    bool Recover();
    bool FanAuto();
    bool DeliverFanAuto();
    template <class R, class Fn> R Call(R fail, Fn fn);

    const unsigned gpu_;
    // One driver call at a time, and never one while the libraries are being
    // replaced: an emergency handler calls control() from another thread.
    mutable std::mutex mutex_;
    std::unique_ptr<Nvml> nvml_;     // never null; not initialised while disconnected
    std::unique_ptr<Nvapi> nvapi_;   // never null; not initialised while disconnected
    bool live_ = false;              // both initialised and no call has faulted since
    GpuControl inner_;               // make_gpu_control over the current libraries; empty while !live_
    GpuControl outer_;
    // A set_fan_auto that was refused (no connection) or did not succeed: the
    // fans may still be at a manual speed. Recover delivers it.
    bool fan_auto_owed_ = false;
};

}
