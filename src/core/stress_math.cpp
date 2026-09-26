#include "core/stress_math.hpp"
#include <random>

namespace gao {

std::vector<float> make_stress_matrix(std::uint32_t seed, int n) {
    std::mt19937 rng(seed);
    std::vector<float> m(static_cast<size_t>(n) * n);
    constexpr std::uint32_t span = 2 * kStressMaxAbs + 1;
    for (float& v : m) v = static_cast<float>(static_cast<int>(rng() % span) - kStressMaxAbs);
    return m;
}

std::vector<float> reference_matmul(const std::vector<float>& a,
                                    const std::vector<float>& b, int n) {
    std::vector<float> c(static_cast<size_t>(n) * n, 0.0f);
    // i-k-j order walks B and C row by row (cache friendly); the result is
    // exact in float regardless of order -- see the header.
    for (int i = 0; i < n; ++i)
        for (int k = 0; k < n; ++k) {
            const float aik = a[static_cast<size_t>(i) * n + k];
            const float* brow = &b[static_cast<size_t>(k) * n];
            float* crow = &c[static_cast<size_t>(i) * n];
            for (int j = 0; j < n; ++j) crow[j] += aik * brow[j];
        }
    return c;
}

}
