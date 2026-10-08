// GpuAutoOptimizer.exe: the window and the tray. Launched normally it shows
// the window; launched with --tray (by the logon task, elevated) it applies
// the saved tune and stays in the tray, where the watchdog keeps the tune
// applied for the rest of the session.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dbghelp.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <wrl/client.h>

#include "app/common.hpp"
#include "app/guarded_gpu.hpp"
#include "app/gui/ui.hpp"
#include "app/gui/worker.hpp"
#include "core/boot.hpp"
#include "core/fan_curve.hpp"
#include "core/version.hpp"
#include "core/watchdog.hpp"
#include "hw/app_files.hpp"
#include "hw/boot_task.hpp"
#include "hw/gpu_control.hpp"
#include "hw/nvapi.hpp"
#include "hw/nvml.hpp"
#include "hw/stress.hpp"

#include "imgui.h"
#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"

#include <algorithm>
#include <chrono>
#include <cwchar>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

using Microsoft::WRL::ComPtr;
using gao::app::guarded;
using gao::app::kGpu;

namespace {

constexpr wchar_t kWindowClass[] = L"GpuAutoOptimizerWindow";   // also used by gao --reset
constexpr wchar_t kInstanceMutex[] = L"Local\\GpuAutoOptimizer.Instance";
constexpr UINT WM_APP_TRAY = WM_APP + 1;   // tray icon callback
constexpr UINT WM_APP_WAKE = WM_APP + 2;   // the optimize worker has news
constexpr UINT WM_APP_SHOW = WM_APP + 3;   // a second launch asks us to come forward
constexpr UINT WM_APP_UPDATE = WM_APP + 4; // an update thread left news in g_update_news
// The running version, as a window property a second launch can read
// without opening this (elevated) process. Builds before 0.3.1 have none.
constexpr wchar_t kVersionProp[] = L"GpuAutoOptimizer.Version";
constexpr UINT_PTR kTimerTelemetry = 1;    // 1 s
constexpr UINT_PTR kTimerWatchdog = 2;     // 30 s
constexpr UINT_PTR kTimerStrike = 3;       // one-shot, 2 min after a logon apply
constexpr UINT_PTR kTimerUpdate = 4;       // 24 h: look for a new release again
enum MenuId : UINT { kMenuOpen = 1, kMenuReapply, kMenuRevert, kMenuExit };

struct App {
    HWND hwnd = nullptr;
    HANDLE instance_mutex = nullptr;
    UINT taskbar_created = 0;
    UINT tray_notice = 0;
    bool visible = false;
    bool exit_requested = false;
    int input_frames = 0;   // frames still to draw after input, so hover/click feedback shows

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<IDXGISwapChain> swapchain;
    ComPtr<ID3D11RenderTargetView> rtv;
    // Invariant: !device_lost implies device, ctx and swapchain are non-null
    // and the ImGui renderer backend is initialised (backend_exists()). Only
    // recover_device() clears the flag; on_telemetry() calls it, spaced out
    // as described below, until a new device exists. render() only ever sets it.
    bool device_lost = false;
    unsigned device_generation = 0;   // counts recover_device() and abandon_backend() runs
    // A recovery that keeps failing must not rebuild the device every second
    // for ever: each attempt can leak a device and write a log line. `recoveries`
    // counts recover_device() runs (up to 3) in the current run of failures. The
    // first two run at the telemetry tick, the third 5 s after the second, every
    // later one 30 s after the one before. The run ends (end_failure_run()) at a
    // frame that reached the screen with all its creations done, or when
    // on_telemetry() finds the device not lost 30 s after the last
    // recover_device(). A hidden window presents nothing, so only the second
    // ends its runs: resets that lie more than 30 s apart each start at the
    // telemetry tick again, resets closer together count as one run. Those 30 s
    // show that the device was created and not removed, not that it can draw.
    // Showing the window always lifts the wait for one attempt (show_window()),
    // whether or not the device is lost yet.
    int recoveries = 0;
    ULONGLONG recover_at = 0;      // no recover_device() before this tick count
    ULONGLONG recovered_at = 0;    // the tick count of the last recover_device()
    bool abandon_noted = false;    // the abandon note, once per such run of failures
    // The removed state of the window's device is the sign of a driver reset.
    // A device is asked until it says so once (removed_seen), and what it said
    // waits in driver_reset for on_telemetry(), which calls hw_lost().
    bool removed_seen = false;
    bool driver_reset = false;

    // Pointers: re-created after a driver reset (see hw_lost()).
    std::unique_ptr<gao::Nvml> nvml;
    std::unique_ptr<gao::Nvapi> nvapi;
    bool nvml_ok = false, nvapi_ok = false;
    bool hw_lost = false;            // re-create them at hw_retry_at
    ULONGLONG hw_retry_at = 0;
    gao::GpuControl gpu;

    gao::gui::UiState ui;
    std::unique_ptr<gao::gui::OptimizeWorker> worker;
    gao::Watchdog watchdog;
    bool watch = false;   // keep the saved tune applied (off after a revert or a reset by choice)
    bool watch_reset = false;   // a driver reset was seen since the watchdog last looked
    bool worker_was_running = false;
    bool strike_pending = false;   // a logon apply whose 2-minute grace has not passed yet
    std::unique_ptr<gao::FanDriver> fan;   // while the curve drives the fans
    bool told_about_tray = false;  // the "still running in the tray" balloon, once per session
    std::optional<gao::ReleaseInfo> update;   // the newer release on offer, if any
    bool restart = false;          // an update was installed: start the new copy on the way out

    // Crash dumps are written by a thread created up front (a crashing thread
    // may have no stack or heap left to do it itself).
    HANDLE dump_request = nullptr, dump_done = nullptr;
    EXCEPTION_POINTERS* crash_info = nullptr;
    DWORD crash_thread = 0;
};

App g;

// A check or a download must never stall the window, so both run on a thread
// of their own. The thread leaves what it found here and posts WM_APP_UPDATE.
struct UpdateNews {
    bool checked = false;                     // a check finished; `release` is its answer
    std::optional<gao::ReleaseInfo> release;
    bool installed = false;                   // an install finished; `ok` and `message` are its outcome
    bool ok = false;
    std::string message;
};
std::mutex g_update_mutex;
UpdateNews g_update_news;

// ---------------------------------------------------------------- crash dump

DWORD WINAPI dump_thread(void*) {
    WaitForSingleObject(g.dump_request, INFINITE);
    if (HMODULE dbghelp = LoadLibraryExW(L"dbghelp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
        using WriteDump = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                                        PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
        const auto write = reinterpret_cast<WriteDump>(GetProcAddress(dbghelp, "MiniDumpWriteDump"));
        const auto path = gao::app_dir() / (L"crash-" + std::to_wstring(GetTickCount64()) + L".dmp");
        const HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (write && f != INVALID_HANDLE_VALUE) {
            MINIDUMP_EXCEPTION_INFORMATION info{g.crash_thread, g.crash_info, FALSE};
            write(GetCurrentProcess(), GetCurrentProcessId(), f, MiniDumpNormal, &info, nullptr, nullptr);
        }
        if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    }
    SetEvent(g.dump_done);
    return 0;
}

LONG WINAPI on_crash(EXCEPTION_POINTERS* ep) {
    // Dump first: if the crash is inside the driver with a lock held, the reset
    // below could hang, and the dump is the evidence.
    g.crash_info = ep;
    g.crash_thread = GetCurrentThreadId();
    SetEvent(g.dump_request);
    WaitForSingleObject(g.dump_done, 15000);
    // Never leave a candidate applied. Through our own GpuControl, which lives
    // as long as the process; the worker's may be mid-teardown.
    if (g.gpu.set_fan_auto) g.gpu.set_fan_auto();
    if (g.worker && g.worker->running() && g.gpu.reset_to_stock) g.gpu.reset_to_stock();
    return EXCEPTION_EXECUTE_HANDLER;
}

// ---------------------------------------------------------------- rendering

void create_rtv() {
    ComPtr<ID3D11Texture2D> back;
    g.swapchain->GetBuffer(0, IID_PPV_ARGS(&back));
    g.device->CreateRenderTargetView(back.Get(), nullptr, &g.rtv);
}

bool create_device() {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = g.hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0};
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 1, D3D11_SDK_VERSION,
                                               &sd, &g.swapchain, &g.device, nullptr, &g.ctx);
    if (hr == DXGI_ERROR_UNSUPPORTED)
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 1, D3D11_SDK_VERSION, &sd,
                                           &g.swapchain, &g.device, nullptr, &g.ctx);
    if (FAILED(hr)) return false;
    create_rtv();
    return true;
}

void note(const std::string& text, bool warn);   // below, with the tray helpers

// Whether an ImGui renderer backend is initialised. Never g.ctx: a device can
// exist without a backend (a fault that left recover_device() between
// create_device() and Init), and Shutdown without a backend dereferences null.
bool backend_exists() { return ImGui::GetIO().BackendRendererUserData != nullptr; }

// When the device fails a creation, the DX11 backend leaves a texture
// half-made: its own struct in BackendUserData, but no view, so an invalid
// TexID (its checks are asserts, compiled out in Release). The backend's next
// teardown of that texture -- from ImGui_ImplDX11_NewFrame() when it has no
// vertex shader, or from ImGui_ImplDX11_Shutdown() -- then releases the null
// view and faults. That crashed v0.2.0 after a driver install.
//
// Resets those textures (all == false) or every texture (all == true) to
// "the backend has nothing for this", and returns how many it reset. Through
// public members only, as the backend's own destroy does it; SetStatus turns
// Destroyed into WantCreate at once for a texture ImGui still wants, so the
// backend creates it again on the next drawn frame. The backend's small
// struct is leaked, with whatever it holds (a texture without a view, or with
// all == true a complete texture): its layout is private to the backend.
//
// Called before every backend call that can tear a texture down: at the top of
// render(), before the Shutdown in recover_device() and before the one at exit.
int reset_textures(bool all) {
    int count = 0;
    for (ImTextureData* tex : ImGui::GetPlatformIO().Textures) {
        const bool half_made = tex->BackendUserData != nullptr && tex->GetTexID() == ImTextureID_Invalid;
        if (!all && !half_made) continue;
        tex->SetTexID(ImTextureID_Invalid);
        tex->BackendUserData = nullptr;
        tex->SetStatus(ImTextureStatus_Destroyed);
        ++count;
    }
    return count;
}

// Releases the window's device, in an order that lets the next one be created.
// Something leaked can keep the old device alive: the backend's references
// after an abandon, or a texture without a view that reset_textures() left
// behind. A released flip-model swap chain is destroyed only after a flush on
// its device's context, and until then a new swap chain on the same window can
// be refused: so the swap chain goes first, then the flush, then the context.
void release_device() {
    g.rtv.Reset();
    g.swapchain.Reset();
    if (g.ctx) guarded([] { g.ctx->ClearState(); g.ctx->Flush(); });
    g.ctx.Reset();
    g.device.Reset();
    g.removed_seen = false;   // the next device has not been asked yet
}

// Asks the window's device whether it was removed, and remembers a yes for
// on_telemetry(). Called while the device still exists: every telemetry tick,
// and by abandon_backend() before it releases the device, after which there
// is nothing left to ask. Guarded: the caller may be outside the timer's guard.
void check_removed() {
    if (!g.device || g.removed_seen) return;
    bool removed = false;
    guarded([&removed] { removed = g.device->GetDeviceRemovedReason() != S_OK; });
    if (!removed) return;
    g.removed_seen = true;
    g.driver_reset = true;
}

// After an access violation inside a renderer backend call: that backend is
// never called again, not even to shut it down. Everything
// ImGui_ImplDX11_Shutdown() resets outside its own data is reset here through
// public members -- including the context's texture list, which belongs to the
// ImGui context and not to one backend instance: a fresh Init alone would walk
// the same textures again on its first frame. The backend's data and the COM
// references it holds (device, context, factory, shaders, buffers, textures)
// are leaked. Leaves no device and g.device_lost set; recover_device() builds
// the next one. Does not call hw_lost(): that stays with on_telemetry(), which
// learns through check_removed() whether the device went with a driver reset.
// The note is not written on the way out (the exit path is reached only with
// g.exit_requested set): nothing is rebuilt then.
void abandon_backend() {
    g.device_lost = true;
    ++g.device_generation;
    reset_textures(true);
    ImGuiIO& io = ImGui::GetIO();
    io.BackendRendererUserData = nullptr;
    io.BackendRendererName = nullptr;
    io.BackendFlags &= ~(ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures);
    ImGui::GetPlatformIO().ClearRendererHandlers();
    check_removed();
    release_device();
    if (g.abandon_noted || g.exit_requested) return;
    note("The window's graphics device failed; rebuilding it.", true);
    g.abandon_noted = true;
}

// A run of failed recoveries is over: the next loss starts at the telemetry
// tick again, and the next abandon is noted again.
void end_failure_run() {
    g.recoveries = 0;
    g.recover_at = 0;
    g.abandon_noted = false;
}

// After a TDR the UI device is gone. Rebuild it; if that fails too (it can,
// right after a reset), on_telemetry() tries again, at the times the backoff
// allows. Leaves the invariant true or g.device_lost set -- also when a fault
// in here ends in the telemetry timer's guard instead of returning, hence the
// flag and the backoff are set first.
void recover_device() {
    g.device_lost = true;
    ++g.device_generation;
    if (g.recoveries < 3) ++g.recoveries;
    g.recovered_at = GetTickCount64();
    g.recover_at = g.recovered_at + (g.recoveries < 2 ? 0 : g.recoveries == 2 ? 5000 : 30000);
    if (backend_exists()) {
        reset_textures(false);
        if (!guarded([] { ImGui_ImplDX11_Shutdown(); })) abandon_backend();
    }
    release_device();
    if (!create_device()) return;
    // Init dereferences a null device when its DXGI queries fail.
    if (!guarded([] { ImGui_ImplDX11_Init(g.device.Get(), g.ctx.Get()); })) {
        abandon_backend();   // releases the device just created as well
        return;
    }
    g.device_lost = false;
}

// ---------------------------------------------------------------- tray

void tray_icon(DWORD message, const wchar_t* tip = nullptr, const wchar_t* balloon = nullptr) {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g.hwnd;
    nid.uID = 1;   // hWnd + uID, not a GUID: an unsigned exe that may move
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    nid.uCallbackMessage = WM_APP_TRAY;
    nid.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1));
    if (!nid.hIcon) nid.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));   // IDI_APPLICATION
    wcsncpy_s(nid.szTip, tip ? tip : L"GPU Auto Optimizer", _TRUNCATE);
    if (balloon) {
        nid.uFlags |= NIF_INFO;
        wcsncpy_s(nid.szInfoTitle, L"GPU Auto Optimizer", _TRUNCATE);
        wcsncpy_s(nid.szInfo, balloon, _TRUNCATE);
        nid.dwInfoFlags = NIIF_INFO | NIIF_RESPECT_QUIET_TIME;
    }
    // At logon the taskbar may not exist yet when the first NIM_ADD runs: a
    // failed modify means the icon is missing, so add it.
    if (!Shell_NotifyIconW(message, &nid) && message == NIM_MODIFY) {
        message = NIM_ADD;
        Shell_NotifyIconW(NIM_ADD, &nid);
    }
    if (message == NIM_ADD) {
        nid.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &nid);
    }
}

// A line in the window's log for this session.
void note(const std::string& text, bool warn = false) {
    g.ui.notes.push_back({gao::app::now_text(), text, warn});
    if (g.ui.notes.size() > 100) g.ui.notes.erase(g.ui.notes.begin());
}

// A tray balloon. Pass log=false when the event is already in boot.log,
// which the window's log shows too.
void notify(const std::string& text, bool log = true) {
    if (log) note(text, true);
    tray_icon(NIM_MODIFY, nullptr, gao::widen(text).c_str());
}

void show_window() {
    ShowWindow(g.hwnd, SW_SHOW);
    ShowWindow(g.hwnd, SW_RESTORE);
    SetForegroundWindow(g.hwnd);
    g.visible = true;
    g.input_frames = 3;
    // Always lift the wait, lost or not: a first frame that fails right after
    // the window opens sets device_lost in render(), and the old wait must not
    // keep it undrawn. The next telemetry tick then tries at once. Only
    // on_telemetry() reads recover_at, and only while the device is lost, so
    // this costs nothing otherwise; each attempt sets the wait again, so
    // repeated clicks give at most one attempt per tick.
    g.recover_at = 0;
}

void tray_menu(int x, int y) {
    const bool busy = g.worker->running() || gao::app::tuning_in_progress();
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, kMenuOpen, L"Open");
    if (g.ui.elevated) {
        // Writing the GPU under a running search would corrupt its probes.
        AppendMenuW(menu, MF_STRING | (g.ui.profile && !busy ? 0 : MF_GRAYED), kMenuReapply, L"Re-apply saved tune");
        AppendMenuW(menu, MF_STRING | (busy ? MF_GRAYED : 0), kMenuRevert, L"Revert to stock");
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuExit, L"Exit");
    SetForegroundWindow(g.hwnd);   // or the menu does not close when clicking elsewhere
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, x, y, 0, g.hwnd, nullptr);
    PostMessageW(g.hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

// ---------------------------------------------------------------- state

// Fans back to the driver and forget the curve driver. Safe to call anywhere.
void fan_release() {
    if (g.fan) g.fan->release();
    g.fan.reset();
    g.ui.fan_state = {};
}

// Starts, updates or stops the curve driver to match gao.json.
void fan_sync() {
    const gao::Config cfg = gao::app::load_config();
    const auto curve = gao::active_fan_curve(cfg);
    g.ui.fan_available = g.ui.elevated && g.nvml_ok && static_cast<bool>(g.gpu.set_fan_pct);
    g.ui.fan_control = cfg.fan_control;
    g.ui.fan_curve = curve;
    g.ui.fan_tested = cfg.profile ? cfg.profile->fan_curve : std::nullopt;
    g.ui.fan_min_pct = gao::fan_min_for(cfg, g.nvml_ok ? g.nvml->GpuUuid(kGpu) : std::string(), g.gpu.fan_min_pct);
    g.ui.fan_max_temp_c = cfg.profile ? gao::objectives_for(cfg.profile->preset).max_temp_c : 75;
    // A running search drives the fans itself: never take them over mid-run.
    if (g.worker && g.worker->running()) return;
    if (!g.ui.fan_available || !cfg.fan_control || !curve || gao::app::tuning_in_progress()) {
        fan_release();
        return;
    }
    if (g.fan) {
        g.fan->set_curve(*curve, g.ui.fan_max_temp_c);
        return;
    }
    // Fans still manual from a killed earlier instance are ours to take back,
    // not another program's: start from driver control.
    if (g.gpu.set_fan_auto) g.gpu.set_fan_auto();
    g.fan = std::make_unique<gao::FanDriver>(g.gpu, *curve, g.ui.fan_max_temp_c, g.ui.fan_min_pct);
}

// The curve as the saved undervolt sees it; empty without one, or when it
// cannot be read. Reading needs no administrator rights.
std::optional<gao::CurveState> read_curve_state(const gao::Config& cfg) {
    if (!cfg.profile || !cfg.profile->undervolt || !g.gpu.read_vf_curve) return std::nullopt;
    const auto points = g.gpu.read_vf_curve();
    if (!points) return std::nullopt;
    return gao::curve_state(*points, cfg.profile->undervolt->volt_uv);
}

void refresh_status(bool with_task) {
    g.ui.elevated = gao::app::is_elevated();
    const gao::Config cfg = gao::app::load_config();
    g.ui.profile = cfg.profile;
    g.ui.strikes = cfg.boot_strikes;
    g.ui.update_check = cfg.update_check;
    if (g.nvml_ok) {
        g.ui.driver = g.nvml->DriverVersion();
        const std::string gpu_id = g.nvml->GpuUuid(kGpu);
        g.ui.profile_driver_ok = cfg.profile && !g.ui.driver.empty() && g.ui.driver == cfg.profile->driver;
        g.ui.profile_gpu_ok = cfg.profile && !gpu_id.empty() && gpu_id == cfg.profile->gpu;
    }
    if (g.gpu.read_applied) g.ui.applied = g.gpu.read_applied();
    // Not under a running search: it writes the curve from its own thread.
    if (!(g.worker && g.worker->running()) && !gao::app::tuning_in_progress()) g.ui.curve = read_curve_state(cfg);
    const auto log = gao::read_lines(gao::boot_log_path());
    g.ui.boot_log.clear();
    if (log) g.ui.boot_log.assign(log->size() > 200 ? log->end() - 200 : log->begin(), log->end());
    if (with_task) g.ui.boot_on = gao::boot_task_exists();
    fan_sync();
}

// ---------------------------------------------------------------- actions

void hw_lost();   // below, with the other hardware helpers

void act_optimize(gao::Preset preset) {
    fan_release();   // the search drives the fans itself
    const auto fan = g.ui.optimize_fan ? std::optional<gao::FanCurve>(gao::fan_preset_curve(*g.ui.optimize_fan)) : std::nullopt;
    g.worker->start(preset, fan);   // the watchdog skips while it runs
}

void act_abort() { g.worker->abort(); }

void act_restart_elevated() {
    wchar_t self[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, self, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;
    SHELLEXECUTEINFOW sei{sizeof(sei)};
    sei.lpVerb = L"runas";
    sei.lpFile = self;
    sei.nShow = SW_SHOWNORMAL;
    // The elevated copy must not find our single-instance mutex: release it
    // first, and take it back if elevation is cancelled.
    CloseHandle(g.instance_mutex);
    g.instance_mutex = nullptr;
    if (ShellExecuteExW(&sei)) {
        g.exit_requested = true;
    } else {
        g.instance_mutex = CreateMutexW(nullptr, TRUE, kInstanceMutex);
        note("Elevation was cancelled.");
    }
}

void start_update_check() {
    if (!g.ui.update_check) return;
    std::thread([hwnd = g.hwnd] {
        auto check = gao::app::check_for_update();   // a failed check is not news: it is tried again tomorrow
        {
            const std::lock_guard lock(g_update_mutex);
            g_update_news.checked = true;
            g_update_news.release = std::move(check.release);
        }
        PostMessageW(hwnd, WM_APP_UPDATE, 0, 0);
    }).detach();
}

void act_install_update() {
    if (!g.update || g.ui.update_busy) return;
    if (g.worker->running() || gao::app::tuning_in_progress()) {
        note("An optimize run is in progress; update when it has finished.", true);
        return;
    }
    g.ui.update_busy = true;
    g.ui.update_error.clear();
    std::thread([hwnd = g.hwnd, release = *g.update] {
        std::string message;
        const bool ok = gao::app::install_update(release, &message);
        {
            const std::lock_guard lock(g_update_mutex);
            g_update_news.installed = true;
            g_update_news.ok = ok;
            g_update_news.message = message;
        }
        PostMessageW(hwnd, WM_APP_UPDATE, 0, 0);
    }).detach();
}

// On the window's thread: what an update thread left behind.
void on_update_news() {
    UpdateNews news;
    {
        const std::lock_guard lock(g_update_mutex);
        news = std::move(g_update_news);
        g_update_news = {};
    }
    if (news.checked) {
        g.update = std::move(news.release);
        g.ui.update_version = g.update ? g.update->version : std::string();
    }
    if (news.installed) {
        g.ui.update_busy = false;
        if (news.ok) {   // the files are replaced: leave, and start the new copy on the way out
            g.restart = true;
            g.exit_requested = true;
        } else {
            g.ui.update_error = news.message;
            note("The update failed: " + news.message, true);
        }
    }
    g.input_frames = std::max(g.input_frames, 2);
}

bool refuse_while_tuning() {
    if (g.worker->running()) return true;
    if (!gao::app::tuning_in_progress()) return false;
    note("An optimize is running on the command line; wait for it to finish.", true);
    return true;
}

void act_apply() {
    if (refuse_while_tuning()) return;
    if (!g.nvml_ok || !g.nvapi_ok) { note("The NVIDIA driver is not available right now.", true); return; }
    std::string why;
    if (!gao::app::prepare_state(&why)) { note(why, true); return; }
    gao::Config cfg = gao::app::load_config();
    cfg.boot_strikes = 0;   // strikes only gate the logon apply
    const std::string driver = g.nvml_ok ? g.nvml->DriverVersion() : std::string();
    const auto d = gao::decide_boot(cfg, driver, g.nvml_ok ? g.nvml->GpuUuid(kGpu) : std::string());
    if (d != gao::BootDecision::Apply) { note("Not applied: " + gao::app::decision_text(d, cfg, driver), true); return; }
    if (!gao::apply_profile(g.gpu, *cfg.profile, &why)) { note("Not applied: " + why, true); return; }
    g.watch = true;
    g.watchdog = gao::Watchdog();
    note("Applied " + gao::app::profile_text(*cfg.profile));
    refresh_status(false);
}

void act_revert() {
    if (refuse_while_tuning()) return;
    if (!g.nvml_ok || !g.nvapi_ok) { note("The NVIDIA driver is not available right now.", true); return; }
    const bool ok = g.gpu.reset_to_stock && g.gpu.reset_to_stock();
    g.watch = false;   // stock by choice: the watchdog must not undo it
    note(ok ? "Back at stock. The saved tune is not re-applied until you apply it again."
            : "Reset to stock FAILED; try gao --reset.",
         !ok);
    refresh_status(false);
}

void act_boot(bool on) {
    std::string message;
    const bool ok = on ? gao::app::enable_boot(&message) : gao::app::disable_boot(&message);
    note(message, !ok);
    refresh_status(true);
}

// One frame. Called only while the invariant holds (!g.device_lost). Never
// rebuilds the device: it marks it lost and returns, and on_telemetry()
// rebuilds it on its next tick. The renderer backend calls run guarded;
// draw_ui never does: it runs the action callbacks, which call NVML and NVAPI
// and hold destructible objects, and a guard there would hide any UI bug as a
// silent retry. Both guarded regions lie outside the ImGui frame.
void render() {
    if (g.device->GetDeviceRemovedReason() != S_OK) {
        g.device_lost = true;
        return;
    }
    // A half-made texture means the previous frame's creations failed. Reset
    // it before the backend can meet it, and replace the device: one whose
    // creations fail while Present succeeds would otherwise give a blank
    // window for good.
    if (reset_textures(false) > 0) {
        g.device_lost = true;
        return;
    }
    if (!g.rtv) create_rtv();   // a WM_SIZE whose resize failed left none
    if (!g.rtv) {
        g.device_lost = true;
        return;
    }
    if (!guarded([] { ImGui_ImplDX11_NewFrame(); })) {
        abandon_backend();   // before any ImGui frame is opened
        return;
    }
    const unsigned generation = g.device_generation;
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    gao::gui::UiActions act;
    act.optimize = act_optimize;
    act.abort = act_abort;
    act.restart_elevated = act_restart_elevated;
    act.apply_profile = act_apply;
    act.revert_to_stock = act_revert;
    act.set_boot = act_boot;
    act.set_fan_curve = [](const gao::FanCurve& curve) {
        if (!gao::valid(curve)) return;
        gao::Config cfg = gao::app::load_config();
        cfg.fan_curve = curve;
        if (!gao::app::save_config(cfg)) note("Could not save the fan curve.", true);
        refresh_status(false);
    };
    act.set_fan_control = [](bool on) {
        gao::Config cfg = gao::app::load_config();
        cfg.fan_control = on;
        if (!gao::app::save_config(cfg)) note("Could not save the fan setting; nothing changed.", true);
        else note(on ? "Fan curve on." : "Fan curve off; the NVIDIA driver controls the fans.");
        refresh_status(false);
    };
    act.reset_fan_curve = [] {
        gao::Config cfg = gao::app::load_config();
        cfg.fan_curve.reset();
        if (!gao::app::save_config(cfg)) note("Could not save the fan curve.", true);
        refresh_status(false);
    };
    act.install_update = act_install_update;
    act.set_update_check = [](bool on) {
        gao::Config cfg = gao::app::load_config();
        cfg.update_check = on;
        if (!gao::app::save_config(cfg)) {
            note(g.ui.elevated ? "Could not save the setting." : "Saving this setting needs administrator rights.", true);
            return;
        }
        refresh_status(false);
        if (on) start_update_check();
        else { g.update.reset(); g.ui.update_version.clear(); }
    };
    act.detect_gpu = [] {   // e.g. after a driver update: re-create NVML and NVAPI now
        if (refuse_while_tuning()) return;
        hw_lost();
        g.hw_retry_at = 0;
    };
    gao::gui::draw_ui(g.ui, g.worker->snapshot(), act);
    ImGui::Render();
    // A callback that pumps messages (the elevation prompt) lets the telemetry
    // timer run inside draw_ui. If its recover_device() failed there is no
    // device; if it succeeded, the new backend has not had its
    // ImGui_ImplDX11_NewFrame() and has no shaders or buffers yet. Either way
    // this frame is not drawn; the next one starts properly.
    if (g.device_lost || g.device_generation != generation) return;
    HRESULT hr = E_FAIL;
    const bool drawn = guarded([&hr] {
        const float clear[4] = {0.08f, 0.08f, 0.10f, 1.0f};
        g.ctx->OMSetRenderTargets(1, g.rtv.GetAddressOf(), nullptr);
        g.ctx->ClearRenderTargetView(g.rtv.Get(), clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        hr = g.swapchain->Present(1, 0);
    });
    if (!drawn) {
        abandon_backend();
        return;
    }
    if (FAILED(hr)) {   // any failure, not only a removed device
        g.device_lost = true;
        return;
    }
    // The frame reached the screen. That alone does not show a working device:
    // one whose creations fail can still present. Only a frame that also left
    // no texture half-made ends the run of failed recoveries and its backoff.
    if (reset_textures(false) > 0) {
        g.device_lost = true;
        return;
    }
    end_failure_run();
}

// ---------------------------------------------------------------- hardware

// A driver reset (TDR) can leave NVML and NVAPI state in this long-running
// process stale, and a call during the reset has faulted inside nvml.dll
// (hardware check 33). The tray exists to survive exactly that: its periodic
// hardware work runs guarded (app/guarded_gpu.hpp), and after a reset the
// libraries are re-created.

void init_hw() {
    g.nvml = std::make_unique<gao::Nvml>();
    g.nvapi = std::make_unique<gao::Nvapi>();
    g.nvml_ok = g.nvml->Init();
    g.nvapi_ok = g.nvapi->Init();
    g.gpu = g.nvml_ok && g.nvapi_ok ? gao::make_gpu_control(*g.nvml, *g.nvapi, kGpu) : gao::GpuControl{};
}

// Stop using the libraries and re-create them a few seconds from now, when
// the driver is back. The watchdog then finds the tune gone and re-applies it.
void hw_lost() {
    // Never call into a library that may just have faulted; after a reset the
    // driver owns the fans again anyway.
    g.fan.reset();
    g.ui.fan_state = {};
    g.gpu = {};
    g.nvml_ok = g.nvapi_ok = false;
    g.hw_lost = true;
    g.hw_retry_at = GetTickCount64() + 5000;
    // Shutting down a library that just faulted may fault again; then leave it.
    gao::Nvml* nvml = g.nvml.release();
    gao::Nvapi* nvapi = g.nvapi.release();
    guarded([nvml] { delete nvml; });
    guarded([nvapi] { delete nvapi; });
}

void retry_hw() {
    if (!g.hw_lost || GetTickCount64() < g.hw_retry_at) return;
    if (!guarded([] { init_hw(); }) || !g.nvml_ok || !g.nvapi_ok) {
        hw_lost();   // not back yet
        return;
    }
    g.hw_lost = false;
    refresh_status(false);
}

// ---------------------------------------------------------------- timers

void on_telemetry() {
    // A removed UI device is the first sign of a driver reset; the window is
    // often hidden, so no Present() would report it. Read from the old device,
    // before recover_device() replaces it; abandon_backend() has read it from
    // a device it released since the last tick. A removed device is lost at
    // once, also while the backoff holds its replacement back. (While a failed
    // creation is backed off there is no device to ask: a second driver reset
    // in that gap is not seen here and hw_lost() is not called for it; stale
    // NVML/NVAPI state is then left to the guards around the timers and to the
    // watchdog.)
    check_removed();
    const bool reset = g.driver_reset;
    g.driver_reset = false;
    if (reset) g.device_lost = true;
    if (reset) g.watch_reset = true;   // for the watchdog: a reset with the tune applied counts against it
    if (g.device_lost && GetTickCount64() >= g.recover_at) recover_device();
    // A device that is still there 30 s after it was built ends the run of
    // failures. Only recover_device() clears g.device_lost, so not lost now
    // means not lost since. This is the only end a hidden window has; a
    // visible one whose device fails at its first frame never gets here.
    if (!g.device_lost && g.recoveries > 0 && GetTickCount64() - g.recovered_at >= 30000) end_failure_run();
    if (reset && !g.hw_lost) hw_lost();
    retry_hw();
    if (g.nvml_ok) {
        g.ui.telemetry = g.nvml->Read(kGpu);
        gao::gui::push_history(g.ui.temp_history, static_cast<float>(std::max(g.ui.telemetry.temp_c, 0)));
        gao::gui::push_history(g.ui.power_history, static_cast<float>(std::max(g.ui.telemetry.power_w, 0)));
    }
    // The curve, while nothing else owns the fans: a search drives them itself.
    if (g.fan && !g.worker->running() && !gao::app::tuning_in_progress()) {
        const gao::FanMode before = g.fan->state().mode;
        g.ui.fan_state = g.fan->tick(g.ui.telemetry.temp_c, g.ui.telemetry.power_w, std::chrono::steady_clock::now());
        if (g.ui.fan_state.min_pct > g.ui.fan_min_pct && g.ui.fan_state.mode == gao::FanMode::Curve) {
            // The fans stalled at the old minimum: remember the new one for this card.
            g.ui.fan_min_pct = g.ui.fan_state.min_pct;
            gao::Config cfg = gao::app::load_config();
            cfg.fan_min_pct = g.ui.fan_min_pct;
            cfg.fan_min_gpu = g.nvml->GpuUuid(kGpu);
            if (!gao::app::save_config(cfg)) note("Could not save the learned fan minimum.", true);
            note("The fans stalled below " + std::to_string(g.ui.fan_min_pct) + " %; that is their minimum from now on.");
        }
        if (g.ui.fan_state.mode != before && g.ui.fan_state.mode == gao::FanMode::Failed)
            notify("A fan speed did not verify, so the NVIDIA driver controls the fans again. Fan control is off until the app restarts.");
        if (g.ui.fan_state.mode != before && g.ui.fan_state.mode == gao::FanMode::Foreign)
            notify("Another program (Afterburner, NVIDIA App...) set the fan speed. Leaving the fans alone.");
    }
    if (g.gpu.read_applied) g.ui.applied = g.gpu.read_applied();
    wchar_t tip[128];
    const gao::Telemetry& t = g.ui.telemetry;
    swprintf_s(tip, L"GPU Auto Optimizer\n%d C, %d MHz%s", t.temp_c, t.core_mhz, g.watch ? L", tune kept applied" : L"");
    tray_icon(NIM_MODIFY, tip);
    // A finished optimize run: its result is applied, so watch it from now on.
    const bool running = g.worker->running();
    if (g.worker_was_running && !running) {
        const auto snap = g.worker->snapshot();
        // A run that never started left the GPU alone: keep watching as before.
        // One that ran ends either at its saved result or at stock.
        if (snap.ran()) {
            g.watch = snap.kept();
            g.watchdog = gao::Watchdog();
            g.watch_reset = false;   // the search's own resets are not the result's
        }
        refresh_status(false);
    }
    g.worker_was_running = running;
}

void on_watchdog() {
    // Never under a running search -- ours, or gao --optimize in a shell. A
    // search resets the driver on purpose: those resets say nothing about
    // the saved tune.
    if (!g.watch || g.worker->running() || gao::app::tuning_in_progress()) {
        g.watch_reset = false;
        return;
    }
    // Right after a driver reset the libraries are not back yet: the reset
    // that was seen waits for the tick that can look at the card.
    if (!g.ui.elevated || !g.nvml_ok || !g.nvapi_ok) return;
    const gao::Config cfg = gao::app::load_config();
    if (!cfg.profile) return;
    const auto applied = g.gpu.read_applied();
    if (!applied) {   // NVAPI handles go stale after a driver reset: re-create
        hw_lost();
        return;
    }
    g.ui.curve = read_curve_state(cfg);
    const bool driver_reset = g.watch_reset;
    g.watch_reset = false;
    const std::string driver = g.nvml->DriverVersion(), gpu_id = g.nvml->GpuUuid(kGpu);
    const auto action = g.watchdog.check(*cfg.profile, applied, !driver.empty() && driver == cfg.profile->driver,
                                         !gpu_id.empty() && gpu_id == cfg.profile->gpu, std::chrono::steady_clock::now(),
                                         g.ui.curve, driver_reset);
    switch (action) {
        case gao::WatchAction::None: return;
        case gao::WatchAction::Reapply: {
            std::string why;
            if (gao::apply_profile(g.gpu, *cfg.profile, &why)) {
                gao::app::boot_log("watchdog: the tune had been reset (driver reset or TDR); re-applied");
                notify("The tune had been reset (driver reset or TDR) and was re-applied.", false);
            } else {
                gao::app::boot_log("watchdog: re-apply failed: " + why);
                notify("The tune had been reset and could not be re-applied: " + why, false);
            }
            break;
        }
        case gao::WatchAction::GiveUpUnstable: {
            // The driver may have kept all or part of the tune: stock, not whatever is left.
            const bool stock = g.gpu.reset_to_stock && g.gpu.reset_to_stock();
            g.watch = false;
            gao::app::boot_log(std::string("watchdog: 4 resets within an hour; stopped re-applying, ") +
                               (stock ? "card at stock" : "reset to stock FAILED, run `gao --reset`"));
            notify(std::string("The tune was reset, or the driver reset with it applied, 4 times within an hour, which usually "
                               "means it is not stable. ") +
                       (stock ? "The card is back at stock" : "Setting the card to stock FAILED (run gao --reset)") +
                       " and the tune is not re-applied; optimize again.",
                   false);
            break;
        }
        case gao::WatchAction::BackOffForeign:
            gao::app::boot_log("watchdog: another program changed the GPU settings; leaving them alone");
            notify("Another program (Afterburner, NVIDIA App...) changed the GPU settings. Leaving them alone.", false);
            break;
        case gao::WatchAction::NotifyDriverChanged:
            notify("The NVIDIA driver or the card changed since the tune was made. Optimize again to tune for it.");
            break;
    }
    refresh_status(false);
}

// ---------------------------------------------------------------- window

// The strike counts crashes during the first 2 minutes after a logon apply.
// A clean exit or logoff in that time is not a crash: clear it then too.
void clear_strike() {
    if (!g.strike_pending) return;
    g.strike_pending = false;
    KillTimer(g.hwnd, kTimerStrike);
    gao::app::clear_boot_strike();
}

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return 1;
    if (msg == g.taskbar_created && g.taskbar_created) {   // Explorer restarted: the icon is gone
        tray_icon(NIM_ADD);
        return 0;
    }
    if (msg == g.tray_notice && g.tray_notice) {   // gao --reset / gao --apply in a shell
        if (wp == static_cast<WPARAM>(gao::app::TrayNotice::TuneApplied)) {
            g.watch = true;
            g.watchdog = gao::Watchdog();
            note("Applied from the command line; kept applied from now on.");
        } else {
            g.watch = false;
            note("Set to stock from the command line; not re-applied until you apply it again.", true);
        }
        refresh_status(false);
        return 0;
    }
    switch (msg) {
        case WM_SIZE:
            if (wp == SIZE_MINIMIZED) {
                g.visible = false;
            } else {
                g.visible = IsWindowVisible(hwnd) != FALSE;
                if (g.swapchain) {
                    g.rtv.Reset();
                    g.swapchain->ResizeBuffers(0, LOWORD(lp), HIWORD(lp), DXGI_FORMAT_UNKNOWN, 0);
                    create_rtv();
                }
                g.input_frames = 2;
            }
            return 0;
        case WM_DPICHANGED: {
            gao::gui::apply_style(HIWORD(wp) / 96.0f);
            const RECT* r = reinterpret_cast<const RECT*>(lp);
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_CLOSE:
            // Closing hides to the tray, like other tray apps; Exit in the tray
            // menu quits. Say so the first time, or it looks like a quit.
            ShowWindow(hwnd, SW_HIDE);
            g.visible = false;
            if (!g.told_about_tray) {
                g.told_about_tray = true;
                tray_icon(NIM_MODIFY, nullptr, L"Still running in the tray. Right-click the icon and choose Exit to quit.");
            }
            return 0;
        case WM_TIMER:
            if (wp == kTimerTelemetry) { if (!guarded([] { on_telemetry(); })) hw_lost(); }
            else if (wp == kTimerWatchdog) { if (!guarded([] { on_watchdog(); })) hw_lost(); }
            else if (wp == kTimerStrike) { clear_strike(); refresh_status(false); }
            else if (wp == kTimerUpdate) start_update_check();
            return 0;
        case WM_APP_WAKE:
            g.input_frames = std::max(g.input_frames, 1);
            return 0;
        case WM_APP_SHOW:
            show_window();
            return 0;
        case WM_APP_UPDATE:
            on_update_news();
            return 0;
        case WM_APP_TRAY:
            switch (LOWORD(lp)) {
                case WM_CONTEXTMENU: tray_menu(static_cast<short>(LOWORD(wp)), static_cast<short>(HIWORD(wp))); break;
                case NIN_SELECT:
                case NIN_KEYSELECT:
                case NIN_BALLOONUSERCLICK: show_window(); break;
            }
            return 0;
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case kMenuOpen: show_window(); break;
                case kMenuReapply: act_apply(); break;
                case kMenuRevert: act_revert(); break;
                case kMenuExit:
                    g.exit_requested = true;
                    if (g.worker->running()) g.worker->abort();   // it restores stock, then we exit
                    break;
            }
            return 0;
        case WM_QUERYENDSESSION:
            // Logoff or shutdown in the middle of a run: stop it (it restores stock).
            if (g.worker->running()) g.worker->abort();
            return TRUE;
        case WM_ENDSESSION:
            if (wp) {   // the session really ends: stock now, the run cannot finish
                clear_strike();
                fan_release();
                if (g.worker->running() && g.gpu.reset_to_stock) g.gpu.reset_to_stock();
                // A run drives the fans itself (g.fan is empty then): hand them back too.
                if (g.worker->running() && g.gpu.set_fan_auto) g.gpu.set_fan_auto();
            }
            return 0;
        case WM_POWERBROADCAST:
            if (wp == PBT_APMSUSPEND) {   // never sleep with a manual speed, ours or a run's
                fan_release();
                if (g.worker->running() && g.gpu.set_fan_auto) g.gpu.set_fan_auto();
            }
            else if (wp == PBT_APMRESUMEAUTOMATIC) refresh_status(false);   // take the curve up again
            return TRUE;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    if (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) g.input_frames = 3;
    if (msg >= WM_KEYFIRST && msg <= WM_KEYLAST) g.input_frames = 3;
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// One instance only: two watchdogs would fight. A second launch brings the
// first forward. An elevated first instance owns the mutex with a DACL a
// normal user cannot open: access denied also means "already running".
// The copy in the tray is older than this one. It usually runs elevated (the
// logon task starts it), so only an elevated process can make it leave: ask,
// and on a yes start this program again with --replace and administrator
// rights. True when that copy was started; this one then just exits.
bool offer_replace(unsigned theirs) {
    std::wstring text = L"An older version of GPU Auto Optimizer is running in the tray";
    if (theirs) text += L" (" + std::to_wstring(theirs >> 16) + L"." + std::to_wstring((theirs >> 8) & 255) + L"." +
                        std::to_wstring(theirs & 255) + L")";
    text += L".\n\nReplace it with version " + gao::widen(std::string(gao::kVersion)) +
            L"? Windows asks for administrator rights, because the running app has them.";
    if (MessageBoxW(nullptr, text.c_str(), L"GPU Auto Optimizer", MB_YESNO | MB_ICONQUESTION | MB_SETFOREGROUND) != IDYES) return false;
    wchar_t self[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, self, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return false;
    SHELLEXECUTEINFOW sei{sizeof(sei)};
    sei.lpVerb = L"runas";
    sei.lpFile = self;
    sei.lpParameters = L"--replace";
    sei.nShow = SW_SHOWNORMAL;
    return ShellExecuteExW(&sei) != FALSE;
}

// --replace: the copy that runs in the tray makes way for this one. It is
// asked to exit the way its own tray menu does; every released version has
// that command, hands the fans back on it and, in the middle of an optimize
// run, stops the run and restores stock first. Only a copy that does not go
// while no run is under way is ended.
void end_other_instance() {
    const HWND other = FindWindowW(kWindowClass, nullptr);
    if (!other) return;
    DWORD pid = 0;
    GetWindowThreadProcessId(other, &pid);
    const HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, pid);
    PostMessageW(other, WM_COMMAND, kMenuExit, 0);
    if (!process) { Sleep(3000); return; }
    if (WaitForSingleObject(process, 20000) != WAIT_OBJECT_0 && !gao::app::tuning_in_progress()) {
        TerminateProcess(process, 0);
        WaitForSingleObject(process, 5000);
    }
    CloseHandle(process);
}

bool claim_single_instance(bool tray_mode, bool may_offer) {
    g.instance_mutex = CreateMutexW(nullptr, TRUE, kInstanceMutex);
    const DWORD err = GetLastError();
    if (g.instance_mutex && err != ERROR_ALREADY_EXISTS) return true;
    if (HWND other = FindWindowW(kWindowClass, nullptr); other && !tray_mode) {
        // 0: a build from before 0.3.1, which did not say its version.
        const auto theirs = static_cast<unsigned>(reinterpret_cast<UINT_PTR>(GetPropW(other, kVersionProp)));
        if (may_offer && theirs < gao::kVersionNumber && offer_replace(theirs)) return false;
        DWORD pid = 0;
        GetWindowThreadProcessId(other, &pid);
        AllowSetForegroundWindow(pid);   // or its SetForegroundWindow is refused
        PostMessageW(other, WM_APP_SHOW, 0, 0);
    }
    return false;
}

}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmdline, int) {
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);
    const bool tray_mode = cmdline && std::wcsstr(cmdline, L"--tray");
    // --replace: take over from an older copy in the tray. --resume: this is
    // the new copy after an update. Both carry on where the copy before them
    // was: a tune that is applied stays watched.
    const bool replace = cmdline && std::wcsstr(cmdline, L"--replace");
    const bool resume = replace || (cmdline && std::wcsstr(cmdline, L"--resume"));
    if (replace) end_other_instance();
    if (!claim_single_instance(tray_mode, !replace)) {
        if (replace)
            MessageBoxW(nullptr, L"The running app is still busy (an optimize run?). Try again when it has finished.",
                        L"GPU Auto Optimizer", MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
        return 0;
    }

    g.dump_request = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g.dump_done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    CloseHandle(CreateThread(nullptr, 0, dump_thread, nullptr, 0, nullptr));
    SetUnhandledExceptionFilter(on_crash);

    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));   // IDC_ARROW
    wc.lpszClassName = kWindowClass;
    RegisterClassExW(&wc);
    g.hwnd = CreateWindowW(kWindowClass, L"GPU Auto Optimizer", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1180, 860,
                           nullptr, nullptr, inst, nullptr);
    if (!g.hwnd || !create_device()) return 1;
    SetPropW(g.hwnd, kVersionProp, reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(gao::kVersionNumber)));
    const float scale = GetDpiForWindow(g.hwnd) / 96.0f;   // the monitor the window actually opened on
    SetWindowPos(g.hwnd, nullptr, 0, 0, static_cast<int>(1180 * scale), static_cast<int>(860 * scale),
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    const BOOL dark = TRUE;   // a dark title bar to match the dark window
    DwmSetWindowAttribute(g.hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));

    // Explorer runs unelevated; let its tray notifications and a relaunch's
    // "come forward" reach an elevated window. Not the command-line notices:
    // gao --reset and --apply are always elevated, and an unelevated process
    // must not be able to switch the watchdog on or off.
    g.taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
    g.tray_notice = gao::app::tray_notice_message();
    for (UINT m : {WM_APP_TRAY, g.taskbar_created, WM_APP_SHOW})
        ChangeWindowMessageFilterEx(g.hwnd, m, MSGFLT_ALLOW, nullptr);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;   // nothing to persist; the layout is fixed
    wchar_t windows[MAX_PATH];
    const UINT wn = GetWindowsDirectoryW(windows, MAX_PATH);
    gao::gui::load_fonts(wn && wn < MAX_PATH ? (std::filesystem::path(windows) / L"Fonts").string() : std::string());
    gao::gui::apply_style(scale);
    ImGui_ImplWin32_Init(g.hwnd);
    // Init dereferences a null device when its DXGI queries fail.
    if (!guarded([] { ImGui_ImplDX11_Init(g.device.Get(), g.ctx.Get()); })) return 1;

    g.worker = std::make_unique<gao::gui::OptimizeWorker>([] { PostMessageW(g.hwnd, WM_APP_WAKE, 0, 0); });
    init_hw();
    g.ui.gpu_name = gao::nvidia_adapter_name();

    tray_icon(NIM_ADD);
    refresh_status(true);
    if (tray_mode && g.ui.elevated) {
        std::string why;
        if (gao::app::prepare_state(&why)) {
            const auto logon = gao::app::apply_at_logon();
            if (logon.applied) {
                g.watch = true;
                g.strike_pending = true;
                SetTimer(g.hwnd, kTimerStrike, 2 * 60 * 1000, nullptr);
            } else if (logon.decision != gao::BootDecision::NoProfile) {
                notify(logon.message, false);   // apply_at_logon wrote boot.log
            }
        }
        refresh_status(false);
    }
    // The copy before this one kept the tune applied; so does this one.
    if (resume && g.ui.elevated && g.ui.profile && g.ui.profile_driver_ok && g.ui.profile_gpu_ok && g.ui.applied &&
        gao::tune_applied(*g.ui.profile, *g.ui.applied, g.ui.curve))
        g.watch = true;
    // Apply-at-logon runs the copy in Program Files: keep it this version.
    if (g.ui.elevated) {
        if (const std::string updated = gao::app::update_logon_copy(); !updated.empty()) {
            note(updated);
            refresh_status(true);
        }
    }
    SetTimer(g.hwnd, kTimerTelemetry, 1000, nullptr);
    SetTimer(g.hwnd, kTimerWatchdog, 30 * 1000, nullptr);
    SetTimer(g.hwnd, kTimerUpdate, 24 * 60 * 60 * 1000, nullptr);
    start_update_check();
    on_telemetry();
    if (!tray_mode) show_window();

    // Render only when needed: nothing while hidden, a few frames after input,
    // ~10 Hz during a run and ~4 Hz otherwise. The tool measures the GPU; its
    // own window must not load it.
    for (;;) {
        // While the device is lost nothing is drawn either: wait for a message
        // (the telemetry timer rebuilds the device, when the backoff lets it)
        // instead of polling.
        const DWORD timeout =
            !g.visible || g.device_lost ? INFINITE : g.input_frames > 0 ? 0 : g.worker->running() ? 100 : 250;
        MsgWaitForMultipleObjectsEx(0, nullptr, timeout, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        MSG msg;
        bool quit = false;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) quit = true;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (quit) break;
        if (g.exit_requested && !g.worker->running() && !g.ui.update_busy) {   // never leave in the middle of replacing the files
            clear_strike();
            fan_release();
            DestroyWindow(g.hwnd);
            continue;
        }
        if (!g.visible || IsIconic(g.hwnd) || g.device_lost) continue;
        render();
        if (g.input_frames > 0) --g.input_frames;
    }

    tray_icon(NIM_DELETE);
    g.worker.reset();
    if (backend_exists()) {   // not after an abandon: there is nothing to shut down then
        reset_textures(false);
        if (!guarded([] { ImGui_ImplDX11_Shutdown(); })) abandon_backend();
    }
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    if (g.restart) {   // an update replaced the executables: start the new copy
        wchar_t self[MAX_PATH];
        const DWORD n = GetModuleFileNameW(nullptr, self, MAX_PATH);
        if (n != 0 && n < MAX_PATH) {
            // Released first, or the new copy would take this one for a
            // running instance and only bring it forward.
            if (g.instance_mutex) CloseHandle(g.instance_mutex);
            std::wstring command = L"\"" + std::wstring(self) + L"\" --resume";
            STARTUPINFOW si{sizeof(si)};
            PROCESS_INFORMATION pi{};
            if (CreateProcessW(self, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
                CloseHandle(pi.hProcess);
                CloseHandle(pi.hThread);
            }
        }
    }
    return 0;
}
