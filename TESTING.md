# Testing AmethystTool in an isolated environment

AmethystTool injects a DLL into Steam and installs in-process hooks. That is inherently risky for your Steam account and is exactly what anti-cheat and anti-fraud systems look for. **Test in an isolated environment first — never on your main account.** Everything below assumes a throwaway setup you can lose without consequence.

> The risk is entirely yours. Valve can suspend or ban accounts that tamper with the Steam client. Use a disposable account and a machine (or VM) you don't care about.

## 1. Prepare an isolated machine

A virtual machine is the safest option; a spare physical machine works too.

- **OS:** Windows 10 or 11 (x64). The tool is Windows-only and x64.
- **VM software:** VirtualBox, VMware, or Hyper-V. Give the VM enough resources to run Steam and a game (4+ CPU cores, 8+ GB RAM, GPU passthrough only if you need to launch 3D titles — many checks can be validated without actually rendering a game).
- **Snapshot before you start.** Take a clean VM snapshot after installing Windows + Steam but before injecting anything, so you can roll back to a known-good state after each test.
- **Network:** leave normal internet access — the tool makes outbound HTTPS calls on launch (see the pattern-lookup section of the [README](README.md#steam-version-compatibility)). If you want to observe exactly what it sends, run a proxy (e.g. Fiddler/mitmproxy) inside the VM.

## 2. Use a disposable Steam account

- Create a **separate, throwaway Steam account** — do not sign in with your main account, not even once, on the test machine.
- Assume the account may be flagged or banned. Do not link payment methods, real email, or phone you care about.
- Install Steam fresh in the VM and let it fully update before injecting.

## 3. Build and copy the DLLs

1. Build the project on your dev machine (see [README → Build](README.md#build)):
   ```powershell
   .\build.ps1
   ```
2. Copy the three generated DLLs into the **Steam root directory** (the folder containing `steam.exe`) on the test machine:
   - `AmethystTool.dll`
   - `dwmapi.dll`
   - `xinput1_4.dll`

   Use the matching build for what you're testing — Release for normal use, Debug if you want the per-module log files (see step 5).
   - Release: `build/Release/…`
   - Debug: `build/Debug/…`
3. If your antivirus quarantines the DLLs, that is an expected false positive for injection/Detours code — add an exclusion for the three specific files (not the whole Steam folder) or restore from quarantine. See [README → Antivirus and SmartScreen](README.md#antivirus-and-smartscreen).

## 4. Configure the TOML

**First use.** Copy `amethysttool.example.toml` to the Steam root directory and rename it to `amethysttool.toml`. Edit values as needed. If you don't provide a config, built-in defaults are used (no file is auto-created).

- Keep the privacy defaults unless you have a reason to change them: `[stats] enable_api = false` and `[update] enabled = false` (self-update is compiled out regardless).
- For a Debug build, set `[log] level = "debug"` (or `trace`) so the log files are verbose.

**Testing config migration.** To verify the `opensteamtool.toml → amethysttool.toml` fallback ([README → Config migration](README.md#config-migration)):

1. Ensure there is **no** `amethysttool.toml` in the Steam root.
2. Place an `opensteamtool.toml` there (e.g. a copy of the example with a distinctive value like `[log] level = "trace"`).
3. Launch Steam. On startup the tool copies it to `amethysttool.toml`.
4. Confirm:
   - `amethysttool.toml` now exists with the same contents.
   - `opensteamtool.toml` is **still present** (copied, not moved).
   - `main.log` contains `Migrated config from opensteamtool.toml to amethysttool.toml` (Debug build).
5. Re-launch and confirm the migration does **not** run again and does not overwrite your `amethysttool.toml`.

## 5. Check the logs

Debug builds write per-module log files to **`<Steam>\amethysttool\`** (inside the Steam install directory, next to `steam.exe` — not under `%AppData%`). Start with:

- `main.log` — init, config loading (including the migration line above), Lua parsing.
- `pipe.log` — pipe handshakes, process inspection, Denuvo authorization, injection.
- `manifest.log`, `netpacket.log`, `ipc.log` — feature-specific diagnostics.

The full table of log files is in [README → Debug logging](README.md#debug-logging). Release builds are much quieter by design.

Also run the unit test suite on your dev machine before deploying a build:
```powershell
ctest --preset test-debug --output-on-failure
```

## 6. Warning signs — stop and roll back

Treat any of the following as a failed test. Restore your clean VM snapshot before trying again:

- **Steam crashes on launch** or fails to reach the library — likely a pattern mismatch for the current Steam build (check `main.log` / the one-shot popup naming the unmatched DLL and its SHA-256).
- **A game process crashes** shortly after launch — a partial/incorrect hook. Note the AppId and the relevant log.
- **Any account warning, VAC/anti-cheat notice, or trade/market restriction** — stop immediately; do not reuse the account.
- **Unexpected network activity** to endpoints not documented in the README — capture it with your proxy and investigate before continuing.
- **Antivirus flags behaviour at runtime** (not just the file on disk) — worth understanding before you trust the build.

## 7. Disclaimer

Using AmethystTool on your **main account is entirely at your own risk.** This guide exists so you can evaluate the tool without exposing an account you care about. The project is provided for research and educational purposes only; you are responsible for complying with local laws, platform terms of service, and software licenses.
