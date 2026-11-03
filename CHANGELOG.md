# Changelog

## [v1.3.5] - Unreleased

> **Pending local validation:** written in a Linux session without a Windows build. Expected: 276/276 tests per configuration (256 + 20 new). Run `.\build.ps1 -Clean` before tagging.

### Added
- **Denuvo compatibility mode (`[compatibility] denuvo_aliases`, off by default).** The official Denuvo activation flow (TokeerDRM + LuaTools Validator) only recognises the upstream engine by its file names next to `steam.exe`, so fork users got *"Set up your activation engine first"*. When enabled, AmethystTool drops `OpenSteamTool.dll` (byte copy of `AmethystTool.dll`) and `opensteamtool.toml` (copy of `amethysttool.toml` behind a 3-line marker header) at Steam startup. Nothing in AmethystTool reads the aliases; no network or registry access is involved. Policy and file I/O in `Utils/Config/CompatAliasLogic.{h,cpp}`, called from `dllmain.cpp` (`ApplyCompatibilityAliases`) right after `Config::Load`.
  - Never overwrites a file it did not create: an existing `OpenSteamTool.dll` is "ours" only if it contains the `amethysttool.toml` marker string (every fork build has it, upstream never does); an existing `opensteamtool.toml` only if it starts with the marker header. Anything else is left untouched and logged as a warning.
  - Aliases it did create are refreshed when they differ (e.g. after an update), including a hand-renamed older `AmethystTool.dll`.
  - Writes go to `<alias>.amethyst-tmp` and are renamed into place; every error is logged and init continues (fail-safe).
  - `denuvo_aliases = false` stops maintaining the aliases but does not delete them. Read at startup only (not hot-reloaded).
- **`tools/enable-denuvo-compat.ps1`** (shipped in the release ZIP as `enable-denuvo-compat.ps1`): one-shot fallback that creates the same aliases without editing the TOML (Steam path from `HKCU:\Software\Valve\Steam\SteamPath`, overridable with `-SteamPath`), same safety rules and byte-identical marker header as the DLL. `-Remove` deletes only aliases AmethystTool created.

### Changed
- **One active AmethystTool image per Steam process.** `DllMain` now claims a per-process named mutex (`Local\AmethystTool.Active.<pid>`) before starting the init thread; a second image of the same binary (e.g. the `OpenSteamTool.dll` alias loaded by a leftover upstream proxy) stays inert instead of installing every detour twice. `DLL_PROCESS_DETACH` cleanup runs only in the owning image. Fail-open: if the mutex cannot be created, behaviour is exactly as before.

### Tests
- 20 new tests in `compat_alias_logic_test.cpp`: decision matrix (6), content predicates (4), file behaviour (10) -- create, idempotent second run, real upstream install untouched, stale own copy refreshed, missing source DLL does not block the TOML alias, directory under the alias name, write failure reported without throwing and without a partial alias, read-only decide helpers, non-ASCII Steam path through `Utf8ToPath`.

## [v1.3.4] - 2026-11-02

> **Validated locally:** `build.ps1 -Clean` Release + Debug, 256/256 tests per configuration (213 + 43 new), warnings Release 9 / Debug 8 (baseline), `TokeerUri` @ ordinal 1, static `/MT` CRT.

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

### Security hardening (pre-merge audit, session 36)
- **H-1 — working patterns can no longer be rewritten.** Background revalidation only checked that the candidate TOML kept every function *key*, so a compromised mirror could move the RVA/sig of hooks that work today, as long as it also added a missing function. The candidate must now keep every non-missing entry byte-identical (name + rva + sig) (`PatternRefreshLogic::EntriesAreConsistent`). A candidate RVA counts as resolving a missing function only if it lies inside the loaded module image, and a candidate that carries a *new or changed* entry it cannot resolve for any missing function is refused outright (fail-closed). An entry identical to today's cached one doesn't block a partial fix, because it already installs no hook.
- **M-1 — no `std::terminate` from the revalidation worker.** The worker body and the `std::thread` creation are wrapped in `try`/`catch (...)`, so an exception there can no longer take Steam down.
- **M-4 — revalidation is https-only.** The background revalidation skips any non-`https://` mirror URL (`RemoteTomlLogic::IsHttpsUrl`). The cache-miss path and `[remote] url_template` itself are unchanged.
- L-1: `[[nodiscard]]` on `ReplaceCacheFile`, `TouchStamp`, `AccountKeyedCache::Store`/`HasValue`/`AccountId`.
- L-2: `MoveFileExW` now also uses `MOVEFILE_WRITE_THROUGH`; orphan `<sha256>.toml.<tid>.tmp` files are removed when a pattern module loads.
- L-3: `WIN32_LEAN_AND_MEAN` + `NOMINMAX` before `<windows.h>` in `RemoteToml.cpp`.
- L-4: the corrupt-cache refetch timeouts are named constants (`kCorruptCacheRefetch*TimeoutMs`).
- L-5: `static_assert`s on `AccountKeyedCache<T>` requirements; `ShouldReplaceCache` parameters renamed `existingKeys`/`candidateKeys`.
- L-6: the lock hierarchy is documented once, next to `g_RPMutex` in `Hooks_NetPacket.cpp`.

### Tests
- 43 new tests: `forge_base_cache_test.cpp` (14), `remote_toml_load_source_test.cpp` (6), `pattern_refresh_logic_test.cpp` (23).
- Not covered yet (planned for v1.3.5): I/O integration of `Revalidate` / `ReplaceCacheFile` / `TouchStamp`, and non-ASCII Steam paths in `CachePathFor` (audit M-3, partial).

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

### Known limitations
- **The revalidation worker is a detached thread (M-2).** In the worst case it runs about 45 s per pattern module (5 mirrors × 2+2+2+3 s timeouts), plus however long the user leaves the "Signatures Updated" popup open.
  - It does **not** delay Steam's shutdown: at process exit Windows terminates the thread, and the atomic rename means no cache file is ever half-written.
  - The real hazard is unloading `AmethystTool.dll` mid-session (`FreeLibrary`, not a supported path) while the thread runs, which would crash.
  - `SteamDiagnostics` popups already use the same pattern.

### Static analysis
- clang-tidy (Windows, `-misc-include-cleaner`) over the 12 changed `.cpp`: no new defects on v1.3.4 lines; remaining hits are the existing style categories (`misc-const-correctness`, magic numbers, internal linkage). `Hooks_NetPacket.cpp` is analysed only partially: protobuf headers don't parse under the clang frontend (pre-existing).
