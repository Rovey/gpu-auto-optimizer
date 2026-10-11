#include "core/busy_card.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace gao {

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string without_exe(const std::string& name) {
    return name.size() > 4 && lower(name.substr(name.size() - 4)) == ".exe" ? name.substr(0, name.size() - 4) : name;
}

// This tool, and what nobody can close.
bool never_named(const std::string& program) {
    static const char* const kNames[] = {"gao", "gpuautooptimizer", "dwm", "system", "csrss", "idle"};
    const std::string p = lower(program);
    return p.empty() || std::any_of(std::begin(kNames), std::end(kNames), [&](const char* n) { return p == n; });
}

std::string joined(const std::vector<std::string>& names) {
    std::string s;
    for (size_t i = 0; i < names.size(); ++i) s += (i == 0 ? "" : i + 1 == names.size() ? " and " : ", ") + names[i];
    return s;
}

}

std::vector<BusyProgram> busy_programs(const std::vector<GpuUse>& use) {
    std::vector<BusyProgram> all;
    for (const GpuUse& u : use) {
        if (!(u.pct >= 0 && u.pct <= 100)) continue;   // also false for a NaN
        const std::string program = without_exe(u.program);
        if (never_named(program)) continue;
        auto it = std::find_if(all.begin(), all.end(), [&](const BusyProgram& p) { return lower(p.program) == lower(program); });
        if (it == all.end()) it = all.insert(all.end(), BusyProgram{program, 0, false});
        it->pct += u.pct;
        if (lower(u.engine).rfind("video", 0) == 0 && u.pct > 0) it->video = true;
    }
    std::erase_if(all, [](const BusyProgram& p) { return p.pct < kBusyPct; });
    std::stable_sort(all.begin(), all.end(), [](const BusyProgram& a, const BusyProgram& b) { return a.pct > b.pct; });
    return all;
}

std::string busy_text(const std::vector<BusyProgram>& programs) {
    if (programs.empty()) return "none";
    std::string s;
    for (const BusyProgram& p : programs) {
        char pct[32];
        std::snprintf(pct, sizeof(pct), " (%d %%%s)", std::max(1, static_cast<int>(std::lround(p.pct))), p.video ? ", video" : "");
        s += (s.empty() ? "" : ", ") + p.program + pct;
    }
    return s;
}

std::string busy_change(const std::vector<BusyProgram>& before, const std::vector<BusyProgram>& after) {
    auto only_in = [](const std::vector<BusyProgram>& a, const std::vector<BusyProgram>& b) {
        std::vector<std::string> names;
        for (const BusyProgram& p : a)
            if (std::none_of(b.begin(), b.end(), [&](const BusyProgram& q) { return lower(q.program) == lower(p.program); }))
                names.push_back(p.program);
        return names;
    };
    const auto stopped = only_in(before, after), started = only_in(after, before);
    if (stopped.empty() && started.empty()) return {};
    if (started.empty()) return joined(stopped) + " stopped using the card during the run";
    if (stopped.empty()) return joined(started) + " started using the card during the run";
    return joined(stopped) + " stopped using the card during the run, and " + joined(started) + " started";
}

BusyGate busy_gate(const std::function<std::vector<GpuUse>()>& look,
                   const std::function<BusyAnswer(const std::vector<BusyProgram>&)>& ask,
                   const std::function<void(const std::string&)>& log) {
    for (;;) {
        BusyGate gate;
        gate.programs = busy_programs(look());
        log("other programs on the card: " + busy_text(gate.programs));
        if (gate.programs.empty() || !ask) return gate;
        switch (ask(gate.programs)) {
            case BusyAnswer::GoOn:
                log("going on with them: the run will measure lower and less steadily");
                return gate;
            case BusyAnswer::Stop:
                gate.refusal = "other programs are using the graphics card: " + busy_text(gate.programs);
                return gate;
            case BusyAnswer::CheckAgain:
                break;
        }
    }
}

}
