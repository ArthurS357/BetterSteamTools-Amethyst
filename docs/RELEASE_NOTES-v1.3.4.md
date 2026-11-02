# AmethystTool v1.3.4

Status: **prep** — pending local build and test validation.
Base: v1.3.3 (`e20ad7a`).

## Highlights

- **Fix: Denuvo error 54 after switching accounts.** The forge base ticket cache is now tied to the active Steam account, so changing account or using Family Sharing without restarting Steam no longer reuses the previous account's SteamID.
- **Fix: data races in `Hooks_NetPacket`.** RichPresence state shared between the send and recv hooks is now mutex-protected, and the achievements state got the same hardening.
- **Polish:** accurate `cache hit` / `cache fallback` log labels, corrected comment in `RemoteToml.h`, and the IPC-spec-missing popup now points to the maintained `madoiscool/steam-monitor` mirror.
- **Tests:** 15 new unit tests (expected total 228).

## Compatibility notes

- **`amethysttool://refreshachievements` remains dormant.** The URI scheme is registered only in builds configured with `-DOST_TOKEER_URL=...`; the default build does not register it (a registered protocol handler lets any web page launch the DLL via `rundll32`). Nothing changes for default builds.
- **Forge base cache invalidates on account change.** The active user is re-read on each forge request; when it differs from the cached base's account, the cache is dropped and the base is re-resolved from the new account's `localconfig.vdf` (or the existing fallback probes).
- **Data races fixed** (RichPresence send/recv; achievements schema/held/pending queues). No behaviour or config changes; only synchronization.
- No changes to `[donate]`, `[cache]` or `[manifest] url` defaults, and the depot-aware → gid-only fallback is untouched.

## Verification

Not yet built or tested by CI. Validate locally:

```powershell
.\build.ps1          # builds and tests Release + Debug
```

Expected: 228/228 tests per configuration. Then tag `v1.3.4`, and `release.ps1` generates the packaged artifacts (its auto-generated notes go to the git-ignored `release/` folder).

See `CHANGELOG.md` for the full list.
