#include "doctest/doctest.h"

#include <windows.h>
#include "app/common.hpp"
#include "core/version.hpp"
#include "hw/app_files.hpp"
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

// A folder of this test's own, removed when the test ends.
struct TempDir {
    std::filesystem::path path = std::filesystem::temp_directory_path() / ("gao-run-log-" + std::to_string(GetCurrentProcessId()));
    TempDir() {
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

// The line a run starts with: the path in UTF-8, as the log writes it.
std::string where_line(const std::filesystem::path& file) {
    const std::u8string path = file.u8string();
    return "this run is logged to " + std::string(path.begin(), path.end());
}

}

TEST_CASE("every line of a run is on disk before it is shown") {
    const TempDir dir;
    const auto file = dir.path / "run.log";
    std::vector<std::string> shown;
    const auto log = gao::app::logged_to(file, [&](const std::string& line) {
        // A machine that freezes in the step this line announces has it on disk.
        const auto on_disk = gao::read_lines(file);
        REQUIRE(on_disk);
        REQUIRE_FALSE(on_disk->empty());
        CHECK(on_disk->back().find(line) != std::string::npos);
        shown.push_back(line);
    });
    log("baseline: 30 s at stock");
    log("core +0 / mem +50: STABLE  score=4547 it/s");
    CHECK(shown == std::vector<std::string>{where_line(file), "baseline: 30 s at stock",
                                            "core +0 / mem +50: STABLE  score=4547 it/s"});
    const auto lines = gao::read_lines(file);
    REQUIRE(lines);
    REQUIRE(lines->size() == 4);
    // Which build wrote it, where it is, then the lines, each with the time it was written.
    CHECK((*lines)[0].find("=== " + std::string(gao::kProductName) + " " + std::string(gao::kVersion)) != std::string::npos);
    CHECK((*lines)[1].find("  " + where_line(file)) == std::string("2026-10-11 14:00:00").size());
    CHECK((*lines)[2].find("  baseline: 30 s at stock") == std::string("2026-10-11 14:00:00").size());
    CHECK((*lines)[3].find("core +0 / mem +50: STABLE") != std::string::npos);
}

TEST_CASE("a run says where it logs at its start, and what belongs to the run before it does not say it again") {
    const TempDir dir;
    const auto file = dir.path / "run.log";
    std::vector<std::string> shown;
    const auto show = [&](const std::string& line) { shown.push_back(line); };
    gao::app::logged_to(file, show)("stock: ABORTED");
    gao::app::logged_to(file, show, false)("stopped: the tune is back");
    CHECK(shown == std::vector<std::string>{where_line(file), "stock: ABORTED", "stopped: the tune is back"});
}

TEST_CASE("a second run adds to the log of the first") {
    const TempDir dir;
    const auto file = dir.path / "run.log";
    gao::app::logged_to(file, {})("the first run");
    gao::app::logged_to(file, {})("the second run");
    const auto lines = gao::read_lines(file);
    REQUIRE(lines);
    CHECK(lines->size() == 6);   // two headers, where each run logs, two lines
}

TEST_CASE("what a stopped run does afterwards is part of that run in the log, under no header of its own") {
    const TempDir dir;
    const auto file = dir.path / "run.log";
    gao::app::logged_to(file, {})("stock: ABORTED");
    gao::app::logged_to(file, {}, false)("stopped: the tune that was on the card before the run is back");
    const auto lines = gao::read_lines(file);
    REQUIRE(lines);
    REQUIRE(lines->size() == 4);   // one header, where the run logs, two lines
    CHECK((*lines)[3].find("stopped: the tune") != std::string::npos);
}

TEST_CASE("a run log that has grown large is moved aside, and one older file is kept") {
    const TempDir dir;
    const auto file = dir.path / "run.log";
    const auto old = dir.path / "run.log.old";
    { std::ofstream(old) << "from long ago\n"; }
    { std::ofstream(file) << std::string(gao::app::kRunLogMaxBytes + 1, 'x') << "\n"; }
    gao::app::logged_to(file, {})("a new run");
    CHECK(std::filesystem::file_size(old) > gao::app::kRunLogMaxBytes);   // the large one replaced the older one
    const auto lines = gao::read_lines(file);
    REQUIRE(lines);
    CHECK(lines->size() == 3);
}
