#include "Utils/Config/CompatAliasLogic.h"

#include <fstream>
#include <iterator>

namespace CompatAliasLogic {
namespace {

    namespace fs = std::filesystem;

    enum class ReadStatus { Absent, Ok, Unreadable };

    struct ReadOut {
        ReadStatus  status = ReadStatus::Absent;
        std::string bytes;
    };

    // followLinks = false for alias paths: a symlink/junction under the
    // upstream name is never something we created, so it reads as Unreadable
    // (-> Foreign, untouched) instead of being followed and compared.
    ReadOut ReadFileCapped(const fs::path& p, bool followLinks) {
        ReadOut out;
        std::error_code ec;
        const fs::file_status st = followLinks ? fs::status(p, ec) : fs::symlink_status(p, ec);
        if (st.type() == fs::file_type::not_found) return out;            // Absent
        out.status = ReadStatus::Unreadable;
        if (ec || st.type() != fs::file_type::regular) return out;

        const std::uintmax_t size = fs::file_size(p, ec);
        if (ec || size > kMaxInspectBytes) return out;

        std::ifstream in(p, std::ios::binary);
        if (!in) return out;
        out.bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        if (in.bad()) { out.bytes.clear(); return out; }
        out.status = ReadStatus::Ok;
        return out;
    }

    fs::path TempPathFor(const fs::path& alias) {
        fs::path tmp = alias;
        tmp += ".amethyst-tmp";
        return tmp;
    }

    // Moves a fully written temp file over the alias (std::filesystem::rename
    // replaces an existing regular file -- MoveFileExW(MOVEFILE_REPLACE_EXISTING)
    // on MSVC). The temp is removed on failure so nothing half-written stays.
    std::error_code CommitTemp(const fs::path& tmp, const fs::path& alias) {
        std::error_code ec;
        fs::rename(tmp, alias, ec);
        if (ec) {
            std::error_code ignored;
            fs::remove(tmp, ignored);
        }
        return ec;
    }

    std::error_code CopyFileViaTemp(const fs::path& source, const fs::path& alias) {
        const fs::path tmp = TempPathFor(alias);
        std::error_code ec;
        // overwrite_existing only ever applies to our own temp name; the alias
        // itself is replaced by CommitTemp, and only after Decide() proved it
        // is absent or ours.
        fs::copy_file(source, tmp, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            std::error_code ignored;
            fs::remove(tmp, ignored);
            return ec;
        }
        return CommitTemp(tmp, alias);
    }

    std::error_code WriteBytesViaTemp(const std::string& bytes, const fs::path& alias) {
        const fs::path tmp = TempPathFor(alias);
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            if (out) {
                out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
                out.flush();
            }
            if (!out) {
                out.close();
                std::error_code ignored;
                fs::remove(tmp, ignored);
                return std::make_error_code(std::errc::io_error);
            }
        }
        return CommitTemp(tmp, alias);
    }

    struct DllFacts {
        Decision decision = Decision::Disabled;
    };

    struct TomlFacts {
        Decision    decision = Decision::Disabled;
        std::string payload;   // the alias contents to write when ShouldWriteAlias
    };

    DllFacts InspectDll(bool enabled, const fs::path& source, const fs::path& alias) {
        if (!enabled) return {Decision::Disabled};
        const ReadOut src = ReadFileCapped(source, /*followLinks=*/true);
        if (src.status != ReadStatus::Ok)
            return {Decide(true, false, false, false, false)};
        const ReadOut dst = ReadFileCapped(alias, /*followLinks=*/false);
        const bool present = dst.status != ReadStatus::Absent;
        const bool ours    = dst.status == ReadStatus::Ok && LooksLikeAmethystDll(dst.bytes);
        const bool matches = ours && dst.bytes == src.bytes;
        return {Decide(true, true, present, ours, matches)};
    }

    TomlFacts InspectToml(bool enabled, const fs::path& source, const fs::path& alias) {
        TomlFacts facts;
        if (!enabled) return facts;
        const ReadOut src = ReadFileCapped(source, /*followLinks=*/true);
        if (src.status != ReadStatus::Ok) {
            facts.decision = Decide(true, false, false, false, false);
            return facts;
        }
        facts.payload = BuildTomlAlias(src.bytes);
        const ReadOut dst = ReadFileCapped(alias, /*followLinks=*/false);
        const bool present = dst.status != ReadStatus::Absent;
        const bool ours    = dst.status == ReadStatus::Ok && IsGeneratedTomlAlias(dst.bytes);
        const bool matches = ours && dst.bytes == facts.payload;
        facts.decision = Decide(true, true, present, ours, matches);
        return facts;
    }

} // namespace

    const char* DescribeDecision(Decision d) {
        switch (d) {
        case Decision::Disabled:      return "disabled";
        case Decision::SourceMissing: return "source missing";
        case Decision::Create:        return "create";
        case Decision::Refresh:       return "refresh";
        case Decision::UpToDate:      return "up to date";
        case Decision::Foreign:       return "foreign file present, left untouched";
        }
        return "???";
    }

    bool LooksLikeAmethystDll(std::string_view bytes) {
        return bytes.find(kAmethystDllMarker) != std::string_view::npos;
    }

    bool IsGeneratedTomlAlias(std::string_view contents) {
        return contents.substr(0, kTomlAliasHeader.size()) == kTomlAliasHeader;
    }

    std::string BuildTomlAlias(std::string_view sourceContents) {
        std::string out;
        out.reserve(kTomlAliasHeader.size() + sourceContents.size());
        out.append(kTomlAliasHeader);
        out.append(sourceContents);
        return out;
    }

    Decision DecideDllAlias(bool enabled, const fs::path& sourceDll, const fs::path& aliasDll) {
        return InspectDll(enabled, sourceDll, aliasDll).decision;
    }

    Decision DecideTomlAlias(bool enabled, const fs::path& sourceToml, const fs::path& aliasToml) {
        return InspectToml(enabled, sourceToml, aliasToml).decision;
    }

    ApplyResult Apply(bool enabled, const fs::path& steamDir) {
        ApplyResult result;
        if (!enabled) return result;   // both Disabled, zero I/O

        const fs::path sourceDll  = steamDir / fs::path(kSourceDllName);
        const fs::path aliasDll   = steamDir / fs::path(kAliasDllName);
        const fs::path sourceToml = steamDir / fs::path(kSourceTomlName);
        const fs::path aliasToml  = steamDir / fs::path(kAliasTomlName);

        // Each alias is independent: a failure on the DLL never skips the TOML.
        result.dll.decision = InspectDll(true, sourceDll, aliasDll).decision;
        if (ShouldWriteAlias(result.dll.decision)) {
            result.dll.error   = CopyFileViaTemp(sourceDll, aliasDll);
            result.dll.written = !result.dll.error;
        }

        TomlFacts toml = InspectToml(true, sourceToml, aliasToml);
        result.toml.decision = toml.decision;
        if (ShouldWriteAlias(toml.decision)) {
            result.toml.error   = WriteBytesViaTemp(toml.payload, aliasToml);
            result.toml.written = !result.toml.error;
        }
        return result;
    }

} // namespace CompatAliasLogic
