cmake_minimum_required(VERSION 3.28)
project(gpu_auto_optimizer CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# doctest vendored as a single header (no vcpkg / network dependency)
add_library(doctest INTERFACE)
target_include_directories(doctest INTERFACE third_party)

# core logic library (pure, unit-testable)
add_library(core STATIC
  src/core/version.hpp
  src/core/version.cpp
)
target_include_directories(core PUBLIC src)

enable_testing()
add_executable(core_tests
  tests/test_main.cpp
  tests/test_smoke.cpp
)
target_link_libraries(core_tests PRIVATE core doctest)
add_test(NAME core_tests COMMAND core_tests)
# real-profile fixture is opened relative to the source root
set_tests_properties(core_tests PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
