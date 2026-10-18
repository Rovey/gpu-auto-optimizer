#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace gao {

// The stress load multiplies two N x N matrices of small whole numbers stored
// as float. With |values| <= 8, every product is <= 64 and every dot product
// is <= N * 64, which must stay below 2^24 -- the largest range in which FP32
// represents every integer exactly. Inside that range the GPU's result does
// not depend on summation order or FMA use, so a CPU reference is exact and
// any difference is a real computation error.
inline constexpr int kStressN = 1024;
inline constexpr int kStressMaxAbs = 8;
static_assert(kStressN * kStressMaxAbs * kStressMaxAbs < (1 << 24),
              "stress inputs would leave the exact-FP32 range");

// n*n values in -kStressMaxAbs..kStressMaxAbs, row-major. Same seed -> same
// matrix on every compiler: std::mt19937's output sequence is fixed by the
// standard (the distribution classes are not, so none is used).
std::vector<float> make_stress_matrix(std::uint32_t seed, int n = kStressN);

// C = A * B, row-major, n x n.
std::vector<float> reference_matmul(const std::vector<float>& a,
                                    const std::vector<float>& b, int n = kStressN);

// The runs of one bandwidth measurement in one line, for a log: how many gave
// a reading and between which values, and how many were thrown away. A run is
// its GB/s, or 0 when the driver's timer called it unusable (the clock changed
// under it). What a measurement that never settled looked like is the only
// way to tell why it did not.
std::string bandwidth_runs_text(const std::vector<double>& runs);

// A light load for --stress --duty: the pause after a batch that took
// elapsed_ms, so the card works duty_pct of the time and idles the rest. A
// card that stays cool while it draws more than at idle is how the fans'
// idle rule is checked on hardware (core/fan_curve.hpp). 0 at 100 % or more.
int duty_pause_ms(double elapsed_ms, int duty_pct);

}
