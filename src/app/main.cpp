#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "app/cli_commands.hpp"
#include "app/common.hpp"
#include "app/guarded_gpu.hpp"
#include "core/version.hpp"
#include "core/vf_curve.hpp"
#include "hw/stress.hpp"
#include <optional>
#include <string>
#include <charconv>
#include <cstdio>
#include <cstring>

// The whole argument must be an int: std::atoi would read a typo like
// "--set-core +x" as 0 and silently reset the offset.
static bool ParseIntArg(const char* text, int* out) {
    const char* end = text + std::strlen(text);
    const auto [p, ec] = std::from_chars(text, end, *out);
    return ec == std::errc() && p == end && p != text;
}

using namespace gao::cli;

int main(int argc, char** argv) {
    // Before anything loads a DLL: System32 only (the delay-loaded
    // d3dcompiler_47.dll included), never the exe's own folder.
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);
    gao::app::set_fault_log(gao::app::boot_log);
    // The manifest makes the process code page UTF-8; match the console so
    // paths print correctly.
    SetConsoleOutputCP(CP_UTF8);
    if (argc > 2 && std::strcmp(argv[argc - 1], "--busy-ok") == 0) {
        g_busy_ok = true;
        --argc;
    }
    if (argc > 1 && std::strcmp(argv[1], "--version") == 0) {
        std::printf("%s %s\n", gao::kProductName.data(), gao::kVersion.data());
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "--probe") == 0) return probe();
    if (argc > 1 && std::strcmp(argv[1], "--busy") == 0) {
        std::printf("other programs on the card: %s\n", gao::busy_text(gao::app::busy_now()).c_str());
        return 0;
    }
    if (argc > 2 && std::strcmp(argv[1], "--set-core") == 0) {
        int mhz = 0;
        if (!ParseIntArg(argv[2], &mhz)) { std::printf("--set-core expects an integer MHz value, got '%s'\n", argv[2]); return 1; }
        return set_offset("core offset", mhz, true);
    }
    if (argc > 2 && std::strcmp(argv[1], "--set-mem") == 0) {
        int mhz = 0;
        if (!ParseIntArg(argv[2], &mhz)) { std::printf("--set-mem expects an integer MHz value, got '%s'\n", argv[2]); return 1; }
        return set_offset("mem offset", mhz, false);
    }
    if (argc > 1 && std::strcmp(argv[1], "--reset") == 0) return reset();
    if (argc > 2 && std::strcmp(argv[1], "--stress") == 0) {
        int seconds = 0;
        if (!ParseIntArg(argv[2], &seconds) || seconds < 1 || seconds > 3600) {
            std::printf("--stress expects a duration of 1-3600 seconds, got '%s'\n", argv[2]);
            return 1;
        }
        int max_temp = 85;
        auto selftest = gao::StressSelftest::None;
        int duty = 100;
        for (int i = 3; i < argc; i += 2) {
            if (i + 1 >= argc) { std::printf("%s expects a value\n", argv[i]); return 1; }
            if (std::strcmp(argv[i], "--max-temp") == 0) {
                if (!ParseIntArg(argv[i + 1], &max_temp) || max_temp < 40 || max_temp > 95) {
                    std::printf("--max-temp expects 40-95 C, got '%s'\n", argv[i + 1]);
                    return 1;
                }
            } else if (std::strcmp(argv[i], "--duty") == 0) {
                if (!ParseIntArg(argv[i + 1], &duty) || duty < 5 || duty > 100) {
                    std::printf("--duty expects 5-100 %%, got '%s'\n", argv[i + 1]);
                    return 1;
                }
            } else if (std::strcmp(argv[i], "--stress-selftest") == 0) {
                if (std::strcmp(argv[i + 1], "wrong") == 0) selftest = gao::StressSelftest::WrongResult;
                else { std::printf("--stress-selftest expects 'wrong', got '%s'\n", argv[i + 1]); return 1; }
            } else {
                std::printf("unknown --stress option '%s'\n", argv[i]);
                return 1;
            }
        }
        return stress(seconds, max_temp, selftest, duty);
    }
    if (argc > 1 && std::strcmp(argv[1], "--bandwidth") == 0) {
        int after_load_s = 0;
        if (argc > 2 && (argc != 4 || std::strcmp(argv[2], "--after-load") != 0 || !ParseIntArg(argv[3], &after_load_s) || after_load_s < 1 ||
                         after_load_s > 600)) {
            std::printf("--bandwidth accepts --after-load <seconds, 1 to 600>\n");
            return 1;
        }
        return bandwidth(after_load_s);
    }
    if (argc > 1 && std::strcmp(argv[1], "--stress-recreate") == 0) return stress_recreate();
    if (argc > 1 && std::strcmp(argv[1], "--apply") == 0) return apply();
    if (argc > 1 && std::strcmp(argv[1], "--retest") == 0) return putting_back(retest);
    if (argc > 2 && std::strcmp(argv[1], "--boot") == 0) {
        if (std::strcmp(argv[2], "on") == 0) return boot(true);
        if (std::strcmp(argv[2], "off") == 0) return boot(false);
        std::printf("--boot expects on or off, got '%s'\n", argv[2]);
        return 1;
    }
    if (argc > 2 && std::strcmp(argv[1], "--fan") == 0) {
        if (std::strcmp(argv[2], "auto") == 0) return fan_auto();
        std::printf("--fan expects auto, got '%s'\n", argv[2]);
        return 1;
    }
    if (argc > 1 && std::strcmp(argv[1], "--status") == 0) return status();
    if (argc > 1 && std::strcmp(argv[1], "--curve") == 0) return curve();
    if (argc > 1 && std::strcmp(argv[1], "--undervolt") == 0) {
        const bool on_overclock = argc > 2 && std::strcmp(argv[2], "--on-overclock") == 0;
        if (argc > 2 && !on_overclock) { std::printf("--undervolt accepts --on-overclock, got '%s'\n", argv[2]); return 1; }
        return putting_back([on_overclock] { return undervolt(on_overclock); });
    }
    if (argc > 1 && std::strcmp(argv[1], "--curve-flatten") == 0) {
        int mv = 0, mhz = 0, drop = gao::kVfTailDropKhz / 1000, core = 0;
        if (argc < 4 || !ParseIntArg(argv[2], &mv) || !ParseIntArg(argv[3], &mhz) ||
            (argc > 4 && (!ParseIntArg(argv[4], &drop) || drop < 0 || drop > 200)) ||
            (argc > 5 && (!ParseIntArg(argv[5], &core) || core < 0 || core > 1000))) {
            std::printf("--curve-flatten expects <mV> <MHz> [<MHz the points above it are stored lower, 0-200; default %d> "
                        "[<core offset in MHz the flat top sits on>]]\n",
                        gao::kVfTailDropKhz / 1000);
            return 1;
        }
        return curve_flatten(mv, mhz, drop, core);
    }
    if (argc > 1 && std::strcmp(argv[1], "--update") == 0) return update();
    if (argc > 1 && std::strcmp(argv[1], "--optimize") == 0) {
        gao::Preset preset = gao::Preset::BestOfMyGpu;
        if (argc > 2) {
            const std::string p = argv[2];
            if (p == "all") return putting_back(all_in_one);
            if (p == "best") preset = gao::Preset::BestOfMyGpu;
            else if (p == "quiet") preset = gao::Preset::Quiet;
            else if (p == "cool") preset = gao::Preset::CoolAndEfficient;
            else if (p == "max") preset = gao::Preset::MaxPerformance;
            else { std::printf("--optimize expects all, best, quiet, cool or max, got '%s'\n", argv[2]); return 1; }
        }
        std::optional<gao::FanCurve> fan_curve;
        if (argc > 3) {
            const auto fp = argc > 4 && std::strcmp(argv[3], "--fan-curve") == 0 ? gao::fan_preset_from_name(argv[4]) : std::nullopt;
            if (!fp) { std::printf("--optimize <profile> accepts --fan-curve silent|normal|cool|aggressive\n"); return 1; }
            fan_curve = gao::fan_preset_curve(*fp);
        }
        return putting_back([&] { return optimize(preset, fan_curve); });
    }
    std::printf("usage: gao [--version | --probe | --set-core <mhz> | --set-mem <mhz> | --reset\n"
                "            | --busy (the other programs that use the card now, as a run looks for them; changes nothing)\n"
                "            | --stress <sec> [--max-temp <c>] [--duty <pct> (a light load: works that share of the time)]\n"
                "            | --bandwidth [--after-load <sec>]\n"
                "            | --stress-recreate (loads the card for two 3 s runs around a device rebuild;\n"
                "              changes no setting; exit 0 = both STABLE, rebuilt and a clean first batch, 1 = anything else,\n"
                "              never --stress's 2 / 3)\n"
                "            | --optimize [best|quiet|cool|max [--fan-curve silent|normal|cool|aggressive]]\n"
                "            | --optimize all (all in one: stock measured, overclock, an undervolt on top of it at a clock\n"
                "              between the two, the quietest fan curve, and a final test; about 40 minutes)\n"
                "            | --apply | --boot on|off | --fan auto | --status\n"
                "            | --retest (after a driver update: the saved tune under full load for five minutes; applied and\n"
                "              saved for the new driver when that is stable, stock when it is not)\n"
                "            | --curve (prints the voltage/frequency curve; changes nothing)\n"
                "            | --undervolt [--on-overclock] (the load clock on the lowest stable voltage; saved like --optimize.\n"
                "              Alone it replaces a saved overclock; with --on-overclock it is searched on top of the\n"
                "              saved tune and saved into it)\n"
                "            | --curve-flatten <mV> <MHz> [<MHz>] (the point at <mV> runs <MHz> and no point above it more;\n"
                "              the third number is how much lower those are stored, 0 for exactly flat; --reset undoes it)\n"
                "            | --update (installs the latest release into this folder, if there is a newer one)]\n"
                "A run (--optimize, --undervolt, --retest) looks first at what else uses the card and does not start next to\n"
                "programs that do; --busy-ok as the last argument lets it.\n");
    return argc > 1 ? 1 : 0;
}
