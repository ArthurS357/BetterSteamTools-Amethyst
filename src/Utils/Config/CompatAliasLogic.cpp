#include "Utils/Config/CompatAliasLogic.h"

#include <atomic>
#include <chrono>
#include <fstream>
#include <iterator>
#include <string>

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

    // "<alias>.<tag>.amethyst-tmp" -- see IsTempName for the exact shape.
    bool IsTempName(const std::string& name, std::string_view alias) {
        return name.size() >= alias.size() + kTempSuffix.size()
            && name.compare(0, alias.size(), alias) == 0
            && name[alias.size()] == '.'
            && name.compare(name.size() - kTempSuffix.size(), kTempSuffix.size(), kTempSuffix) == 0;
    }

    struct DllFacts {
        Decision    decision = Decision::Disabled;
        std::string payload;   // the DLL bytes to write when ShouldWriteAlias
    };

    struct TomlFacts {
        Decision    decision = Decision::Disabled;
        std::string payload;   // the alias contents to write when ShouldWriteAlias
    };

    DllFacts InspectDll(bool enabled, const fs::path& source, const fs::path& alias) {
        DllFacts facts;
        if (!enabled) return facts;
        ReadOut src = ReadFileCapped(source, /*followLinks=*/true);
        if (src.status != ReadStatus::Ok) {
            facts.decision = Decide(true, false, false, false, false);
            return facts;
        }
        const ReadOut dst = ReadFileCapped(alias, /*followLinks=*/false);
        const bool present = dst.status != ReadStatus::Absent;
        const bool ours    = dst.status == ReadStatus::Ok && LooksLikeAmethystDll(dst.bytes);
        const bool matches = ours && dst.bytes == src.bytes;
        facts.decision = Decide(true, true, present, ours, matches);
        // The alias is written from the very bytes just inspected (not a second
        // read of the source), through the same temp-and-rename path as the TOML.
        if (ShouldWriteAlias(facts.decision)) facts.payload = std::move(src.bytes);
        return facts;
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

    namespace detail {

        std::string MakeTempTag(std::string_view processTag) {
            // Process-wide, so two threads of one process never collide; the clock
            // tick + the caller's process tag separate processes.
            static std::atomic<std::uint64_t> counter{ 0 };
            const std::uint64_t n = counter.fetch_add(1, std::memory_order_relaxed);
            const auto ticks = static_cast<std::uint64_t>(
                std::chrono::steady_clock::now().time_since_epoch().count());

            std::string tag;
            // Bounded and restricted to file-name-safe characters: the tag ends up
            // inside a path, so a stray separator or ".." must not escape steamDir.
            for (char c : processTag.substr(0, 24)) {
                const bool safe = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z')
                               || (c >= 'A' && c <= 'Z') || c == '-' || c == '_';
                tag.push_back(safe ? c : '_');
            }
            if (!tag.empty()) tag.push_back('-');
            tag += std::to_string(n);
            tag.push_back('-');
            tag += std::to_string(ticks);
            return tag;
        }

        fs::path TempPathFor(const fs::path& alias, std::string_view tag) {
            std::string suffix;
            suffix.reserve(tag.size() + kTempSuffix.size() + 1);
            suffix.push_back('.');
            suffix.append(tag);
            suffix.append(kTempSuffix);
            fs::path tmp = alias;
            tmp += suffix;
            return tmp;
        }

        std::error_code WriteBytesViaTemp(const std::string& bytes, const fs::path& alias,
                                          std::string_view tag) {
            const fs::path tmp = TempPathFor(alias, tag);
            {
                // trunc: this temp name is unique to the call, so it normally does
                // not exist; if it somehow does it is ours to overwrite.
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

    } // namespace detail

    std::size_t RemoveStaleTemps(const fs::path& steamDir, std::chrono::seconds minAge) {
        std::size_t removed = 0;
        try {
            std::error_code ec;
            fs::directory_iterator it(steamDir, fs::directory_options::skip_permission_denied, ec);
            const fs::directory_iterator end;
            if (ec) return 0;
            const auto now = fs::file_time_type::clock::now();
            for (; !ec && it != end; it.increment(ec)) {
                // u8string(), not string(): on MSVC string() converts through the ANSI
                // code page and can throw for a name it cannot represent.
                const auto u8 = it->path().filename().u8string();
                const std::string name(reinterpret_cast<const char*>(u8.data()), u8.size());
                if (!IsTempName(name, kAliasDllName) && !IsTempName(name, kAliasTomlName)) continue;

                std::error_code e;
                const fs::file_status st = fs::symlink_status(it->path(), e);
                if (e || st.type() != fs::file_type::regular) continue;   // never a link / directory
                const auto mtime = fs::last_write_time(it->path(), e);
                if (e) continue;
                // `now - mtime` is negative for a future timestamp: not stale, leave it.
                if (now - mtime < minAge) continue;
                if (fs::remove(it->path(), e) && !e) ++removed;
            }
        } catch (...) {
            // Best effort by contract: a sweep problem must never reach the caller.
        }
        return removed;
    }

    ApplyResult Apply(bool enabled, const fs::path& steamDir, std::string_view processTag) {
        ApplyResult result;
        if (!enabled) return result;   // both Disabled, zero I/O

        const fs::path sourceDll  = steamDir / fs::path(kSourceDllName);
        const fs::path aliasDll   = steamDir / fs::path(kAliasDllName);
        const fs::path sourceToml = steamDir / fs::path(kSourceTomlName);
        const fs::path aliasToml  = steamDir / fs::path(kAliasTomlName);

        // Orphans from a process that was killed mid-write (see the header).
        result.staleTempsRemoved = RemoveStaleTemps(steamDir);

        // One tag per call, shared by both aliases: their temp names already differ
        // by alias name.
        const std::string tag = detail::MakeTempTag(processTag);

        // Each alias is independent: a failure on the DLL never skips the TOML.
        DllFacts dll = InspectDll(true, sourceDll, aliasDll);
        result.dll.decision = dll.decision;
        if (ShouldWriteAlias(dll.decision)) {
            result.dll.error   = detail::WriteBytesViaTemp(dll.payload, aliasDll, tag);
            result.dll.written = !result.dll.error;
        }

        TomlFacts toml = InspectToml(true, sourceToml, aliasToml);
        result.toml.decision = toml.decision;
        if (ShouldWriteAlias(toml.decision)) {
            result.toml.error   = detail::WriteBytesViaTemp(toml.payload, aliasToml, tag);
            result.toml.written = !result.toml.error;
        }
        return result;
    }

} // namespace CompatAliasLogic
