#pragma once

#include "Utils/SteamMetadata/PatternMigrateLogic.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Pure side of the shadow-mode recorder (v1.5.0, pillar 4): everything that does not
// touch the disk or Steam. PatternRecorder.cpp reads the files, PatternLoader starts the
// worker; this header decides, parses the PE layout and writes the fingerprint text.
//
// Why the DLL is read FROM DISK and not from the loaded module (this differs from the
// first plan in docs/ROADMAP-v1.5.0.md): by the time the recorder runs the hooks are
// installed, and an inline detour overwrites the first bytes of exactly the functions
// we want to fingerprint with a jmp to our own code. The file on disk still has the
// original bytes, is what the SHA-256 in the file name was computed from, and is also
// what tools/pattern_migrate reads, so recorded and migrated data come from the same
// kind of source.
namespace PatternRecorderLogic {

    // ── whether to record ────────────────────────────────────────────────────────

    enum class Decision {
        Record,
        Disabled,           // [shadow] record = false (the default)
        RequiredMissing,    // a required function did not resolve: the TOML is suspect, record nothing
        NothingResolved,    // nothing to fingerprint
        AlreadyRecorded,    // <sha256>.toml exists: once per Steam build
    };

    struct DecisionInputs {
        bool enabled = false;
        std::size_t requiredMisses = 0;
        std::size_t resolvedCount = 0;
        bool fileExists = false;
    };

    [[nodiscard]] constexpr Decision Decide(const DecisionInputs& in) noexcept
    {
        if (!in.enabled) return Decision::Disabled;
        if (in.requiredMisses != 0) return Decision::RequiredMissing;
        if (in.resolvedCount == 0) return Decision::NothingResolved;
        if (in.fileExists) return Decision::AlreadyRecorded;
        return Decision::Record;
    }

    [[nodiscard]] constexpr std::string_view ToString(Decision d) noexcept
    {
        switch (d) {
            case Decision::Record:          return "record";
            case Decision::Disabled:        return "disabled by [shadow] record";
            case Decision::RequiredMissing: return "a required function is unresolved";
            case Decision::NothingResolved: return "no function resolved";
            case Decision::AlreadyRecorded: return "already recorded for this build";
        }
        return "unknown";
    }

    // The component and SHA-256 become path segments of the output file, so they are
    // checked rather than trusted.
    [[nodiscard]] inline bool IsSafeComponent(std::string_view s) noexcept
    {
        if (s.empty() || s.size() > 32) return false;
        return std::all_of(s.begin(), s.end(), [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
        });
    }

    // Lower-case hex, exactly 64 digits (Hash::Sha256Of's contract).
    [[nodiscard]] inline bool IsSha256Hex(std::string_view s) noexcept
    {
        if (s.size() != 64) return false;
        return std::all_of(s.begin(), s.end(), [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        });
    }

    // ── PE layout (headers only) ─────────────────────────────────────────────────

    // A run of the file that maps to [rva, rva + size).
    struct FileRange {
        std::uint32_t rva = 0;
        std::uint64_t fileOffset = 0;
        std::uint32_t size = 0;
    };

    struct PeLayout {
        FileRange text;     // the ".text" section
        FileRange pdata;    // the exception directory (.pdata): RUNTIME_FUNCTION entries
    };

    // Headers larger than this are not looked at: real DLLs keep the section table in
    // the first KB or two.
    inline constexpr std::size_t kHeaderWindow = 8192;
    // Upper bounds on what the worker will read and allocate for one DLL, several times
    // what the real ones need (steamclient64.dll: ~27 MB file, ~16 MB .text, ~2 MB .pdata).
    inline constexpr std::uint64_t kMaxFileBytes = 256ull << 20;
    inline constexpr std::uint32_t kMaxTextBytes = 64u << 20;
    inline constexpr std::uint32_t kMaxPdataBytes = 32u << 20;

    // The header arithmetic below mixes uint32 fields into size_t sums; on a 32-bit size_t
    // some of them could wrap. This is an x64-only code path.
    static_assert(sizeof(std::size_t) >= 8, "PatternRecorderLogic assumes a 64-bit size_t");

    namespace detail {
        // Bounds-checked little-endian reads; any out-of-range read latches `bad`.
        struct Reader {
            std::span<const std::uint8_t> d;
            bool bad = false;

            [[nodiscard]] std::uint32_t U16(std::size_t off) noexcept
            {
                if (off > d.size() || d.size() - off < 2) { bad = true; return 0; }
                return static_cast<std::uint32_t>(d[off]) | (static_cast<std::uint32_t>(d[off + 1]) << 8);
            }
            [[nodiscard]] std::uint32_t U32(std::size_t off) noexcept
            {
                if (off > d.size() || d.size() - off < 4) { bad = true; return 0; }
                return static_cast<std::uint32_t>(d[off]) | (static_cast<std::uint32_t>(d[off + 1]) << 8) |
                       (static_cast<std::uint32_t>(d[off + 2]) << 16) | (static_cast<std::uint32_t>(d[off + 3]) << 24);
            }
        };

        struct Section {
            std::string name;
            std::uint32_t virtualSize = 0;
            std::uint32_t virtualAddress = 0;
            std::uint32_t rawSize = 0;
            std::uint32_t rawOffset = 0;

            // Bytes of the section that are really in the file.
            [[nodiscard]] std::uint64_t Backed() const noexcept
            {
                return virtualSize != 0 ? std::min(virtualSize, rawSize) : rawSize;
            }
        };

        // [rva, rva+size) inside one section's file-backed bytes and inside the file.
        [[nodiscard]] inline std::optional<FileRange> MapRange(const std::vector<Section>& secs, std::uint32_t rva,
                                                               std::uint32_t size, std::uint64_t fileSize,
                                                               std::uint32_t maxBytes)
        {
            if (size == 0 || size > maxBytes) return std::nullopt;
            for (const Section& s : secs) {
                if (rva < s.virtualAddress) continue;
                const std::uint64_t within = static_cast<std::uint64_t>(rva) - s.virtualAddress;
                if (within + size > s.Backed()) continue;
                const std::uint64_t off = static_cast<std::uint64_t>(s.rawOffset) + within;
                if (off + size > fileSize) return std::nullopt;
                return FileRange{ rva, off, size };
            }
            return std::nullopt;
        }
    } // namespace detail

    // Reads the PE32+ x64 headers in `hdr` (the first bytes of the file) and returns where
    // .text and the exception directory live in the file. Anything unexpected -- not a
    // PE32+ x64 image, truncated or forged fields, ranges outside the file, sizes above
    // the caps, no .text, no exception directory -- gives nullopt: fail closed.
    [[nodiscard]] inline std::optional<PeLayout> ParsePeLayout(std::span<const std::uint8_t> hdr,
                                                               std::uint64_t fileSize)
    {
        detail::Reader r{ hdr };
        if (r.U16(0) != 0x5A4D) return std::nullopt;                      // "MZ"
        const std::size_t pe = r.U32(0x3C);
        if (r.bad || pe < 0x40) return std::nullopt;
        if (r.U32(pe) != 0x00004550) return std::nullopt;                 // "PE\0\0"
        if (r.U16(pe + 4) != 0x8664) return std::nullopt;                 // x64
        const std::size_t sectionCount = r.U16(pe + 6);
        const std::size_t optSize = r.U16(pe + 20);
        const std::size_t opt = pe + 24;
        if (r.U16(opt) != 0x20B) return std::nullopt;                     // PE32+
        if (r.bad || sectionCount == 0 || sectionCount > 96) return std::nullopt;

        // Data directories start at opt+112; the exception directory is entry 3.
        const std::size_t dirCount = r.U32(opt + 108);
        if (r.bad || dirCount <= 3 || optSize < 112 + dirCount * 8) return std::nullopt;
        const std::uint32_t excRva = r.U32(opt + 112 + 3 * 8);
        const std::uint32_t excSize = r.U32(opt + 112 + 3 * 8 + 4);

        std::vector<detail::Section> secs;
        const std::size_t table = opt + optSize;
        for (std::size_t i = 0; i < sectionCount; ++i) {
            const std::size_t base = table + i * 40;
            if (base > hdr.size() || hdr.size() - base < 40) return std::nullopt;
            detail::Section s;
            for (std::size_t k = 0; k < 8 && hdr[base + k] != 0; ++k)
                s.name.push_back(static_cast<char>(hdr[base + k]));
            s.virtualSize = r.U32(base + 8);
            s.virtualAddress = r.U32(base + 12);
            s.rawSize = r.U32(base + 16);
            s.rawOffset = r.U32(base + 20);
            secs.push_back(std::move(s));
        }
        if (r.bad) return std::nullopt;

        const auto text = std::find_if(secs.begin(), secs.end(),
                                       [](const detail::Section& s) { return s.name == ".text"; });
        if (text == secs.end()) return std::nullopt;
        // .text is taken straight from its own section header (not looked up by RVA, which
        // a forged image with overlapping sections could answer with another section).
        const std::uint64_t textBytes = text->Backed();
        if (textBytes == 0 || textBytes > kMaxTextBytes) return std::nullopt;
        if (static_cast<std::uint64_t>(text->rawOffset) + textBytes > fileSize) return std::nullopt;
        if (static_cast<std::uint64_t>(text->virtualAddress) + textBytes > 0xFFFFFFFFull) return std::nullopt;   // RVAs are 32-bit
        const FileRange textRange{ text->virtualAddress, text->rawOffset, static_cast<std::uint32_t>(textBytes) };

        const auto pdataRange = detail::MapRange(secs, excRva, excSize, fileSize, kMaxPdataBytes);
        if (!pdataRange) return std::nullopt;
        return PeLayout{ textRange, *pdataRange };
    }

    // ── recording ────────────────────────────────────────────────────────────────

    struct RecordedSet {
        std::vector<PatternMigrateLogic::FunctionFingerprint> fps;   // sorted by name
        std::vector<std::string> skipped;                            // names whose rva is outside .text
    };

    // Fingerprints every resolved function of `img`. `resolved` is (name, rva) as the
    // hooks resolved them; duplicates by name keep the first. All of them are passed as
    // the "known" set, so a call between two resolved functions becomes an anchor.
    [[nodiscard]] inline RecordedSet RecordFunctions(const PatternMigrateLogic::ImageView& img,
                                                     std::vector<PatternMigrateLogic::NamedRva> resolved)
    {
        std::stable_sort(resolved.begin(), resolved.end(),
                         [](const PatternMigrateLogic::NamedRva& a, const PatternMigrateLogic::NamedRva& b) {
                             return a.name < b.name;
                         });
        resolved.erase(std::unique(resolved.begin(), resolved.end(),
                                   [](const PatternMigrateLogic::NamedRva& a, const PatternMigrateLogic::NamedRva& b) {
                                       return a.name == b.name;
                                   }),
                       resolved.end());

        RecordedSet out;
        for (const PatternMigrateLogic::NamedRva& f : resolved) {
            auto fp = PatternMigrateLogic::BuildFingerprint(img, f.name, f.rva, resolved);
            if (fp) out.fps.push_back(std::move(*fp));
            else out.skipped.push_back(f.name);
        }
        return out;
    }

    // ── fingerprint file ─────────────────────────────────────────────────────────

    inline constexpr int kFileVersion = 1;

    // A TOML basic string: quotes, backslashes and control characters escaped.
    [[nodiscard]] inline std::string TomlQuote(std::string_view s)
    {
        static constexpr char kHex[] = "0123456789ABCDEF";
        std::string out = "\"";
        for (const char ch : s) {
            const auto c = static_cast<unsigned char>(ch);
            if (c == '"') out += "\\\"";
            else if (c == '\\') out += "\\\\";
            else if (c < 0x20 || c == 0x7F) {
                out += "\\u00";
                out += kHex[c >> 4];
                out += kHex[c & 0xF];
            } else out += ch;
        }
        out += '"';
        return out;
    }

    // "48 89 5C ?? ??": the notation of the `sig` field of the pattern TOMLs.
    [[nodiscard]] inline std::string SigString(const std::vector<std::uint8_t>& bytes,
                                               const std::vector<std::uint8_t>& mask)
    {
        static constexpr char kHex[] = "0123456789ABCDEF";
        std::string out;
        for (std::size_t i = 0; i < bytes.size() && i < mask.size(); ++i) {
            if (i != 0) out += ' ';
            if (mask[i] == 0) { out += "??"; continue; }
            out += kHex[bytes[i] >> 4];
            out += kHex[bytes[i] & 0xF];
        }
        return out;
    }

    [[nodiscard]] inline std::string HexRva(std::uint32_t rva)
    {
        static constexpr char kHex[] = "0123456789ABCDEF";
        std::string digits;
        do {
            digits.insert(digits.begin(), kHex[rva & 0xF]);
            rva >>= 4;
        } while (rva != 0);
        return "\"0x" + digits + "\"";
    }

    // The file body. Local only: it holds names, RVAs, masked code bytes and sizes of
    // functions of a public Steam DLL, nothing about the machine, account or time.
    [[nodiscard]] inline std::string SerializeFingerprints(std::string_view component, std::string_view sha256,
                                                           const RecordedSet& set)
    {
        std::string out;
        out += "# AmethystTool shadow-mode fingerprints (v1.5.0, pillar 4).\n";
        out += "# Recorded once per Steam build from the DLL on disk. Local only: never uploaded,\n";
        out += "# never read at runtime. Input for tools/pattern_migrate.\n";
        out += "version = " + std::to_string(kFileVersion) + "\n";
        out += "component = " + TomlQuote(component) + "\n";
        out += "sha256 = " + TomlQuote(sha256) + "\n";
        out += "prefix_len = " + std::to_string(PatternMigrateLogic::kPrefixLen) + "\n";
        if (!set.skipped.empty()) {
            out += "skipped = [";
            for (std::size_t i = 0; i < set.skipped.size(); ++i)
                out += (i ? ", " : "") + TomlQuote(set.skipped[i]);
            out += "]\n";
        }
        for (const PatternMigrateLogic::FunctionFingerprint& fp : set.fps) {
            out += "\n[[function]]\n";
            out += "name = " + TomlQuote(fp.name) + "\n";
            out += "rva = " + HexRva(fp.rva) + "\n";
            out += std::string("pdata = ") + (fp.hasPdata ? "true" : "false") + "\n";
            out += "size = " + std::to_string(fp.funcSize) + "\n";
            out += std::string("complete = ") + (fp.decodeComplete ? "true" : "false") + "\n";
            out += "matches = " + std::to_string(fp.matchCountOld) + "\n";
            out += "sig = " + TomlQuote(SigString(fp.bytes, fp.mask)) + "\n";
            out += "calls = [";
            for (std::size_t i = 0; i < fp.calls.size(); ++i)
                out += (i ? ", " : "") + TomlQuote(fp.calls[i]);
            out += "]\n";
        }
        return out;
    }

} // namespace PatternRecorderLogic
