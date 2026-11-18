#include "ManifestClient.h"
#include "OSTPlatform/include/Http.h"
#include "Utils/Config/Config.h"
#include "Utils/Config/LuaConfig.h"
#include "Utils/Logging/Log.h"
#include "Utils/SteamMetadata/ManifestFailoverLogic.h"
#include "Utils/SteamMetadata/ManifestUrlLogic.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace ManifestClient {

    // ── parsers ────────────────────────────────────────────────────
    using Parser = bool (*)(std::string_view body, uint64_t* out);

    static bool ParsePlainUint(std::string_view body, uint64_t* out) {
        uint64_t code = 0;
        auto [_, ec] = std::from_chars(body.data(), body.data() + body.size(), code);
        if (ec != std::errc{}) return false;
        *out = code;
        return true;
    }

    static bool ParseSteamRunJson(std::string_view body, uint64_t* out) {
        size_t key = body.find("\"content\"");
        if (key == std::string_view::npos) return false;
        size_t q1 = body.find('"', key + 9);
        if (q1 == std::string_view::npos) return false;
        size_t q2 = body.find('"', q1 + 1);
        if (q2 == std::string_view::npos) return false;
        return ParsePlainUint(body.substr(q1 + 1, q2 - q1 - 1), out);
    }

    // ── provider table ────────────────────────────────────────────
    //
    // Adding a new provider: add one row to kProviders below.
    // host / port / tls / path are all derived from the URL template
    // by Make() at compile time.

    struct Provider {
        std::string_view name;          // matches [manifest] url = "..."
        const char*      urlTemplate;   // one %llu (gid) — the pre-2026-09-09 shape
        // Optional depot-aware form: %u app, %u depot, %llu gid. Preferred
        // whenever the caller knows the depot, and required for correctness —
        // see kProviders below. nullptr for providers that only speak gid.
        const char*      urlTemplateEx;
        Parser           parse;
    };

    consteval Provider Make(std::string_view name, const char* url, const char* urlEx, Parser parse) {
        return {name, url, urlEx, parse};
    }

    // Valve made the request code depot-bound on 2026-09-09: it is derived from
    // (depot_id, manifest_id, time, secret), and the CDN answers 401 for a code
    // minted against any other depot. A gid-only request cannot name the depot,
    // so the server falls back to a free-to-play carrier and the resulting code
    // only works for 731/571/441 — every other depot 401s at the CDN and Steam
    // reports "Failed downloading 1 manifests".
    //
    // So the three-segment form is not an optimisation, it is the only shape
    // that works for ordinary depots. The gid-only template is kept solely as a
    // fallback for when the depot is genuinely unknown, and for the two
    // third-party providers that expose no depot-aware route.
    static constexpr Provider kProviders[] = {
        // manifest.opensteamtool.com stopped serving manifests for games the
        // account does not own (2026-09) -- kept as a selectable provider
        // (users can still opt back in via [manifest] url), but no longer
        // the default. See ManifestUrlLogic::kDefaultProviderName.
        Make("opensteamtool", "https://manifest.opensteamtool.com/%llu",
                              "https://manifest.opensteamtool.com/%u/%u/%llu",  ParsePlainUint),
        Make("wudrm",         "http://gmrc.wudrm.com/manifest/%llu",
                              nullptr,                                          ParsePlainUint),
        Make("steamrun",      "https://manifest.steam.run/api/manifest/%llu",
                              nullptr,                                          ParseSteamRunJson),
    };

    // Resolves ManifestUrlLogic::kDefaultProviderName to its row in kProviders
    // at compile time -- a rename or typo of the constant fails the build
    // instead of silently falling back to kProviders[0].
    consteval std::size_t FindDefaultProviderIndex() {
        for (std::size_t i = 0; i < sizeof(kProviders) / sizeof(kProviders[0]); ++i) {
            if (kProviders[i].name == ManifestUrlLogic::kDefaultProviderName) return i;
        }
        throw "ManifestUrlLogic::kDefaultProviderName has no matching row in kProviders";
    }

    // g_mutex guards g_active, g_urlTemplateOverride and g_failover, and nothing
    // else. It is NEVER held across a network call or a Lua call (v1.5.0): several
    // depots ask for a code at once, and each of those waits could take seconds.
    // Readers copy what they need under the lock (SnapshotState) and let go.
    static const Provider* g_active = &kProviders[FindDefaultProviderIndex()];
    static std::mutex      g_mutex;

    // Empty = no override, use g_active (see SetUrlTemplateOverride).
    static std::string g_urlTemplateOverride;

    // [manifest] failover. On by default; see ManifestFailoverLogic.h for the policy.
    static bool g_failover = true;

    // Thread-safe by itself (its own leaf mutex), so it lives outside g_mutex.
    static ManifestFailoverLogic::CircuitBreaker g_breaker;

    static const Provider* FindProvider(std::string_view name) {
        for (const auto& p : kProviders)
            if (p.name == name) return &p;
        return nullptr;
    }

    bool SetProvider(std::string_view name) {
        const Provider* found = FindProvider(name);
        if (!found) return false;
        std::lock_guard<std::mutex> lock(g_mutex);
        g_active = found;
        return true;
    }

    const char* ActiveProviderName() {
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_active->name.data();
    }

    // {gid} remains the one REQUIRED placeholder: it's what makes the template
    // usable at all (a manifest gid is the one thing every request varies on).
    // {appid}/{depotid} (see ManifestUrlLogic::Build) are optional additions
    // for the post-2026-09-09 depot-aware shape -- a template written before
    // they existed, using only {gid}, is still valid and unaffected.
    static bool HasGidPlaceholder(std::string_view urlTemplate) {
        return urlTemplate.find("{gid}") != std::string_view::npos;
    }

    bool SetUrlTemplateOverride(std::string_view urlTemplate) {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (urlTemplate.empty()) {
            g_urlTemplateOverride.clear();
            return true;
        }
        if (!HasGidPlaceholder(urlTemplate)) {
            LOG_MANIFEST_WARN("manifest.url_template missing {{gid}}, ignoring: {}", urlTemplate);
            return false;
        }
        g_urlTemplateOverride = std::string(urlTemplate);
        return true;
    }

    void SetFailoverEnabled(bool enabled) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_failover = enabled;
    }

    // ── request ───────────────────────────────────────────────────

    void Shutdown() {
        std::lock_guard<std::mutex> lock(g_mutex);
    }

    // ── fetch ─────────────────────────────────────────────────────

    constexpr int kHttpOk = 200;

    using ManifestFailoverLogic::AttemptOutcome;
    using FailoverClock = ManifestFailoverLogic::Clock;

    // The settings one fetch needs, copied under g_mutex so it can run without it.
    struct State {
        const Provider* active;
        std::string     overrideTemplate;
        bool            failover;
    };

    static State SnapshotState() {
        std::lock_guard<std::mutex> lock(g_mutex);
        return { g_active, g_urlTemplateOverride, g_failover };
    }

    // One GET. With `fit`, every WinHTTP phase is capped at the time left until
    // `deadline` (ManifestFailoverLogic::CapTimeouts); without it the timeouts are
    // exactly the configured ones, as before v1.5.0.
    static OSTPlatform::Http::Result Get(const std::string& url, const Config::ManifestTimeouts& cfg,
                                         FailoverClock::time_point deadline, bool fit) {
        ManifestFailoverLogic::PhaseTimeouts t{ cfg.resolve, cfg.connect, cfg.send, cfg.recv };
        if (fit) {
            t = ManifestFailoverLogic::CapTimeouts(
                t, std::chrono::duration_cast<ManifestFailoverLogic::Millis>(deadline - FailoverClock::now()));
        }
        return OSTPlatform::Http::Execute(L"GET", url.c_str(), nullptr, 0, nullptr,
                                          t.resolve, t.connect, t.send, t.recv);
    }

    // What a Choice means for parsing and logging. Override (if set) short-circuits
    // provider selection entirely -- ManifestUrlLogic::Build never falls back to a
    // provider template when the override is non-empty -- and the user's endpoint
    // always answers with a plain decimal code.
    struct ChoiceInfo {
        Parser      parse;
        // Only read by the LOG_MANIFEST_* macros, which are no-ops in Release
        // (see the OPENSTEAMTOOL_LOGGING_ENABLED comment in CMakeLists.txt).
        [[maybe_unused]] const char* label;
        [[maybe_unused]] const char* shape;
    };

    static ChoiceInfo DescribeChoice(const Provider& p, const ManifestUrlLogic::Choice& choice) {
        switch (choice.shape) {
        case ManifestUrlLogic::Shape::Override:   return { ParsePlainUint, "custom", "override" };
        case ManifestUrlLogic::Shape::DepotAware: return { p.parse, p.name.data(), "app/depot/gid" };
        case ManifestUrlLogic::Shape::GidOnly:    break;
        }
        return { p.parse, p.name.data(), "gid-only" };
    }

    // ONE provider's whole fetch (including its depot-aware -> gid-only retry).
    // `fit` is true when other providers are waiting behind this one, so it must
    // finish by `deadline`; false keeps the single-provider behaviour unchanged.
    static AttemptOutcome FetchFromProvider(const Provider& p, const State& state, uint64_t gid,
                                            AppId_t appId, AppId_t depotId,
                                            FailoverClock::time_point deadline, bool fit) {
        const Config::ManifestTimeouts timeouts = Config::GetManifestTimeouts();
        const ManifestUrlLogic::Choice choice = ManifestUrlLogic::Build(
            state.overrideTemplate, p.urlTemplate, p.urlTemplateEx, appId, depotId, gid);
        const ChoiceInfo info = DescribeChoice(p, choice);
        // Choice::truncated is always false for Override (see Build()).
        if (choice.truncated) {
            LOG_MANIFEST_WARN("manifest provider URL unexpectedly long, truncated: {}", p.name);
        }

        auto r = Get(choice.url, timeouts, deadline, fit);

        // Log which shape was used: a gid-only request for a non-carrier depot
        // yields a code the CDN will reject, and that is otherwise invisible
        // until the download fails.
        LOG_MANIFEST_INFO("Manifest {} status={} gid={} depot={} shape={}",
                          info.label, r.status, gid, depotId, info.shape);

        // Single fallback retry: a depot-aware request that failed (transport
        // error, non-200 -- includes a WAF/route rejection a provider server
        // does not recognize yet) gets one retry against the plain gid-only
        // URL before giving up. Never applies to Override (the user's own
        // url_template is reported as failed, not silently swapped for
        // something else) or to GidOnly (nothing simpler to fall back to).
        // depotId=0 reuses Build()'s own "unknown depot" rule to force
        // Shape::GidOnly rather than duplicating that decision here.
        // With failover on, ShouldRetryGidOnly also skips the retry for a provider
        // that is down and when too little of this attempt's time is left.
        const auto left = std::chrono::duration_cast<ManifestFailoverLogic::Millis>(deadline - FailoverClock::now());
        if (ManifestFailoverLogic::ShouldRetryGidOnly(
                ManifestUrlLogic::ShouldFallbackToGidOnly(choice.shape, !r.ok || r.status != kHttpOk),
                fit, r.ok, r.status, left)) {
            LOG_MANIFEST_DEBUG("Manifest {} depot-aware failed (status={}), retrying gid-only for gid={}",
                               info.label, r.status, gid);

            const ManifestUrlLogic::Choice fallback = ManifestUrlLogic::Build(
                state.overrideTemplate, p.urlTemplate, p.urlTemplateEx, appId, /*depotId=*/0, gid);

            r = Get(fallback.url, timeouts, deadline, fit);

            LOG_MANIFEST_INFO("Manifest {} status={} gid={} depot={} shape=gid-only (fallback)",
                              info.label, r.status, gid, depotId);
        }

        AttemptOutcome outcome;
        outcome.transportOk = r.ok;
        outcome.status = r.status;
        outcome.gotCode = r.ok && r.status == kHttpOk && info.parse(r.body, &outcome.code);
        return outcome;
    }

    // The provider chain for the current settings (ManifestFailoverLogic.h), with
    // no lock held while the providers are on the wire.
    static bool FetchFromProviders(uint64_t gid, uint64_t* outCode, AppId_t appId, AppId_t depotId) {
        const State state = SnapshotState();
        const bool hasOverride = !state.overrideTemplate.empty();

        // "custom" names the user's own endpoint in the breaker, so a failure there
        // never quarantines a built-in provider that was not even contacted.
        const std::vector<std::string> chain = ManifestFailoverLogic::BuildChain(
            hasOverride ? std::string_view("custom") : state.active->name, state.failover, hasOverride);
        const bool fit = chain.size() > 1;

        const ManifestFailoverLogic::ChainResult result = ManifestFailoverLogic::RunChain(
            chain, g_breaker, ManifestFailoverLogic::kTotalBudget,
            [] { return FailoverClock::now(); },
            [&](const std::string& name, FailoverClock::time_point deadline) {
                // "custom" has no table row: the override URL is built from the state,
                // and the active provider only lends its (unused) templates and parser.
                const Provider* p = FindProvider(name);
                if (!p) p = state.active;
                const AttemptOutcome outcome = FetchFromProvider(*p, state, gid, appId, depotId, deadline, fit);
                if (!outcome.gotCode && fit && ManifestFailoverLogic::IsProviderDown(outcome.transportOk, outcome.status)) {
                    LOG_MANIFEST_WARN("Manifest provider {} is down (ok={} status={}); skipping it for {} min",
                                      name, outcome.transportOk, outcome.status,
                                      ManifestFailoverLogic::kQuarantine.count());
                }
                return outcome;
            });

        if (!result.ok) {
            if (fit)
                LOG_MANIFEST_WARN("Manifest gid={} no provider answered ({} asked)", gid, result.attempts);
            return false;
        }
        if (result.attempts > 1) {
            LOG_MANIFEST_INFO("Manifest gid={} answered by {} after {} attempts", gid, result.provider, result.attempts);
        }
        *outCode = result.code;
        return true;
    }

    // ── public ────────────────────────────────────────────────────

    bool FetchManifestRequestCode(uint64_t manifestGid, uint64_t* outRequestCode,
                                  AppId_t appId, AppId_t depotId)
    {
        // No lock here (v1.5.0): the Lua functions call http_get and the providers
        // are network calls; either can take seconds and must not queue other depots.
        if (appId && depotId && LuaConfig::HasManifestCodeFuncEx()) {
            if (LuaConfig::CallManifestFetchCodeEx(appId, depotId, manifestGid, outRequestCode)) {
                LOG_MANIFEST_INFO("Manifest gid={} resolved via fetch_manifest_code_ex", manifestGid);
                return true;
            }
            LOG_MANIFEST_WARN("Manifest gid={} fetch_manifest_code_ex returned nil, trying fetch_manifest_code", manifestGid);
        }

        if (LuaConfig::HasManifestCodeFunc()) {
            if (LuaConfig::CallManifestFetchCode(manifestGid, outRequestCode)) {
                LOG_MANIFEST_INFO("Manifest gid={} resolved via manifest.lua", manifestGid);
                return true;
            }
            LOG_MANIFEST_WARN("Manifest gid={} lua returned nil, falling back to config", manifestGid);
        }

        return FetchFromProviders(manifestGid, outRequestCode, appId, depotId);
    }
}
