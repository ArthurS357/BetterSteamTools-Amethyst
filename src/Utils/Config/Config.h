#pragma once

#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

#include "Steam/Types.h"

namespace Config {

    enum class LogLevel { Trace, Debug, Info, Warn, Error };

    struct ManifestTimeouts {
        uint32_t resolve = 5000;
        uint32_t connect = 5000;
        uint32_t send    = 10000;
        uint32_t recv    = 10000;
    };

    // [[inject]] entry: a DLL loaded into a matching game process at the IPC handshake.
    struct InjectDll {
        std::string                 path;        // resolved absolute path
        std::string                 whenCmdline; // substring required in the game command line
        std::unordered_set<AppId_t> whenAppids;  // appids this entry applies to
        bool                        allGames = false;  // false: only Lua-unlocked games
    };

    struct CloudSettings {
        bool enabled = false;
        std::string library;
    };

    // [cache] — optional pre-seed of <steam>\depotcache from a manifest
    // archive. Empty url (the default) means the feature is fully off.
    struct CacheSettings {
        std::string url;
    };

    // [donate] — contribute manifest request codes for depots this account
    // owns, to fill gaps the backend's own sessions can't reach. Amethyst
    // fork default: enabled=false (opt-IN, unlike upstream's opt-out) —
    // this is the only feature in the tool that sends data (depot id, gid,
    // a short-lived manifest request code) to a third party, so it stays
    // off until the user explicitly turns it on. See ManifestDonor.h.
    struct DonateSettings {
        bool        enabled  = false;
        std::string url;                       // base; empty = built-in default
        uint32_t    intervalSecs        = 30;
        uint32_t    maxMintsPerCycle    = 25;
        uint32_t    minMintIntervalMs   = 2000;
        uint32_t    maxMintsPerSession  = 0;     // 0 = unlimited (mint all session)
        uint32_t    wantedRefreshSecs   = 300;   // re-pull the (large) wanted list only this often; minting still runs every intervalSecs
    };

    struct LoadResult {
        bool applied = false;
        bool luaPathsChanged = false;
    };

    LoadResult Load(const std::string& configPath);

    ManifestTimeouts GetManifestTimeouts();
    LogLevel GetLogLevel();
    std::string GetLogDir();
    std::vector<std::string> GetLuaPaths();
    std::vector<std::string> GetRemoteUrlTemplates();
    std::string GetHttpUserAgent();
    CloudSettings GetCloudSettings();
    CacheSettings GetCacheSettings();
    DonateSettings GetDonateSettings();
    bool GetStatsEnableApi();
    bool GetUpdateEnabled();

    // [manifest] — provider selection lives in ManifestClient (table-driven).
    inline uint32_t manifestTimeoutResolve = 5000;
    inline uint32_t manifestTimeoutConnect = 5000;
    inline uint32_t manifestTimeoutSend    = 10000;
    inline uint32_t manifestTimeoutRecv    = 10000;

    // [log]
    inline LogLevel logLevel = LogLevel::Debug;

    // derived from configPath: <steam>/amethysttool/
    inline std::string logDir;

    // [lua]
    inline std::vector<std::string> luaPaths;

    // [remote] — one or more mirror templates, tried in order. Empty = built-in defaults.
    inline std::vector<std::string> remoteUrlTemplates;

    // [http] — empty = OSTPlatform::Http::kDefaultUserAgent ("OpenSteamTool/1.0",
    // the value manifest.opensteamtool.com's WAF allowlists).
    inline std::string httpUserAgent;

    // [stats]
    inline bool statsEnableApi = true;

    // [update] - self-update check on startup (staged for next Steam launch).
    inline bool updateEnabled = true;

    // [[inject]] - optional DLL injection into matching game processes.
    inline std::vector<InjectDll> injectDlls;

    // [cloud] - optional Steam Cloud save redirection via CloudRedirect.
    inline bool cloudEnabled = false;
    inline std::string cloudLibrary;

    // [cache] - empty (default) disables ManifestCache entirely: no request
    // is ever made. A non-empty base URL opts in to pre-seeding <steam>\
    // depotcache from that archive.
    inline std::string cacheUrl;

    // [donate] - opt-in mint-and-submit of manifest request codes for depots
    // this account owns. enabled=false (default) means zero network activity
    // from this feature: no wanted-list GET, no passive capture, no submit
    // POST. See DonateSettings above and ManifestDonor.h.
    inline DonateSettings donate;

}
