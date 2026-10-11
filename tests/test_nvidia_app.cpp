#include "doctest/doctest.h"

#include <windows.h>
#include "hw/nvidia_app.hpp"
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

// A settings database like the driver's, made with the SQLite library Windows
// ships. The two tables are as driver 617.42 creates them.
struct Database {
    std::filesystem::path dir = std::filesystem::temp_directory_path() / ("gao nvidia app " + std::to_string(GetCurrentProcessId()));
    std::filesystem::path file = dir / "nvtopps.db3";
    void* db = nullptr;
    int (*exec)(void*, const char*, void*, void*, char**) = nullptr;
    int (*close)(void*) = nullptr;

    Database() {
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);   // a space in the path, as in "NVIDIA Corporation"
        const HMODULE h = LoadLibraryExW(L"winsqlite3.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        REQUIRE(h);
        const auto open = reinterpret_cast<int (*)(const char*, void**, int, const char*)>(GetProcAddress(h, "sqlite3_open_v2"));
        exec = reinterpret_cast<decltype(exec)>(GetProcAddress(h, "sqlite3_exec"));
        close = reinterpret_cast<decltype(close)>(GetProcAddress(h, "sqlite3_close"));
        REQUIRE(open);
        REQUIRE(exec);
        REQUIRE(close);
        REQUIRE(open(file.string().c_str(), &db, 0x2 | 0x4, nullptr) == 0);   // read-write, create
    }
    ~Database() {
        done();
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
    void run(const std::string& sql) { REQUIRE(exec(db, sql.c_str(), nullptr, nullptr, nullptr) == 0); }
    void done() {   // the reader opens the file as it is on disk
        if (db) close(db);
        db = nullptr;
    }
    void tables() {
        run("CREATE TABLE 'ocScannerSettings' ( 'gpuId' TEXT PRIMARY KEY NOT NULL , 'backgroundScanEnabled' INTEGER NOT NULL , "
            "'grdOcEnabled' INTEGER NOT NULL )");
        run("CREATE TABLE 'TelemetryGpuCfg' ( 'id' TEXT PRIMARY KEY NOT NULL , 'time' INTEGER NOT NULL , 'shortName' TEXT NOT NULL , "
            "'deviceId' INTEGER NOT NULL , 'subsystemId' INTEGER NOT NULL , 'revision' INTEGER NOT NULL , 'extDeviceId' INTEGER NOT NULL , "
            "'projectId' TEXT NOT NULL , 'partner' TEXT NOT NULL , 'sessionId' TEXT NOT NULL , 'msiActive' INTEGER NOT NULL , "
            "'pciGen' INTEGER NOT NULL , 'pciLaneCnt' INTEGER NOT NULL , 'fanCnt' INTEGER NOT NULL , 'throttleTime' INTEGER NOT NULL )");
    }
    // One card as the driver records it, with its two switches.
    void card(const std::string& id, const std::string& name, long long device_and_vendor, int scan, int applied) {
        run("INSERT INTO TelemetryGpuCfg VALUES ('" + id + "', 1791698692515740, '" + name + "', " + std::to_string(device_and_vendor) +
            ", 1089344600, 161, 10118, '127984', 'gigabyte', 'P90069R1', 1, 4, 16, 0, 0)");
        run("INSERT INTO ocScannerSettings VALUES ('" + id + "', " + std::to_string(scan) + ", " + std::to_string(applied) + ")");
    }
};

constexpr long long kRtx4070 = 0x278610DE, kRtx2060 = 0x1F0610DE;
const std::vector<unsigned> kThisPc{0x2786};

}

TEST_CASE("Automatic Tuning of the NVIDIA app reads as on when its scanner is switched on for this card") {
    Database d;
    d.tables();
    d.card("0000000000e062c0_00000005714819e1", "AD104-A", kRtx4070, 1, 0);   // as recorded while the switch was on
    d.done();
    CHECK(gao::nvidia_auto_tuning_in(d.file, kThisPc) == std::optional<bool>(true));
}

TEST_CASE("Automatic Tuning reads as on when a tune it found is kept applied") {
    Database d;
    d.tables();
    d.card("0000000000e062c0_00000005714819e1", "AD104-A", kRtx4070, 0, 1);
    d.done();
    CHECK(gao::nvidia_auto_tuning_in(d.file, kThisPc) == std::optional<bool>(true));
}

TEST_CASE("Automatic Tuning reads as off when both switches are off, and when the card was never seen") {
    Database d;
    d.tables();
    d.card("0000000000e062c0_00000005714819e1", "AD104-A", kRtx4070, 0, 0);   // as recorded before and after
    d.done();
    CHECK(gao::nvidia_auto_tuning_in(d.file, kThisPc) == std::optional<bool>(false));
    CHECK(gao::nvidia_auto_tuning_in(d.file, {0x2684}) == std::optional<bool>(false));
}

TEST_CASE("a card that is no longer in the PC does not switch Automatic Tuning on") {
    Database d;
    d.tables();
    d.card("0000000001001080_0000005978002191", "TU106-A", kRtx2060, 1, 1);   // an earlier card, left switched on
    d.card("0000000000e062c0_00000005714819e1", "AD104-A", kRtx4070, 0, 0);
    d.done();
    CHECK(gao::nvidia_auto_tuning_in(d.file, kThisPc) == std::optional<bool>(false));
    CHECK(gao::nvidia_auto_tuning_in(d.file, {0x1F06}) == std::optional<bool>(true));
}

TEST_CASE("what cannot be read is unknown, not off") {
    Database d;
    CHECK_FALSE(gao::nvidia_auto_tuning_in(d.dir / "not there.db3", kThisPc).has_value());
    CHECK_FALSE(gao::nvidia_auto_tuning_in({}, kThisPc).has_value());
    SUBCASE("a database without the tables") {
        d.run("CREATE TABLE other (id INTEGER)");
        d.done();
        CHECK_FALSE(gao::nvidia_auto_tuning_in(d.file, kThisPc).has_value());
    }
    SUBCASE("a table that lost a column in a later driver") {
        d.run("CREATE TABLE ocScannerSettings (gpuId TEXT PRIMARY KEY NOT NULL, backgroundScanEnabled INTEGER NOT NULL)");
        d.run("CREATE TABLE TelemetryGpuCfg (id TEXT PRIMARY KEY NOT NULL, deviceId INTEGER NOT NULL)");
        d.done();
        CHECK_FALSE(gao::nvidia_auto_tuning_in(d.file, kThisPc).has_value());
    }
    SUBCASE("a file that is no database") {
        d.done();
        { std::ofstream(d.file, std::ios::trunc) << "this is not a database, and it is long enough to be read as a header ........................"; }
        CHECK_FALSE(gao::nvidia_auto_tuning_in(d.file, kThisPc).has_value());
    }
}
