#include "Hook/Hooks_Package.h"
#include "Utils/Config/Config.h"
#include "Utils/Config/LuaConfig.h"
#include "Utils/Config/LuaFileWatcher.h"
#include "Utils/Config/ConfigFileWatcher.h"
#include "Utils/Logging/Log.h"
#include "OSTPlatform/include/DirectoryWatch.h"
#include "OSTPlatform/include/Encoding.h"

#include <atomic>
#include <cctype>
#include <filesystem>
#include <string_view>
#include <thread>
#include <vector>

namespace ConfigFileWatcher {
namespace {

std::atomic<bool> g_running{false};
std::thread g_watcherThread;
std::string g_configPath;
std::string g_defaultLuaDir;

constexpr uint32_t kDebounceMs = 500;

bool SameFileName(std::string_view lhs, std::string_view rhs) {
    if (lhs.size() != rhs.size()) return false;
    for (size_t i = 0; i < lhs.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(lhs[i])) !=
            std::tolower(static_cast<unsigned char>(rhs[i]))) {
            return false;
        }
    }
    return true;
}

bool ContainsConfigChange(
    const std::vector<OSTPlatform::DirectoryWatch::Change>& changes,
    std::string_view targetFileName) {
    for (const auto& change : changes) {
        if (SameFileName(change.relativePath, targetFileName)) {
            return true;
        }
    }
    return false;
}

std::vector<std::string> BuildLuaWatchDirs() {
    return LuaConfig::MergeWatchDirs(Config::GetLuaPaths(), g_defaultLuaDir);
}

void RestartLuaWatcher() {
    std::vector<std::string> watchDirs = BuildLuaWatchDirs();

    LuaFileWatcher::Stop();
    LuaConfig::ReloadDirectories(watchDirs);
    LuaFileWatcher::Start(watchDirs);

    Hooks_Package::NotifyLicenseChanged();
    LOG_INFO("Lua directories refreshed after config reload: {}", static_cast<uint32_t>(watchDirs.size()));
}

void ReloadConfig() {
    LOG_INFO("Reloading config: {}", g_configPath);

    const Config::LoadResult result = Config::Load(g_configPath);
    if (!result.applied) {
        LOG_WARN("Config reload skipped; keeping previous valid config");
        return;
    }

    Log::ApplyConfigLevel();

    if (result.luaPathsChanged) {
        RestartLuaWatcher();
    }
}

void WatcherThread() {
    // g_configPath is UTF-8 (see Start(), sourced from dllmain.cpp's ConfigPath);
    // decode via Utf8ToPath rather than the ANSI-codepage path(std::string)
    // constructor -- a non-ASCII Steam install path would otherwise resolve to
    // the wrong watch directory.
    const std::filesystem::path configPath = OSTPlatform::Encoding::Utf8ToPath(g_configPath);
    const std::filesystem::path dirPath = configPath.parent_path();
    // "amethysttool.toml" is a pure-ASCII literal, so narrowing it via .string() is
    // safe regardless of codepage, unlike dirPath below.
    const std::string targetFileName = configPath.filename().string();

    // watch.Open expects UTF-8 (it converts via Encoding::Utf8ToWide internally);
    // PathToUtf8, not .string() (ANSI codepage), keeps this consistent with the
    // decode above.
    const std::string dirPathUtf8 = OSTPlatform::Encoding::PathToUtf8(dirPath);
    OSTPlatform::DirectoryWatch::Watch watch;
    if (!watch.Open(dirPathUtf8, 4096)) {
        LOG_WARN("Failed to open config watch directory: {}", dirPathUtf8);
        return;
    }
    if (!watch.IssueRead()) {
        return;
    }

    LOG_INFO("Watching config file: {}", g_configPath);

    OSTPlatform::DirectoryWatch::Watch* watchPtr = &watch;
    std::vector<OSTPlatform::DirectoryWatch::Watch*> watches{watchPtr};

    auto drainEvent = [&]() {
        bool changed = ContainsConfigChange(watch.Drain(), targetFileName);
        watch.IssueRead();
        return changed;
    };

    while (g_running) {
        auto waitResult = OSTPlatform::DirectoryWatch::WaitAny(watches, 1000);

        if (!g_running) break;
        if (waitResult.status == OSTPlatform::DirectoryWatch::WaitStatus::Timeout) continue;
        if (waitResult.status != OSTPlatform::DirectoryWatch::WaitStatus::Signaled) continue;

        bool changed = drainEvent();
        while (g_running) {
            auto debounceResult = OSTPlatform::DirectoryWatch::WaitAny(watches, kDebounceMs);
            if (!g_running) break;
            if (debounceResult.status == OSTPlatform::DirectoryWatch::WaitStatus::Timeout) break;
            if (debounceResult.status != OSTPlatform::DirectoryWatch::WaitStatus::Signaled) break;
            changed = drainEvent() || changed;
        }

        if (changed) {
            ReloadConfig();
        }
    }

    watch.Cancel();
    LOG_INFO("Config watcher stopped");
}

} // namespace

void Start(const std::string& configPath, const std::string& defaultLuaDir) {
    if (g_running.exchange(true)) {
        LOG_WARN("Config watcher already running");
        return;
    }

    g_configPath = configPath;
    g_defaultLuaDir = defaultLuaDir;
    g_watcherThread = std::thread(WatcherThread);
}

void Stop() {
    if (!g_running) return;
    g_running = false;
    if (g_watcherThread.joinable()) {
        g_watcherThread.join();
    }
}

} // namespace ConfigFileWatcher
