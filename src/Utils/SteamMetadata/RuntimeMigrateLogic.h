#pragma once

#include "Utils/SteamMetadata/PatternMigrateCliLogic.h"
#include "Utils/SteamMetadata/PatternRecorderLogic.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

// Runtime pattern migration (v1.6.0): when Steam updated and neither the cache nor any
// mirror has a pattern TOML for the new DLL, PatternLoader runs tools/pattern_migrate's
// engine at startup against a fingerprint the shadow recorder left for an earlier build
// (<Steam>\amethysttool\fingerprint\<component>\<sha256>.toml), and uses the result for
// this session and the next ones. This header is the part with no I/O and no threads: which
// fingerprints to try and in what order, when a result is good enough to hook Steam with,
// the file it becomes, and the never-overwrite rule for writing it. The disk, the worker
// thread and its time limit are PatternLoader.cpp's.
//
// It is the same engine and the same candidate TOML as the offline tool
// (PatternMigrateCliLogic::RunMigration / RenderCandidateToml), plus a marker line. The
// marker makes an auto-migrated file a stopgap: PatternLoader keeps asking the mirrors on
// every start while the cached file carries it, and the published file replaces it.
//
// No fingerprint, no migration: they exist only where [shadow] record was on for an
// earlier build. [shadow] auto_migrate = false turns this off.
namespace RuntimeMigrateLogic {

    namespace fs = std::filesystem;
    using PatternMigrateCliLogic::FingerprintFile;
    using PatternMigrateCliLogic::RunResult;

    // First line of every auto-migrated file.
    inline constexpr std::string_view kMarker = "# AmethystTool auto-migrated pattern file";

    // Fingerprint files tried, best first. Each costs a full engine run (~1 s Release on a
    // real steamclient64.dll), so the list is short.
    inline constexpr std::size_t kMaxCandidates = 3;

    // ponytail: a fixed threshold. Below half of the recorded functions the build changed a
    // lot, and the anchor cross-check (PatternMigrateLogic::MigrateAll phase 2) has little
    // left to cross-check against. Each accepted function still passed all five engine
    // checks; what is not migrated stays unhooked and is reported by the usual popup.
    inline constexpr std::size_t kMinMigratedPercent = 50;

    // How long Steam's init thread waits for the worker before it carries on without it
    // (the worker then finishes alone and its file is used on the next start), and after
    // how long the worker stops trying further candidates.
    inline constexpr std::chrono::seconds kWaitBudget{ 15 };
    inline constexpr std::chrono::seconds kWorkBudget{ 60 };

    [[nodiscard]] inline bool IsAutoMigrated(std::string_view body) noexcept
    {
        if (body.substr(0, 3) == "\xEF\xBB\xBF") body.remove_prefix(3);
        return body.substr(0, kMarker.size()) == kMarker;
    }

    struct FingerprintOnDisk {
        std::string stem;        // file name without ".toml"
        std::int64_t mtime = 0;  // any monotonic unit; only the order matters
    };

    // Which fingerprint files to try, best first: the current build's own (a fingerprint of
    // this very DLL, recorded on an earlier start), then the most recently written ones.
    // Names that are not a lower-case SHA-256 are dropped; at most kMaxCandidates.
    [[nodiscard]] inline std::vector<std::string> OrderCandidates(std::vector<FingerprintOnDisk> files,
                                                                  std::string_view currentSha)
    {
        std::erase_if(files, [](const FingerprintOnDisk& f) { return !PatternRecorderLogic::IsSha256Hex(f.stem); });
        std::sort(files.begin(), files.end(), [&](const FingerprintOnDisk& a, const FingerprintOnDisk& b) {
            const bool ac = a.stem == currentSha, bc = b.stem == currentSha;
            if (ac != bc) return ac;
            if (a.mtime != b.mtime) return a.mtime > b.mtime;
            return a.stem < b.stem;
        });
        std::vector<std::string> out;
        for (const FingerprintOnDisk& f : files) {
            if (!out.empty() && out.back() == f.stem) continue;
            out.push_back(f.stem);
            if (out.size() == kMaxCandidates) break;
        }
        return out;
    }

    [[nodiscard]] constexpr bool EnoughMigrated(std::size_t migrated, std::size_t total) noexcept
    {
        return migrated > 0 && migrated <= total && migrated * 100 >= total * kMinMigratedPercent;
    }

    // The pattern TOML an accepted run becomes: the marker, then the offline tool's file.
    [[nodiscard]] inline std::string Render(const FingerprintFile& fp, const RunResult& run)
    {
        std::string out(kMarker);
        out += " (v1.6.0), made at Steam startup from the fingerprint of build " + fp.sha256 + ".\n";
        out += "# A stopgap: AmethystTool keeps asking the mirrors, and their file replaces this one.\n";
        out += PatternMigrateCliLogic::RenderCandidateToml(fp, run);
        return out;
    }

    struct Attempt {
        bool accepted = false;
        std::string why;          // when not accepted
        std::size_t migrated = 0;
        std::size_t total = 0;
        std::string body;         // the pattern TOML, when accepted
    };

    // One fingerprint file against the new DLL. `fileStem` is the file's name, which must
    // be the build the file says it describes; `component` is the one being loaded.
    [[nodiscard]] inline Attempt TryOne(std::string_view fingerprintText, std::string_view component,
                                        std::string_view fileStem, const PatternMigrateLogic::ImageView& img)
    {
        Attempt a;
        std::string err;
        const std::optional<FingerprintFile> fp = PatternMigrateCliLogic::ParseFingerprintFile(fingerprintText, err);
        if (!fp) {
            a.why = "unreadable fingerprint (" + err + ")";
            return a;
        }
        if (fp->component != component) {
            a.why = "fingerprint is for another component (" + fp->component + ")";
            return a;
        }
        if (fp->sha256 != fileStem) {
            a.why = "fingerprint names another build than its file name";
            return a;
        }
        const RunResult run = PatternMigrateCliLogic::RunMigration(img, *fp, {});
        a.migrated = run.migrated;
        a.total = run.results.size();
        if (!EnoughMigrated(a.migrated, a.total)) {
            a.why = "only " + std::to_string(a.migrated) + " of " + std::to_string(a.total) + " function(s) migrated";
            return a;
        }
        a.body = Render(*fp, run);
        a.accepted = true;
        return a;
    }

    // Tries `stems` in order and returns the first accepted attempt (or the last refusal).
    // `read(stem)` returns the file text or nullopt; `now()` is a steady clock. No candidate
    // is started once `deadline` has passed. `onRefused(stem, attempt)` sees each refusal.
    template <class Read, class Now, class OnRefused>
    [[nodiscard]] Attempt FirstAccepted(const std::vector<std::string>& stems, std::string_view component,
                                        const PatternMigrateLogic::ImageView& img, Read read, Now now,
                                        std::chrono::steady_clock::time_point deadline, OnRefused onRefused)
    {
        Attempt last;
        last.why = "no fingerprint file";
        for (const std::string& stem : stems) {
            if (now() >= deadline) {
                last.why = "time budget spent";
                last.accepted = false;
                return last;
            }
            const std::optional<std::string> text = read(stem);
            if (!text) {
                last = Attempt{};
                last.why = "cannot read the fingerprint file";
            } else {
                last = TryOne(*text, component, stem, img);
                if (last.accepted) return last;
            }
            onRefused(stem, last);
        }
        return last;
    }

    enum class PersistResult { Written, TargetExists, WriteFailed };

    // Writes `body` to `target` with `write(target, body) -> bool` (an atomic temp-and-rename
    // writer), but never over a file that is there: the target is the cache entry of a build
    // nothing else had a file for, and anything found there now (a mirror's copy that landed
    // meanwhile, a file the user dropped in) wins. Fail-closed: a target whose existence
    // cannot be checked counts as present.
    // ponytail: check-then-write, and the DLL's writer renames with REPLACE_EXISTING. No
    // writer in the process can race it (a module whose load failed gets no revalidation),
    // so the gap is a second Steam process only. A rename without REPLACE_EXISTING would
    // close it. An empty target also counts as present: the migration then runs again on
    // every start and is used in-session, but is never saved.
    template <class Write>
    [[nodiscard]] PersistResult Persist(const fs::path& target, const std::string& body, Write write)
    {
        std::error_code ec;
        const bool exists = fs::exists(target, ec);
        if (ec || exists) return PersistResult::TargetExists;
        return write(target, body) ? PersistResult::Written : PersistResult::WriteFailed;
    }

} // namespace RuntimeMigrateLogic
