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

    // Self-healing cache refresh (v1.3.4). Call once, after hook install. For each
    // module whose TOML came from the local cache AND had a required function fail
    // to resolve, re-checks the mirror chain on a background thread (short
    // timeouts, at most once per 24h per file, [remote] revalidate = false turns it
    // off). The cache is replaced only by a strictly better TOML; the user is told
    // to restart Steam. Healthy modules never cause a request. Non-blocking.
    void StartCacheRevalidation();

    // Shadow-mode recorder (v1.5.0, pillar 4). Call once, after StartCacheRevalidation.
    // With [shadow] record = true (default false), for each module whose required
    // functions all resolved and that has no fingerprint file yet, a low-priority
    // background thread reads the DLL from disk and writes
    // <Steam>/amethysttool/fingerprint/<component>/<sha256>.toml (temp file + atomic
    // rename). It works on a copy of the (name, rva) list taken here, shares no lock
    // with the hooks, and catches every exception. Local files only; nothing is sent.
    void StartShadowRecording();

} // namespace PatternLoader
