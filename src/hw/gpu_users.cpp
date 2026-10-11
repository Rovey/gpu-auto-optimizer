#include "hw/gpu_users.hpp"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dxgi.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <tlhelp32.h>
#include <wrl/client.h>
#include <cwchar>
#include <map>
#include <utility>

namespace gao {

namespace {

constexpr UINT kNvidiaVendorId = 0x10DE;

// The adapter the tool tunes; nothing when it cannot be had, and then every
// adapter counts.
std::optional<unsigned long long> nvidia_luid() {
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return std::nullopt;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(adapter->GetDesc1(&desc)) && desc.VendorId == kNvidiaVendorId)
            return (static_cast<unsigned long long>(static_cast<unsigned long>(desc.AdapterLuid.HighPart)) << 32) | desc.AdapterLuid.LowPart;
        adapter.Reset();
    }
    return std::nullopt;
}

std::map<DWORD, std::string> process_names() {
    std::map<DWORD, std::string> names;
    const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return names;
    PROCESSENTRY32W e{};
    e.dwSize = sizeof(e);
    for (BOOL more = Process32FirstW(snap, &e); more; more = Process32NextW(snap, &e)) {
        std::string name;   // shown to the user and matched against ASCII names; anything else reads as '?'
        for (const wchar_t* c = e.szExeFile; *c; ++c) name += *c < 128 ? static_cast<char>(*c) : '?';
        names[e.th32ProcessID] = std::move(name);
    }
    CloseHandle(snap);
    return names;
}

}

std::optional<GpuEngineInstance> parse_gpu_engine_instance(const std::wstring& name) {
    unsigned pid = 0, high = 0, low = 0, phys = 0, eng = 0;
    wchar_t type[64] = {};
    if (swscanf_s(name.c_str(), L"pid_%u_luid_0x%x_0x%x_phys_%u_eng_%u_engtype_%63ls", &pid, &high, &low, &phys, &eng, type,
                  static_cast<unsigned>(std::size(type))) != 6)
        return std::nullopt;
    GpuEngineInstance out{pid, (static_cast<unsigned long long>(high) << 32) | low, {}};
    for (const wchar_t* c = type; *c; ++c) out.engine += *c < 128 ? static_cast<char>(*c) : '?';
    return out;
}

std::vector<GpuUse> gpu_use(int samples) {
    std::vector<GpuUse> out;
    if (samples < 1) return out;
    PDH_HQUERY query = nullptr;
    if (PdhOpenQueryW(nullptr, 0, &query) != ERROR_SUCCESS) return out;
    PDH_HCOUNTER counter = nullptr;
    // The first collection only sets the baseline a rate is taken against.
    if (PdhAddEnglishCounterW(query, L"\\GPU Engine(*)\\Utilization Percentage", 0, &counter) != ERROR_SUCCESS ||
        PdhCollectQueryData(query) != ERROR_SUCCESS) {
        PdhCloseQuery(query);
        return out;
    }
    const auto luid = nvidia_luid();
    const DWORD self = GetCurrentProcessId();
    std::map<std::pair<unsigned, std::string>, double> sum;   // (pid, engine) -> percent, added up over the looks
    for (int i = 0; i < samples; ++i) {
        Sleep(1000);
        if (PdhCollectQueryData(query) != ERROR_SUCCESS) continue;
        DWORD bytes = 0, count = 0;
        if (PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &bytes, &count, nullptr) != static_cast<PDH_STATUS>(PDH_MORE_DATA)) continue;
        std::vector<unsigned char> buffer(bytes);
        auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
        if (PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &bytes, &count, items) != ERROR_SUCCESS) continue;
        for (DWORD k = 0; k < count; ++k) {
            const DWORD status = items[k].FmtValue.CStatus;
            if (status != PDH_CSTATUS_VALID_DATA && status != PDH_CSTATUS_NEW_DATA) continue;
            const auto instance = parse_gpu_engine_instance(items[k].szName);
            if (!instance || instance->pid == self || (luid && instance->luid != *luid)) continue;
            sum[{instance->pid, instance->engine}] += items[k].FmtValue.doubleValue;
        }
    }
    PdhCloseQuery(query);
    const auto names = process_names();
    for (const auto& [key, total] : sum) {
        const auto name = names.find(key.first);
        // A process that is gone by now has no name; it is not there to be closed either.
        if (name != names.end()) out.push_back({name->second, key.first, key.second, total / samples});
    }
    return out;
}

}
