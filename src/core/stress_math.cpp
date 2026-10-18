#include "core/stress_math.hpp"
#include <algorithm>
#include <cstdio>
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

int duty_pause_ms(const double elapsed_ms, const int duty_pct) {
    if (duty_pct >= 100 || duty_pct <= 0 || elapsed_ms <= 0) return 0;
    return static_cast<int>(elapsed_ms * (100 - duty_pct) / duty_pct + 0.5);
}

std::string bandwidth_runs_text(const std::vector<double>& runs) {
    int readings = 0, discarded = 0;
    double lo = 0, hi = 0;
    for (const double r : runs) {
        if (r <= 0) { ++discarded; continue; }
        lo = readings ? std::min(lo, r) : r;
        hi = readings ? std::max(hi, r) : r;
        ++readings;
    }
    char buf[160];
    if (readings == 0) std::snprintf(buf, sizeof(buf), "no reading");
    else if (readings == 1) std::snprintf(buf, sizeof(buf), "1 reading of %.1f GB/s", lo);
    else std::snprintf(buf, sizeof(buf), "%d readings from %.1f to %.1f GB/s (spread %.1f %%)", readings, lo, hi, (hi / lo - 1) * 100);
    std::string text = buf;
    if (discarded > 0) text += ", " + std::to_string(discarded) + " discarded for a clock change";
    return text;
}

}
