#pragma once
#include "Steam/Types.h"
#include <string_view>

// ─────────────────────────────────────────────────────────────────
//  ManifestClient — HTTP client for depot manifest request codes.
//  Provider table is internal (see kProviders in ManifestClient.cpp);
//  adding a new provider only requires one row there.
//
//  Thread-safe — serialises access to the underlying WinHTTP connection.
// ─────────────────────────────────────────────────────────────────
namespace ManifestClient {

    // Select the active provider by its string name (matches kProviders[i].name).
    // Returns false if no provider matches; the previous selection is kept.
    bool SetProvider(std::string_view name);

    // Name of the currently active provider (for logging / diagnostics).
    const char* ActiveProviderName();

    // Overrides every built-in provider with a custom URL template containing
    // the literal placeholder "{gid}" (substituted with the manifest gid as an
    // unsigned 64-bit decimal). Takes precedence over SetProvider while set.
    // Pass an empty string to clear the override and revert to the provider
    // selected via SetProvider.
    //
    // Returns false (override left unchanged) if `urlTemplate` is non-empty and
    // missing "{gid}". The response is expected as a plain decimal request code
    // in the body -- same wire format as the "opensteamtool"/"wudrm" providers;
    // a custom endpoint cannot reuse the "steamrun" JSON parser.
    bool SetUrlTemplateOverride(std::string_view urlTemplate);

    // Resolve a manifest GID to its request code. Tries Lua first
    // (fetch_manifest_code_ex, then fetch_manifest_code), then the
    // active provider. Returns true and sets *outRequestCode on success.
    bool FetchManifestRequestCode(uint64_t manifestGid, uint64_t* outRequestCode,
                                  AppId_t appId = 0, AppId_t depotId = 0);

    // Tear down the cached WinHTTP connection (call at unload).
    void Shutdown();
}
