# AmethystTool v1.5.0 roadmap: independence and stability

Status: approved scope, in progress. Branch: `v1.5.0-independence-and-ux`.

## Why

AmethystTool already reads its pattern and IPC metadata cache-first, keyed by the SHA-256 of the Steam DLL, so
connectivity is not the problem. The weak point is a **third-party publisher**: when Steam ships a new build,
the hooks of that build stay down until the `madoiscool/steam-monitor` bot publishes a TOML for its hash. The
second mirror (`OpenSteam001`) has had no commit since 2026-09-06. Manifest request codes are a second single
point of failure: one provider is active at a time and nothing takes over when it stops answering.

v1.5.0 removes the avoidable network dependencies, adds failover where a dependency cannot be removed, and
prepares the data needed to migrate patterns safely in v1.5.1.

## Decisions

| # | Decision |
|---|---|
| 1 | Manifest failover is **on by default** (`wudrm` then `steamrun`), with `[manifest] failover = false` as the escape hatch. |
| 2 | Pattern migration **at runtime** is deferred to v1.5.1. v1.5.0 ships the shadow recorder and an offline tool only. |
| 3 | Option A (Steam Overlay and Input fixer) is **dropped to the backlog**: no blocking flag or call was identified in the code, and `ISteamInput` has no IPC spec entry. |
| 4 | The UX feature is **B', "Pin current version"**, built on the existing `setManifestid`. The `GetAppUpdateInfo` hook originally proposed does not exist and is not planned. |

## Pillars

### 1. Embedded IPC spec (implemented)

- `Utils/SteamMetadata/IPCEmbedded.h`: a `constexpr` table of the 6 methods in use (`funcHash`, `fencepost`, `argc`).
- Load order in `IPCLoader::Load`: cache by SHA, then the embedded table, then the mirror chain, then the popup.
- The embedded table is used only after **verification**: all 12 constants must occur as `uint32_t` in the loaded
  `steamclient64.dll` `.text` section. If one is missing (Valve changed the protocol), the loader falls through to the mirrors,
  which is today's behaviour.
- No request is made for IPC on a Steam update when the table verifies, so the DLL hash is not sent for this component.
- Limits: it covers the 6 methods only. A new handler needs a new table row, and a unit test fails if a functional
  handler has no row.

### 2. Manifest failover (implemented)

- Sequential chain: the configured provider first, then the remaining defaults in the order `wudrm`, `steamrun`.
  A `[manifest] url_template` override is never swapped for a built-in provider.
- Total budget of about 10 s, below the 12 s Steam's receive path waits (`Hooks_NetPacket.cpp`, `kMaxWaitSeconds`).
  Each attempt gets its share of what is left, and no WinHTTP phase may wait longer than that share. The limit is best
  effort (WinHTTP applies timeouts per phase and per read); Steam's 12 s wait is the hard stop. Shrinking the phases in
  proportion was tried first and rejected in review: it cut a 5 s connect to under 1 s, less than one lost SYN costs.
- Circuit breaker: a transport failure or an HTTP 5xx puts that provider in quarantine for 5 minutes. A 429 is not
  counted, because several depots ask at once and the burst may be ours. A 4xx or an
  unparsable body falls through to the next provider without quarantine (the provider is up, it just lacks the code).
  When every provider is quarantined all of them are tried anyway.
- `ManifestClient` no longer holds its mutex across the network or Lua call, so concurrent depot requests stop serializing.
- Privacy: on failure the manifest `gid` is also sent to the next provider. That is documented in the README.
- Not changed: `wudrm` is still `http://` and gid-only. Its behaviour after 2026-09-09 should be measured before the default order is revisited.

### 3. Pin current version (B') (implemented as the offline tool `tools/version_pin`)

- Reads `InstalledDepots` from `steamapps\appmanifest_<appid>.acf` and writes `<appid>.pin.lua` with one
  `setManifestid(depot, "gid")` per depot into `config\stplug-in`. No new hook, no IPC change, no network, not in the DLL.
  Logic in `Utils/SteamMetadata/VersionPinLogic.h`, CLI in `tools/version_pin/`.
- No size is written: the engine forces it to 0 (`lua_setManifestid`), and the README that said otherwise was corrected.
- Real Lua files (the Luie ones) already call `setManifestid` per depot with the manifest of the day they were made. The tool
  therefore checks the other `.lua` files and warns about a different pin for the same depot; the engine lets the file parsed
  last win, so the tool does not guess. On the audit machine the three Lua-added games already pinned exactly the installed
  gids (no conflict); only a shared redistributable depot conflicted.
- Pinning to the installed gid needs no request code when `depotcache\<depot>_<gid>.manifest` is on disk (seen for the
  installed depots of one app on the audit machine).
- Still open, needs a real game: whether Steam shows an update as pending because the `buildid` differs. The tool was
  exercised against real manifests and the real `LuaConfig` parses its output, but it was not run against a Steam that
  has an update waiting.

### 4. Shadow migration, not started

- Recorder: on each healthy launch, store a fingerprint per resolved function (masked bytes, `.pdata` function size,
  match count) under `amethysttool\fingerprint\`. No behaviour change.
- Offline tool `tools/pattern_migrate`: a maintainer runs it against a new Steam DLL, reviews the diff and publishes to a mirror.
- Both share one pure engine, unit-tested like `PatternRefreshLogic.h`.
- Runtime migration waits for v1.5.1 and for data from at least two Steam updates.

## Evidence behind the scope

Measured on one Steam install (steamclient64.dll build of 2026-10-21) against three cached TOMLs. One sample, so treat it as
direction, not proof.

- IPC: `funcHash`, `fencepost`, `argc` and `method_index` were identical across 3 builds spanning 7 weeks. `wrapper_rva` and
  `vtable_rva` changed every build and are not read at runtime. All 12 constants occur in `.text`.
- Patterns: carrying the previous build's signatures over resolved 21 of 26 functions correctly under the proposed checks
  (unique match, `.pdata` function start, size within 25%). One wrong function was still accepted (`SendCallbackToPipe`, 76 B
  against 79 B) and 2 correct leaf functions had no `.pdata` entry. That error floor is why runtime migration is not in v1.5.0.
- Manifests: the request code rotates within minutes and is bound to a depot the provider's account owns, so it can be
  neither cached nor minted locally for unowned depots. Failover is the realistic goal, not offline operation.

## Out of scope for v1.5.0

- Runtime pattern migration (v1.5.1, opt-in `[remote] migrate`).
- Steam Overlay and Input fixer (backlog).
- Version pinning by hooking an update-info call.
- New IPC methods: the embedded table does not discover them.

## Acceptance for each pillar

- Pure logic lives in a header or small unit with GoogleTest coverage and no network or Windows dependency.
- `.\build.ps1 -Clean` is green for Release and Debug, and the full `AmethystToolTests` suite passes.
- A failure in a new path falls back to the previous behaviour (mirrors, single provider, popup) and never to a silent wrong answer.
