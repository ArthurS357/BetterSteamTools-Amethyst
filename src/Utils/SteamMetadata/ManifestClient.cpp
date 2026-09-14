#include "ManifestClient.h"
#include "OSTPlatform/include/Http.h"
#include "Utils/Config/Config.h"
#include "Utils/Config/LuaConfig.h"
#include "Utils/Logging/Log.h"
#include "Utils/SteamMetadata/ManifestUrlLogic.h"

#include <algorithm>
#include <charconv>
#include <mutex>
#include <string>
#include <string_view>

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
        Make("opensteamtool", "https://manifest.opensteamtool.com/%llu",
                              "https://manifest.opensteamtool.com/%u/%u/%llu",  ParsePlainUint),
        Make("wudrm",         "http://gmrc.wudrm.com/manifest/%llu",
                              nullptr,                                          ParsePlainUint),
        Make("steamrun",      "https://manifest.steam.run/api/manifest/%llu",
                              nullptr,                                          ParseSteamRunJson),
    };

    static const Provider* g_active = &kProviders[0];   // opensteamtool
    static std::mutex      g_mutex;

    // Empty = no override, use g_active (see SetUrlTemplateOverride). Only ever
    // read/written under g_mutex.
    static std::string g_urlTemplateOverride;

    bool SetProvider(std::string_view name) {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (const auto& p : kProviders)
            if (p.name == name) {
                g_active = &p;
                return true;
            }
        return false;
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

    // ── request ───────────────────────────────────────────────────

    void Shutdown() {
        std::lock_guard<std::mutex> lock(g_mutex);
    }

    // ── fetch ─────────────────────────────────────────────────────

    constexpr int kHttpOk = 200;

    static bool FetchActive(uint64_t gid, uint64_t* outCode, AppId_t appId, AppId_t depotId) {
        // Called with g_mutex already held by FetchManifestRequestCode below, so
        // g_urlTemplateOverride/g_active are read without a nested lock.
        const Config::ManifestTimeouts timeouts = Config::GetManifestTimeouts();
        const Provider& p = *g_active;

        // Override (if set) short-circuits provider selection entirely --
        // ManifestUrlLogic::Build never falls back to a provider template when
        // g_urlTemplateOverride is non-empty. Depot-aware-vs-gid-only for the
        // provider path is likewise decided inside Build(), not here.
        const ManifestUrlLogic::Choice choice = ManifestUrlLogic::Build(
            g_urlTemplateOverride, p.urlTemplate, p.urlTemplateEx, appId, depotId, gid);

        Parser parse = p.parse;
        // Only consumed by LOG_MANIFEST_INFO below, which is a no-op in Release
        // (see the OPENSTEAMTOOL_LOGGING_ENABLED comment in CMakeLists.txt) --
        // maybe_unused avoids an unused-but-set-variable warning in that config.
        [[maybe_unused]] const char* providerName = p.name.data();
        [[maybe_unused]] const char* shape = "gid-only";

        if (choice.shape == ManifestUrlLogic::Shape::Override) {
            parse = ParsePlainUint;
            providerName = "custom";
            shape = "override";
            // Choice::truncated is always false for Override (see Build()) --
            // nothing to warn about here.
        } else {
            shape = (choice.shape == ManifestUrlLogic::Shape::DepotAware) ? "app/depot/gid" : "gid-only";
            if (choice.truncated) {
                LOG_MANIFEST_WARN("manifest provider URL unexpectedly long, truncated: {}", p.name);
            }
        }

        auto r = OSTPlatform::Http::Execute(
            L"GET",
            choice.url.c_str(),
            nullptr,
            0,
            nullptr,
            timeouts.resolve,
            timeouts.connect,
            timeouts.send,
            timeouts.recv);

        // Log which shape was used: a gid-only request for a non-carrier depot
        // yields a code the CDN will reject, and that is otherwise invisible
        // until the download fails.
        LOG_MANIFEST_INFO("Manifest {} status={} gid={} depot={} shape={}",
                          providerName, r.status, gid, depotId, shape);

        // Single fallback retry: a depot-aware request that failed (transport
        // error, non-200 -- includes a WAF/route rejection a provider server
        // does not recognize yet) gets one retry against the plain gid-only
        // URL before giving up. Never applies to Override (the user's own
        // url_template is reported as failed, not silently swapped for
        // something else) or to GidOnly (nothing simpler to fall back to).
        // depotId=0 reuses Build()'s own "unknown depot" rule to force
        // Shape::GidOnly rather than duplicating that decision here.
        if (ManifestUrlLogic::ShouldFallbackToGidOnly(choice.shape, !r.ok || r.status != kHttpOk)) {
            LOG_MANIFEST_DEBUG("Manifest {} depot-aware failed (status={}), retrying gid-only for gid={}",
                               providerName, r.status, gid);

            const ManifestUrlLogic::Choice fallback = ManifestUrlLogic::Build(
                g_urlTemplateOverride, p.urlTemplate, p.urlTemplateEx, appId, /*depotId=*/0, gid);

            r = OSTPlatform::Http::Execute(
                L"GET",
                fallback.url.c_str(),
                nullptr,
                0,
                nullptr,
                timeouts.resolve,
                timeouts.connect,
                timeouts.send,
                timeouts.recv);

            LOG_MANIFEST_INFO("Manifest {} status={} gid={} depot={} shape=gid-only (fallback)",
                              providerName, r.status, gid, depotId);
        }

        if (!r.ok || r.status != kHttpOk) return false;
        return parse(r.body, outCode);
    }

    // ── public ────────────────────────────────────────────────────

    bool FetchManifestRequestCode(uint64_t manifestGid, uint64_t* outRequestCode,
                                  AppId_t appId, AppId_t depotId)
    {
        std::lock_guard<std::mutex> lock(g_mutex);

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

        return FetchActive(manifestGid, outRequestCode, appId, depotId);
    }
}
