#include "doctest/doctest.h"
#include "core/stress_math.hpp"
#include <string>

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

TEST_CASE("the runs of a bandwidth measurement read as one line: how many, between what, how many thrown away") {
    // 0 is a run the driver's timer called unusable: the clock changed under it.
    CHECK(bandwidth_runs_text({41.0, 41.2, 41.1}) == "3 readings from 41.0 to 41.2 GB/s (spread 0.5 %)");
    CHECK(bandwidth_runs_text({30.0, 0, 36.0, 0, 0, 33.0}) ==
          "3 readings from 30.0 to 36.0 GB/s (spread 20.0 %), 3 discarded for a clock change");
    CHECK(bandwidth_runs_text({42.5}) == "1 reading of 42.5 GB/s");
}

TEST_CASE("a bandwidth measurement without a single reading says so") {
    CHECK(bandwidth_runs_text({0, 0, 0, 0}) == "no reading, 4 discarded for a clock change");
    CHECK(bandwidth_runs_text({}) == "no reading");
}
