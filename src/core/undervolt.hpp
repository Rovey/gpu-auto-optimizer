#pragma once
#include "core/journal.hpp"
#include "core/search.hpp"
#include "core/stability.hpp"
#include "core/types.hpp"
#include <functional>
#include <string>

namespace gao {

// The automatic undervolt: the same speed on less voltage.
//
// Under load at stock the card settles on some clock (on a card that is
// power-limited, well below the top of its curve). The search keeps that
// clock and lowers the voltage it is reached at: a flat top (core/vf_curve)
// anchored one curve point lower each time, until a probe fails. What a
// candidate asks of the card is its "raise": how far the anchor runs above
// its built-in frequency. That is the same quantity as a core clock offset,
// seen at one point of the curve, so the same rules apply: the first failure
// ends the descent, the edge is confirmed with a 30 s probe, the result keeps
// the core search's safety margin on the raise and must pass the 300 s soak.
//
// Every candidate is journaled, with its raise, before the curve is written;
// a raise that froze the machine is a ceiling for later runs. Between two
// candidates the curve goes back to stock (apply_flat_top clears first). A
// driver reset ends the descent: the card is reconnected, set to stock,
// rested and must compute normally again before the confirm probe, which
// then starts kResetBackoffSteps points higher. A second reset ends the run.
// Any failure ends at stock.

inline constexpr double kUvBaselineS = 30, kUvProbeS = 3, kUvConfirmS = 30, kUvSoakS = 300;
inline constexpr int kUvConfirmTries = 3;
inline constexpr int kUvSoakAttempts = 3;
// A candidate must run the target clock within what the built-in curve moves
// with temperature (45 MHz between cold and warm on the reference card: a
// flat top written on a cooler card runs that much lower once it is hot), and
// keep this share of the stock score (3 s probes scatter by a few percent
// around the 30 s baseline). One that computes right but clearly slower took
// the curve without running it ("SLOW").
inline constexpr int kUvClockSlackKhz = 45000;
inline constexpr double kUvScoreKeep = 0.9;

struct UndervoltIo {
    Probe probe;                                    // one stress run, as in the clock search
    std::function<bool()> aborted;
    std::function<void(const std::string&)> log;
    std::function<bool(double seconds)> rest;       // waits without load; false: cut short by a stop request
    std::function<bool()> prepare_load;             // a fresh stress load after a driver reset; false: not ready
};

struct UndervoltResult {
    bool ok = false;
    std::string reason;            // when !ok
    bool stock_restored = true;    // when !ok: false if the card could not be put back to stock
    int freq_khz = 0;              // the clock the card keeps under load
    int stock_uv = 0;              // the voltage at which it reaches that clock by itself
    int edge_uv = 0;               // the lowest voltage that passed a short probe
    int confirmed_uv = 0;          // the lowest that held the 30 s probe
    int applied_uv = 0;            // what is applied now: confirmed, plus the margin
    int applied_index = -1;        // its slot on the curve
    int driver_resets = 0;
    StabilityResult baseline;      // 30 s at stock
    StabilityResult after;         // the soak, with the result applied
};

// Leaves the undervolt applied when ok. perf_push: as in Objectives, how much
// of the confirmed raise is kept.
UndervoltResult find_undervolt(const GpuControl& gpu, Journal& journal, const UndervoltIo& io, int max_temp_c, float perf_push);

}
