#pragma once
// The one place the version lives. Macros, because the resource compiler
// (app.rc, the exe's file properties) cannot read C++.
#define GAO_VERSION_MAJOR 0
#define GAO_VERSION_MINOR 1
#define GAO_VERSION_PATCH 0
#define GAO_VERSION_STRING "0.1.0"

#ifndef RC_INVOKED
#include <string_view>

namespace gao {
inline constexpr std::string_view kProductName = "gpu-auto-optimizer";
inline constexpr std::string_view kVersion = GAO_VERSION_STRING;
}
#endif
