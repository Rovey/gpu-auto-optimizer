cmake_minimum_required(VERSION 3.28)
project(gpu_auto_optimizer CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(doctest INTERFACE)
target_include_directories(doctest INTERFACE third_party)
# Without this, doctest forward-declares std::string/ostream itself (its own
# comment calls this "not permitted by the C++ Standard") instead of including
# <string>/<ostream>, which breaks as soon as a test streams a real std type.
target_compile_definitions(doctest INTERFACE DOCTEST_CONFIG_USE_STD_HEADERS)

# Pure logic. No Windows, no driver, no D3D — this is what CI tests.
# INTERFACE, not STATIC: header-only, so there is nothing to compile into an
# archive (a STATIC target with zero translation units builds "successfully"
# but never produces a .lib, which breaks every consumer at link time).
add_library(core INTERFACE
  src/core/version.hpp
)
target_include_directories(core INTERFACE src)

# The only code allowed to touch hardware.
add_library(hw STATIC
  src/hw/nvml.hpp src/hw/nvml.cpp
)
target_include_directories(hw PUBLIC src)
target_link_libraries(hw PUBLIC core)

add_executable(gao src/app/main.cpp)
target_link_libraries(gao PRIVATE core hw)

enable_testing()
add_executable(core_tests
  tests/test_main.cpp
  tests/test_version.cpp
)
target_link_libraries(core_tests PRIVATE core doctest)
add_test(NAME core_tests COMMAND core_tests)
