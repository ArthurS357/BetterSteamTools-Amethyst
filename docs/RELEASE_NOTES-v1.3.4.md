# AmethystTool v1.3.4

Base: v1.3.3 (`e20ad7a`).

## Highlights

- **New: self-healing pattern cache.** If a cached signature file leaves required hooks unresolved, AmethystTool quietly re-checks the mirrors in the background (short timeouts, at most once a day per file) and, if a fixed file exists for your exact Steam build, swaps it in and asks you to restart Steam. No more deleting `<Steam>\amethysttool\pattern` by hand. A cache that parses to nothing is re-downloaded automatically. Healthy caches never cause a request. Opt out: `[remote] revalidate = false`.
- **Fix: Denuvo error 54 after switching accounts.** The forge base ticket cache is now tied to the active Steam account, so changing account or using Family Sharing without restarting Steam no longer reuses the previous account's SteamID.
- **Fix: data races in `Hooks_NetPacket`.** RichPresence state shared between the send and recv hooks is now mutex-protected, the achievements state got the same hardening, the game-name cache is locked, and Steam's AppInfo lookup no longer runs while our lock is held (lock-order risk).
- **Fix: forge fallback tickets from another account.** The appid 7 / 228980 / seen-ticket fallbacks now also reject tickets issued to a different account (another error-54 path).
- **Polish:** accurate `cache hit` / `cache fallback` log labels, corrected comment in `RemoteToml.h`, and the IPC-spec-missing popup now points to the maintained `madoiscool/steam-monitor` mirror.
- **Tests:** 43 new unit tests (total 256).

## Security hardening

- **Working patterns are never modified by a refresh (H-1).** A downloaded signature file may only add or fix the functions that are missing right now. Every entry that already works must be byte-identical in the download, or the download is refused. A fix only counts if its RVA lies inside the loaded Steam module. A download that carries a new or changed bad entry for any missing function is refused as a whole (fail-closed).
- **https-only refresh (M-4).** The background refresh never uses a plain `http://` mirror, even if one is configured in `[remote] url_template`. Cache-miss downloads are unchanged.
- **The refresh worker can't crash Steam (M-1).** Every exception inside the background worker, or while starting it, is caught; the current cache stays in use.
- Cache file replacement is flushed to disk (`MOVEFILE_WRITE_THROUGH`), and orphaned temp files from an interrupted write are cleaned up on the next start.

## Compatibility notes

- **`amethysttool://refreshachievements` remains dormant.** The URI scheme is registered only in builds configured with `-DOST_TOKEER_URL=...`; the default build does not register it (a registered protocol handler lets any web page launch the DLL via `rundll32`). Nothing changes for default builds.
- **Forge base cache invalidates on account change.** The active user is re-read on each forge request; when it differs from the cached base's account, the cache is dropped and the base is re-resolved from the new account's `localconfig.vdf` (or the existing fallback probes).
- **`[remote] revalidate = true` by default.** Only triggers when the pattern cache is broken (missing required signature). No new data leaves the machine beyond the DLL SHA-256, which is already sent when the cache is empty. Can be disabled with `[remote] revalidate = false`.
  - At most one mirror check per file per 24 h. A cached file that no longer parses is re-downloaded like a cache miss. Both behaviours are controlled by this key, so `false` restores the pre-v1.3.4 network behaviour exactly.
- **Data races fixed** (RichPresence send/recv; achievements schema/held/pending queues). No behaviour or config changes; only synchronization.
- No changes to `[donate]`, `[cache]` or `[manifest] url` defaults, and the depot-aware → gid-only fallback is untouched.

## Verification

Validated locally with `build.ps1 -Clean` (no CI):

- Release and Debug build; 256/256 tests in each.
- Warnings: Release 9 / Debug 8 (baseline).
- ABI: `TokeerUri` at ordinal 1 (single export). CRT: static `/MT` (no vcruntime140/msvcp140/ucrtbase).
- clang-tidy: no new defects on lines changed in v1.3.4.

See `CHANGELOG.md` for the full list.
