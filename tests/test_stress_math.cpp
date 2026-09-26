#include "doctest/doctest.h"
#include "core/stress_math.hpp"

using namespace gao;

TEST_CASE("stress matrix is deterministic per seed and stays in range") {
    const auto a = make_stress_matrix(1, 32);
    const auto again = make_stress_matrix(1, 32);
    const auto other = make_stress_matrix(2, 32);
    REQUIRE(a.size() == 32u * 32u);
    CHECK(a == again);
    CHECK(a != other);
    bool has_negative = false, has_positive = false;
    for (float v : a) {
        CHECK(v >= -kStressMaxAbs);
        CHECK(v <= kStressMaxAbs);
        CHECK(v == static_cast<float>(static_cast<int>(v)));   // whole numbers only
        has_negative |= v < 0;
        has_positive |= v > 0;
    }
    CHECK(has_negative);
    CHECK(has_positive);
}

TEST_CASE("reference matmul equals a naive double computation") {
    const int n = 8;
    const auto a = make_stress_matrix(3, n);
    const auto b = make_stress_matrix(4, n);
    const auto c = reference_matmul(a, b, n);
    REQUIRE(c.size() == static_cast<size_t>(n * n));
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j) {
            double sum = 0;
            for (int k = 0; k < n; ++k) sum += double(a[i * n + k]) * double(b[k * n + j]);
            CHECK(c[i * n + j] == static_cast<float>(sum));
        }
}
