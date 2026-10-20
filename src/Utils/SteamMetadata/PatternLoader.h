#pragma once

#include "OSTPlatform/include/DynamicLibrary.h"

#include <string>

namespace PatternLoader {

    // Load metadata before installing hooks for a module.
    bool Load(OSTPlatform::DynamicLibrary::ModuleHandle module, const std::string& dllPath, const std::string& component);

    // Resolve by RVA first, then fall back to signature scanning. A miss is
    // listed in the "Missing Signatures" popup (ReportMissingFunctions).
    void* FindPattern(OSTPlatform::DynamicLibrary::ModuleHandle module, const char* funcName);

    // Same lookup for best-effort hooks: a miss is logged and returns nullptr,
    // but is never listed in the popup.
    void* FindPatternOptional(OSTPlatform::DynamicLibrary::ModuleHandle module, const char* funcName);

    // Report unresolved required functions after all hooks have been installed.
    void ReportMissingFunctions();

} // namespace PatternLoader
