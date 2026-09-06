#include "dllmain.h"
#include "Hook/HookManager.h"
#include "Utils/Config/Config.h"
#include "Utils/Config/ConfigMigration.h"
#include "Utils/Config/ConfigFileWatcher.h"
#include "Utils/Config/LuaFileWatcher.h"
#include "Utils/CloudRedirect/CloudRedirectHost.h"
#include "Utils/SteamMetadata/IPCLoader.h"
#include "Utils/SteamMetadata/PatternLoader.h"
#include "Utils/SteamMetadata/SteamDiagnostics.h"
#include "Utils/Tokeer/TokeerBridge.h"
#include "Utils/Update/AppUpdater.h"
#include "OSTPlatform/include/Dialog.h"
#include "OSTPlatform/include/DynamicLibrary.h"
#include "OSTPlatform/include/Encoding.h"
#include "OSTPlatform/include/Thread.h"

#include <cwchar>
#include <string>
#include <windows.h>

// prepare key runtime paths.
bool InitializeSteamComponents()
{
    const std::string steamInstallPath = OSTPlatform::DynamicLibrary::GetCurrentDirectoryPath();
    if (steamInstallPath.empty()) {
        return false;
    }
    sprintf_s(SteamInstallPath, kRuntimePathCapacity, "%s", steamInstallPath.c_str());
    sprintf_s(SteamclientPath, kRuntimePathCapacity, "%s\\steamclient64.dll",  SteamInstallPath);
    sprintf_s(SteamUIPath,     kRuntimePathCapacity, "%s\\steamui.dll",        SteamInstallPath);
    sprintf_s(DiversionPath,   kRuntimePathCapacity, "%s\\bin\\diversion.dll", SteamInstallPath);
    sprintf_s(LuaDir,          kRuntimePathCapacity, "%s\\config\\stplug-in",  SteamInstallPath);
    sprintf_s(ConfigPath,      kRuntimePathCapacity, "%s\\amethysttool.toml", SteamInstallPath);
    
    // SteamclientPath/SteamUIPath are UTF-8 (built from GetCurrentDirectoryPath()
    // above); Load() takes a std::filesystem::path, so decode via Utf8ToPath rather
    // than the ANSI-codepage path(std::string) constructor its implicit conversion
    // would otherwise use -- a non-ASCII Steam install path would otherwise fail to
    // load steamclient64.dll/steamui.dll at all.
    client_hModule = OSTPlatform::DynamicLibrary::Load(OSTPlatform::Encoding::Utf8ToPath(SteamclientPath));
    if (!client_hModule) {
        LOG_ERROR("Load steamclient64.dll failed: {} (err={})",
                  SteamclientPath, OSTPlatform::DynamicLibrary::GetLastErrorCode());
        return false;
    }
    LOG_INFO("Loaded steamclient64.dll from {}", SteamclientPath);

    ui_hModule = OSTPlatform::DynamicLibrary::Load(OSTPlatform::Encoding::Utf8ToPath(SteamUIPath));
    if(!ui_hModule) {
        LOG_ERROR("Load failed for steamui.dll: err={}", OSTPlatform::DynamicLibrary::GetLastErrorCode());
        return false;
    }
    return true;
}

// All initialisation that touches the filesystem, loads modules, scans
// memory, or installs detours runs here on a worker thread — we MUST NOT do
// any of that from inside DllMain (loader lock).
static uint32_t InitThread(OSTPlatform::DynamicLibrary::ModuleHandle selfModule) {
    Log::Init(selfModule);
    LOG_INFO("AmethystTool init thread started");

    if (!InitializeSteamComponents()) {
        LOG_ERROR("InitializeSteamComponents failed");
        return 1;
    }

    // Carry settings over from the pre-rename config name for users updating
    // from the upstream project (opensteamtool.toml -> amethysttool.toml).
    // One-time copy; never overwrites an existing amethysttool.toml.
    {
        const std::string legacyConfig =
            std::string(SteamInstallPath) + "\\opensteamtool.toml";
        // Both are UTF-8 (ConfigPath per InitializeSteamComponents above; legacyConfig
        // derived the same way from SteamInstallPath) -- decode via Utf8ToPath rather
        // than MigrateLegacyConfig's implicit ANSI-codepage path(std::string) conversion.
        if (Config::MigrateLegacyConfig(OSTPlatform::Encoding::Utf8ToPath(ConfigPath),
                                         OSTPlatform::Encoding::Utf8ToPath(legacyConfig)))
            LOG_INFO("Migrated config from opensteamtool.toml to amethysttool.toml");
    }

    Config::Load(ConfigPath);
    Log::InitModules();
    Log::InstallPlatformLogSink();
    SteamDiagnostics::Initialize(SteamclientPath, SteamUIPath);

    // Load pattern files for steamclient64.dll and steamui.dll.
    // Each call computes the SHA-256 of the DLL on disk, checks the local
    // cache, and downloads from GitHub if needed.  Both calls are synchronous
    // but run on this worker thread, never under the loader lock.
    PatternLoader::Load(ui_hModule, SteamUIPath, "steamui");
    PatternLoader::Load(client_hModule, SteamclientPath, "steamclient");

    // IPC method metadata (funcHash, fencepost, argc, ...)
    IPCLoader::Load(SteamclientPath);

    std::vector<std::string> watchDirs =
        LuaConfig::MergeWatchDirs(Config::GetLuaPaths(), std::string(LuaDir));
    for (const auto& dir : watchDirs)
        LuaConfig::ParseDirectory(dir);

    LuaFileWatcher::Start(watchDirs);
    ConfigFileWatcher::Start(ConfigPath, LuaDir);

    SteamUI::CoreHook();
    SteamClient::CoreHook();

    // Surface any functions that FindPattern() could not locate.
    PatternLoader::ReportMissingFunctions();

    // Optional Steam Cloud save redirection (CloudRedirect). No-op unless
    // [cloud].enabled is set and cloud_redirect.dll is present.
    CloudRedirectHost::Initialize(SteamInstallPath);

    // Register the amethysttool:// URI scheme so the website can drive code redemption via this
    // DLL (rundll32 handler). HKCU, no admin; idempotent. Only registered when the build
    // is configured with a Tokeer code server (OST_TOKEER_URL) — otherwise redemption is
    // inert (see TokeerBridge::Redeem) and registering the handler would just add attack
    // surface (a site-invocable protocol handler) with no corresponding feature enabled.
#if defined(OST_TOKEER_URL)
    TokeerBridge::RegisterUriScheme(std::string(SteamInstallPath) + "\\AmethystTool.dll");
#else
    LOG_INFO("OST_TOKEER_URL unset; amethysttool:// protocol handler not registered");
#endif

    // Self-update — DISABLED BY DEFAULT in the Amethyst fork (compiled out).
    //
    // The original path downloaded a replacement DLL and overwrote OpenSteamTool.dll
    // in place, then restarted Steam. Integrity was checked only against a SHA-256
    // fetched from the same channel that served the DLL, so whoever controlled a
    // mirror controlled the code that runs on the user's machine (RCE), and the fork
    // stayed tethered to the upstream project's infrastructure. Disabled endpoints
    // (see Utils/SteamMetadata/Mirror.cpp):
    //   - raw.githubusercontent.com/madoiscool/BetterSteamTools@updates/*
    //   - cdn.jsdelivr.net/gh/madoiscool/BetterSteamTools@updates/*
    //   - git.lua.tools/luatools/BetterSteamTools/.../updates/*
    //
    // Define OST_ENABLE_AUTOUPDATE at build time to restore the old behaviour.
#if defined(OST_ENABLE_AUTOUPDATE)
    if (Config::GetUpdateEnabled()) {
        OSTPlatform::Thread::StartDetached([] () -> uint32_t {
            const std::string self = std::string(SteamInstallPath) + "\\AmethystTool.dll";
            AppUpdater::CleanupStagedBackup(self);

            const AppUpdater::CheckResult upd = AppUpdater::Check();
            if (!upd.updateAvailable) return 0;
            if (!AppUpdater::DownloadAndStage(upd, self)) return 0;

            const bool restart = OSTPlatform::Dialog::ShowConfirm(
                "AmethystTool Updated!",
                upd.oldVersion + " -> " + upd.newVersion +
                "\n\nRestart Steam now to apply?");
            if (restart) AppUpdater::RestartSteam();
            return 0;
        });
    }
#endif
    // (When OST_ENABLE_AUTOUPDATE is undefined, Config::GetUpdateEnabled() is still
    //  parsed from the TOML for compatibility but has no effect — no self-update.)

    LOG_INFO("AmethystTool init complete");
    return 0;
}

// True only when the host process is steam.exe. The proxy DLLs already gate injection to
// Steam, but rundll32 loads this DLL directly to service an amethysttool:// link — there we must NOT
// run the Steam-injection machinery (steamclient load, hooks, watchers); the TokeerUri
// export does its work standalone.
static bool IsSteamHost()
{
    // GetModuleFileNameW (not the ANSI variant): the exe path can contain
    // non-ASCII bytes anywhere before the final component (a Windows profile
    // name, an install directory chosen by the user, ...) that the ANSI
    // codepage can misdecode — including, on DBCS codepages, mangling '\\'
    // detection itself if a lead byte happens to combine with 0x5C. Only the
    // final "steam.exe" segment is compared and it's pure ASCII either way,
    // but resolving the path in UTF-16 first avoids that class of bug entirely.
    wchar_t exePath[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, exePath, MAX_PATH)) return false;
    const wchar_t* name = wcsrchr(exePath, L'\\');
    name = name ? name + 1 : exePath;
    return _wcsicmp(name, L"steam.exe") == 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD dwReason, PVOID pvReserved)
{
    if (dwReason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hModule);
        if (!IsSteamHost())
            return TRUE;   // e.g. rundll32 amethysttool:// handler — no injection here
        // Hand off all real work to a worker thread to avoid running file I/O,
        // module loading and detour transactions under the loader lock.
        OSTPlatform::Thread::StartDetached([module = reinterpret_cast<OSTPlatform::DynamicLibrary::ModuleHandle>(hModule)] {
            return InitThread(module);
        });
    }
    else if (dwReason == DLL_PROCESS_DETACH && IsSteamHost())
    {
        ConfigFileWatcher::Stop();
        LuaFileWatcher::Stop();
        SteamUI::CoreUnhook();
        SteamClient::CoreUnhook();
        CloudRedirectHost::Shutdown();
    }

    return TRUE;
}
