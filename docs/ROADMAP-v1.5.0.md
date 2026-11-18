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
- Status 2026-11-18: delivered and unit-tested; the test also runs in the Linux logic-test job. Not yet run against a real
  game (the field tests that day covered pillar 2 and an unrelated install error), so the open point above stands.

### 4. Shadow migration: engine and recorder implemented, tool not started

Status 2026-11-18: `PatternMigrateLogic.h` (engine), `PatternRecorderLogic.h` / `PatternRecorder.cpp` and
`PatternLoader::StartShadowRecording` (recorder, `[shadow] record`, **off by default**) are in. `tools/pattern_migrate`
is not. Two things changed against the plan below, both found while implementing it:

- The recorder reads the DLL **from disk**, not from the mapped module. It runs after the hooks are installed, and an
  inline detour overwrites the first bytes of exactly the functions being fingerprinted; the file on disk keeps the
  original bytes and is what the SHA-256 in the file name was computed from.
- The default is **off**, closing the open decision at the end of this section: the files only help a maintainer who
  will run the tool, and the cost is a ~20 MB read per Steam build. The maintainer turns it on before the next update.

Goal: when Steam ships a build that no mirror has a TOML for yet, a maintainer can produce one from data the client
recorded while the previous build worked, with a measured error rate. Nothing here changes hooking or migrates at runtime.

**Recorder (in the DLL, "shadow mode")**

- When: after hooks install, only if every required function resolved, and only **once per DLL SHA-256**. If
  `<Steam>\amethysttool\fingerprint\<component>\<sha256>.toml` exists, nothing runs. The cost is one pass per Steam build,
  not one per launch.
- Where: a detached worker thread started after `PatternLoader::StartCacheRevalidation`, never on Steam's loader path. Same
  shape as the revalidation job: the worker gets a copied snapshot (module handle, SHA, resolved name -> RVA) and shares
  no lock with the hooks.
- ~~It reads only the already-mapped `.text` and `.pdata` through `OSTPlatform::Memory::GetModuleSection`.~~ Replaced:
  it reads the DLL file on disk (PE headers, then the `.text` and exception-directory ranges, with caps on sizes and
  every range checked against the file), see the status above.
- What, per resolved function:
  - name and RVA;
  - `.pdata` range (begin, size), or "none" for leaf functions without an unwind entry;
  - a masked byte prefix (rel32 `call`/`jmp`/`jcc` displacements and RIP-relative `disp32` masked);
  - how many times that masked prefix matches in `.text`;
  - the known functions it calls (call-graph anchors, as names resolved through the same RVA map).
- Storage: one TOML per component and SHA, written through a temp file and a rename (the `ReplaceCacheFile` pattern), a
  few KB per build. Local only: no network, nothing uploaded. Leftover temp files are cleaned by the same
  `RemoveOrphanTempFiles` sweep.
- Budget: about 36 functions, each a 64-byte prefix plus one uniqueness scan over `.text` (~15 MB). Expected well under a
  second on the worker thread. The duration is logged so this can be checked.

**Engine (pure, header-only `PatternMigrateLogic.h`)**, shared by the recorder and the tool

- Masking rules: a minimal x64 decoder covering only the encodings above, unit-tested on hand-assembled buffers.
- Candidate search in a new image: unique masked match, the match starts at a `.pdata` function begin, and the size is
  within 25% of the recorded size.
- New in the plan: the candidate must call the same already-migrated anchors.
- A function with no `.pdata` entry is never auto-accepted; it is reported "needs manual".
- Tests like `module_section_test`: synthetic PE images, including the shapes from the evidence below (a near-identical
  neighbour 3 bytes shorter, a leaf function without `.pdata`).

**Offline tool `tools/pattern_migrate`**

- Input:
  - `--fingerprint <old fingerprint .toml>`;
  - `--dll <new steamclient64.dll or steamui.dll>`, read from the file on disk and mapped by section, never loaded;
  - optional `--required <old pattern .toml>`, so required functions are known.
- Output:
  - a candidate `<new sha256>.toml` in the steam-monitor format (RVA per function);
  - a report per function: `migrated`, `ambiguous`, `not found`, `size mismatch` or `needs manual`.
  - Non-zero exit if any required function is not `migrated`.
- It never writes into a Steam folder. Maintainer flow: run, review the report, try the TOML locally by dropping it into
  `<Steam>\amethysttool\pattern\<component>\` (the existing manual path), then publish to a mirror of one's own
  (`[remote] url_template`).

**Risk: the ~4% silent error, and how the recorder reduces it**

- Measured without the recorder (one Steam build, see Evidence below): 21 of 26 functions migrated correctly, 2 leaf
  functions could not be checked, and 1 wrong function was accepted (`SendCallbackToPipe`, 76 B against 79 B). That is
  1 in 26, about 4%, wrong and not flagged.
- Mitigations:
  - The recorder gives ground truth from the build that actually ran with a working TOML (exact size, masked bytes,
    anchors) instead of signatures carried over from a TOML.
  - The anchor check rejects a same-sized neighbour that calls different functions, which is the shape of the
    `SendCallbackToPipe` case.
  - Leaf functions without `.pdata` are flagged, never guessed.
  - Output goes to a human and through the existing H-1/H-1b checks (RVA inside the image) before any client uses it.
  - Fingerprints from at least two Steam updates measure the real error rate before v1.5.1 considers runtime migration.
- Remaining risk: a wrong RVA that passes review hooks the wrong function and can crash Steam. That is why v1.5.0 stops
  at an offline, reviewed tool.

**Acceptance for pillar 4**

- The recorder writes one fingerprint per build, with no measurable delay to hook install (duration logged), and is
  silent when the file exists.
- Engine tests are green on Windows and in the Linux logic-test job.
- Replayed on the cached TOMLs of the audit machine (builds of 2026-10-21 and 2026-11-12/17), the tool reaches at least
  21 of 26, and rejects `SendCallbackToPipe` or reports it as ambiguous.
- No runtime migration, no new network traffic.

**Open decisions**

- ~~Recorder default: on or opt-in.~~ Decided: opt-in, `[shadow] record = false`.
- Length of the masked prefix (64 bytes proposed).
- Whether the anchor check makes v1.5.0 or waits for v1.5.1.

### Outside the pillars: cache-miss mirror order (from upstream v1.0.9, implemented)

- Upstream v1.0.9 fixed "invalid content configuration" on networks without GitHub raw: its `RemoteToml::Fetch` was
  network-first, so hooks installed ~24 s after Steam start. The fork has been cache-first since v1.3.x, so only the
  cache miss (first launch after a Steam update) had that window.
- Taken: jsDelivr before GitHub raw for each repo, and 2.5 s resolve/connect, 5 s send/recv on the miss walk.
- Not taken: upstream's detached refresh on every launch. It sends the DLL hash on every start and overwrites a working
  cache without the H-1/H-1b checks. The throttled, validated revalidation in `PatternRefreshLogic.h` stays.
- Limit: if no mirror answers and there is no cache, a miss can still take ~5 s per mirror per component.

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
