#include "doctest/doctest.h"
#include "hw/gpu_users.hpp"

using namespace gao;

TEST_CASE("the name of a GPU Engine counter instance gives the process, the adapter and the engine") {
    const auto a = parse_gpu_engine_instance(L"pid_15616_luid_0x00000000_0x0000E9C1_phys_0_eng_0_engtype_3D");
    REQUIRE(a);
    CHECK(a->pid == 15616);
    CHECK(a->luid == 0xE9C1ull);
    CHECK(a->engine == "3D");
    const auto b = parse_gpu_engine_instance(L"pid_4_luid_0x0000000A_0x00012345_phys_1_eng_7_engtype_VideoDecode");
    REQUIRE(b);
    CHECK(b->pid == 4);
    CHECK(b->luid == 0x0000000A00012345ull);
    CHECK(b->engine == "VideoDecode");
}

TEST_CASE("a name that is not one gives nothing") {
    CHECK_FALSE(parse_gpu_engine_instance(L""));
    CHECK_FALSE(parse_gpu_engine_instance(L"_Total"));
    CHECK_FALSE(parse_gpu_engine_instance(L"pid_12_luid_0x0_0x1_phys_0_eng_0"));
    CHECK_FALSE(parse_gpu_engine_instance(L"luid_0x00000000_0x0000E9C1_phys_0"));
}
