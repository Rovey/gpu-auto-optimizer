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
add_library(core STATIC
  src/core/version.hpp
  src/core/types.hpp
  src/core/objectives.hpp
  src/core/objectives.cpp
  src/core/config.hpp
  src/core/config.cpp
  src/core/stress_math.hpp
  src/core/stress_math.cpp
  src/core/stability.hpp
  src/core/stability.cpp
  src/core/journal.hpp
  src/core/journal.cpp
  src/core/search.hpp
  src/core/search.cpp
)
target_include_directories(core PUBLIC src)
target_include_directories(core PRIVATE third_party)

# The only code allowed to touch hardware.
add_library(hw STATIC
  src/hw/nvml.hpp src/hw/nvml.cpp
  src/hw/nvapi.hpp src/hw/nvapi.cpp
  src/hw/gpu_control.hpp src/hw/gpu_control.cpp
  src/hw/stress.hpp src/hw/stress.cpp
  src/hw/journal_file.hpp src/hw/journal_file.cpp
)
target_include_directories(hw PUBLIC src)
target_link_libraries(hw PUBLIC core d3d11 dxgi d3dcompiler)

add_executable(gao src/app/main.cpp)
target_link_libraries(gao PRIVATE core hw)

enable_testing()
add_executable(core_tests
  tests/test_main.cpp
  tests/test_version.cpp
  tests/test_objectives.cpp
  tests/test_config.cpp
  tests/test_types.cpp
  tests/test_stress_math.cpp
  tests/test_stability.cpp
  tests/test_journal.cpp
  tests/test_search.cpp
)
target_link_libraries(core_tests PRIVATE core doctest)
add_test(NAME core_tests COMMAND core_tests)
