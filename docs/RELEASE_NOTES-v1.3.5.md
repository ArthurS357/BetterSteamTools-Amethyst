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

The validator script itself (`https://luatools.vercel.app/Devuvo.ps1`) could not be downloaded from the build environment, so the mechanism was derived from the field test and confirmed by the design, not by reading the script:

- **File names next to `steam.exe`** (`Test-Path`-style check). Field test (Phantom Blade Zero, AppID 4115450): copying `AmethystTool.dll` to `OpenSteamTool.dll` and `amethysttool.toml` to `opensteamtool.toml` (the originals stayed in place) was enough for the validator to report VALIDATED and generate the ticket, with nothing else changed.
- **Not a module-in-memory check.** Steam kept running with the original `AmethystTool.dll` loaded; the `OpenSteamTool.dll` copy was never loaded into `steam.exe` by anything. The validator passed anyway, so it cannot be checking `steam.exe` for a module named `OpenSteamTool.dll`.
- **Not a registry or service check.** Copying two files touches neither, and it was sufficient.

That is why plain file aliases are the whole fix. To confirm against the script yourself:

```powershell
Invoke-WebRequest "https://luatools.vercel.app/Devuvo.ps1" -OutFile validator.ps1
Select-String -Path validator.ps1 -Pattern 'OpenSteamTool|opensteamtool|OST engine|activation engine|Test-Path|Get-Process|Modules|HKCU|HKLM|Get-Service' -Context 2,2
```

If that ever shows a `Get-Process steam | ... .Modules` or a registry lookup, the aliases alone will not be enough; please open an issue with the output.

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

Manual step still pending for users who want Denuvo tickets:
run `enable-denuvo-compat.ps1` (or set `[compatibility] denuvo_aliases = true`),
restart Steam, then run the LuaTools Validator once against the **DLL-generated**
aliases. The only difference from the field test is the 3-line `#` header on
`opensteamtool.toml`. If the validator ever rejects it, the fallback is a
header-less alias with ownership tracked differently.

See `CHANGELOG.md` for the full list.
