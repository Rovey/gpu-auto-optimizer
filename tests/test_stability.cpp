#include "doctest/doctest.h"
#include "core/stability.hpp"
#include <string>

using namespace gao;

namespace {
Telemetry tel(int temp, int power = 190, int core = 2700, int mem = 10500) {
    Telemetry t;
    t.ok = true; t.temp_c = temp; t.power_w = power; t.core_mhz = core; t.mem_mhz = mem;
    return t;
}
StressBatch good(long long its = 100, double ms = 250) { return {its, 0, false, ms}; }
}

TEST_CASE("twelve good 250 ms batches make a stable 3 s run") {
    int calls = 0;
    const auto r = run_stability([&] { ++calls; return good(); },
                                 [] { return tel(60); }, 3.0, 85);
    CHECK(r.verdict == Verdict::Stable);
    CHECK(calls == 12);
    CHECK(r.seconds == doctest::Approx(3.0));
    CHECK(r.score == doctest::Approx(1200 / 3.0));
    CHECK(r.peak_temp_c == 60);
    CHECK(r.avg_power_w == 190);
    CHECK(r.avg_core_mhz == 2700);
    CHECK(r.avg_mem_mhz == 10500);
}

TEST_CASE("the first wrong value stops the run") {
    int calls = 0;
    const auto r = run_stability([&] { ++calls; return calls == 3 ? StressBatch{100, 1, false, 250} : good(); },
                                 [] { return tel(60); }, 60.0, 85);
    CHECK(r.verdict == Verdict::WrongResult);
    CHECK(calls == 3);
}

TEST_CASE("a lost device stops the run") {
    int calls = 0;
    const auto r = run_stability([&] { ++calls; return calls == 2 ? StressBatch{0, 0, true, 2000} : good(); },
                                 [] { return tel(60); }, 60.0, 85);
    CHECK(r.verdict == Verdict::DeviceLost);
    CHECK(calls == 2);
}

TEST_CASE("device lost wins over a garbage error count") {
    const auto r = run_stability([] { return StressBatch{0, 12345, true, 2000}; },
                                 [] { return tel(60); }, 60.0, 85);
    CHECK(r.verdict == Verdict::DeviceLost);
}

TEST_CASE("overheating stops the run and records the peak") {
    int reads = 0;
    const auto r = run_stability([] { return good(); },
                                 [&] { ++reads; return tel(reads == 4 ? 86 : 70); }, 60.0, 85);
    CHECK(r.verdict == Verdict::TooHot);
    CHECK(reads == 4);
    CHECK(r.peak_temp_c == 86);
}

TEST_CASE("temperature at the ceiling is not too hot") {
    const auto r = run_stability([] { return good(); }, [] { return tel(85); }, 1.0, 85);
    CHECK(r.verdict == Verdict::Stable);
}

TEST_CASE("losing telemetry stops the run") {
    int calls = 0;
    const auto r = run_stability([&] { ++calls; return good(); },
                                 [] { return Telemetry{}; }, 60.0, 85);   // ok == false
    CHECK(r.verdict == Verdict::NoTelemetry);
    CHECK(calls == 1);
}

TEST_CASE("unknown power is excluded from the average") {
    int reads = 0;
    const auto r = run_stability([] { return good(); },
                                 [&] { ++reads; return tel(60, reads % 2 ? -1 : 200); }, 1.0, 85);
    CHECK(r.avg_power_w == 200);
    const auto none = run_stability([] { return good(); }, [] { return tel(60, -1); }, 1.0, 85);
    CHECK(none.avg_power_w == -1);
}

TEST_CASE("averages are weighted by batch time, so short warm-up batches do not dominate") {
    // hw starts with ~1 ms batches while it calibrates; ten of those at low
    // power followed by one 250 ms batch at full power is a full-power run.
    int calls = 0;
    const auto r = run_stability([&] { ++calls; return calls <= 10 ? good(1, 1) : good(100, 250); },
                                 [&] { return tel(60, calls <= 10 ? 50 : 200); }, 0.26, 85);
    CHECK(calls == 11);
    CHECK(r.avg_power_w == 194);   // (10*1*50 + 250*200) / 260 = 194.2
}

TEST_CASE("a zero-length run still judges one batch") {
    int calls = 0;
    const auto r = run_stability([&] { ++calls; return good(); }, [] { return tel(60); }, 0.0, 85);
    CHECK(calls == 1);
    CHECK(r.verdict == Verdict::Stable);
    CHECK(r.score > 0);
}

TEST_CASE("a batch reporting no elapsed time cannot hang the loop") {
    int calls = 0;
    // 0.0095 s, not 0.01: ten 1 ms steps summed in floating point may land a
    // hair below 0.01 and make the count flaky.
    const auto r = run_stability([&] { ++calls; return good(100, 0); }, [] { return tel(60); }, 0.0095, 85);
    CHECK(r.verdict == Verdict::Stable);
    CHECK(calls == 10);   // each batch counts as at least 1 ms
}

TEST_CASE("every verdict has a name") {
    CHECK(std::string(verdict_name(Verdict::Stable)) == "STABLE");
    CHECK(std::string(verdict_name(Verdict::WrongResult)) == "WRONG RESULT");
    CHECK(std::string(verdict_name(Verdict::DeviceLost)) == "DEVICE LOST");
    CHECK(std::string(verdict_name(Verdict::TooHot)) == "TOO HOT");
    CHECK(std::string(verdict_name(Verdict::NoTelemetry)) == "NO TELEMETRY");
    CHECK(std::string(verdict_name(Verdict::Aborted)) == "ABORTED");
    CHECK(std::string(verdict_name(Verdict::Stalled)) == "STALLED");
}

TEST_CASE("a stop request ends the run after the batch it arrives in") {
    int calls = 0;
    const auto r = run_stability([&] { ++calls; return good(); }, [] { return tel(60); }, 300.0, 85,
                                 [&] { return calls >= 3; });
    CHECK(r.verdict == Verdict::Aborted);
    CHECK(calls == 3);
    CHECK(r.seconds == doctest::Approx(0.75));
}

TEST_CASE("a failure in the same batch wins over a stop request") {
    const auto wrong = run_stability([] { return StressBatch{100, 1, false, 250}; }, [] { return tel(60); },
                                     300.0, 85, [] { return true; });
    CHECK(wrong.verdict == Verdict::WrongResult);
    const auto hot = run_stability([] { return good(); }, [] { return tel(90); }, 300.0, 85, [] { return true; });
    CHECK(hot.verdict == Verdict::TooHot);
}

TEST_CASE("a stop callback that never fires changes nothing") {
    int calls = 0;
    const auto r = run_stability([&] { ++calls; return good(); }, [] { return tel(60); }, 3.0, 85,
                                 [] { return false; });
    CHECK(r.verdict == Verdict::Stable);
    CHECK(calls == 12);
}

TEST_CASE("a card that stops computing ends the run as STALLED within seconds") {
    int calls = 0;
    // Twelve good batches (400 it/s), then batches that take their time and compute nothing.
    const auto r = run_stability([&] { ++calls; return calls <= 12 ? good() : StressBatch{0, 0, false, 250}; },
                                 [] { return tel(60); }, 300.0, 85, {}, 100.0);
    CHECK(r.verdict == Verdict::Stalled);
    CHECK(calls == 22);   // the tenth dead batch leaves 200 iterations in the last three seconds: 67 it/s
}

TEST_CASE("slow warm-up batches do not trip the stall floor") {
    int calls = 0;
    // Ten 10 ms batches of one iteration (100 it/s, below the floor), then full batches.
    const auto r = run_stability([&] { ++calls; return calls <= 10 ? good(1, 10) : good(100, 250); },
                                 [] { return tel(60); }, 3.0, 85, {}, 300.0);
    CHECK(r.verdict == Verdict::Stable);
}

TEST_CASE("one delayed batch is not a stall") {
    int calls = 0;
    // A single batch that takes 2 s and computes little, in an otherwise healthy run.
    const auto r = run_stability([&] { ++calls; return calls == 20 ? StressBatch{10, 0, false, 2000} : good(); },
                                 [] { return tel(60); }, 20.0, 85, {}, 100.0);
    CHECK(r.verdict == Verdict::Stable);
}

TEST_CASE("a real failure in the same batch wins over a stall") {
    const auto r = run_stability([] { return StressBatch{0, 1, false, 4000}; }, [] { return tel(60); },
                                 300.0, 85, {}, 100.0);
    CHECK(r.verdict == Verdict::WrongResult);
}

TEST_CASE("without a floor a run that computes nothing is not stalled") {
    const auto r = run_stability([] { return StressBatch{0, 0, false, 250}; }, [] { return tel(60); }, 4.0, 85);
    CHECK(r.verdict == Verdict::Stable);
}
