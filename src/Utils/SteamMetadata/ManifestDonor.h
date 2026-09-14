#pragma once

#include <cstdint>

// ─────────────────────────────────────────────────────────────────
//  ManifestDonor — contributes manifest request codes for depots this
//  account owns.
//
//  The backend mints codes from its own Steam sessions, but that only
//  reaches depots those sessions can access; anything else answers 403.
//  This closes that gap by minting from a signed-in user who owns the
//  depot, on request.
//
//  Sits opposite ManifestClient/ManifestCache, which *consume* codes and
//  manifests from the same service. Nothing here feeds the local Steam
//  client -- this namespace only ever sends.
//
//  Direction matters: the server sends what it wants first, and the
//  client answers only with the intersection it owns. The user's library
//  is never uploaded.
//
//  Codes are bound to (depot, manifest) and rotate a few minutes after
//  issuance, so they are minted on demand and posted immediately -- there
//  is no store-and-forward, and a stored code would simply be dead.
//
//  OFF BY DEFAULT (Amethyst fork): [donate] enabled=false unless the user
//  opts in explicitly in amethysttool.toml. This is the deliberate
//  opposite of upstream BetterSteamTools, which defaults enabled=true
//  (opt-out) -- see the Fase C session audit. Every entry point below
//  re-checks Config::GetDonateSettings().enabled independently rather than
//  trusting a caller to have checked it first.
// ─────────────────────────────────────────────────────────────────
namespace ManifestDonor {

    // Starts the worker thread. No-op when [donate] is disabled (the
    // default), and safe to call once per process. Returns immediately;
    // all work is detached.
    void Start();

    // Signals the worker to stop. It finishes the request in flight rather
    // than being interrupted.
    void Stop();

    // Passive capture: submit a genuine manifest request code that the user's
    // own Steam just received for a depot it can access. Costs no extra Steam
    // traffic -- the code was already spent on a real download. Posts on a
    // detached worker; a no-op when [donate] is disabled or any argument is 0.
    // Codes rotate within minutes, so this fires immediately rather than
    // batching.
    void SubmitCapturedCode(uint32_t depotId, uint64_t gid, uint64_t code);
}
