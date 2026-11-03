#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

// [compatibility] denuvo_aliases (v1.3.5) -- decision + file logic.
//
// External activation tooling (the LuaTools Validator used for Denuvo tickets)
// only recognises the upstream engine by its file names next to steam.exe:
// OpenSteamTool.dll and opensteamtool.toml. This fork ships AmethystTool.dll /
// amethysttool.toml instead, so the validator reports "Set up your activation
// engine first". When the user opts in, AmethystTool drops alias copies under
// the upstream names. The aliases are for external tools only: nothing in the
// fork reads them (the alias DLL is never loaded by our proxies, and a second
// image is kept inert by the per-process guard in dllmain.cpp).
//
// Safety rules (see Decide below):
//   - Never touch a file under the upstream name that is not ours. A DLL is
//     "ours" iff its bytes contain kAmethystDllMarker; a TOML is "ours" iff it
//     starts with kTomlAliasHeader (which only we -- and the PowerShell
//     fallback, tools/enable-denuvo-compat.ps1 -- ever write).
//   - Never fatal: every filesystem error is reported via std::error_code.
//   - Writes go to a temp file first and are renamed into place, so a full
//     disk or a crash never leaves a truncated alias behind.
//   - Every call writes its OWN temp file ("<alias>.<tag>.amethyst-tmp", tag =
//     caller's process tag + a process-wide counter + a clock tick). Two writers
//     -- two steam.exe processes started close together (the main one and a
//     short-lived `steam://` forwarder), or two threads -- therefore never share
//     a temp file. With one shared name a writer could rename another writer's
//     temp into place while it was still being written (a reader then sees a
//     partial alias) or find its own temp already gone. Orphans left by a process
//     killed mid-write (a worker thread dies with its process) are swept by
//     RemoveStaleTemps once they are old enough that no live writer can own them.
//
// Pure (std::filesystem + <fstream> only): compiled straight into the unit
// test binary like ConfigMigration.cpp, without Lua, WinHTTP or spdlog.
namespace CompatAliasLogic {

    inline constexpr std::string_view kSourceDllName = "AmethystTool.dll";
    inline constexpr std::string_view kAliasDllName  = "OpenSteamTool.dll";
    inline constexpr std::string_view kSourceTomlName = "amethysttool.toml";
    inline constexpr std::string_view kAliasTomlName  = "opensteamtool.toml";

    // Present in every AmethystTool.dll build since the rename (dllmain.cpp
    // builds ConfigPath from "%s\\amethysttool.toml"), never in the upstream
    // OpenSteamTool.dll. Also matches a manual rename of an older AmethystTool
    // build, so those copies get refreshed rather than reported as foreign.
    inline constexpr std::string_view kAmethystDllMarker = "amethysttool.toml";

    // First bytes of every opensteamtool.toml alias we generate. Must stay
    // byte-identical to $TomlAliasHeader in tools/enable-denuvo-compat.ps1.
    inline constexpr std::string_view kTomlAliasHeader =
        "# AmethystTool compatibility alias -- generated from amethysttool.toml.\n"
        "# Only read by external tools (e.g. LuaTools Validator); AmethystTool\n"
        "# itself always reads amethysttool.toml. Safe to delete.\n";

    // Every temp file ends with this; see the header comment for the full name.
    inline constexpr std::string_view kTempSuffix = ".amethyst-tmp";

    // A temp file older than this cannot belong to a live writer (a write takes
    // milliseconds) and is removed by RemoveStaleTemps.
    inline constexpr std::chrono::minutes kStaleTempAge{ 10 };

    // Files above this size are never read into memory; they are treated as
    // not-ours (left untouched). The real DLL is a few MB.
    inline constexpr std::uintmax_t kMaxInspectBytes = 256ull * 1024 * 1024;

    enum class Decision {
        Disabled,       // denuvo_aliases = false: do nothing
        SourceMissing,  // our own DLL/TOML is not there to copy from
        Create,         // alias absent -> write it
        Refresh,        // alias is ours but differs (e.g. after an update) -> rewrite it
        UpToDate,       // alias is ours and identical -> nothing to do
        Foreign,        // a file we did not write (or cannot read) holds the name -> never touch
    };

    [[nodiscard]] constexpr bool ShouldWriteAlias(Decision d) {
        return d == Decision::Create || d == Decision::Refresh;
    }

    [[nodiscard]] const char* DescribeDecision(Decision d);

    // The whole policy, on pre-computed facts. aliasIsOurs / aliasMatches are
    // ignored when the alias is absent.
    [[nodiscard]] constexpr Decision Decide(bool enabled, bool sourcePresent,
                                            bool aliasPresent, bool aliasIsOurs,
                                            bool aliasMatches) {
        if (!enabled)       return Decision::Disabled;
        if (!sourcePresent) return Decision::SourceMissing;
        if (!aliasPresent)  return Decision::Create;
        if (!aliasIsOurs)   return Decision::Foreign;
        return aliasMatches ? Decision::UpToDate : Decision::Refresh;
    }

    // Content predicates (no I/O).
    [[nodiscard]] bool LooksLikeAmethystDll(std::string_view bytes);
    [[nodiscard]] bool IsGeneratedTomlAlias(std::string_view contents);
    [[nodiscard]] std::string BuildTomlAlias(std::string_view sourceContents);

    // File-based decisions (read-only).
    [[nodiscard]] Decision DecideDllAlias(bool enabled,
                                          const std::filesystem::path& sourceDll,
                                          const std::filesystem::path& aliasDll);
    [[nodiscard]] Decision DecideTomlAlias(bool enabled,
                                           const std::filesystem::path& sourceToml,
                                           const std::filesystem::path& aliasToml);

    struct FileOutcome {
        Decision        decision = Decision::Disabled;
        bool            written  = false;
        std::error_code error;     // set iff a write was attempted and failed
    };

    struct ApplyResult {
        FileOutcome dll;
        FileOutcome toml;
        std::size_t staleTempsRemoved = 0;   // orphaned temp files swept before writing
    };

    // Removes orphaned "<alias>.*.amethyst-tmp" files (both aliases) directly
    // inside steamDir that are at least `minAge` old. Only regular files that
    // match the temp naming exactly are touched -- never a symlink, directory or
    // any other file -- and a temp newer than `minAge` is left alone because it
    // may belong to a writer that is running right now. Best effort: returns how
    // many were removed, never throws.
    std::size_t RemoveStaleTemps(const std::filesystem::path& steamDir,
                                 std::chrono::seconds minAge = kStaleTempAge);

    // Decide + write both aliases inside steamDir. Never throws on filesystem
    // errors; with enabled = false it performs no I/O at all.
    //
    // processTag distinguishes this process's temp files from other processes'
    // (pass the PID); it is sanitised to [A-Za-z0-9_-] and may be empty, since a
    // counter and a clock tick already separate threads of one process.
    [[nodiscard]] ApplyResult Apply(bool enabled, const std::filesystem::path& steamDir,
                                    std::string_view processTag = {});

    // Building blocks of Apply, exposed for the unit tests (deterministic failure
    // injection). Not part of the feature's API.
    namespace detail {
        // "<processTag>-<counter>-<tick>" with processTag sanitised; never empty.
        [[nodiscard]] std::string MakeTempTag(std::string_view processTag);
        // "<alias>.<tag>.amethyst-tmp", next to the alias.
        [[nodiscard]] std::filesystem::path TempPathFor(const std::filesystem::path& alias,
                                                        std::string_view tag);
        // Writes `bytes` to this call's own temp file, then renames it over
        // `alias`. On any failure the temp is removed and `alias` is untouched.
        [[nodiscard]] std::error_code WriteBytesViaTemp(const std::string& bytes,
                                                        const std::filesystem::path& alias,
                                                        std::string_view tag);
    }

} // namespace CompatAliasLogic
