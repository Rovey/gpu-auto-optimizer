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
    CHECK(shown == std::vector<std::string>{"baseline: 30 s at stock", "core +0 / mem +50: STABLE  score=4547 it/s"});
    const auto lines = gao::read_lines(file);
    REQUIRE(lines);
    REQUIRE(lines->size() == 3);
    // Which build wrote it, then the lines, each with the time it was written.
    CHECK((*lines)[0].find("=== " + std::string(gao::kProductName) + " " + std::string(gao::kVersion)) != std::string::npos);
    CHECK((*lines)[1].find("  baseline: 30 s at stock") == std::string("2026-10-11 14:00:00").size());
    CHECK((*lines)[2].find("core +0 / mem +50: STABLE") != std::string::npos);
}

TEST_CASE("a second run adds to the log of the first") {
    const TempDir dir;
    const auto file = dir.path / "run.log";
    gao::app::logged_to(file, {})("the first run");
    gao::app::logged_to(file, {})("the second run");
    const auto lines = gao::read_lines(file);
    REQUIRE(lines);
    CHECK(lines->size() == 4);   // two headers, two lines
}

TEST_CASE("what a stopped run does afterwards is part of that run in the log, under no header of its own") {
    const TempDir dir;
    const auto file = dir.path / "run.log";
    gao::app::logged_to(file, {})("stock: ABORTED");
    gao::app::logged_to(file, {}, false)("stopped: the tune that was on the card before the run is back");
    const auto lines = gao::read_lines(file);
    REQUIRE(lines);
    REQUIRE(lines->size() == 3);   // one header, two lines
    CHECK((*lines)[2].find("stopped: the tune") != std::string::npos);
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
    CHECK(lines->size() == 2);
}
