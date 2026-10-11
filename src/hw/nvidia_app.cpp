#include "hw/nvidia_app.hpp"
#include "hw/app_files.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstdio>
#include <string>

namespace gao {

namespace {

// The few calls of SQLite this needs, from the library Windows ships
// (winsqlite3.dll, in System32 since Windows 10). Loaded from System32 only.
struct Sqlite {
    using Db = void;
    using Stmt = void;
    int (*open_v2)(const char*, Db**, int, const char*) = nullptr;
    int (*prepare_v2)(Db*, const char*, int, Stmt**, const char**) = nullptr;
    int (*step)(Stmt*) = nullptr;
    long long (*column_int64)(Stmt*, int) = nullptr;
    int (*finalize)(Stmt*) = nullptr;
    int (*close)(Db*) = nullptr;
    bool ok = false;

    Sqlite() {
        const HMODULE h = LoadLibraryExW(L"winsqlite3.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!h) return;
        open_v2 = reinterpret_cast<decltype(open_v2)>(GetProcAddress(h, "sqlite3_open_v2"));
        prepare_v2 = reinterpret_cast<decltype(prepare_v2)>(GetProcAddress(h, "sqlite3_prepare_v2"));
        step = reinterpret_cast<decltype(step)>(GetProcAddress(h, "sqlite3_step"));
        column_int64 = reinterpret_cast<decltype(column_int64)>(GetProcAddress(h, "sqlite3_column_int64"));
        finalize = reinterpret_cast<decltype(finalize)>(GetProcAddress(h, "sqlite3_finalize"));
        close = reinterpret_cast<decltype(close)>(GetProcAddress(h, "sqlite3_close"));
        ok = open_v2 && prepare_v2 && step && column_int64 && finalize && close;
    }
};

constexpr int kSqliteOk = 0, kSqliteRow = 100, kSqliteDone = 101;
constexpr int kOpenReadOnly = 0x1, kOpenUri = 0x40;

// file:///C:/a%20b/c.db3?immutable=1 -- immutable: no lock is taken and no
// journal is looked for, so the service that owns the file never waits for
// this reader.
std::string immutable_uri(const std::filesystem::path& file) {
    std::string uri = "file:///";
    for (const char8_t c8 : file.generic_u8string()) {
        const unsigned char c = static_cast<unsigned char>(c8);
        const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '/' || c == ':' || c == '.' ||
                           c == '-' || c == '_';
        if (plain) {
            uri += static_cast<char>(c);
        } else {
            char hex[4];
            std::snprintf(hex, sizeof(hex), "%%%02X", static_cast<unsigned>(c));
            uri += hex;
        }
    }
    return uri + "?immutable=1";
}

// The PCI device ids of the NVIDIA cards in this PC.
std::vector<unsigned> nvidia_device_ids() {
    std::vector<unsigned> ids;
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return ids;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(adapter->GetDesc1(&desc)) && desc.VendorId == 0x10DE) ids.push_back(desc.DeviceId);
        adapter.Reset();
    }
    return ids;
}

}

std::optional<bool> nvidia_auto_tuning_in(const std::filesystem::path& database, const std::vector<unsigned>& device_ids) {
    static const Sqlite sqlite;
    std::error_code ec;
    if (!sqlite.ok || database.empty() || !std::filesystem::is_regular_file(database, ec)) return std::nullopt;
    Sqlite::Db* db = nullptr;
    if (sqlite.open_v2(immutable_uri(database).c_str(), &db, kOpenReadOnly | kOpenUri, nullptr) != kSqliteOk) {
        if (db) sqlite.close(db);
        return std::nullopt;
    }
    // TelemetryGpuCfg says which card a row is of: its deviceId is the PCI
    // device id in the upper half and the vendor in the lower (0x278610DE).
    Sqlite::Stmt* rows = nullptr;
    std::optional<bool> on;
    if (sqlite.prepare_v2(db,
                          "select t.deviceId, s.backgroundScanEnabled, s.grdOcEnabled "
                          "from ocScannerSettings s join TelemetryGpuCfg t on t.id = s.gpuId",
                          -1, &rows, nullptr) == kSqliteOk) {
        bool found = false;
        int step = kSqliteRow;
        while ((step = sqlite.step(rows)) == kSqliteRow) {
            const unsigned device = static_cast<unsigned>((sqlite.column_int64(rows, 0) >> 16) & 0xFFFF);
            if (std::find(device_ids.begin(), device_ids.end(), device) == device_ids.end()) continue;
            found = found || sqlite.column_int64(rows, 1) != 0 || sqlite.column_int64(rows, 2) != 0;
        }
        if (step == kSqliteDone) on = found;   // anything else: the read did not get to the end
    }
    if (rows) sqlite.finalize(rows);
    sqlite.close(db);
    return on;
}

std::optional<bool> nvidia_auto_tuning() {
    if (app_dir().empty()) return std::nullopt;
    return nvidia_auto_tuning_in(app_dir().parent_path() / L"NVIDIA Corporation" / L"nvtopps" / L"nvtopps.db3", nvidia_device_ids());
}

}
