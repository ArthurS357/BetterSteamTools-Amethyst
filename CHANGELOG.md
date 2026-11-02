# Changelog

## v1.3.4 (draft — pending local build/ctest)

### Fixed
- `RemoteToml.h`: comment now states the real flow (cache first, remote only on miss).
- `PatternLoader` / `IPCLoader`: load log distinguishes `cache hit` (normal) from `cache fallback` (network failed, cache used as plan B). Added `RemoteToml::Result::cacheFallback` to carry this.
- `IPCLoader`: missing-spec popup now links to the active mirror `madoiscool/steam-monitor` (was `OpenSteam001`).

- `AppTicket`: forge base cache is now keyed by the active account (`ForgeBaseCache.h`); switching account / Family Sharing without restarting Steam re-resolves the base instead of reusing the old SteamID (error 54).
- `Hooks_NetPacket`: fixed send/recv data races -- RichPresence state (playing appid, self-PersonaState template, inject packet, RP KVs) now under `g_RPMutex`; achievements schema/held/pending state under `g_AchMutex`.
- `TokeerBridge.cpp`: added `<system_error>` (clang-tidy).
- Tests: `forge_base_cache_test.cpp`, `remote_toml_load_source_test.cpp` (`DescribeLoadSource`).

### Decided
- `amethysttool://refreshachievements` stays dormant in the default build (scheme only registered when `OST_TOKEER_URL` is set): registering it would expose a web-invocable `rundll32` entry point with no default feature depending on it. Documented in `dllmain.cpp` and READMEs.

### Not changed
- `[donate]`, `[cache]`, `[manifest] url` defaults; depot-aware → gid-only fallback.

### Notes
- clang-tidy could not run on Linux (Windows/Steam headers and generated `ost_log_macros.h` missing); run locally.
