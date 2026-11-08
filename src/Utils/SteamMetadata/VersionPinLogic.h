#pragma once

#include "Utils/Config/CompatAliasLogic.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

// Game version pinning (v1.5.0), the logic behind tools/version_pin.
//
// Steam decides what to update from the depot manifests it gets for an app. The
// Lua config already lets a script override one: setManifestid(depotId, "gid")
// (LuaConfig.cpp, lua_setManifestid; BuildDepotDependency swaps the gid in the
// depot list Steam built). What was missing is a way to learn the gids of the
// version that is INSTALLED. Steam records them in
// <steam>\steamapps\appmanifest_<appid>.acf:
//
//   "AppState" { "appid" "3357650" ... "InstalledDepots" { "3357651" { "manifest" "7378..." "size" "..." } } }
//
// This reads that file and writes <appid>.pin.lua with one setManifestid per
// installed depot. Offline: no hook, no network, nothing in the injected DLL.
//
// Pure apart from the <filesystem>/<fstream> calls in PinAcf: no <windows.h>, no
// Detours, no WinHTTP, so it also builds in the Linux logic-test job.
//
// Everything read from the .acf is untrusted text. It reaches the .lua only as
// digits that were parsed as numbers, or as a comment restricted to a small safe
// alphabet: .lua files are trusted code inside steam.exe, so a game name holding
// a newline must not be able to add a statement.
namespace VersionPinLogic {

    namespace fs = std::filesystem;

    inline constexpr std::uintmax_t kMaxAcfBytes     = 1ull * 1024 * 1024;   // a real one is a few KB; 1 MiB bounds the node count (~100 B each)
    inline constexpr std::uintmax_t kMaxLuaScanBytes = 1ull * 1024 * 1024;
    inline constexpr int            kMaxNesting      = 32;                   // a real one nests 3 deep
    inline constexpr std::size_t    kMaxNotes        = 10;                   // skipped-depot / conflict notes kept

    // First line of every generated file. It is how the tool recognises a file as
    // its own: anything else holding the target name is never replaced without --force.
    inline constexpr std::string_view kMarker =
        "-- AmethystTool version_pin: generated file, safe to delete.";

    // ── Valve text KeyValues (VDF) ───────────────────────────────

    struct KvNode {
        std::string         key;
        std::string         value;      // when !isBlock
        std::vector<KvNode> children;   // when isBlock
        bool                isBlock = false;
    };

    struct KvParse {
        bool                ok = false;
        std::string         error;      // set iff !ok; includes the byte offset
        std::vector<KvNode> nodes;      // the top-level nodes
    };

    // Strings are quoted ("a b", with \\ \" \n \t escapes; any other \x is kept as
    // written) or bare (up to whitespace, a quote or a brace). // starts a comment
    // outside strings. A leading UTF-8 BOM, CRLF and tabs are fine. Truncated
    // input, an unmatched brace, a key without a value or nesting beyond
    // kMaxNesting is an error, never a partial result.
    //
    // ponytail: no #include / #base directives and no [$platform] conditionals. An
    // appmanifest has neither; add them if this ever reads other VDF files.
    class KvReader {
    public:
        explicit KvReader(std::string_view text) : s_(text) {
            if (s_.starts_with("\xEF\xBB\xBF")) pos_ = 3;
        }

        [[nodiscard]] KvParse Parse() {
            KvParse out;
            if (s_.size() > kMaxAcfBytes) {
                out.error = "file is larger than " + std::to_string(kMaxAcfBytes) + " bytes";
                return out;
            }
            out.ok = ReadNodes(out.nodes, 0);
            if (!out.ok) {
                out.error = std::move(error_);
                out.nodes.clear();
            }
            return out;
        }

    private:
        static bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f'; }
        static bool IsDelimiter(char c) { return IsSpace(c) || c == '"' || c == '{' || c == '}'; }

        bool Fail(std::string message) {
            error_ = std::move(message) + " (at byte " + std::to_string(pos_) + ")";
            return false;
        }

        void SkipSpace() {
            while (pos_ < s_.size()) {
                if (IsSpace(s_[pos_])) { ++pos_; continue; }
                if (s_.compare(pos_, 2, "//") != 0) return;
                while (pos_ < s_.size() && s_[pos_] != '\n') ++pos_;
            }
        }

        // The caller has checked that a string starts at pos_.
        bool ReadString(std::string& out) {
            if (s_[pos_] != '"') {
                const std::size_t start = pos_;
                while (pos_ < s_.size() && !IsDelimiter(s_[pos_])) ++pos_;
                out.assign(s_.substr(start, pos_ - start));
                return true;
            }
            ++pos_;
            while (pos_ < s_.size()) {
                const char c = s_[pos_++];
                if (c == '"') return true;
                if (c != '\\' || pos_ >= s_.size()) { out.push_back(c); continue; }
                const char e = s_[pos_++];
                switch (e) {
                case 'n':  out.push_back('\n'); break;
                case 't':  out.push_back('\t'); break;
                case '\\': out.push_back('\\'); break;
                case '"':  out.push_back('"');  break;
                default:   out.push_back('\\'); out.push_back(e); break;
                }
            }
            return Fail("unterminated string");
        }

        // Reads `key value` / `key { ... }` pairs up to the closing brace (or, at
        // depth 0, the end of the input).
        bool ReadNodes(std::vector<KvNode>& into, int depth) {
            if (depth > kMaxNesting) return Fail("nesting deeper than " + std::to_string(kMaxNesting) + " levels");
            for (;;) {
                SkipSpace();
                if (pos_ >= s_.size()) return depth == 0 ? true : Fail("end of file inside a block");
                if (s_[pos_] == '}') {
                    if (depth == 0) return Fail("unmatched '}'");
                    ++pos_;
                    return true;
                }
                if (s_[pos_] == '{') return Fail("'{' without a key");
                KvNode node;
                if (!ReadString(node.key)) return false;
                SkipSpace();
                if (pos_ >= s_.size() || s_[pos_] == '}') return Fail("key '" + Shown(node.key) + "' has no value");
                if (s_[pos_] == '{') {
                    ++pos_;
                    node.isBlock = true;
                    if (!ReadNodes(node.children, depth + 1)) return false;
                } else if (!ReadString(node.value)) {
                    return false;
                }
                into.push_back(std::move(node));
            }
        }

        // Untrusted text in a message: printable ASCII only, and short.
        static std::string Shown(std::string_view s) {
            std::string out;
            for (char c : s.substr(0, 40)) out.push_back(c >= 0x20 && c < 0x7F ? c : '?');
            return out;
        }

        std::string_view s_;
        std::size_t      pos_ = 0;
        std::string      error_;
    };

    [[nodiscard]] inline KvParse ParseKv(std::string_view text) { return KvReader(text).Parse(); }

    [[nodiscard]] inline bool EqualsNoCase(std::string_view a, std::string_view b) {
        return std::ranges::equal(a, b, [](char x, char y) {
            return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
        });
    }

    // KeyValues keys are case-insensitive. First match wins.
    [[nodiscard]] inline const KvNode* FindChild(std::span<const KvNode> nodes, std::string_view key) {
        for (const KvNode& n : nodes)
            if (EqualsNoCase(n.key, key)) return &n;
        return nullptr;
    }

    // ── appmanifest -> pins ──────────────────────────────────────

    struct DepotPin {
        std::uint32_t depotId = 0;
        std::uint64_t manifestId = 0;
    };

    struct AppPins {
        std::uint32_t         appId = 0;
        std::string           name;         // untrusted; only ever passed through SafeComment
        std::vector<DepotPin> depots;       // by depotId, no duplicates
        std::size_t           skipped = 0;  // InstalledDepots entries that were not usable
        std::vector<std::string> skippedNotes;   // first kMaxNotes reasons
    };

    struct ExtractResult {
        bool        ok = false;
        std::string error;
        AppPins     pins;
    };

    // Decimal digits only (no sign, no space, no overflow).
    template <class T>
    [[nodiscard]] inline std::optional<T> ParseDecimal(std::string_view s) {
        if (s.empty() || s.front() < '0' || s.front() > '9') return std::nullopt;
        T value{};
        const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
        if (ec != std::errc{} || end != s.data() + s.size()) return std::nullopt;
        return value;
    }

    namespace detail {

        inline void Skip(AppPins& pins, std::string note) {
            ++pins.skipped;
            if (pins.skippedNotes.size() < kMaxNotes) pins.skippedNotes.push_back(std::move(note));
        }

        // One `"<depotId>" { "manifest" "<gid>" ... }` entry.
        inline void CollectDepot(const KvNode& entry, std::vector<DepotPin>& out, AppPins& pins) {
            const auto depot = ParseDecimal<std::uint32_t>(entry.key);
            if (!depot || *depot == 0) { Skip(pins, "depot key is not a depot id"); return; }
            const std::string id = std::to_string(*depot);
            if (!entry.isBlock) { Skip(pins, "depot " + id + " is not a block"); return; }
            const KvNode* manifest = FindChild(entry.children, "manifest");
            const auto gid = manifest && !manifest->isBlock ? ParseDecimal<std::uint64_t>(manifest->value)
                                                             : std::nullopt;
            if (!gid || *gid == 0) { Skip(pins, "depot " + id + " has no usable manifest"); return; }
            out.push_back({ *depot, *gid });
        }

    } // namespace detail

    // The pins for one parsed appmanifest. Only InstalledDepots counts: SharedDepots
    // maps a depot to ANOTHER app and carries no manifest of its own.
    [[nodiscard]] inline ExtractResult ExtractPins(std::span<const KvNode> top) {
        ExtractResult r;
        const KvNode* app = FindChild(top, "AppState");
        if (!app || !app->isBlock) { r.error = "no AppState block (not an appmanifest file?)"; return r; }

        const KvNode* id = FindChild(app->children, "appid");
        const auto appId = id && !id->isBlock ? ParseDecimal<std::uint32_t>(id->value) : std::nullopt;
        if (!appId || *appId == 0) { r.error = "appid is missing or not a number"; return r; }
        r.pins.appId = *appId;
        if (const KvNode* name = FindChild(app->children, "name"); name && !name->isBlock)
            r.pins.name = name->value;

        const KvNode* installed = FindChild(app->children, "InstalledDepots");
        if (!installed || !installed->isBlock) { r.error = "no InstalledDepots block (app not installed?)"; return r; }
        if (installed->children.empty()) { r.error = "InstalledDepots is empty (app not installed?)"; return r; }

        std::vector<DepotPin> found;
        for (const KvNode& entry : installed->children) detail::CollectDepot(entry, found, r.pins);

        // Stable, so the first of a duplicated depot id is the one kept.
        std::ranges::stable_sort(found, {}, &DepotPin::depotId);
        for (const DepotPin& pin : found) {
            if (!r.pins.depots.empty() && r.pins.depots.back().depotId == pin.depotId)
                detail::Skip(r.pins, "depot " + std::to_string(pin.depotId) + " listed twice");
            else
                r.pins.depots.push_back(pin);
        }
        if (r.pins.depots.empty()) {
            r.error = "InstalledDepots has no usable depot (" + std::to_string(r.pins.skipped) + " skipped)";
            return r;
        }
        r.ok = true;
        return r;
    }

    // ── pins -> Lua ──────────────────────────────────────────────

    // Text for a comment line: a small safe alphabet, anything else becomes '?'.
    // Newlines, quotes and brackets are all outside it.
    [[nodiscard]] inline std::string SafeComment(std::string_view s, std::size_t maxLen = 64) {
        std::string out;
        for (char c : s.substr(0, maxLen)) {
            const bool safe = std::isalnum(static_cast<unsigned char>(c)) != 0 || c == ' ' || c == '.' ||
                              c == ',' || c == ':' || c == '_' || c == '-' || c == '\'' || c == '&' || c == '!';
            out.push_back(safe && static_cast<unsigned char>(c) < 0x80 ? c : '?');
        }
        return out;
    }

    // depotId is a Lua integer (lua_setManifestid checks lua_isinteger). The gid is a
    // quoted decimal string so every uint64 stays exact: a bare Lua integer is signed
    // 64-bit and tops out at 2^63-1. There is no third argument because the engine
    // forces the size to 0.
    [[nodiscard]] inline std::string BuildPinLua(const AppPins& pins) {
        const std::string app = std::to_string(pins.appId);
        std::string lua(kMarker);
        lua += "\n-- Pins the installed manifest of each depot of app " + app;
        if (!pins.name.empty()) lua += " (" + SafeComment(pins.name) + ")";
        lua += ".\n-- Source: appmanifest_" + app + ".acf. Delete this file to follow Steam's updates again.\n";
        lua += "-- If another .lua file here also calls setManifestid for the same depot, the one parsed last wins.\n";
        for (const DepotPin& d : pins.depots)
            lua += "setManifestid(" + std::to_string(d.depotId) + ", \"" + std::to_string(d.manifestId) + "\")\n";
        return lua;
    }

    [[nodiscard]] inline std::string PinFileName(std::uint32_t appId) { return std::to_string(appId) + ".pin.lua"; }

    // A file is "ours" if it starts with the marker (after an optional BOM).
    [[nodiscard]] inline bool IsGeneratedPinFile(std::string_view contents) {
        if (contents.starts_with("\xEF\xBB\xBF")) contents.remove_prefix(3);
        return contents.starts_with(kMarker);
    }

    // ── other .lua files that pin the same depots ────────────────

    struct Conflict {
        std::uint32_t depotId = 0;
        std::uint64_t otherManifestId = 0;   // what the other file pins it to
    };

    namespace detail {

        // One `setManifestid(<depot>, "<gid>")` call at or after `from`; the cursor
        // moves past it. Tolerant of spaces, of ' or " quotes and of a bare number
        // for the gid: lua_setManifestid takes anything lua_isstring accepts, and
        // that includes numbers.
        struct Call { std::uint32_t depot; std::uint64_t gid; };

        inline std::optional<Call> NextCall(std::string_view line, std::size_t& from) {
            static constexpr std::string_view kName = "setmanifestid";
            for (std::size_t at = from; at + kName.size() <= line.size(); ++at) {
                if (!EqualsNoCase(line.substr(at, kName.size()), kName)) continue;
                std::size_t i = at + kName.size();
                const auto skipWs = [&] { while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i; };
                const auto digits = [&] { const std::size_t s = i; while (i < line.size() && line[i] >= '0' && line[i] <= '9') ++i; return line.substr(s, i - s); };
                skipWs(); if (i >= line.size() || line[i] != '(') continue; ++i;
                skipWs(); const auto depot = ParseDecimal<std::uint32_t>(digits());
                skipWs(); if (i >= line.size() || line[i] != ',') continue; ++i;
                skipWs(); if (i >= line.size()) continue;
                const char quote = (line[i] == '"' || line[i] == '\'') ? line[i++] : '\0';
                const auto gid = ParseDecimal<std::uint64_t>(digits());
                if (!depot || !gid) continue;
                if (quote) {
                    if (i >= line.size() || line[i] != quote) continue;
                    ++i;
                }
                skipWs();   // then ')' or the (ignored) size argument
                if (i >= line.size() || (line[i] != ')' && line[i] != ',')) continue;
                from = i;
                return Call{ *depot, *gid };
            }
            return std::nullopt;
        }

    } // namespace detail

    // The calls in `luaText` that pin one of `pins` to a DIFFERENT gid. When two
    // files do this the one the engine parsed last wins (LuaConfig.cpp,
    // RebuildManifestOverride), so the caller warns instead of guessing.
    //
    // ponytail: a line-by-line text scan, not a Lua parse. It misses a call built
    // at run time or split over lines, and sees one inside a block comment; the
    // result is a warning only, so the cost of a miss is a missing hint.
    [[nodiscard]] inline std::vector<Conflict> FindConflicts(std::string_view luaText,
                                                             std::span<const DepotPin> pins) {
        std::vector<Conflict> found;
        while (!luaText.empty()) {
            const std::size_t eol = luaText.find('\n');
            std::string_view line = luaText.substr(0, eol);
            luaText = eol == std::string_view::npos ? std::string_view{} : luaText.substr(eol + 1);
            line = line.substr(0, line.find("--"));
            std::size_t from = 0;
            while (const auto call = detail::NextCall(line, from)) {
                const auto mine = std::ranges::find(pins, call->depot, &DepotPin::depotId);
                if (mine != pins.end() && mine->manifestId != call->gid) found.push_back({ call->depot, call->gid });
            }
        }
        return found;
    }

    // ── what to do with the target file ──────────────────────────

    enum class WriteAction {
        Create,         // nothing there
        UpToDate,       // ours, byte-identical
        Replace,        // exists and --force was given
        RefuseOwn,      // ours but different: pinning again changes the pin
        RefuseForeign,  // not ours (or unreadable)
    };

    [[nodiscard]] constexpr WriteAction DecideWrite(bool exists, bool ours, bool identical, bool force) {
        if (!exists) return WriteAction::Create;
        if (ours && identical) return WriteAction::UpToDate;
        if (force) return WriteAction::Replace;
        return ours ? WriteAction::RefuseOwn : WriteAction::RefuseForeign;
    }

    // ── the whole job for one appmanifest ────────────────────────

    enum class PinStatus { Written, UpToDate, DryRun, Refused, Failed };

    struct PinOptions {
        fs::path    outDir;            // <steam>\config\stplug-in
        bool        force = false;
        bool        dryRun = false;
        std::string processTag;        // temp-file tag (the PID); see CompatAliasLogic::detail::MakeTempTag
    };

    struct PinReport {
        PinStatus                status = PinStatus::Failed;
        std::uint32_t            appId = 0;
        std::size_t              depots = 0;
        fs::path                 file;        // the .pin.lua (target)
        std::string              message;     // one line: why, or what happened
        std::vector<std::string> warnings;    // other .lua files that pin the same depots differently
        std::string              lua;         // what was (or, with dryRun, would be) written
    };

    // Whole file, or nullopt when it is missing, unreadable, not a regular file or over `cap`.
    [[nodiscard]] inline std::optional<std::string> ReadSmallFile(const fs::path& path, std::uintmax_t cap) {
        std::error_code ec;
        if (!fs::is_regular_file(path, ec)) return std::nullopt;
        const std::uintmax_t size = fs::file_size(path, ec);
        if (ec || size > cap) return std::nullopt;
        std::ifstream in(path, std::ios::binary);
        if (!in) return std::nullopt;
        std::string text(static_cast<std::size_t>(size), '\0');
        if (!in.read(text.data(), static_cast<std::streamsize>(size)) && size != 0) return std::nullopt;
        return text;
    }

    // `appmanifest_*.acf` regular files directly inside `steamapps`, by name.
    [[nodiscard]] inline std::vector<fs::path> ListAcfFiles(const fs::path& steamapps, std::error_code& ec) {
        std::vector<fs::path> files;
        for (fs::directory_iterator it(steamapps, ec), end; !ec && it != end; it.increment(ec)) {
            const std::string name = it->path().filename().string();
            std::error_code fileEc;
            if (name.starts_with("appmanifest_") && name.ends_with(".acf") && it->is_regular_file(fileEc))
                files.push_back(it->path());
        }
        std::ranges::sort(files);
        return files;
    }

    namespace detail {

        inline PinReport Fail(PinReport r, std::string message) {
            r.status = PinStatus::Failed;
            r.message = std::move(message);
            return r;
        }

        // Warnings for every other .lua in outDir that pins one of our depots elsewhere.
        inline void ScanForConflicts(const PinOptions& opt, const fs::path& target, const AppPins& pins, PinReport& r) {
            std::error_code ec;
            std::vector<fs::path> luas;
            for (fs::directory_iterator it(opt.outDir, ec), end; !ec && it != end; it.increment(ec)) {
                std::error_code fileEc;
                if (it->path().extension() == ".lua" && it->path() != target && it->is_regular_file(fileEc))
                    luas.push_back(it->path());
            }
            std::ranges::sort(luas);
            std::size_t reported = 0;   // its own cap, so other warnings cannot crowd conflicts out
            for (const fs::path& other : luas) {
                const auto text = ReadSmallFile(other, kMaxLuaScanBytes);
                if (!text) continue;
                for (const Conflict& c : FindConflicts(*text, pins.depots)) {
                    if (reported++ >= kMaxNotes) return;
                    r.warnings.push_back("depot " + std::to_string(c.depotId) + " is also pinned to " +
                                         std::to_string(c.otherManifestId) + " in " + other.filename().string() +
                                         "; the file parsed last wins");
                }
            }
        }

        inline const char* RefusalReason(WriteAction action) {
            switch (action) {
            case WriteAction::RefuseOwn:     return "exists with a different pin (made by version_pin); use --force to replace it";
            case WriteAction::RefuseForeign: return "exists and was not made by version_pin; use --force to overwrite it";
            case WriteAction::Create:
            case WriteAction::UpToDate:
            case WriteAction::Replace:       return nullptr;
            }
            return nullptr;   // unreachable; a new WriteAction without a case warns (C4062)
        }

        // "appmanifest_<N>.acf" -> N, when the name has that shape.
        inline std::optional<std::uint32_t> AppIdFromAcfName(const fs::path& acf) {
            constexpr std::string_view kPrefix = "appmanifest_", kSuffix = ".acf";
            const std::string name = acf.filename().string();
            if (!name.starts_with(kPrefix) || !name.ends_with(kSuffix) || name.size() <= kPrefix.size() + kSuffix.size())
                return std::nullopt;
            return ParseDecimal<std::uint32_t>(
                std::string_view(name).substr(kPrefix.size(), name.size() - kPrefix.size() - kSuffix.size()));
        }

        // Applies the decision. Writes to this call's own temp file and renames it
        // into place, so the target is never seen half-written.
        //
        // ponytail: the decision was made on a read a moment ago, and the rename
        // replaces whatever is there now. A foreign file created in that window of
        // milliseconds is lost. Closing it needs a no-clobber create (a hard link of the
        // temp file); not worth it for a tool a person runs by hand.
        inline PinReport Apply(PinReport r, WriteAction action, const PinOptions& opt) {
            if (action == WriteAction::UpToDate) {
                r.status = PinStatus::UpToDate;
                r.message = "already pinned to the installed version";
                return r;
            }
            if (const char* why = RefusalReason(action)) {
                r.status = PinStatus::Refused;
                r.message = why;
                return r;
            }
            if (opt.dryRun) {
                r.status = PinStatus::DryRun;
                r.message = "dry run, nothing written";
                return r;
            }

            const std::error_code ec = CompatAliasLogic::detail::WriteBytesViaTemp(
                r.lua, r.file, CompatAliasLogic::detail::MakeTempTag(opt.processTag));
            if (ec) return Fail(std::move(r), "could not write " + r.file.string() + ": " + ec.message());
            r.status = PinStatus::Written;
            r.message = action == WriteAction::Replace ? "replaced" : "written";
            return r;
        }

    } // namespace detail

    // Reads one appmanifest and writes <appid>.pin.lua into opt.outDir. Never throws
    // on filesystem errors; every failure comes back as a report with a message.
    [[nodiscard]] inline PinReport PinAcf(const fs::path& acfPath, const PinOptions& opt) {
        PinReport r;
        const std::string acfName = acfPath.filename().string();

        const auto text = ReadSmallFile(acfPath, kMaxAcfBytes);
        if (!text) return detail::Fail(std::move(r), acfName + ": missing, unreadable or larger than the " + std::to_string(kMaxAcfBytes) + " byte limit");
        const KvParse kv = ParseKv(*text);
        if (!kv.ok) return detail::Fail(std::move(r), acfName + ": malformed: " + kv.error);
        const ExtractResult ex = ExtractPins(kv.nodes);
        if (!ex.ok) return detail::Fail(std::move(r), acfName + ": " + ex.error);

        // A manifest copied under another app's name would otherwise pin the wrong game.
        if (const auto named = detail::AppIdFromAcfName(acfPath); named && *named != ex.pins.appId)
            return detail::Fail(std::move(r), acfName + ": the file name says app " + std::to_string(*named) +
                                              " but the manifest says app " + std::to_string(ex.pins.appId));
        // A pin that leaves out a depot would freeze the others while that one follows
        // Steam's next update, leaving the game half old and half new.
        if (ex.pins.skipped != 0)
            return detail::Fail(std::move(r), acfName + ": " + std::to_string(ex.pins.skipped) +
                                              " InstalledDepots entries are unusable (" +
                                              (ex.pins.skippedNotes.empty() ? "?" : ex.pins.skippedNotes.front()) +
                                              "); refusing a partial pin");

        r.appId = ex.pins.appId;
        r.depots = ex.pins.depots.size();
        r.file = opt.outDir / PinFileName(ex.pins.appId);
        r.lua = BuildPinLua(ex.pins);

        std::error_code ec;
        if (!fs::is_directory(opt.outDir, ec))
            return detail::Fail(std::move(r), "output directory not found: " + opt.outDir.string() + " (is AmethystTool installed? see --out-dir)");

        detail::ScanForConflicts(opt, r.file, ex.pins, r);

        const auto existing = ReadSmallFile(r.file, kMaxLuaScanBytes);
        const bool exists = fs::exists(fs::symlink_status(r.file, ec));
        const WriteAction action = DecideWrite(exists, existing && IsGeneratedPinFile(*existing),
                                               existing && *existing == r.lua, opt.force);
        return detail::Apply(std::move(r), action, opt);
    }

} // namespace VersionPinLogic
