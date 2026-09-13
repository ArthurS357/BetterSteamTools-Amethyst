#pragma once

#include "Steam/Types.h"

#include <cstdint>

// ─────────────────────────────────────────────────────────────────
//  ManifestCache — pre-seed <steam>\depotcache from a manifest archive.
//
//  Opt-in only: disabled unless [cache] url is set in amethysttool.toml.
//  When configured, fetches an archived depot manifest from that base URL
//  and drops it into the ROOT depotcache before Steam asks for a manifest
//  request code. Steam then reads the cached manifest and skips the
//  request-code/CDN path entirely.
//
//  All calls are best-effort: a miss (404), a bad body, a disabled feature,
//  or any transport failure simply leaves no file, and Steam falls through
//  to the existing request-code path. Safe to call from a detached worker.
//
//  The actual file/negative-cache/validation logic lives in
//  ManifestCacheLogic, which takes the archive URL and depotcache directory
//  as parameters -- this header just resolves both from [cache] url / the
//  running Steam process and wires in the real HTTP client.
// ─────────────────────────────────────────────────────────────────
namespace ManifestCache {

    // Ensure <steam>\depotcache\<depot>_<gid>.manifest exists, fetching it
    // from [cache] url if configured and missing. Returns true when the file
    // is present afterwards (already cached or freshly written). Returns
    // false -- quietly -- when [cache] url is empty, the manifest is not
    // archived, or the body fails validation. Never throws.
    //
    // recvTimeoutMs bounds the body-receive wait. 0 keeps the built-in
    // (generous) default, sized for the ~19 MB a real manifest can reach.
    //
    // outNotArchived, when provided, is set to true ONLY when the archive
    // gave a definitive 404 -- not for timeouts or other transient failures.
    //
    // bypassNegativeCache forces a fresh fetch even for a depot:gid that
    // recently 404'd. Pass true only when a download is actually starting.
    bool EnsureCached(AppId_t app, uint32_t depot, uint64_t gid,
                      uint32_t recvTimeoutMs = 0, bool* outNotArchived = nullptr,
                      bool bypassNegativeCache = false);

}
