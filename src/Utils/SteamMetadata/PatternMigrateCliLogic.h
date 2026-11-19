#pragma once

#include "Utils/SteamMetadata/PatternMigrateLogic.h"
#include "Utils/SteamMetadata/PatternRecorderLogic.h"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

// Everything tools/pattern_migrate does that is not file I/O (v1.5.0, pillar 4): the
// argument parser, the reader of the recorder's fingerprint file, the run against a DLL
// image, the candidate TOML and the report. pattern_migrate.cpp only reads and writes
// files around it, so the tests drive all of this with strings and synthetic images.
//
// The fingerprint reader is NOT a general TOML parser. It reads exactly what
// PatternRecorderLogic::SerializeFingerprints writes (one `key = value` per line,
// `[[function]]` blocks) and rejects anything else: the file is produced by this
// project, so a line it cannot read means a wrong or damaged file, and guessing would
// feed a wrong fingerprint to the matcher.
namespace PatternMigrateCliLogic {

    namespace fs = std::filesystem;

    // ── arguments ────────────────────────────────────────────────────────────────

    struct Options {
        fs::path fingerprint;
        fs::path dll;
        fs::path out;
        fs::path required;      // a comma-separated list of names, or the path of an old pattern TOML
        bool force = false;
        bool help = false;
    };

    struct ParsedArgs {
        Options options;
        std::string error;      // empty when the arguments are fine
    };

    // A path as UTF-8 text for messages and for flag comparison. Unlike path::string(),
    // which converts to the ANSI code page on Windows and throws on a character outside it,
    // this never throws: a path stays an fs::path until it reaches the file system.
    [[nodiscard]] inline std::string Narrow(const fs::path& p)
    {
        const auto u = p.u8string();
        return std::string(u.begin(), u.end());
    }

    // `args` excludes the program name. A value that looks like a flag is a forgotten
    // value (`--out --force`), not a path. Each flag may appear once.
    [[nodiscard]] inline ParsedArgs ParseArgs(std::span<const fs::path> args)
    {
        ParsedArgs r;
        Options& o = r.options;
        bool seenFp = false, seenDll = false, seenOut = false, seenReq = false;

        for (std::size_t i = 0; i < args.size(); ++i) {
            const std::string flag = Narrow(args[i]);
            const auto value = [&]() -> const fs::path* {
                if (i + 1 >= args.size() || Narrow(args[i + 1]).rfind("--", 0) == 0) return nullptr;
                return &args[++i];
            };
            const auto take = [&](bool& seen, fs::path& dst) -> bool {
                if (seen) { r.error = flag + " given twice"; return false; }
                const fs::path* v = value();
                if (!v || v->empty()) { r.error = flag + " needs a value"; return false; }
                seen = true;
                dst = *v;
                return true;
            };

            if (flag == "--help" || flag == "-h") {
                o.help = true;
            } else if (flag == "--force") {
                o.force = true;
            } else if (flag == "--fingerprint") {
                if (!take(seenFp, o.fingerprint)) return r;
            } else if (flag == "--dll") {
                if (!take(seenDll, o.dll)) return r;
            } else if (flag == "--out") {
                if (!take(seenOut, o.out)) return r;
            } else if (flag == "--required") {
                if (!take(seenReq, o.required)) return r;
            } else {
                r.error = "unknown argument: " + flag;
                return r;
            }
        }
        if (o.help) return r;
        if (!seenFp) r.error = "--fingerprint <file.toml> is required";
        else if (!seenDll) r.error = "--dll <steamclient64.dll or steamui.dll> is required";
        else if (!seenOut) r.error = "--out <file.toml> is required";
        return r;
    }

    // ── required functions ───────────────────────────────────────────────────────

    // Names go into table headers of the output, so they are restricted to what a
    // function name is: [A-Za-z0-9_].
    [[nodiscard]] inline bool IsFunctionName(std::string_view s) noexcept
    {
        if (s.empty() || s.size() > 128) return false;
        return std::all_of(s.begin(), s.end(), [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        });
    }

    namespace detail {
        [[nodiscard]] inline std::string_view Trim(std::string_view s) noexcept
        {
            while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
            while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
            return s;
        }

        template <class F>
        void ForEachLine(std::string_view text, F&& f)
        {
            while (!text.empty()) {
                const std::size_t nl = text.find('\n');
                f(Trim(text.substr(0, nl)));
                if (nl == std::string_view::npos) break;
                text.remove_prefix(nl + 1);
            }
        }
    } // namespace detail

    // "A, B,C": sorted, unique. nullopt when an entry is not a function name.
    [[nodiscard]] inline std::optional<std::vector<std::string>> ParseRequiredList(std::string_view csv)
    {
        std::vector<std::string> out;
        while (!csv.empty()) {
            const std::size_t comma = csv.find(',');
            const std::string_view item = detail::Trim(csv.substr(0, comma));
            if (!IsFunctionName(item)) return std::nullopt;
            out.emplace_back(item);
            if (comma == std::string_view::npos) break;
            csv.remove_prefix(comma + 1);
        }
        if (out.empty()) return std::nullopt;
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
        return out;
    }

    // An old pattern TOML (the steam-monitor format): every `[Name]` table is a required
    // function. Only table headers are read; rva/sig of the old build are irrelevant.
    [[nodiscard]] inline std::optional<std::vector<std::string>> ParseRequiredFromPatternToml(std::string_view text)
    {
        std::vector<std::string> out;
        bool bad = false;
        detail::ForEachLine(text, [&](std::string_view line) {
            if (line.size() < 3 || line.front() != '[' || line.back() != ']' || line[1] == '[') return;
            const std::string_view name = line.substr(1, line.size() - 2);
            if (IsFunctionName(name)) out.emplace_back(name);
            else bad = true;
        });
        if (bad || out.empty()) return std::nullopt;
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
        return out;
    }

    // ── fingerprint file ─────────────────────────────────────────────────────────

    inline constexpr std::size_t kMaxFingerprintFileBytes = 8u << 20;
    // The recorder writes a few dozen functions; every one costs a scan of .text, so the
    // cap also bounds the run time on a hostile file.
    inline constexpr std::size_t kMaxFunctions = 512;
    inline constexpr std::size_t kMaxListItems = 256;  // calls / skipped, per list
    inline constexpr std::size_t kMaxSigBytes = 128;   // kPrefixLen plus the longest instruction

    struct FingerprintFile {
        std::string component;
        std::string sha256;
        std::vector<PatternMigrateLogic::FunctionFingerprint> fps;
        std::vector<std::string> skipped;
    };

    namespace detail {
        // The inverse of PatternRecorderLogic::TomlQuote.
        [[nodiscard]] inline std::optional<std::string> ParseQuoted(std::string_view v)
        {
            if (v.size() < 2 || v.front() != '"' || v.back() != '"') return std::nullopt;
            v = v.substr(1, v.size() - 2);
            std::string out;
            for (std::size_t i = 0; i < v.size(); ++i) {
                const char c = v[i];
                if (c == '"') return std::nullopt;                  // an unescaped quote inside
                if (c != '\\') { out += c; continue; }
                if (++i >= v.size()) return std::nullopt;
                if (v[i] == '"' || v[i] == '\\') { out += v[i]; continue; }
                // \u00XX is the only other escape TomlQuote writes.
                if (v[i] != 'u' || i + 4 >= v.size() || v[i + 1] != '0' || v[i + 2] != '0') return std::nullopt;
                unsigned code = 0;
                const auto [p, ec] = std::from_chars(v.data() + i + 3, v.data() + i + 5, code, 16);
                if (ec != std::errc{} || p != v.data() + i + 5) return std::nullopt;
                out += static_cast<char>(code);
                i += 4;
            }
            return out;
        }

        [[nodiscard]] inline std::optional<std::vector<std::string>> ParseQuotedArray(std::string_view v)
        {
            if (v.size() < 2 || v.front() != '[' || v.back() != ']') return std::nullopt;
            v = Trim(v.substr(1, v.size() - 2));
            std::vector<std::string> out;
            while (!v.empty()) {
                if (v.front() != '"') return std::nullopt;
                std::size_t end = 1;
                while (end < v.size() && v[end] != '"') end += (v[end] == '\\') ? 2 : 1;
                if (end >= v.size()) return std::nullopt;
                auto item = ParseQuoted(v.substr(0, end + 1));
                if (!item) return std::nullopt;
                out.push_back(std::move(*item));
                v = Trim(v.substr(end + 1));
                if (v.empty()) break;
                if (v.front() != ',') return std::nullopt;
                v = Trim(v.substr(1));
                if (v.empty()) return std::nullopt;                  // a trailing comma
            }
            return out;
        }

        [[nodiscard]] inline std::optional<std::uint32_t> ParseU32(std::string_view s, int base = 10)
        {
            std::uint32_t v = 0;
            if (s.empty()) return std::nullopt;
            const auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v, base);
            if (ec != std::errc{} || p != s.data() + s.size()) return std::nullopt;
            return v;
        }

        [[nodiscard]] inline std::optional<bool> ParseBool(std::string_view s) noexcept
        {
            if (s == "true") return true;
            if (s == "false") return false;
            return std::nullopt;
        }

        // "0x1A2B30" (the quotes already removed).
        [[nodiscard]] inline std::optional<std::uint32_t> ParseHexRva(std::string_view s)
        {
            if (s.size() < 3 || s[0] != '0' || (s[1] != 'x' && s[1] != 'X')) return std::nullopt;
            return ParseU32(s.substr(2), 16);
        }

        // "48 89 5C ?? 57": the inverse of PatternRecorderLogic::SigString.
        [[nodiscard]] inline bool ParseSig(std::string_view s, std::vector<std::uint8_t>& bytes,
                                           std::vector<std::uint8_t>& mask)
        {
            bytes.clear();
            mask.clear();
            while (!s.empty()) {
                const std::size_t sp = s.find(' ');
                const std::string_view tok = s.substr(0, sp);
                if (tok == "??") {
                    bytes.push_back(0);
                    mask.push_back(0x00);
                } else if (tok.size() == 2) {
                    unsigned v = 0;
                    const auto [p, ec] = std::from_chars(tok.data(), tok.data() + 2, v, 16);
                    if (ec != std::errc{} || p != tok.data() + 2) return false;
                    bytes.push_back(static_cast<std::uint8_t>(v));
                    mask.push_back(0xFF);
                } else {
                    return false;
                }
                if (bytes.size() > kMaxSigBytes) return false;
                if (sp == std::string_view::npos) break;
                s.remove_prefix(sp + 1);
                if (s.empty()) return false;                         // a trailing space
            }
            return !bytes.empty();
        }
    } // namespace detail

    // `error` says what is wrong, with the 1-based line number.
    [[nodiscard]] inline std::optional<FingerprintFile> ParseFingerprintFile(std::string_view text, std::string& error)
    {
        using PatternMigrateLogic::FunctionFingerprint;
        FingerprintFile file;
        std::optional<FunctionFingerprint> cur;
        std::size_t lineNo = 0;
        bool failed = false;
        bool sawVersion = false;
        bool curHasRva = false, curHasSig = false;
        std::vector<std::string> seenKeys;   // of the header, then of each [[function]]

        if (text.substr(0, 3) == "\xEF\xBB\xBF") text.remove_prefix(3);   // a UTF-8 BOM (PowerShell 5 adds one)

        const auto fail = [&](const std::string& why) {
            if (!failed) error = "line " + std::to_string(lineNo) + ": " + why;
            failed = true;
        };
        // Every key may appear once per block: "the last one wins" would hide a damaged file.
        const auto firstTime = [&](std::string_view key) {
            if (std::find(seenKeys.begin(), seenKeys.end(), key) != seenKeys.end()) {
                fail("key '" + std::string(key) + "' given twice");
                return false;
            }
            seenKeys.emplace_back(key);
            return true;
        };
        // A list of function names, at most kMaxListItems.
        const auto nameList = [&](std::string_view val, std::vector<std::string>& dst, const char* what) {
            auto arr = detail::ParseQuotedArray(val);
            const bool ok = arr && arr->size() <= kMaxListItems &&
                            std::all_of(arr->begin(), arr->end(), [](const std::string& s) { return IsFunctionName(s); });
            if (!ok) { fail(std::string("bad value for ") + what); return; }
            std::sort(arr->begin(), arr->end());
            arr->erase(std::unique(arr->begin(), arr->end()), arr->end());
            dst = std::move(*arr);
        };
        const auto closeFunction = [&]() {
            if (!cur) return;
            if (!IsFunctionName(cur->name)) fail("function without a valid name");
            else if (!curHasRva || !curHasSig) fail("function '" + cur->name + "' needs rva and sig");
            else if (std::any_of(file.fps.begin(), file.fps.end(),
                                 [&](const FunctionFingerprint& f) { return f.name == cur->name; }))
                fail("function '" + cur->name + "' appears twice");
            else file.fps.push_back(std::move(*cur));
            cur.reset();
        };

        detail::ForEachLine(text, [&](std::string_view line) {
            ++lineNo;
            if (failed || line.empty() || line.front() == '#') return;

            if (line == "[[function]]") {
                closeFunction();
                if (failed) return;
                if (file.fps.size() >= kMaxFunctions) { fail("too many functions"); return; }
                cur.emplace();
                seenKeys.clear();
                curHasRva = curHasSig = false;
                return;
            }
            const std::size_t eq = line.find('=');
            if (eq == std::string_view::npos) { fail("not a `key = value` line"); return; }
            const std::string_view key = detail::Trim(line.substr(0, eq));
            const std::string_view val = detail::Trim(line.substr(eq + 1));
            if (!firstTime(key)) return;

            if (!cur) {                                              // file header
                if (key == "version") {
                    sawVersion = true;
                    if (detail::ParseU32(val) != static_cast<std::uint32_t>(PatternRecorderLogic::kFileVersion))
                        fail("unsupported version");
                } else if (key == "component") {
                    auto q = detail::ParseQuoted(val);
                    if (!q || !PatternRecorderLogic::IsSafeComponent(*q)) { fail("bad value for component"); return; }
                    file.component = std::move(*q);
                } else if (key == "sha256") {
                    auto q = detail::ParseQuoted(val);
                    if (!q || !PatternRecorderLogic::IsSha256Hex(*q)) { fail("bad value for sha256"); return; }
                    file.sha256 = std::move(*q);
                } else if (key == "skipped") {
                    nameList(val, file.skipped, "skipped");
                }                                                    // prefix_len and unknown keys: not needed
                return;
            }

            FunctionFingerprint& f = *cur;                           // inside a [[function]]
            if (key == "name") {
                auto q = detail::ParseQuoted(val);
                if (!q) fail("bad value for name"); else f.name = std::move(*q);
            } else if (key == "rva") {
                auto q = detail::ParseQuoted(val);
                auto v = q ? detail::ParseHexRva(*q) : std::nullopt;
                if (!v) fail("bad rva"); else { f.rva = *v; curHasRva = true; }
            } else if (key == "pdata") {
                if (auto v = detail::ParseBool(val)) f.hasPdata = *v; else fail("bad value for pdata");
            } else if (key == "complete") {
                if (auto v = detail::ParseBool(val)) f.decodeComplete = *v; else fail("bad value for complete");
            } else if (key == "size") {
                if (auto v = detail::ParseU32(val)) f.funcSize = *v; else fail("bad value for size");
            } else if (key == "matches") {
                if (auto v = detail::ParseU32(val)) f.matchCountOld = *v; else fail("bad value for matches");
            } else if (key == "sig") {
                auto q = detail::ParseQuoted(val);
                if (!q || !detail::ParseSig(*q, f.bytes, f.mask)) fail("bad sig");
                else curHasSig = true;
            } else if (key == "calls") {
                nameList(val, f.calls, "calls");
            }
        });
        if (!failed) closeFunction();
        if (failed) return std::nullopt;
        if (!sawVersion) { error = "not a fingerprint file (no version key)"; return std::nullopt; }
        if (file.fps.empty()) { error = "the fingerprint file has no function"; return std::nullopt; }
        if (file.component.empty() || file.sha256.empty()) { error = "component and sha256 are required"; return std::nullopt; }
        return file;
    }

    // ── the DLL ──────────────────────────────────────────────────────────────────

    // The view points into `file`, which must outlive it. Same rules as the recorder:
    // PE32+ x64, .text and the exception directory inside the file, sizes capped.
    [[nodiscard]] inline std::optional<PatternMigrateLogic::ImageView> MakeImageView(
        std::span<const std::uint8_t> file, std::string& error)
    {
        using namespace PatternRecorderLogic;
        if (file.empty() || file.size() > kMaxFileBytes) { error = "the DLL size is outside the supported range"; return std::nullopt; }
        const std::optional<PeLayout> layout = ParsePeLayout(file.first(std::min(file.size(), kHeaderWindow)), file.size());
        if (!layout) { error = "not a PE32+ x64 image with a usable .text and exception directory"; return std::nullopt; }

        PatternMigrateLogic::ImageView img;
        img.textRva = layout->text.rva;
        img.text = file.subspan(static_cast<std::size_t>(layout->text.fileOffset), layout->text.size);
        img.pdata = PatternMigrateLogic::ParsePdata(
            file.subspan(static_cast<std::size_t>(layout->pdata.fileOffset), layout->pdata.size));
        return img;
    }

    // ── run, candidate TOML, report ──────────────────────────────────────────────

    struct RunResult {
        std::vector<PatternMigrateLogic::NamedMigration> results;   // same order as the fingerprint file
        std::vector<std::string> requiredFailed;                    // required and not migrated (or unknown)
        std::size_t migrated = 0;
    };

    [[nodiscard]] inline RunResult RunMigration(const PatternMigrateLogic::ImageView& img, const FingerprintFile& fp,
                                       const std::vector<std::string>& required)
    {
        using PatternMigrateLogic::Verdict;
        RunResult r;
        r.results = PatternMigrateLogic::MigrateAll(img, fp.fps);
        for (const auto& m : r.results)
            if (m.result.verdict == Verdict::Migrated) ++r.migrated;
        for (const std::string& name : required) {
            const auto it = std::find_if(r.results.begin(), r.results.end(),
                                         [&](const PatternMigrateLogic::NamedMigration& m) { return m.name == name; });
            if (it == r.results.end() || it->result.verdict != Verdict::Migrated) r.requiredFailed.push_back(name);
        }
        return r;
    }

    [[nodiscard]] constexpr int ExitCodeFor(const RunResult& r) noexcept
    {
        return (r.requiredFailed.empty() && r.migrated != 0) ? 0 : 2;
    }

    // The steam-monitor format: one `[Name]` table with `rva` and `sig`. Only migrated
    // functions are written; the others are comments, so the file loads as it is.
    [[nodiscard]] inline std::string RenderCandidateToml(const FingerprintFile& fp, const RunResult& run)
    {
        using namespace PatternMigrateLogic;
        std::string out;
        out += "# AmethystTool candidate pattern file, made by tools/pattern_migrate (v1.5.0).\n";
        out += "# REVIEW IT BEFORE USE: a wrong RVA hooks the wrong function and can crash Steam.\n";
        out += "# Migrated from component " + PatternRecorderLogic::TomlQuote(fp.component) + ", build " +
               PatternRecorderLogic::TomlQuote(fp.sha256) + ".\n";
        out += "# Save it as <sha256 of the new DLL>.toml (Get-FileHash -Algorithm SHA256 <dll>), lower case.\n";
        for (std::size_t i = 0; i < run.results.size() && i < fp.fps.size(); ++i) {
            const NamedMigration& m = run.results[i];
            if (m.result.verdict == Verdict::Migrated) {
                out += "\n[" + m.name + "]\n";
                out += "rva = " + PatternRecorderLogic::HexRva(m.result.rva) + "\n";
                out += "sig = " + PatternRecorderLogic::TomlQuote(
                                      PatternRecorderLogic::SigString(fp.fps[i].bytes, fp.fps[i].mask)) + "\n";
            } else {
                out += "\n# " + m.name + ": " + std::string(ToString(m.result.verdict)) + ", not written\n";
            }
        }
        return out;
    }

    namespace detail {
        [[nodiscard]] inline std::string Hex(std::uint32_t v)
        {
            std::string h = PatternRecorderLogic::HexRva(v);
            return h.substr(1, h.size() - 2);                        // without the quotes
        }
    } // namespace detail

    [[nodiscard]] inline std::string RenderReport(const FingerprintFile& fp, const RunResult& run)
    {
        using namespace PatternMigrateLogic;
        std::string out;
        for (std::size_t i = 0; i < run.results.size() && i < fp.fps.size(); ++i) {
            const NamedMigration& m = run.results[i];
            const MigrationResult& r = m.result;
            out += "[" + std::string(ToString(r.verdict)) + "] " + m.name;
            switch (r.verdict) {
                case Verdict::Migrated:
                    out += ": " + detail::Hex(fp.fps[i].rva) + " -> " + detail::Hex(r.rva) + ", size " +
                           std::to_string(fp.fps[i].funcSize) + " -> " + std::to_string(r.newSize);
                    if (r.anchorsUnverified != 0)
                        out += ", " + std::to_string(r.anchorsUnverified) + " callee(s) not checked";
                    break;
                case Verdict::Ambiguous:
                    out += ": " + std::to_string(r.matchCount) + " matches";
                    break;
                case Verdict::NotFound:
                    break;
                case Verdict::SizeMismatch:
                    out += ": " + detail::Hex(r.rva) + " is " + std::to_string(r.newSize) + " bytes, recorded " +
                           std::to_string(fp.fps[i].funcSize);
                    break;
                case Verdict::CallsMismatch:
                    out += ": " + detail::Hex(r.rva) + " calls different functions than the recorded one";
                    break;
                case Verdict::NeedsManual:
                    out += ": " + std::string(ToString(r.reason));
                    if (r.matchCount != 0) out += " (match at " + detail::Hex(r.rva) + ")";
                    break;
            }
            out += "\n";
        }
        for (const std::string& s : fp.skipped) out += "[skipped] " + s + ": was outside .text when recorded\n";
        out += std::to_string(run.migrated) + " of " + std::to_string(run.results.size()) + " migrated\n";
        for (const std::string& name : run.requiredFailed) out += "REQUIRED NOT MIGRATED: " + name + "\n";
        return out;
    }

    // ── where the output may go ──────────────────────────────────────────────────

    // Empty when `out` may be written; otherwise why not. The tool never writes into a
    // Steam folder (the one that holds steam.exe, or any folder below it), never over
    // one of its inputs (`inputs`: the fingerprint, the DLL and an old pattern TOML given
    // to --required), never onto a directory, and never over an existing file without
    // --force. Any file-system error while checking refuses: the guard fails closed.
    // The check and the later rename are not atomic; that gap is not defended (a process of
    // the same user racing this one gains nothing it could not do directly).
    [[nodiscard]] inline std::string RefuseOutput(const fs::path& out, std::span<const fs::path> inputs, bool force)
    {
        std::error_code ec;
        // absolute first: on MSVC weakly_canonical leaves a relative path relative, and the
        // walk over the parents below would then see nothing.
        const fs::path abs = fs::absolute(out, ec);
        if (ec) return "cannot resolve --out: " + ec.message();
        const fs::path target = fs::weakly_canonical(abs, ec);
        if (ec || target.empty() || target.filename().empty()) return "cannot resolve --out (is it a file path?)";

        for (fs::path dir = target.parent_path(); !dir.empty(); dir = dir.parent_path()) {
            const bool steam = fs::exists(dir / "steam.exe", ec);
            if (ec) return "cannot inspect " + Narrow(dir) + ": " + ec.message();
            if (steam)
                return "--out is inside a Steam folder (" + Narrow(dir) + "); write it elsewhere and copy it by hand";
            if (dir == dir.parent_path()) break;
        }

        const bool exists = fs::exists(target, ec);
        if (ec) return "cannot inspect --out: " + ec.message();
        if (!exists) return {};
        if (fs::is_directory(target, ec)) return "--out is a directory";
        for (const fs::path& input : inputs) {
            const bool inputExists = fs::exists(input, ec);
            if (ec) return "cannot inspect " + Narrow(input) + ": " + ec.message();
            if (!inputExists) continue;
            const bool same = fs::equivalent(target, input, ec);
            if (ec) return "cannot compare --out with " + Narrow(input) + ": " + ec.message();
            if (same) return "--out is the same file as an input (" + Narrow(input) + ")";
        }
        if (!force) return "--out already exists (pass --force to replace it)";
        return {};
    }

} // namespace PatternMigrateCliLogic
