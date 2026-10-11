#pragma once
#include <functional>
#include <string>
#include <vector>

namespace gao {

// Other programs on the card. A run measures the card under full load, and
// whatever else uses it takes from every number: with an editor standing open
// that drew 2.2 % of the 3D engine while idle, the reference RTX 4070 scored
// 5329 it/s instead of 5828 and its memory bandwidth read 454 GB/s instead of
// 482; a video playing in a browser made the readings jump by 3 to 8 % from
// one to the next (hardware check 96). Such a program does nothing to the
// card, unlike another tuning tool, so a run is not refused next to it: the
// user is told, and decides.

// One program's use of one engine of the card, averaged over a few seconds.
struct GpuUse {
    std::string program;   // the executable's name, "Trae.exe"
    unsigned pid = 0;
    std::string engine;    // as Windows names it: "3D", "VideoDecode", "Copy", ...
    double pct = 0;
};

struct BusyProgram {
    std::string program;   // without ".exe"
    double pct = 0;        // all its processes and engines together
    bool video = false;    // it decodes or encodes video
};

// A program below this is not named. The desktop itself idles at 0.05 %, an
// idle editor that still cost 8 % of the score at 2.2 %.
inline constexpr double kBusyPct = 1.0;

// The programs that use the card enough to matter, the heaviest first. Never
// this tool itself, never what Windows needs to draw the desktop (nobody can
// close that), and no reading outside 0 .. 100 % (the counters give one now
// and then).
std::vector<BusyProgram> busy_programs(const std::vector<GpuUse>& use);

// "chrome (12 %, video), Trae (2 %)"; "none" for an empty list.
std::string busy_text(const std::vector<BusyProgram>& programs);

// What changed between the look before a run and the one after it, as a
// sentence without a full stop; empty when the same programs were there. A
// program that only uses more or less than before is no change.
std::string busy_change(const std::vector<BusyProgram>& before, const std::vector<BusyProgram>& after);

enum class BusyAnswer { GoOn, CheckAgain, Stop };

struct BusyGate {
    std::string refusal;                  // why the run does not start; empty: it does
    std::vector<BusyProgram> programs;    // what was on the card when the run went on
};

// The check before a run. `look`: what uses the card now. `ask`: what to do
// about the programs found (empty: nobody to ask, and the run goes on).
// Every look is logged, so the log of a run always says what else was on the
// card when it began.
BusyGate busy_gate(const std::function<std::vector<GpuUse>()>& look,
                   const std::function<BusyAnswer(const std::vector<BusyProgram>&)>& ask,
                   const std::function<void(const std::string&)>& log);

}
