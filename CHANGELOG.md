# Changelog

## v1.3.4 (draft — pending local build/ctest)

### Fixed
- `RemoteToml.h`: comment now states the real flow (cache first, remote only on miss).
- `PatternLoader` / `IPCLoader`: load log distinguishes `cache hit` (normal) from `cache fallback` (network failed, cache used as plan B). Added `RemoteToml::Result::cacheFallback` to carry this.
- `IPCLoader`: missing-spec popup now links to the active mirror `madoiscool/steam-monitor` (was `OpenSteam001`).

### Decided
- `amethysttool://refreshachievements` stays dormant in the default build (scheme only registered when `OST_TOKEER_URL` is set): registering it would expose a web-invocable `rundll32` entry point with no default feature depending on it. Documented in `dllmain.cpp` and READMEs.

### Not changed
- `[donate]`, `[cache]`, `[manifest] url` defaults; depot-aware → gid-only fallback.

### Notes
- clang-tidy could not run on Linux (Windows/Steam headers and generated `ost_log_macros.h` missing); run locally.
