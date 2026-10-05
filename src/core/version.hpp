#pragma once
// The one place the version lives. Macros, because the resource compiler
// (app.rc, the exe's file properties) cannot read C++.
#define GAO_VERSION_MAJOR 0
#define GAO_VERSION_MINOR 3
#define GAO_VERSION_PATCH 1
#define GAO_VERSION_STRING "0.3.1"

#ifndef RC_INVOKED
#include <string_view>

namespace gao {
inline constexpr std::string_view kProductName = "gpu-auto-optimizer";
inline constexpr std::string_view kVersion = GAO_VERSION_STRING;
// The version as one number that orders like it (see version_number()).
inline constexpr unsigned kVersionNumber = (GAO_VERSION_MAJOR << 16) | (GAO_VERSION_MINOR << 8) | GAO_VERSION_PATCH;
}
#endif
