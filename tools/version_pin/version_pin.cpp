// version_pin -- freeze the installed version of a game by writing setManifestid
// calls for its installed depots. Offline: reads appmanifest_<appid>.acf and writes
// <steam>\config\stplug-in\<appid>.pin.lua. No hook, no network, not part of the DLL.
// All the logic lives in Utils/SteamMetadata/VersionPinLogic.h (unit-tested).

#include "Utils/SteamMetadata/VersionPinLogic.h"

#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <process.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using namespace VersionPinLogic;

namespace {

    constexpr int kExitOk = 0;
    constexpr int kExitUsage = 1;
    constexpr int kExitFailed = 2;   // at least one app failed or was refused

    void PrintUsage(std::ostream& out) {
        out <<
            "version_pin - freeze the installed version of a game (setManifestid)\n"
            "\n"
            "Usage: version_pin (--appid <id> | --all) [--steam-dir <dir>] [--steamapps <dir>]\n"
            "                   [--out-dir <dir>] [--force] [--dry-run]\n"
            "\n"
            "  --appid <id>       Pin one app.\n"
            "  --all              Pin every appmanifest_*.acf. Required for that: freezing every game\n"
            "                     at once must be asked for, never the result of running with no arguments.\n"
            "  --steam-dir <dir>  Steam install. Default: read from the registry (Windows).\n"
            "  --steamapps <dir>  Library to read, for games installed outside the Steam folder.\n"
            "                     Default: <steam-dir>\\steamapps\n"
            "  --out-dir <dir>    Where <appid>.pin.lua goes. Default: <steam-dir>\\config\\stplug-in\n"
            "  --force            Replace a pin file that already exists.\n"
            "  --dry-run          Print what would be written; change nothing.\n"
            "\n"
            "Writes <appid>.pin.lua with one setManifestid call per installed depot. If another .lua\n"
            "there pins the same depot differently, a warning says so (the file parsed last wins).\n"
            "Delete the .pin.lua to follow Steam's updates again.\n";
    }

    struct Args {
        std::optional<fs::path> steamDir;
        std::optional<fs::path> steamapps;
        std::optional<fs::path> outDir;
        std::optional<std::uint32_t> appId;
        bool all = false;
        bool force = false;
        bool dryRun = false;
        bool help = false;
    };

    // Flags are ASCII, so the lossy narrowing of a non-ASCII argument is harmless
    // there; values stay fs::path and are never narrowed.
    std::string Narrow(const fs::path::value_type* arg) { return fs::path(arg).string(); }

    // Returns an error message, or empty when the arguments are fine.
    std::string ParseArgs(int argc, fs::path::value_type** argv, Args& a) {
        for (int i = 1; i < argc; ++i) {
            const std::string flag = Narrow(argv[i]);
            // A value that looks like a flag is a forgotten value (`--out-dir --force`), not a path.
            const auto value = [&]() -> const fs::path::value_type* {
                return i + 1 < argc && Narrow(argv[i + 1]).rfind("--", 0) != 0 ? argv[++i] : nullptr;
            };
            if (flag == "--force")        a.force = true;
            else if (flag == "--all")     a.all = true;
            else if (flag == "--dry-run") a.dryRun = true;
            else if (flag == "--help" || flag == "-h") a.help = true;
            else if (flag == "--steam-dir" || flag == "--steamapps" || flag == "--out-dir" || flag == "--appid") {
                const auto* v = value();
                if (!v) return flag + " needs a value";
                if (flag == "--steam-dir")       a.steamDir = fs::path(v);
                else if (flag == "--steamapps")  a.steamapps = fs::path(v);
                else if (flag == "--out-dir")    a.outDir = fs::path(v);
                else {
                    a.appId = ParseDecimal<std::uint32_t>(Narrow(v));
                    if (!a.appId || *a.appId == 0) return "--appid must be a positive number";
                }
            } else {
                return "unknown argument: " + flag;
            }
        }
        return {};
    }

#ifdef _WIN32
    std::optional<fs::path> RegistryString(HKEY root, const wchar_t* subKey, const wchar_t* name) {
        DWORD type = 0, bytes = 0;
        if (RegGetValueW(root, subKey, name, RRF_RT_REG_SZ, &type, nullptr, &bytes) != ERROR_SUCCESS || bytes < sizeof(wchar_t))
            return std::nullopt;
        std::wstring value(bytes / sizeof(wchar_t), L'\0');
        if (RegGetValueW(root, subKey, name, RRF_RT_REG_SZ, &type, value.data(), &bytes) != ERROR_SUCCESS)
            return std::nullopt;
        value.resize(wcsnlen(value.c_str(), value.size()));
        if (value.empty()) return std::nullopt;   // an empty path would resolve against the current directory
        return fs::path(value);
    }

    // HKCU's SteamPath is what Steam itself keeps current; the machine-wide key is the fallback.
    std::optional<fs::path> FindSteamDirInRegistry() {
        if (auto p = RegistryString(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath")) return p;
        return RegistryString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath");
    }

    std::string ProcessTag() { return std::to_string(_getpid()); }
#else
    std::optional<fs::path> FindSteamDirInRegistry() { return std::nullopt; }
    std::string ProcessTag() { return std::to_string(getpid()); }
#endif

    struct Paths {
        fs::path steamapps;
        fs::path outDir;
    };

    // Resolves the two directories, or explains why it cannot.
    std::optional<Paths> ResolvePaths(const Args& a, std::string& error) {
        std::optional<fs::path> steam = a.steamDir;
        if (!steam && (!a.steamapps || !a.outDir)) {
            steam = FindSteamDirInRegistry();
            if (!steam) {
                error = "cannot find Steam: pass --steam-dir <dir> (or both --steamapps and --out-dir)";
                return std::nullopt;
            }
        }
        if (steam) *steam = steam->lexically_normal();   // the registry value uses '/'
        Paths p;
        p.steamapps = a.steamapps ? *a.steamapps : *steam / "steamapps";
        p.outDir = a.outDir ? *a.outDir : *steam / "config" / "stplug-in";

        std::error_code ec;
        if (!fs::is_directory(p.steamapps, ec)) {
            error = "steamapps directory not found: " + p.steamapps.string() + " (is --steam-dir the Steam folder?)";
            return std::nullopt;
        }
        return p;
    }

    const char* StatusLabel(PinStatus s) {
        switch (s) {
        case PinStatus::Written:   return "pinned";
        case PinStatus::UpToDate:  return "up to date";
        case PinStatus::DryRun:    return "dry run";
        case PinStatus::Refused:   return "refused";
        case PinStatus::Failed:    return "FAILED";
        }
        return "?";
    }

    void PrintReport(const fs::path& acf, const PinReport& r) {
        std::cout << "[" << StatusLabel(r.status) << "] ";
        if (r.appId) std::cout << "app " << r.appId << " (" << r.depots << " depots): ";
        else         std::cout << acf.filename().string() << ": ";
        std::cout << r.message << "\n";
        for (const std::string& w : r.warnings) std::cout << "    warning: " << w << "\n";
        if (r.status == PinStatus::DryRun) std::cout << "--- " << r.file.string() << " ---\n" << r.lua;
        else if (r.status == PinStatus::Written) std::cout << "    " << r.file.string() << "\n";
    }

    std::vector<fs::path> SelectAcfFiles(const Args& a, const fs::path& steamapps, std::string& error) {
        if (a.appId) {
            const fs::path acf = steamapps / ("appmanifest_" + std::to_string(*a.appId) + ".acf");
            std::error_code ec;
            if (!fs::is_regular_file(acf, ec)) {
                error = acf.filename().string() + " not found in " + steamapps.string() +
                        " (a game installed in another library needs --steamapps <that library>\\steamapps)";
                return {};
            }
            return { acf };
        }
        std::error_code ec;
        std::vector<fs::path> files = ListAcfFiles(steamapps, ec);
        if (ec) error = "cannot read " + steamapps.string() + ": " + ec.message();
        else if (files.empty()) error = "no appmanifest_*.acf in " + steamapps.string();
        return files;
    }

    int Run(const Args& a) {
        std::string error;
        const auto paths = ResolvePaths(a, error);
        if (!paths) { std::cerr << "error: " << error << "\n"; return kExitFailed; }

        const std::vector<fs::path> files = SelectAcfFiles(a, paths->steamapps, error);
        if (!error.empty()) { std::cerr << "error: " << error << "\n"; return kExitFailed; }

        PinOptions opt;
        opt.outDir = paths->outDir;
        opt.force = a.force;
        opt.dryRun = a.dryRun;
        opt.processTag = ProcessTag();

        std::size_t counts[static_cast<std::size_t>(PinStatus::Failed) + 1] = {};   // Failed is the last PinStatus
        for (const fs::path& acf : files) {
            const PinReport r = PinAcf(acf, opt);
            PrintReport(acf, r);
            ++counts[static_cast<std::size_t>(r.status)];
        }
        const auto n = [&](PinStatus s) { return counts[static_cast<std::size_t>(s)]; };
        std::cout << n(PinStatus::Written) << " pinned, " << n(PinStatus::UpToDate) << " up to date, "
                  << n(PinStatus::DryRun) << " dry run, " << n(PinStatus::Refused) << " refused, "
                  << n(PinStatus::Failed) << " failed\n";
        return n(PinStatus::Refused) + n(PinStatus::Failed) == 0 ? kExitOk : kExitFailed;
    }

} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
#else
int main(int argc, char** argv) {
#endif
    Args args;
    if (const std::string error = ParseArgs(argc, argv, args); !error.empty()) {
        std::cerr << "error: " << error << "\n\n";
        PrintUsage(std::cerr);
        return kExitUsage;
    }
    if (args.help) {
        PrintUsage(std::cout);
        return kExitOk;
    }
    if (args.appId.has_value() == args.all) {   // neither, or both
        std::cerr << "error: pass exactly one of --appid <id> or --all\n\n";
        PrintUsage(std::cerr);
        return kExitUsage;
    }
    try {
        return Run(args);
    } catch (const std::exception& e) {   // e.g. std::bad_alloc on a hostile file
        std::cerr << "error: " << e.what() << "\n";
        return kExitFailed;
    }
}
