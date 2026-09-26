cmake_minimum_required(VERSION 3.28)
project(gpu_auto_optimizer CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT: gao.exe runs elevated, and the VC++ runtime DLLs
# (MSVCP140/VCRUNTIME140) are not KnownDLLs -- a copy planted next to the exe
# would load first. Static linking also drops the redistributable dependency.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

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
  src/core/boot.hpp
  src/core/boot.cpp
  src/core/task_xml.hpp
  src/core/task_xml.cpp
)
target_include_directories(core PUBLIC src)
target_include_directories(core PRIVATE third_party)

# The only code allowed to touch hardware.
add_library(hw STATIC
  src/hw/nvml.hpp src/hw/nvml.cpp
  src/hw/nvapi.hpp src/hw/nvapi.cpp
  src/hw/gpu_control.hpp src/hw/gpu_control.cpp
  src/hw/stress.hpp src/hw/stress.cpp
  src/hw/app_files.hpp src/hw/app_files.cpp
  src/hw/boot_task.hpp src/hw/boot_task.cpp
)
target_include_directories(hw PUBLIC src)
target_link_libraries(hw PUBLIC core d3d11 dxgi d3dcompiler shell32 ole32)

add_executable(gao src/app/main.cpp)
target_link_libraries(gao PRIVATE core hw)
# d3dcompiler_47.dll, d3d11.dll and dxgi.dll are not KnownDLLs: delay-load
# them so they resolve after SetDefaultDllDirectories(SYSTEM32) in main(),
# never from the exe's folder.
target_link_libraries(gao PRIVATE delayimp)
target_link_options(gao PRIVATE /DELAYLOAD:d3dcompiler_47.dll /DELAYLOAD:d3d11.dll /DELAYLOAD:dxgi.dll
                   /DEPENDENTLOADFLAG:0x800)

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
  tests/test_boot.cpp
  tests/test_task_xml.cpp
)
target_link_libraries(core_tests PRIVATE core doctest)
add_test(NAME core_tests COMMAND core_tests)
