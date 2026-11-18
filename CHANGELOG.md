# Changelog

## [v1.5.0] - Unreleased

Independence and stability. Scope and rationale: [docs/ROADMAP-v1.5.0.md](docs/ROADMAP-v1.5.0.md).

### Added
- **Embedded IPC spec.** The 6 IPC methods in use (`IClientUser` x4, `IClientUtils` x2) are compiled in (`Utils/SteamMetadata/IPCEmbedded.h`). `IPCLoader::Load` now tries the cache by SHA, then the embedded table, then the mirrors. The table is used only if all 12 `funcHash`/`fencepost` constants are found in the loaded `steamclient64.dll` `.text`; otherwise it falls through to the mirrors as before. A Steam update no longer triggers an IPC request when the table verifies.
- **Manifest provider failover (`[manifest] failover`, on by default).** If the configured provider fails, the request code is fetched from the next one (`wudrm` then `steamrun`) within a ~10 s total budget (best effort; Steam's 12 s wait is the hard stop). A provider that gives no answer or answers 5xx is skipped for 5 minutes; a 404, a 429 or an unreadable answer only moves on to the next provider. A code of 0 is not an answer. A `url_template` override is never swapped for a built-in provider. `failover = false` restores the single-provider behaviour.

- **`version_pin`, an offline tool that pins the installed version of a game** (`tools/version_pin`, logic in `Utils/SteamMetadata/VersionPinLogic.h`). It reads `steamapps\appmanifest_<appid>.acf`, takes the `InstalledDepots` manifests and writes `<appid>.pin.lua` with one `setManifestid` per depot into `config\stplug-in`. No hook, no network, not part of the DLL. `--appid <id>` or `--all` (one of the two is required, so running it with no arguments pins nothing), `--steam-dir` (default: registry), `--steamapps` for another library, `--out-dir`, `--dry-run`, `--force`. It never overwrites a file it did not write without `--force`, writes through a temp file and a rename, and warns when another `.lua` pins the same depot to a different manifest (the file parsed last wins). Only digits parsed as numbers and a filtered game name reach the `.lua`, because `.lua` files run as trusted code inside Steam.

### Changed
- `README.md`: `setManifestid`'s third argument was documented as an explicit size; the engine ignores it and always uses 0 (`LuaConfig.cpp`, `lua_setManifestid`).
- `ManifestClient` no longer holds its mutex while it waits on the network or on a Lua `fetch_manifest_code*` call, so concurrent depot requests no longer queue behind each other.
- `RemoteToml::Fetch` is split into `LoadCached` (hash plus cache read, no network) and `FetchOnMiss` (mirror walk). Behaviour is unchanged.
- `IPCLoader::Load` takes the loaded steamclient module as a second argument (for the `.text` check). New `OSTPlatform::Memory::GetModuleSection` reads a PE section range from a loaded module.
- A cached IPC TOML that does not parse, or lists no interface, now falls through to the embedded table and the mirrors instead of ending in the "IPC spec missing" popup.
- **Pattern/IPC mirror chain leads with jsDelivr** (from upstream v1.0.9): jsDelivr madoiscool, then git.lua.tools, then GitHub raw madoiscool, then jsDelivr OpenSteam001, then GitHub raw OpenSteam001. Where `raw.githubusercontent.com` is unreachable it fails only after the connect timeout, and a cache miss runs before the hooks install. Upstream traced "invalid content configuration" to that window (~24 s there, because its loader was network-first). AmethystTool has been cache-first since v1.3.x, so only a cache miss (first launch after a Steam update) was exposed.
- The cache-miss walk (`RemoteToml::FetchOnMiss`, loop now `RemoteTomlLogic::WalkMissMirrors`) uses 2.5 s resolve/connect and 5 s send/recv instead of the 5/5/10/10 s defaults. The corrupt-cache refetch uses the same budget. Not measured on a host with several IPs, where WinHTTP may retry the connect per address.
- The background pattern revalidation skips a mirror that answers with the body already in the cache and tries the next one, so a jsDelivr copy that lags the repo cannot end the check (and start the 24 h throttle) before GitHub raw serves the fix.
- Not taken from upstream v1.0.9: its detached refresh on every launch. It would send the DLL hash on every start and overwrite a working cache without the H-1/H-1b checks.

### Privacy
- With failover on, a manifest `gid` (and depot id for depot-aware providers) is sent to the next provider when the first one fails. Nothing extra is sent while the first provider answers. Set `failover = false` to opt out.
- The embedded IPC table removes the IPC spec request (which carries the `steamclient64.dll` hash) on a Steam update, whenever the table verifies.
- On a cache miss the DLL hash now goes to jsDelivr first instead of GitHub. Same data, different first recipient. No request is added to a launch with a healthy cache.

### Tests
- 519 to 525: `remote_toml_logic_test` checks that jsDelivr precedes GitHub raw for each repo and drives `WalkMissMirrors` through a fake network (unreachable GitHub raw, a repo that lacks the hash, everything down, empty chain), asserting every request gets the 2.5/2.5/5/5 s budget and what a dead host costs. Also in the Linux logic-test job, which was not run locally. The skip-identical-body path in `FetchRemoteOnly` has no unit test (WinHTTP).
- 384 to 519. `version_pin_logic_test` (the VDF parser including every truncation of a real manifest, depot extraction, Lua output with hostile game names, conflict scan, and the write path on disk) and one test in `luaconfig_concurrency_test` that feeds the tool's output to the real `LuaConfig`. `version_pin_logic_test` is also in the Linux logic-test job (ASan + UBSan), which could not be run locally. Earlier in v1.5.0: `ipc_embedded_test` (verification on synthetic buffers; the table must match the registered handlers and the IDL), `module_section_test` (PE section lookup against the test binary and kernel32, plus forged-header cases on a synthetic image next to an unreadable page), `manifest_failover_logic_test` (chain, budget, timeout capping, retry rule, circuit breaker and the whole walk with a fake clock and fake providers). Windows suite only: not yet added to the Linux logic-test job, which could not be run locally.

### Field test (2026-11-18, one Steam install, Debug build)
- **Failover, Atomic Heart (668580):** the download started (`Downloading 162791 chunks for depot 668581`) with no `Access Denied`, where v1.3.x failed 13 times on 2026-10-27. Which provider answered is not known: `manifest.log` is rewritten at every Steam start and was overwritten by the next session.
- **Undertale (391540), not solved:** `Failed building depot dependency (Invalid content configuration)`, produced by the `BuildDepotDependency` empty-list guard (14 rejections in 5 s, over 2 sessions).
  - Ruled out: the `.lua` (3 depots with keys and `setManifestid`), the cached appinfo (has the 3 depots and the same gids, and was not being refreshed: `apps updated 0`), hook timing (hooks were in place 5 s before), and a real license owning the app without depots.
  - The same app installed with 1 mounted depot on 2026-09-25, 10-08, 10-09 and 10-15. Since then both the DLL (pre-v1.3.3 to v1.5.0; no hook change after v1.4.0) and the Steam client (build 1794536281) changed.
  - The guard's "appinfo mid-refresh" wording does not fit here: the list stays empty. Not changed: without the guard Steam records a "0 mounted depots" install, as on 2026-09-13/14.
  - Next: a `log.level = "trace"` run (pSteamApp/pBuildId/nCount per call), and the same attempt with the v1.4.0 DLL.

### Planned for this release (not implemented yet)
- **Shadow fingerprint recorder** and the offline `pattern_migrate` tool. Runtime migration is deferred to v1.5.1.

## [v1.4.0] - 2026-11-06

### Added
- **Steam Workshop for Lua-managed apps.** License/access errors on a fixed list of Workshop and RemoteStorage callbacks are rewritten to OK, only for apps added through Lua and only for the game process that made the call (`Hooks_IPC_ISteamUGC.cpp`, decision logic in `UgcLogic`). Log-only `IClientUGC` diagnostics in Debug builds.
- **LuaConfig concurrency tests** against the real `LuaConfig.cpp` and real Lua, with HTTP faked on a latch so the interleavings are deterministic. A new `Linux logic tests` job runs them under ThreadSanitizer and ASan+UBSan with Lua instrumented (`src/tests/linux/luaconfig`), since MSVC has no TSan.

### Fixed
- **LuaConfig data races.** All config state is guarded by one `std::shared_mutex`; accessors return copies. The Lua interpreter is guarded by `g_luaMutex`, with a per-call coroutine so the lock can be released around `http_get`/`http_post`.
- **Reload gap.** A reload no longer unloads a file before parsing it: mutations are staged and committed under one exclusive lock, so readers see the old file or the new one. A file that cannot be opened, is empty or ends mid-statement keeps the loaded version.
- **Injected license list.** This DLL's accesses to `AppIdVec` are serialized, the initial fill runs once, and `NotifyLicenseChanged` reconciles against the current config instead of replaying its queues, which could leave duplicate or stale ids.
- **3 pre-existing crashes:** Lua strict-mode `__index` panic (lookups outside `lua_pcall` are now raw), null pointer on a non-string `http_get`/`http_post` header entry (the entry is skipped), use-after-free when a second license list arrived mid-walk (handed over under a mutex and walked as a private copy).

### Changed
- An emptied `.lua` file no longer unloads its games while Steam runs; delete the file instead, or restart Steam.
- `fetch_manifest_code` and `fetch_manifest_code_ex` must be raw fields of `_G`; defining them behind a custom metatable is no longer seen. Not a security boundary: `.lua` files remain fully trusted code.

### Known limitations
- Steam's own threads read the injected `AppIdVec` without any lock this tool can take, so changing it during a reload can still race with them.

## [v1.3.6] - 2026-11-04

Hotfix. Built from the v1.3.5 tip (the `main` branch predates v1.3.4 and carries neither this changelog nor the Linux CI).

### Removed
- **Family Share bypass** (Steam validates Family Sharing server-side; suppressing its enforcement messages on the client causes logouts). `Hooks_NetPacket.cpp` no longer drops either of the two inbound messages the bypass cleared by replacing their body with an empty one (`Hooks_NetPacket_FamilySharing::ClearBody`, now deleted):
  - `FamilyGroupsClient.NotifyRunningApps#1` (eMsg 147 service job, `HASH_JOB_NotifyRunningApps`),
  - `CMsgClientSharedLibraryStopPlaying` (eMsg 9406).
  Both now reach Steam untouched, so a borrowed library is locked while its owner plays, as Steam intends. There was no TOML key for this, so there is no config default to change and no config migration.

### Kept on purpose
- `AppOwnership::bFamilyShared` (`Steam/Structs.h`) and the `CFamilyGroupsClient_NotifyRunningApps_Notification` / `CMsgClientSharedLibraryStopPlaying` definitions in `proto/steam_messages.proto` are read-only layout/parse definitions and stay, so the struct ABI is unchanged. Nothing ever wrote `bFamilyShared`.
- `CheckAppOwnership` (`Hook/Hooks_Package.cpp`) still sets `bOwnsLicense = true` for Lua-added apps. That is local ownership state (it is what makes DLC of a Lua-added game show as owned) and never leaves the client, so it is not part of this change.

### Changed
- `README.md`, `README_ES.md`, `README_ZH.md`: the "Family Sharing" bullet no longer advertises the bypass.

### Tests
- No test changes. The removal is structural (two `case` labels, one helper and one hash constant deleted from the Windows-only `Hooks_NetPacket.cpp`), so there is no remaining decision to unit-test; the Linux pure-logic suite is untouched (19 files / 259 tests). Needs an MSVC build (`build.ps1`) before release.

## [v1.3.5] - 2026-11-03

Validated locally: 349/349 tests in Release and Debug, warnings 9/8, ABI TokeerUri ordinal 1, CRT /MT.

### Added
- **Denuvo compatibility mode (`[compatibility] denuvo_aliases`, off by default).** The official Denuvo activation flow (TokeerDRM + LuaTools Validator) only recognises the upstream engine by its file names next to `steam.exe`, so fork users got *"Set up your activation engine first"*. When enabled, AmethystTool drops `OpenSteamTool.dll` (byte copy of `AmethystTool.dll`) and `opensteamtool.toml` (copy of `amethysttool.toml` behind a 3-line marker header) at Steam startup. Nothing in AmethystTool reads the aliases; no network or registry access is involved. Policy and file I/O in `Utils/Config/CompatAliasLogic.{h,cpp}`, called from `dllmain.cpp` (`ApplyCompatibilityAliases`) right after `Config::Load`.
  - Never overwrites a file it did not create: an existing `OpenSteamTool.dll` is "ours" only if it contains the `amethysttool.toml` marker string (every fork build has it, upstream never does); an existing `opensteamtool.toml` only if it starts with the marker header. Anything else is left untouched and logged as a warning.
  - Aliases it did create are refreshed when they differ (e.g. after an update), including a hand-renamed older `AmethystTool.dll`.
  - Writes go to a temp file and are renamed into place; every error is logged and init continues (fail-safe). Each call writes its **own** temp (`<alias>.<pid>-<n>-<tick>.amethyst-tmp`) -- see *Fixed* below.
  - `denuvo_aliases = false` stops maintaining the aliases but does not delete them. Read at startup only (not hot-reloaded).
- **`tools/enable-denuvo-compat.ps1`** (shipped in the release ZIP as `enable-denuvo-compat.ps1`): one-shot fallback that creates the same aliases without editing the TOML (Steam path from `HKCU:\Software\Valve\Steam\SteamPath`, overridable with `-SteamPath`), same safety rules and byte-identical marker header as the DLL. `-Remove` deletes only aliases AmethystTool created.
- **Linux CI for the pure-logic tests** (`.github/workflows/linux-logic-tests.yml`, project in `src/tests/linux/`). Every push (any branch) and pull request builds and runs the dependency-light unit tests on `ubuntu-latest` under AddressSanitizer + UndefinedBehaviorSanitizer, with no MSVC: 19 test files / 259 tests today. It complements, and does not replace, the Windows `CI` workflow (real DLL + full test executable). `encoding`, `hash`, `manifest_cache_logic` (need `<windows.h>`), `net_packet_logic`/`license_list_logic` (protobuf + Windows `MemoryQuery`), `donate_config_logic` (toml++) and `numbers` stay Windows-only; `compat_alias_logic_test` uses a Linux stand-in for `Encoding.h` (paths are already UTF-8 there). A status badge was added to `README.md`.

### Changed
- Denuvo Compatibility Mode: field-validated against the official LuaTools Validator (2026-11-03); DLL-generated aliases are recognised as `OpenSteamTool (official)`.
- **One active AmethystTool image per Steam process.** `DllMain` now claims a per-process named mutex (`Local\AmethystTool.Active.<pid>`) before starting the init thread; a second image of the same binary (e.g. the `OpenSteamTool.dll` alias loaded by a leftover upstream proxy) stays inert instead of installing every detour twice. `DLL_PROCESS_DETACH` cleanup runs only in the owning image. Fail-open: if the mutex cannot be created, behaviour is exactly as before.
- **Decision logic split out of `Hooks_NetPacket.cpp`** (no behaviour change; the hooks keep all I/O, locking and `invokeOriginal`):
  - `Hook/AchievementsLogic.{h,cpp}` -- `RequestedAccount`, `ShouldSkipReconstruction` (a friend's page, or nobody logged in, keeps Valve's reply), `ShouldRebuildReply`, `ShouldHoldReply` (reply beat its schema and the queue has room), `ShouldReleaseHeld` (8 s timeout), `ShouldKeepCachedSchema` (a hash_only reply must not wipe a cached schema); owns `kHoldTimeout` / `kMaxHeldUserAch`.
  - `Hook/RichPresenceLogic.{h,cpp}` -- `AppIdFromGameId`, `ShouldTrackAppId`, `DecideTrackAction` (inject / clear / defer-to-server / unchanged -- the "no flicker to Online" rule), `ShouldCaptureRpUpload`, `ShouldInjectRichPresence`, `WithRichPresenceFlag`, and the binary-KV1 parser `ExtractStringKVs` (moved verbatim).
  - Both are registered in `src/CMakeLists.txt` and `src/tests/CMakeLists.txt`. The "ExtractionParity" suites restate the old inline expressions and compare them with the extracted functions over the whole input domain.
  - `ShouldHoldReply` / `ShouldInjectRichPresence` take the parameters the code actually decides on (schema cached + queue size; tracked appid), not the `appId / account / isLuaGame` triple first sketched: the original conditions never consult those, and adding the lua check to the live-push patch path would have changed behaviour.

### Fixed
- **Concurrent `CompatAliasLogic::Apply` calls shared one temp file name.** Two `steam.exe` processes started close together (the main one plus a short-lived `steam://` forwarder or relaunch -- both load the proxies from the Steam folder; the per-process mutex does not span processes) could rename each other's half-written temp into place or find their own already moved. Reproduced with 8 concurrent writers x 60 rounds: 49 failed writes and 7 observations of a partial alias; after the fix, 0 and 0. Each call now writes its own `<alias>.<pid>-<n>-<tick>.amethyst-tmp`, the alias is written from the bytes already inspected (not a second `copy_file`, whose Windows `CopyFileW` also preserves the source's mtime), and orphans left by a process killed mid-write are swept once older than 10 minutes (exact name match, regular files only). A crash or kill can never leave a partial *alias*: only the rename publishes it. The PowerShell fallback is unchanged (one-shot, user-run).
- **H-1b: a pattern TOML could add an out-of-image RVA outside the missing set.** `EntriesAreConsistent` compared only entries the cache already had, and the revalidation only probes the missing *required* functions, so a mirror copy could *add* e.g. an optional function with a wild RVA that the next launch would hook unvalidated. `EntriesAreConsistent` / the map-based `ShouldReplaceCache` now take the module image size and refuse any new entry (outside the missing set) whose RVA is not inside it; signature-only entries are fine, an unknown image size fails closed. 9 tests, 3 of which fail without the check.
- `RemoteToml::FetchRemoteOnly` is now `[[nodiscard]]`.

### Tests
- `compat_alias_logic_test.cpp`: 31 tests (decision matrix, content predicates, file behaviour -- create, idempotent second run, real upstream install untouched, stale own copy refreshed, missing source DLL does not block the TOML alias, directory under the alias name, read-only decide helpers, non-ASCII Steam path through `Utf8ToPath` -- plus temp-name uniqueness/sanitising, deterministic write- and rename-failure injection, 8-writer concurrency, and the stale-temp sweep).
- `achievements_logic_test.cpp` (22) and `rich_presence_logic_test.cpp` (31): happy path / hostile input / false-positive guards for every extracted decision, the KV1 parser (truncation at every length against an exact-size buffer, deep nesting, UTF-8), and the parity suites.
- `pattern_refresh_logic_test.cpp`: +9 for H-1b (existing calls updated for the new `imageSize` argument).

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
