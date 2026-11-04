# AmethystTool v1.3.5

Base: v1.3.4 (`f9aa1f7`).

## Highlights

- **New: Denuvo compatibility mode.** LuaTools Validator (the official TokeerDRM ticket flow) only recognises the upstream engine by its file names, so AmethystTool users were stopped with *"Set up your activation engine first"*. Turn on `[compatibility] denuvo_aliases = true` (or run `enable-denuvo-compat.ps1` once) and AmethystTool places `OpenSteamTool.dll` / `opensteamtool.toml` aliases next to `steam.exe`. The validator passes; AmethystTool itself keeps running as `AmethystTool.dll` with `amethysttool.toml`.
- **New: `enable-denuvo-compat.ps1`** in the release ZIP, for users who would rather not edit the TOML. `-Remove` cleans up afterwards.
- **Hardening: one AmethystTool per Steam process.** If anything ever loads a second copy of the DLL (such as the alias), that copy stays inert instead of hooking Steam twice.
- **Hardening: concurrent alias writers.** Each write now has its own temp file, so two `steam.exe` processes starting together can no longer expose a partial alias; orphaned temps from a killed process are swept.
- **Hardening: pattern revalidation (H-1b).** A mirror copy can no longer add an entry with an out-of-image RVA outside the missing functions.
- **Internal:** achievement and rich-presence *decisions* moved out of `Hooks_NetPacket.cpp` into testable headers; a Linux CI now runs the pure-logic tests on every push.
- **Tests:** 93 new unit tests (expected total **349**).

## How the validator detects the engine

The LuaTools Validator checks that two files exist next to `steam.exe`: `OpenSteamTool.dll` and `opensteamtool.toml`. It does not look at loaded modules, the registry or services.

With Denuvo Compatibility Mode on, AmethystTool creates both files itself at Steam startup, so the Validator recognises the engine with no manual renaming. AmethystTool keeps running as `AmethystTool.dll` with `amethysttool.toml`; the aliases are only there for the Validator to find.

If the Validator ever rejects the aliases, please open an issue with its output.

## Compatibility notes

- **Off by default.** With `denuvo_aliases` absent or `false`, the feature does no I/O at all. Nothing changes for users who don't use Denuvo tickets.
- **Privacy unchanged.** No network or registry access. The aliases are local copies that only external tools read.
- **A real OpenSteamTool install is safe.** An `OpenSteamTool.dll` without the AmethystTool marker string, or an `opensteamtool.toml` without the alias header, is never overwritten or deleted (by the DLL or by the script). A warning is logged and the existing file still satisfies the validator.
- **Interaction with config migration.** Migration (`opensteamtool.toml` → `amethysttool.toml`) still runs first and only when `amethysttool.toml` is missing. The alias is written from `amethysttool.toml` afterwards, so the two never loop.
- **Disabling.** `denuvo_aliases = false` stops refreshing the aliases but leaves them in place. Delete them by hand or with `enable-denuvo-compat.ps1 -Remove`. The key is read at Steam startup and is not hot-reloaded.
- **Antivirus.** `OpenSteamTool.dll` is a second copy of the same injection DLL, so if your AV whitelists by path, add it too (see `AV_WHITELIST.md`).

## Verification

Validated locally on Windows (2026-11-03):

- `.\build.ps1 -Clean`: 349/349 tests in Release and in Debug (256 + 93 new).
- Warnings: Release 9 / Debug 8 (unchanged baseline).
- ABI: `TokeerUri` at ordinal 1. CRT: static `/MT`.
- Linux CI (`linux-logic-tests.yml`): 259 pure-logic tests passing under ASan + UBSan.

Field-validated (2026-11-03):

- `LuaTools Validator` detected the DLL-generated `OpenSteamTool.dll` as
  `OpenSteamTool (official)` and passed all checks, with no manual renaming.
- The alias DLL is byte-identical to `AmethystTool.dll` (SHA-256 match).
- The 3-line comment header on `opensteamtool.toml` does not affect the
  validator; it only requires the file to exist.

See `CHANGELOG.md` for the full list.
