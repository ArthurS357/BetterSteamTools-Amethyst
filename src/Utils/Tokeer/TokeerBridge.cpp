#include "TokeerBridge.h"

#include "OSTPlatform/include/Dialog.h"
#include "OSTPlatform/include/DynamicLibrary.h"
#include "OSTPlatform/include/Encoding.h"
#include "OSTPlatform/include/Http.h"
#include "OSTPlatform/include/Numbers.h"
#include "OSTPlatform/include/SteamCredentialStore.h"
#include "Utils/Logging/Log.h"
#include "Utils/Tokeer/TokeerParsing.h"

#include <windows.h>
#include <shellapi.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

// Code-server base URL, baked in at build time. Empty by default (Amethyst fork) so
// the fork ships no upstream deployment's backend and stays independent; point a build
// at your own server with:
//   cmake -B build -DOST_TOKEER_URL="https://your-host"
// When empty, the amethysttool:// redeem action is inert (no network request is made).
#ifndef OST_TOKEER_URL
#define OST_TOKEER_URL ""
#endif

namespace TokeerBridge {

namespace {

    using Parsing::HexToBytes;
    using Parsing::JsonString;
    using Parsing::JsonTrue;

    std::string TokeerBaseUrl() {
        std::string url = OST_TOKEER_URL;
        while (!url.empty() && url.back() == '/') url.pop_back();
        return url;
    }

    void Warn(const std::string& title, const std::string& msg) {
        OSTPlatform::Dialog::ShowWarning(title, msg);
    }

    // This DLL lives in the Steam root, so its own directory is the Steam dir.
    // (The rundll32 URI context never runs InitializeSteamComponents, so the
    // SteamInstallPath global is not set here -- resolve from the module.)
    std::filesystem::path SteamDirFromSelf() {
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&SteamDirFromSelf), &self);
        if (!self) return {};
        return OSTPlatform::DynamicLibrary::GetModuleDirectory(
            reinterpret_cast<OSTPlatform::DynamicLibrary::ModuleHandle>(self));
    }

    // Delete cached achievement schemas so Steam re-fetches the current one
    // (picking up newly-added achievements and the sets/groups layout). The
    // target was already validated by ParseRefreshAchievementsArg, so the appid
    // is a plain integer and cannot steer the delete outside appcache\stats.
    // User unlock state (UserGameStats_*, a separate file) is never touched.
    void RefreshAchievements(const Parsing::RefreshTarget& target) {
        namespace fs = std::filesystem;
        constexpr std::wstring_view kSchemaPrefix = L"UserGameStatsSchema_";

        // An empty Steam dir must abort: "" / "appcache" / "stats" is a path
        // relative to rundll32's working directory, not to Steam.
        const fs::path steamDir = SteamDirFromSelf();
        std::error_code ec;
        const fs::path statsDir = steamDir / L"appcache" / L"stats";
        if (steamDir.empty() || !fs::is_directory(statsDir, ec)) {
            Warn("AmethystTool", "Could not locate Steam's stats cache.");
            return;
        }

        // "all" is the broad delete and this entry point is link-invocable, so
        // make the user say yes; a single app is narrow and goes straight through.
        if (target.all && !OSTPlatform::Dialog::ShowConfirm(
                "Refresh Achievements",
                "Clear the cached achievement schema of EVERY game?\n\n"
                "Steam re-downloads them on demand. Your unlocks are untouched."))
            return;

        size_t removed = 0;
        std::error_code rmEc;
        if (target.all) {
            for (fs::directory_iterator it(statsDir, ec), end; !ec && it != end; it.increment(ec)) {
                const std::wstring name = it->path().filename().wstring();
                if (name.rfind(kSchemaPrefix, 0) == 0 && it->path().extension() == L".bin" &&
                    it->is_regular_file(rmEc) && fs::remove(it->path(), rmEc))
                    ++removed;
            }
        } else {
            const fs::path f = statsDir / (std::wstring(kSchemaPrefix) + std::to_wstring(target.appId) + L".bin");
            if (fs::remove(f, rmEc)) ++removed;
        }

        LOG_INFO("TokeerBridge: RefreshAchievements({}) removed {} schema file(s)",
                 target.all ? std::string("all") : std::to_string(target.appId), removed);
        OSTPlatform::Dialog::ShowInfo(
            "Achievements Refreshed",
            std::to_string(removed) + " achievement schema(s) cleared.\n\n"
            "Restart Steam, then open the game's achievement page to pull the "
            "latest (including sets/groups). A running Steam keeps the old schema "
            "in memory until restarted. Your unlocks are untouched.");
    }

} // namespace

void Redeem(const std::string& code) {
    namespace CS = OSTPlatform::SteamCredentialStore;

    const std::string base = TokeerBaseUrl();
    if (base.empty()) {
        // No code server configured in this build (default for the Amethyst fork).
        // Do not fabricate a request against a relative URL — the feature is off.
        LOG_INFO("TokeerBridge: redeem requested but OST_TOKEER_URL is unset; feature disabled");
        Warn("AmethystTool", "Code redemption is not configured in this build.");
        return;
    }

    const std::string url = base + "/drm/redeem";
    const std::string reqBody = "{\"code\":\"" + code + "\"}";

    OSTPlatform::Http::Result r = OSTPlatform::Http::Execute(
        L"POST", url.c_str(), reqBody.data(), static_cast<uint32_t>(reqBody.size()),
        L"Content-Type: application/json\r\n",
        5000, 5000, 10000, 20000, 4u * 1024 * 1024);

    if (!r.ok || r.status != 200 || !JsonTrue(r.body, "success")) {
        std::string reason;
        if (!JsonString(r.body, "reason", reason) && !JsonString(r.body, "error", reason))
            reason = "Server error " + std::to_string(r.status);
        LOG_WARN("TokeerBridge: redeem failed (HTTP {}): {}", r.status, reason);
        Warn("AmethystTool", "Redeem failed:\n\n" + reason);
        return;
    }

    std::string appIdStr, appHex, etHex;
    JsonString(r.body, "app_id", appIdStr);
    JsonString(r.body, "appticket", appHex);
    JsonString(r.body, "eticket", etHex);

    const auto appId = OSTPlatform::Numbers::ParseUInt32(appIdStr);
    const auto appBytes = HexToBytes(appHex);
    const auto etBytes = HexToBytes(etHex);
    if (!appId || !appBytes || appBytes->empty() || !etBytes || etBytes->empty()) {
        LOG_WARN("TokeerBridge: redeem returned an incomplete/invalid ticket");
        Warn("AmethystTool", "The server returned an incomplete ticket.");
        return;
    }

    if (CS::WriteAppTicket(*appId, *appBytes) != CS::Status::Ok ||
        CS::WriteETicket(*appId, *etBytes) != CS::Status::Ok) {
        LOG_ERROR("TokeerBridge: failed to write tickets for app {}", *appId);
        Warn("AmethystTool", "Could not write the ticket to Steam.");
        return;
    }

    LOG_INFO("TokeerBridge: redeemed code for app {} ({} + {} bytes)",
             *appId, appBytes->size(), etBytes->size());

    const bool launchNow = OSTPlatform::Dialog::ShowConfirm(
        "Code Redeemed!",
        "Added app " + std::to_string(*appId) + " to your library.\n\nLaunch it now?");
    if (launchNow) {
        const std::string steamUrl = "steam://rungameid/" + std::to_string(*appId);
        ShellExecuteA(nullptr, "open", steamUrl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        LOG_INFO("TokeerBridge: launching app {} via {}", *appId, steamUrl);
    }
}

void HandleUri(const std::string& rawUrl) {
    LOG_INFO("TokeerBridge: HandleUri raw='{}'", rawUrl);

    // rundll32 does its own lightweight command-line parsing and does NOT strip quotes
    // from the trailing argument the way a normal EXE's argv would — since the registry
    // command wraps %1 in quotes ("...",TokeerUri "%1"), the literal quote characters
    // land inside lpszCmdLine. Parsing::ParseUri strips a single wrapping pair (either
    // quote style) plus incidental whitespace before checking the scheme.
    const Parsing::ParsedUri parsed = Parsing::ParseUri(rawUrl);
    if (!parsed.matchedScheme) {
        LOG_WARN("TokeerBridge: ignoring non-{} URL (cleaned='{}')", Parsing::kUriScheme, parsed.cleaned);
        return;
    }

    if (parsed.action == "redeem") {
        if (!parsed.arg.empty()) Redeem(parsed.arg);
        else Warn("AmethystTool", "Missing code in link.");
    } else if (parsed.action == "refreshachievements") {
        const auto target = Parsing::ParseRefreshAchievementsArg(parsed.arg);
        if (!target) {
            LOG_WARN("TokeerBridge: refreshachievements: rejected argument '{}'", parsed.arg);
            Warn("AmethystTool", "Invalid refreshachievements link: expected an app id or 'all'.");
            return;
        }
        RefreshAchievements(*target);
    } else {
        LOG_WARN("TokeerBridge: unknown action '{}'", parsed.action);
    }
}

void RegisterUriScheme(const std::string& dllPath) {
    // HKCU\Software\Classes\amethysttool  (URL Protocol) ; \shell\open\command -> rundll32 handler.
    const std::string command =
        "rundll32.exe \"" + dllPath + "\",TokeerUri \"%1\"";

    // dllPath (and therefore command) is UTF-8 (built from SteamInstallPath in
    // dllmain.cpp). RegCreateKeyExA/RegSetValueExA decode narrow strings via the
    // ANSI codepage, not UTF-8 -- a non-ASCII Steam install path would register
    // the wrong rundll32 target, so opening an amethysttool:// link would silently
    // fail to find this DLL. Use the W (wide) Registry APIs with an explicit
    // Utf8ToWide decode instead; only reachable when OST_TOKEER_URL is set at
    // build time, so this has no effect on the default build.
    auto writeKey = [](const wchar_t* sub, const wchar_t* valueName, const std::wstring& value) -> bool {
        HKEY key{};
        if (RegCreateKeyExW(HKEY_CURRENT_USER, sub, 0, nullptr, 0,
                            KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
            return false;
        const LSTATUS s = RegSetValueExW(key, valueName, 0, REG_SZ,
                                         reinterpret_cast<const BYTE*>(value.c_str()),
                                         static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(key);
        return s == ERROR_SUCCESS;
    };

    const std::wstring commandW = OSTPlatform::Encoding::Utf8ToWide(command);

    const bool ok =
        writeKey(L"Software\\Classes\\amethysttool", nullptr, L"URL:AmethystTool") &&
        writeKey(L"Software\\Classes\\amethysttool", L"URL Protocol", L"") &&
        writeKey(L"Software\\Classes\\amethysttool\\shell\\open\\command", nullptr, commandW);

    if (ok) LOG_INFO("TokeerBridge: registered amethysttool:// scheme -> {}", command);
    else    LOG_WARN("TokeerBridge: failed to register amethysttool:// scheme");
}

} // namespace TokeerBridge
