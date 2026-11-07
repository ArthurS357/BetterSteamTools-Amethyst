#pragma once

// Linux stand-in for src/dllmain.h, which drags in the Windows-only platform layer.
// LuaConfig.cpp needs only the Steam types and the logging macros from it, and
// logging is a no-op in the test build on Windows too.
//
// Only reachable through tests/linux/luaconfig/CMakeLists.txt.

#include "Steam/Types.h"
#include "Utils/Config/LuaConfig.h"

#define LOG_INFO(...)          ((void)0)
#define LOG_WARN(...)          ((void)0)
#define LOG_PACKAGE_INFO(...)  ((void)0)
#define LOG_MANIFEST_INFO(...) ((void)0)
#define LOG_MANIFEST_WARN(...) ((void)0)
