#pragma once
#include <cstdint>
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

}
