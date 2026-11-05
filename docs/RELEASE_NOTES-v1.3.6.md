# AmethystTool v1.3.6

Base: v1.3.5 (`d88c96e`). Hotfix release.

> **Please update as soon as v1.3.6 is published.** If you use Steam Family Sharing, older versions can get your Steam account logged out.

## What was wrong

Earlier versions included a Family Sharing "bypass". Steam sends the client two messages that enforce Family Sharing: one that reports which family members are currently playing (`FamilyGroupsClient.NotifyRunningApps`) and one that tells the client to stop a game from a shared library (`SharedLibraryStopPlaying`). AmethystTool silently threw both away.

Steam now validates Family Sharing on its own servers. When the client ignores what the server asked for, the server can end the session, and users were being logged out of Steam.

## What changed

- **The Family Sharing bypass is removed.** AmethystTool no longer touches those two messages; they reach Steam exactly as the server sent them.
- Family Sharing now behaves as Steam intends. For example, if the owner of a shared library starts playing, Steam will ask you to stop playing from that library, as it does for everyone else.
- **Nothing else changes.** Games you add with `addappid` in Lua, DLC unlocking, manifests, tickets, achievements and the Denuvo compatibility mode work as before.
- There is no setting to turn the old behaviour back on, and `amethysttool.toml` needs no changes.

## Upgrading

1. Close Steam completely (tray icon -> Exit).
2. Copy the three DLLs from the v1.3.6 ZIP (`AmethystTool.dll`, `dwmapi.dll`, `xinput1_4.dll`) over the old ones next to `steam.exe`. Self-update is disabled in this fork, so this is a manual step. Keep your `amethysttool.toml`.
3. If you enabled `[compatibility] denuvo_aliases`, the `OpenSteamTool.dll` alias is refreshed automatically the next time Steam starts.
4. Start Steam.

If you share your library with other family members, each of them should update too.

## Verification

Validated locally on Windows (2026-11-04):
- `.\build.ps1 -Clean`: 349/349 tests in Release and Debug.
- Warnings: Release 9 / Debug 8 (baseline).
- ABI: `TokeerUri` ordinal 1. CRT: `/MT` static.

Linux CI (`linux-logic-tests.yml`): 259 pure-logic tests passing under ASan + UBSan (unchanged by this release).

See `CHANGELOG.md` for the full list.
