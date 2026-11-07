# AmethystTool v1.4.0

> **DRAFT.** Release candidate `v1.4.0-rc1` is for batch testing only. The `v1.4.0` tag is not created until the tests below are validated.

Base: v1.3.6 (`7ecd53b`). Feature release.

## Highlights

- **Steam Workshop works for Lua-managed apps** (new).
- **Fix for random Steam crashes while Lua files change or Steam is busy** (concurrency fix).
- Carried over, unchanged: Denuvo compatibility mode (v1.3.5) and the Family Sharing bypass removal (v1.3.6).

## Workshop / UGC support

A game you add with `addappid` has no real license on your account. When such a game used the Workshop (subscribe, unsubscribe, item queries), Steam finished the call with a "no access / no license" error and the Workshop did nothing.

- AmethystTool now rewrites that error to "OK" for Lua-managed apps, so Workshop calls complete normally. This works in **Release builds too**.
- It is deliberately narrow (fail-closed):
  - only for apps you added through Lua, and only for the game process that made the call (a licensed game, or the Steam UI, is never touched);
  - only for a fixed list of Workshop / RemoteStorage callbacks, and only for the access/license-type errors (network errors, timeouts and "not found" are left alone);
  - nothing is written unless the call really completed.
- Games started with `-onlinefix` use a fake AppID (480) on their own pipe, so their Workshop errors are **not** rewritten.
- Debug builds additionally log the Workshop calls (`ipc.log`) to help diagnose problems; Release builds stay quiet.
- 20 new unit tests, also part of the Linux ASan/UBSan CI suite.

## Concurrency fix (LuaConfig)

Steam's IPC, network and pipe threads read the Lua-derived tables (which depots/apps are Lua-managed, decryption keys, owned apps, manifest overrides, tokens...) on every message. The Lua watcher thread changes those same tables whenever you edit a `.lua` file (or change the Lua paths in `amethysttool.toml`), and one hook also changes them while a game is being checked. These reads and writes were not synchronized, so a read that overlapped a change could crash Steam.

- All of that state is now guarded by one reader/writer lock. Readers (the hot path) do not block each other, and the lock is never held while a Lua script runs or while the tool makes a network request, so a slow `http_get` in a script cannot stall Steam.
- A few internal accessors used to hand out references into the live tables; they now return copies.
- The Lua interpreter is now locked too, so a `.lua` reload can no longer corrupt a manifest-code callback running at the same time. The lock is released during `http_get`/`http_post`, so a slow request in one script does not hold up the others.
- **Reloading a `.lua` file no longer makes its games disappear.** The new version is built on the side and swapped in all at once, so Steam sees the old file or the new one, never a gap, even if a script spends seconds in `http_get`.
- **A file that was half-saved keeps its previous version.** If the file cannot be opened, is empty, or ends in the middle of a statement (which also covers a syntax error), the version already loaded stays active and a warning is logged. A single failing statement, such as a call to an unknown function, is still just skipped, as before. `setAppTicket`/`setETicket` take effect while the script runs and are not undone in this case.
- **Behaviour change:** emptying a `.lua` file no longer removes its games while Steam runs. To remove them, delete the file; an emptied file also takes effect on the next Steam start.
- The tool's own changes to the injected license list are now serialized and worked out from the current configuration. Two Steam threads can no longer both inject the initial list, and a `.lua` reload can no longer leave duplicate or stale entries in it. Reloading a file without changing its games no longer touches the list at all, though the library refresh still runs.
- No configuration change is required: this release does not need any edits to `amethysttool.toml` or your `.lua` files.
- Not fully solvable from here: Steam's own threads read the injected license list without any lock this tool can take, so changing it can still race with them. The window is short and only opens when a reload really adds or removes a game; the fix would be to apply the change on Steam's own thread.

## Carried over from earlier releases

- **Denuvo compatibility mode** (v1.3.5, off by default): `[compatibility] denuvo_aliases = true` (or `enable-denuvo-compat.ps1`) lets the official LuaTools Validator recognise AmethystTool. Unchanged in this release.
- **Family Sharing** (v1.3.6): the bypass is gone, so Steam enforces Family Sharing as intended and you are not logged out. Unchanged in this release.

## Upgrading

1. Close Steam completely (tray icon -> Exit).
2. Copy the three DLLs from the ZIP (`AmethystTool.dll`, `dwmapi.dll`, `xinput1_4.dll`) over the old ones next to `steam.exe`. Self-update is disabled in this fork, so this is a manual step. Keep your `amethysttool.toml`.
3. If you enabled `[compatibility] denuvo_aliases`, the `OpenSteamTool.dll` alias is refreshed automatically the next time Steam starts.
4. Start Steam.

## Verification

Validated locally on Windows (2026-11-06):
- `.\build.ps1 -Clean`: 382/382 tests in Release and Debug.
- Warnings: Release 9 / Debug 8 (baseline).
- ABI: `TokeerUri` ordinal 1. CRT: `/MT` static.

The new concurrency tests run the real `LuaConfig.cpp` and the real Lua interpreter. Against the previous `LuaConfig.cpp`, 6 of those 8 tests fail and the stress test crashes; the other 2 pin behaviour that was deliberately kept.

There is no ThreadSanitizer on MSVC, so the race detector only runs on Linux CI (`Linux logic tests`, TSan and ASan+UBSan). The injected license list cannot be exercised without Steam, so that part rests on code review and on unit tests of its decision logic. Real-world testing of the three areas above is what this release candidate is for.
