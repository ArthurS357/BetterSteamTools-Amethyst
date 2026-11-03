#pragma once

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
    };

    // Decide + write both aliases inside steamDir. Never throws on filesystem
    // errors; with enabled = false it performs no I/O at all.
    [[nodiscard]] ApplyResult Apply(bool enabled, const std::filesystem::path& steamDir);

} // namespace CompatAliasLogic
