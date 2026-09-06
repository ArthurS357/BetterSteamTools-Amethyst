#include "ManifestClient.h"
#include "OSTPlatform/include/Http.h"
#include "Utils/Config/Config.h"
#include "Utils/Config/LuaConfig.h"
#include "Utils/Logging/Log.h"

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
        const char*      urlTemplate;   // full literal with one %llu — for log & path
        Parser           parse;
    };

    consteval Provider Make(std::string_view name, const char* url, Parser parse) {
        return {name, url, parse};
    }

    static constexpr Provider kProviders[] = {
        Make("opensteamtool", "https://manifest.opensteamtool.com/%llu",       ParsePlainUint),
        Make("wudrm",         "http://gmrc.wudrm.com/manifest/%llu",           ParsePlainUint),
        Make("steamrun",      "https://manifest.steam.run/api/manifest/%llu",  ParseSteamRunJson),
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

    static bool HasGidPlaceholder(std::string_view urlTemplate) {
        return urlTemplate.find("{gid}") != std::string_view::npos;
    }

    static void ReplaceAll(std::string& text, std::string_view from, std::string_view to) {
        size_t pos = 0;
        while ((pos = text.find(from, pos)) != std::string::npos) {
            text.replace(pos, from.size(), to);
            pos += to.size();
        }
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

    static bool FetchActive(uint64_t gid, uint64_t* outCode) {
        // Called with g_mutex already held by FetchManifestRequestCode below, so
        // g_urlTemplateOverride/g_active are read without a nested lock.
        const Config::ManifestTimeouts timeouts = Config::GetManifestTimeouts();

        char urlLog[256];
        Parser parse = nullptr;
        // Only consumed by LOG_MANIFEST_INFO below, which is a no-op in Release
        // (see the OPENSTEAMTOOL_LOGGING_ENABLED comment in CMakeLists.txt) --
        // maybe_unused avoids an unused-but-set-variable warning in that config.
        [[maybe_unused]] const char* providerName = nullptr;

        if (!g_urlTemplateOverride.empty()) {
            std::string url = g_urlTemplateOverride;
            ReplaceAll(url, "{gid}", std::to_string(gid));
            const int written = std::snprintf(urlLog, sizeof(urlLog), "%s", url.c_str());
            if (written < 0 || static_cast<size_t>(written) >= sizeof(urlLog)) {
                LOG_MANIFEST_WARN("manifest.url_template expansion too long, truncated: {}", url);
            }
            parse = ParsePlainUint;
            providerName = "custom";
        } else {
            const Provider& p = *g_active;
            // p.urlTemplate is one of the 3 short, fixed kProviders literals above --
            // never long enough to truncate against urlLog's 256 bytes -- but check
            // the same way as the url_template branch above for consistency.
            const int written = std::snprintf(urlLog, sizeof(urlLog), p.urlTemplate, gid);
            if (written < 0 || static_cast<size_t>(written) >= sizeof(urlLog)) {
                LOG_MANIFEST_WARN("manifest provider URL unexpectedly long, truncated: {}", p.name);
            }
            parse = p.parse;
            providerName = p.name.data();
        }

        auto r = OSTPlatform::Http::Execute(
            L"GET",
            urlLog,
            nullptr,
            0,
            nullptr,
            timeouts.resolve,
            timeouts.connect,
            timeouts.send,
            timeouts.recv);

        LOG_MANIFEST_INFO("Manifest {} status={} gid={}", providerName, r.status, gid);

        if (!r.ok || r.status != 200) return false;
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

        return FetchActive(manifestGid, outRequestCode);
    }
}
