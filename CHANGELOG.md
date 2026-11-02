# Changelog

## [v1.3.4] - 2026-11-01 (prep)

> **Prep:** nothing here has been built or run yet. Pending local `build.ps1` (Release + Debug) and `ctest`; expected 250 tests (213 + 37 new).

### Added
- **Self-healing pattern cache.** A cached pattern TOML was never re-downloaded, so a mirror fix for an already-cached Steam build (a missing function added, a signature corrected) never reached the user until they deleted `<Steam>\amethysttool\pattern` by hand. Now, when a *required* function from a cached file fails to resolve, `PatternLoader::StartCacheRevalidation` re-checks the mirror chain on a background thread (2 s resolve/connect/send, 3 s recv per mirror; at most once per 24 h per file via a `<sha256>.toml.checked` stamp). The cache is replaced atomically (temp + `MoveFileExW`) only by a TOML that parses, keeps every function key the old file had and resolves at least one missing function; the user is then asked to restart Steam. A healthy cache never triggers a request. Opt out with `[remote] revalidate = false`. Policy in `Utils/SteamMetadata/PatternRefreshLogic.h`.
- **Corrupt cache entry self-repair.** A cached pattern TOML that parses to nothing is now re-downloaded synchronously like a cache miss (instead of failing every launch). Also gated by `[remote] revalidate`.
- If the `.checked` throttle stamp cannot be written, the background revalidation is skipped (otherwise the 24 h limit would degrade to "every launch").

### Fixed
- **Lock-order risk in RichPresence.** `ApplyGameFields` ran under `g_RPMutex` and could call `Hooks_Misc::GetGameNameByAppID`, which on a miss calls into Steam's AppInfo cache (unknown internal locks). The name is now resolved in `TrackSend` *before* taking `g_RPMutex`; under the lock only the new cache-only `TryGetCachedGameName` is used.
- **`g_GameNameCache` data race.** The game-name map was read/written from the send hook (OnlineFix, RichPresence) and the recv hook without a lock; it is now guarded by a leaf mutex that is never held across the Steam call.
- **Forge fallback probes ignored the active account.** Only the localconfig base checked the ticket's SteamID; the fallback probes (appid 7, 228980, seen tickets) could still pick a ticket issued to another account -> error 54. They now use the same check (`ForgeBaseCache::TicketBelongsToAccount`; unknown active account keeps the old behaviour).
- **Forge base ticket cache is now per account.** `AppTicket` cached the forge base for the whole session, so switching Steam accounts (or a Family Sharing session) without restarting Steam reused the old SteamID and caused Denuvo error 54. The cache (`Utils/Tickets/ForgeBaseCache.h`) now remembers which account it was resolved for, re-reads the active user on every call and re-resolves the base when it changes.
- **RichPresence data race (send vs. recv).** The send hook wrote the playing appid, local SteamID, per-app RP KVs and the manufactured inject packet while the recv hook read/wrote the cached self-PersonaState template and delivered the inject packet. All of that state is now guarded by `g_RPMutex`; `TryInject` copies the packet under the lock and calls the original recv outside it.
- **Achievements hardening.** Schema, held-reply and pending-frame state in `Hooks_NetPacket_UserStats` relied on an unenforced "RecvPkt thread only" assumption; it is now guarded by `g_AchMutex` (never held together with `g_JobMutex` or across the original recv call).
- Cosmetic: `RemoteToml.h` comment now describes the real flow (cache first, remote only on miss).
- Cosmetic: `PatternLoader` / `IPCLoader` log `cache hit` for the normal cache-first path and `cache fallback` only when the network failed and the cache was plan B (new `RemoteToml::Result::cacheFallback`, label via `RemoteTomlLogic::DescribeLoadSource`).
- The IPC-spec-missing popup now links to the active mirror `madoiscool/steam-monitor` (was `OpenSteam001`).
- `TokeerBridge.cpp`: added `<system_error>` (clang-tidy `misc-include-cleaner`).

### Tests
- 37 new tests: `forge_base_cache_test.cpp` (14), `remote_toml_load_source_test.cpp` (5), `pattern_refresh_logic_test.cpp` (18).

### Decided
- **`[remote] revalidate = true` by default.** It is the fork's first automatic network behaviour beyond a cache miss, so the default was audited for privacy (v1.3.4 prep, session 34):
  - *Trigger:* only a cached TOML whose required function failed to resolve, the same condition that already shows the "Missing Signatures" popup; or a cached TOML that no longer parses. A healthy cache never makes a request.
  - *Data:* `GET <mirror>/pattern/<component>/<sha256>.toml` with no body, the same fixed `User-Agent` and `Accept-Encoding` as every AmethystTool request, and no cookies, account, path or machine data. The request is identical to the one a cache miss (first run, after a Steam update) already sends, so no new data leaves the machine.
  - *Limits:* at most one attempt per file per 24 h. The stamp mtime is UTC (`file_time_type`), so time-zone or DST changes don't affect it, and it survives restarts. If the stamp can't be written, the check is skipped. It runs on a background thread with 2 s / 3 s timeouts, fails silently and keeps the current cache.
  - *Opt-out:* `[remote] revalidate = false` restores the pre-v1.3.4 network behaviour exactly; the corrupt-cache re-download is gated too.
  - Defaulting to `false` would leave the people who need it (users stuck with a broken signature file) unaware of it, at no privacy gain, because the same request already happens on every Steam update.
- **Pattern refresh: self-healing revalidation, not a per-boot `HEAD` and not a URI.** A per-launch `HEAD`/`If-Modified-Since` would send the installed DLL hash to the mirrors on every Steam start (the exact thing cache-first avoids), add latency to every boot, and be unreliable anyway (jsDelivr caches for days; raw GitHub serves ETags, not dependable `Last-Modified`). A `amethysttool://refreshpatterns` action would be dormant in the default build like the rest of the scheme, so default users would gain nothing. Revalidating only when the cache is provably broken gives the fix automatically with zero cost in the normal case.
- `amethysttool://refreshachievements` stays **dormant** in the default build (the scheme is only registered when built with `-DOST_TOKEER_URL=...`): registering it would expose a web-invocable `rundll32` entry point with no default feature depending on it. Documented in `dllmain.cpp` and the READMEs.

### Not changed
- `[donate]`, `[cache]` and `[manifest] url` defaults; depot-aware → gid-only fallback.
- Upstream `madoiscool/BetterSteamTools` had no commits after 2026-10-30 (already ported in v1.3.3).

### Known / to run locally
- clang-tidy could not run on Linux (Windows/Steam headers and generated `ost_log_macros.h` missing).
