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
- No behaviour or configuration change is intended: this release does not require any edits to `amethysttool.toml` or your `.lua` files.
- Not covered by this fix (tracked separately): the Lua interpreter itself is still shared between the file loader and the manifest-code callbacks without a lock, and the injected license list can be extended while Steam is still initializing it.

## Carried over from earlier releases

- **Denuvo compatibility mode** (v1.3.5, off by default): `[compatibility] denuvo_aliases = true` (or `enable-denuvo-compat.ps1`) lets the official LuaTools Validator recognise AmethystTool. Unchanged in this release.
- **Family Sharing** (v1.3.6): the bypass is gone, so Steam enforces Family Sharing as intended and you are not logged out. Unchanged in this release.

## Upgrading

1. Close Steam completely (tray icon -> Exit).
2. Copy the three DLLs from the ZIP (`AmethystTool.dll`, `dwmapi.dll`, `xinput1_4.dll`) over the old ones next to `steam.exe`. Self-update is disabled in this fork, so this is a manual step. Keep your `amethysttool.toml`.
3. If you enabled `[compatibility] denuvo_aliases`, the `OpenSteamTool.dll` alias is refreshed automatically the next time Steam starts.
4. Start Steam.

## Verification

Validated locally on Windows (2026-11-06), merged tree `3a38213`:
- `.\build.ps1 -Clean`: 369/369 tests in Release and Debug.
- Warnings: Release 9 / Debug 8 (baseline).
- ABI: `TokeerUri` ordinal 1. CRT: `/MT` static.

There is no ThreadSanitizer on MSVC, so the concurrency fix is verified by code review (no critical or high findings) and by construction, not by a race detector. Real-world testing of the three areas above is what this release candidate is for.
