#include "doctest/doctest.h"
#include "core/busy_card.hpp"
#include <string>
#include <vector>

using namespace gao;

namespace {
std::vector<std::string> names(const std::vector<BusyProgram>& programs) {
    std::vector<std::string> out;
    for (const auto& p : programs) out.push_back(p.program);
    return out;
}
}

TEST_CASE("a card nobody else uses has no busy programs") {
    CHECK(busy_programs({}).empty());
    CHECK(busy_text({}) == "none");
}

TEST_CASE("an editor that stands open is a busy program") {
    // Measured on the reference card (hardware check 96): Trae, doing nothing,
    // used 2.2 % of the 3D engine, and with it the stress load scored 5329
    // it/s instead of 5828. The desktop itself was at 0.05 %.
    const auto busy = busy_programs({{"Trae.exe", 4100, "3D", 2.16}, {"dwm.exe", 900, "3D", 0.05}, {"msedgewebview2.exe", 15616, "3D", 0.01}});
    REQUIRE(busy.size() == 1);
    CHECK(busy[0].program == "Trae");
    CHECK(busy[0].pct == doctest::Approx(2.16));
    CHECK_FALSE(busy[0].video);
    CHECK(busy_text(busy) == "Trae (2 %)");
}

TEST_CASE("a program's processes and engines add up, and a video is named") {
    const auto busy = busy_programs({
        {"chrome.exe", 10, "VideoDecode", 8.6}, {"chrome.exe", 11, "3D", 2.8}, {"chrome.exe", 12, "3D", 0.3},
        {"Trae.exe", 20, "3D", 2.2}, {"Trae.exe", 20, "Copy", 0.06},
    });
    REQUIRE(busy.size() == 2);
    CHECK(busy[0].program == "chrome");            // the heaviest first
    CHECK(busy[0].pct == doctest::Approx(11.7));
    CHECK(busy[0].video);
    CHECK(busy[1].program == "Trae");
    CHECK_FALSE(busy[1].video);
    CHECK(busy_text(busy) == "chrome (12 %, video), Trae (2 %)");
}

TEST_CASE("many small processes of one program count together") {
    // Each below the threshold, the program above it.
    const auto busy = busy_programs({{"app.exe", 1, "3D", 0.2}, {"app.exe", 2, "3D", 0.2}, {"APP.EXE", 3, "3D", 0.2}});
    REQUIRE(busy.size() == 1);
    CHECK(busy[0].pct == doctest::Approx(0.6));
    CHECK(busy_text(busy) == "app (1 %)");   // never "0 %" for a program that is named
}

TEST_CASE("the editor is named in a look that catches it at half its usual share") {
    // Two looks at the same idle editor, seconds apart, read 2.2 % and 1 %.
    CHECK(busy_programs({{"Trae.exe", 1, "3D", 1.0}}).size() == 1);
    CHECK(busy_programs({{"Trae.exe", 1, "3D", 0.6}}).size() == 1);
    // What only stands there is not.
    CHECK(busy_programs({{"msedgewebview2.exe", 1, "3D", 0.01}, {"explorer.exe", 2, "3D", 0.05}}).empty());
}

TEST_CASE("what is below the threshold is not named") {
    CHECK(busy_programs({{"notepad.exe", 1, "3D", kBusyPct - 0.01}}).empty());
    CHECK(busy_programs({{"notepad.exe", 1, "3D", kBusyPct}}).size() == 1);
}

TEST_CASE("the tool itself and what Windows needs are never named") {
    // Nobody can close the desktop, and the run's own load is not another program.
    CHECK(busy_programs({{"gao.exe", 1, "3D", 99}, {"GpuAutoOptimizer.exe", 2, "3D", 5}, {"dwm.exe", 3, "3D", 4},
                         {"System", 4, "Copy", 3}, {"csrss.exe", 5, "3D", 2}, {"", 6, "3D", 9}}).empty());
}

TEST_CASE("readings that cannot be are left out") {
    // Seen on the reference card: one sample of 183 455 208 183 776 %.
    const auto busy = busy_programs({{"a.exe", 1, "Copy", 183455208183776.0}, {"a.exe", 1, "3D", -3.0}, {"b.exe", 2, "3D", 100.0}});
    CHECK(names(busy) == std::vector<std::string>{"b"});
}

TEST_CASE("a program that uses less than one percent of a busy list still reads as one") {
    CHECK(busy_text({{"x", 1.2, false}}) == "x (1 %)");
    CHECK(busy_text({{"x", 1.6, true}}) == "x (2 %, video)");
}

TEST_CASE("what changed between two looks is said in a sentence; nothing when nothing did") {
    const std::vector<BusyProgram> trae = {{"Trae", 2.2, false}}, chrome = {{"chrome", 9, true}}, both = {{"chrome", 9, true}, {"Trae", 2.2, false}};
    CHECK(busy_change(trae, trae).empty());
    CHECK(busy_change({}, {}).empty());
    CHECK(busy_change(trae, {{"trae", 3.5, false}}).empty());   // the same program, a little more: not a change
    CHECK(busy_change(trae, {}) == "Trae stopped using the card during the run");
    CHECK(busy_change({}, chrome) == "chrome started using the card during the run");
    CHECK(busy_change(trae, chrome) == "Trae stopped using the card during the run, and chrome started");
    CHECK(busy_change(both, {}) == "chrome and Trae stopped using the card during the run");
}

namespace {
struct Gate {
    std::vector<std::vector<GpuUse>> looks;   // what each look finds, in order; the last one repeats
    std::vector<BusyAnswer> answers;          // what is answered, in order
    int looked = 0, asked = 0;
    std::vector<std::string> log;
    BusyGate go(bool with_ask = true) {
        return busy_gate([this] { const auto& l = looks[std::min<size_t>(looked, looks.size() - 1)]; ++looked; return l; },
                         with_ask ? std::function<BusyAnswer(const std::vector<BusyProgram>&)>([this](const std::vector<BusyProgram>&) {
                             return answers.at(asked++);
                         }) : nullptr,
                         [this](const std::string& m) { log.push_back(m); });
    }
};
const std::vector<GpuUse> kTrae = {{"Trae.exe", 4100, "3D", 2.16}};
}

TEST_CASE("the check before a run: a quiet card goes on without a question") {
    Gate g{{{}}, {}};
    const BusyGate r = g.go();
    CHECK(r.refusal.empty());
    CHECK(r.programs.empty());
    CHECK(g.asked == 0);
    CHECK(g.log == std::vector<std::string>{"other programs on the card: none"});
}

TEST_CASE("the check before a run: a busy card asks, and going on is logged") {
    Gate g{{kTrae}, {BusyAnswer::GoOn}};
    const BusyGate r = g.go();
    CHECK(r.refusal.empty());
    CHECK(names(r.programs) == std::vector<std::string>{"Trae"});
    CHECK(g.asked == 1);
    CHECK(g.log == std::vector<std::string>{"other programs on the card: Trae (2 %)", "going on with them: the run will measure lower and less steadily"});
}

TEST_CASE("the check before a run: stopping gives the reason, with the programs in it") {
    Gate g{{kTrae}, {BusyAnswer::Stop}};
    const BusyGate r = g.go();
    CHECK(r.refusal == "other programs are using the graphics card: Trae (2 %)");
    CHECK(g.looked == 1);
}

TEST_CASE("the check before a run: checking again looks again, and a card that is quiet now goes on") {
    Gate g{{kTrae, kTrae, {}}, {BusyAnswer::CheckAgain, BusyAnswer::CheckAgain}};
    const BusyGate r = g.go();
    CHECK(r.refusal.empty());
    CHECK(r.programs.empty());
    CHECK(g.looked == 3);
    CHECK(g.asked == 2);
    CHECK(g.log.back() == "other programs on the card: none");
}

TEST_CASE("the check before a run: with nobody to ask the run goes on, and the log says what was there") {
    Gate g{{kTrae}, {}};
    const BusyGate r = g.go(false);
    CHECK(r.refusal.empty());
    CHECK(names(r.programs) == std::vector<std::string>{"Trae"});
    CHECK(g.log == std::vector<std::string>{"other programs on the card: Trae (2 %)"});
}
