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
#include "app/gui/ui.hpp"
#include "app/gui/worker.hpp"
#include "core/boot.hpp"
#include "core/fan_curve.hpp"
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
#include <string>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

using Microsoft::WRL::ComPtr;
using gao::app::kGpu;

namespace {

constexpr wchar_t kWindowClass[] = L"GpuAutoOptimizerWindow";   // also used by gao --reset
constexpr wchar_t kInstanceMutex[] = L"Local\\GpuAutoOptimizer.Instance";
constexpr UINT WM_APP_TRAY = WM_APP + 1;   // tray icon callback
constexpr UINT WM_APP_WAKE = WM_APP + 2;   // the optimize worker has news
constexpr UINT WM_APP_SHOW = WM_APP + 3;   // a second launch asks us to come forward
constexpr UINT_PTR kTimerTelemetry = 1;    // 1 s
constexpr UINT_PTR kTimerWatchdog = 2;     // 30 s
constexpr UINT_PTR kTimerStrike = 3;       // one-shot, 2 min after a logon apply
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
    bool device_lost = false;   // retried every second until a new device exists

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
    bool worker_was_running = false;
    bool strike_pending = false;   // a logon apply whose 2-minute grace has not passed yet
    std::unique_ptr<gao::FanDriver> fan;   // while the curve drives the fans
    bool told_about_tray = false;  // the "still running in the tray" balloon, once per session

    // Crash dumps are written by a thread created up front (a crashing thread
    // may have no stack or heap left to do it itself).
    HANDLE dump_request = nullptr, dump_done = nullptr;
    EXCEPTION_POINTERS* crash_info = nullptr;
    DWORD crash_thread = 0;
};

App g;

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
    if (g.worker && g.worker->running() && g.gpu.reset_to_stock) g.gpu.reset_to_stock();
    if (g.gpu.set_fan_auto) g.gpu.set_fan_auto();
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

// After a TDR the UI device is gone. Rebuild it; if that fails too (it can,
// right after a reset), try again on the next telemetry tick.
void recover_device() {
    if (g.ctx) ImGui_ImplDX11_Shutdown();
    g.rtv.Reset();
    g.ctx.Reset();
    g.swapchain.Reset();
    g.device.Reset();
    g.device_lost = !create_device();
    if (!g.device_lost) ImGui_ImplDX11_Init(g.device.Get(), g.ctx.Get());
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
    g.ui.fan_min_pct = g.gpu.fan_min_pct;
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
    g.fan = std::make_unique<gao::FanDriver>(g.gpu, *curve, g.ui.fan_max_temp_c);
}

void refresh_status(bool with_task) {
    g.ui.elevated = gao::app::is_elevated();
    const gao::Config cfg = gao::app::load_config();
    g.ui.profile = cfg.profile;
    g.ui.strikes = cfg.boot_strikes;
    if (g.nvml_ok) {
        g.ui.driver = g.nvml->DriverVersion();
        const std::string gpu_id = g.nvml->GpuUuid(kGpu);
        g.ui.profile_driver_ok = cfg.profile && !g.ui.driver.empty() && g.ui.driver == cfg.profile->driver;
        g.ui.profile_gpu_ok = cfg.profile && !gpu_id.empty() && gpu_id == cfg.profile->gpu;
    }
    if (g.gpu.read_applied) g.ui.applied = g.gpu.read_applied();
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
    g.worker->start(preset);   // the watchdog skips while it runs
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

void render() {
    ImGui_ImplDX11_NewFrame();
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
        if (!gao::app::save_config(cfg)) note("Could not save the fan setting.", true);
        note(on ? "Fan curve on." : "Fan curve off; the NVIDIA driver controls the fans.");
        refresh_status(false);
    };
    act.reset_fan_curve = [] {
        gao::Config cfg = gao::app::load_config();
        cfg.fan_curve.reset();
        if (!gao::app::save_config(cfg)) note("Could not save the fan curve.", true);
        refresh_status(false);
    };
    act.detect_gpu = [] {   // e.g. after a driver update: re-create NVML and NVAPI now
        if (refuse_while_tuning()) return;
        hw_lost();
        g.hw_retry_at = 0;
    };
    gao::gui::draw_ui(g.ui, g.worker->snapshot(), act);
    ImGui::Render();
    const float clear[4] = {0.08f, 0.08f, 0.10f, 1.0f};
    g.ctx->OMSetRenderTargets(1, g.rtv.GetAddressOf(), nullptr);
    g.ctx->ClearRenderTargetView(g.rtv.Get(), clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    const HRESULT hr = g.swapchain->Present(1, 0);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) recover_device();
}

// ---------------------------------------------------------------- hardware

// A driver reset (TDR) can leave NVML and NVAPI state in this long-running
// process stale, and a call during the reset has faulted inside nvml.dll
// (hardware check 33). The tray exists to survive exactly that: its periodic
// hardware work runs guarded, and after a reset the libraries are re-created.

void init_hw() {
    g.nvml = std::make_unique<gao::Nvml>();
    g.nvapi = std::make_unique<gao::Nvapi>();
    g.nvml_ok = g.nvml->Init();
    g.nvapi_ok = g.nvapi->Init();
    g.gpu = g.nvml_ok && g.nvapi_ok ? gao::make_gpu_control(*g.nvml, *g.nvapi, kGpu) : gao::GpuControl{};
}

// Structured exceptions, not C++ ones: an access violation inside a driver DLL.
// A function with __try may not hold destructible objects, hence the function
// pointer. Objects in the frames it skips are not destroyed (/EHsc); that leak
// is the price of keeping the watchdog alive.
bool guarded(void (*fn)(void*), void* ctx) {
    __try {
        fn(ctx);
        return true;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
}
template <class F> bool guarded(F f) {
    return guarded([](void* p) { (*static_cast<F*>(p))(); }, &f);
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
    // often hidden, so no Present() would report it.
    const bool reset = g.device && g.device->GetDeviceRemovedReason() != S_OK;
    if (g.device_lost || reset) recover_device();
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
        g.ui.fan_state = g.fan->tick(g.ui.telemetry.temp_c, std::chrono::steady_clock::now());
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
        if (snap.outcome && snap.outcome->ran) {
            g.watch = snap.outcome->result.ok && snap.outcome->saved;
            g.watchdog = gao::Watchdog();
        }
        refresh_status(false);
    }
    g.worker_was_running = running;
}

void on_watchdog() {
    // Never under a running search -- ours, or gao --optimize in a shell.
    if (!g.watch || g.worker->running() || gao::app::tuning_in_progress()) return;
    if (!g.ui.elevated || !g.nvml_ok || !g.nvapi_ok) return;
    const gao::Config cfg = gao::app::load_config();
    if (!cfg.profile) return;
    const auto applied = g.gpu.read_applied();
    if (!applied) {   // NVAPI handles go stale after a driver reset: re-create
        hw_lost();
        return;
    }
    const std::string driver = g.nvml->DriverVersion(), gpu_id = g.nvml->GpuUuid(kGpu);
    const auto action = g.watchdog.check(*cfg.profile, applied, !driver.empty() && driver == cfg.profile->driver,
                                         !gpu_id.empty() && gpu_id == cfg.profile->gpu, std::chrono::steady_clock::now());
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
        case gao::WatchAction::GiveUpUnstable:
            gao::app::boot_log("watchdog: reset 4 times within an hour; stopped re-applying");
            notify("The tune keeps getting reset (4 times within an hour), which usually means it is not stable. "
                   "Stopped re-applying it; optimize again.",
                   false);
            break;
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
            return 0;
        case WM_APP_WAKE:
            g.input_frames = std::max(g.input_frames, 1);
            return 0;
        case WM_APP_SHOW:
            show_window();
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
            }
            return 0;
        case WM_POWERBROADCAST:
            if (wp == PBT_APMSUSPEND) fan_release();                 // never sleep with a manual speed
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
bool claim_single_instance(bool tray_mode) {
    g.instance_mutex = CreateMutexW(nullptr, TRUE, kInstanceMutex);
    const DWORD err = GetLastError();
    if (g.instance_mutex && err != ERROR_ALREADY_EXISTS) return true;
    if (HWND other = FindWindowW(kWindowClass, nullptr); other && !tray_mode) {
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
    if (!claim_single_instance(tray_mode)) return 0;

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
    ImGui_ImplDX11_Init(g.device.Get(), g.ctx.Get());

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
    SetTimer(g.hwnd, kTimerTelemetry, 1000, nullptr);
    SetTimer(g.hwnd, kTimerWatchdog, 30 * 1000, nullptr);
    on_telemetry();
    if (!tray_mode) show_window();

    // Render only when needed: nothing while hidden, a few frames after input,
    // ~10 Hz during a run and ~4 Hz otherwise. The tool measures the GPU; its
    // own window must not load it.
    for (;;) {
        const DWORD timeout = !g.visible ? INFINITE : g.input_frames > 0 ? 0 : g.worker->running() ? 100 : 250;
        MsgWaitForMultipleObjectsEx(0, nullptr, timeout, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        MSG msg;
        bool quit = false;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) quit = true;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (quit) break;
        if (g.exit_requested && !g.worker->running()) {
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
    if (g.ctx) ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    return 0;
}
