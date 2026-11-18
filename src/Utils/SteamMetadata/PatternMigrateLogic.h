#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Pure engine of pillar 4 (shadow migration), v1.5.0: given what was recorded about
// a function in a Steam build that worked (a "fingerprint"), find the same function
// in a newer build, or say why it cannot be found safely. Header-only, no I/O, no
// threads, no Windows types: the recorder and tools/pattern_migrate feed it byte
// spans, and the tests feed it synthetic images. Plan and rationale:
// docs/ROADMAP-v1.5.0.md, pillar 4.
//
// Nothing here is used at runtime yet. A wrong answer would hook the wrong function
// and can crash Steam, so the engine only ever says "migrated" when ALL of these hold:
//   1. the masked prefix has enough fixed bytes (kMinFixedBytes) to mean something;
//   2. it matches exactly once in .text (more than once is "ambiguous");
//   3. the match starts exactly at a .pdata function begin (never guessed for leaf
//      functions without an unwind entry: those are "needs manual");
//   4. the function size differs from the recorded one by at most 25%;
//   5. the already-migrated functions it calls are exactly the ones the old function
//      called (the "anchors"): a different callee set rejects a same-sized neighbour.
// The caller still reviews the output and the existing H-1/H-1b checks (RVA inside the
// image) apply before any client uses it.
//
// Masking: the bytes of a function prefix that change between builds without the
// function changing are wildcarded: the rel32 of a direct call/jmp (E8/E9) and of a
// near conditional jump (0F 80..8F), and the disp32 of a RIP-relative operand (mod=00
// rm=101, including FF 15 and lea/mov [rip+x]). Everything else, immediates and rel8
// included, stays fixed. Walking the instructions needs lengths, so DecodeOne is a small
// x64 length decoder over the encodings MSVC emits in prologues; on anything it does not
// know it stops and the prefix ends there (MaskedPrefix::complete = false), it never
// guesses a length.
namespace PatternMigrateLogic {

    // Recorded prefix length. The last instruction that starts before it is included
    // whole, so a prefix can be up to 14 bytes longer.
    inline constexpr std::size_t kPrefixLen = 64;
    // Fewer fixed bytes than this match too many places to mean anything.
    inline constexpr std::size_t kMinFixedBytes = 16;
    // MigrationResult::candidates keeps at most this many matches (matchCount is exact).
    inline constexpr std::size_t kMaxCandidates = 16;
    // Largest accepted size change, in percent of the recorded size (inclusive).
    inline constexpr std::uint64_t kSizeTolerancePercent = 25;

    // ── x64 length decoder ───────────────────────────────────────────────────────

    namespace detail {
        inline constexpr int fOk    = 1;     // known encoding
        inline constexpr int fModRM = 2;
        inline constexpr int fImm8  = 4;
        inline constexpr int fImmZ  = 8;     // imm32, imm16 with a 66 prefix
        inline constexpr int fImmV  = 16;    // B8+r: imm32, imm64 with REX.W, imm16 with 66
        inline constexpr int fImm16 = 32;
        inline constexpr int fRel8  = 64;
        inline constexpr int fRel32 = 128;
        inline constexpr int fGrp3b = 256;   // F6: imm8 only for /0 and /1
        inline constexpr int fGrp3z = 512;   // F7: imm32 only for /0 and /1

        // 0 = encoding not covered.
        [[nodiscard]] constexpr int OneByte(std::uint8_t op) noexcept
        {
            if (op < 0x40) {
                switch (op & 7) {
                    case 0: case 1: case 2: case 3: return fOk | fModRM;
                    case 4: return fOk | fImm8;
                    case 5: return fOk | fImmZ;
                    default: return 0;           // invalid in 64-bit mode
                }
            }
            if (op < 0x50) return 0;             // REX is consumed as a prefix; a second one is invalid
            if (op < 0x60) return fOk;           // push/pop r64
            if (op >= 0x70 && op <= 0x7F) return fOk | fRel8;
            if (op >= 0x84 && op <= 0x8B) return fOk | fModRM;
            if (op >= 0x90 && op <= 0x99) return fOk;
            if (op >= 0xB0 && op <= 0xB7) return fOk | fImm8;
            if (op >= 0xB8 && op <= 0xBF) return fOk | fImmV;
            if ((op >= 0xD0 && op <= 0xD3) || (op >= 0xD8 && op <= 0xDF)) return fOk | fModRM;   // shifts, x87
            switch (op) {
                case 0x63: case 0x8C: case 0x8D: case 0x8E: case 0x8F: case 0xFE: case 0xFF:
                    return fOk | fModRM;
                case 0x68: return fOk | fImmZ;
                case 0x69: return fOk | fModRM | fImmZ;
                case 0x6A: return fOk | fImm8;
                case 0x6B: return fOk | fModRM | fImm8;
                case 0x80: case 0x83: return fOk | fModRM | fImm8;
                case 0x81: return fOk | fModRM | fImmZ;
                case 0x9C: case 0x9D: case 0xA4: case 0xA5: case 0xAA: case 0xAB:
                case 0xC3: case 0xC9: case 0xCC:
                    return fOk;
                case 0xA8: return fOk | fImm8;
                case 0xA9: return fOk | fImmZ;
                case 0xC0: case 0xC1: case 0xC6: return fOk | fModRM | fImm8;
                case 0xC2: return fOk | fImm16;
                case 0xC7: return fOk | fModRM | fImmZ;
                case 0xE3: case 0xEB: return fOk | fRel8;
                case 0xE8: case 0xE9: return fOk | fRel32;
                case 0xF6: return fOk | fModRM | fGrp3b;
                case 0xF7: return fOk | fModRM | fGrp3z;
                default: return 0;
            }
        }

        // The byte after 0F. 0F 38 / 0F 3A (three-byte opcodes) are not covered.
        [[nodiscard]] constexpr int TwoByte(std::uint8_t op) noexcept
        {
            if (op >= 0x80 && op <= 0x8F) return fOk | fRel32;
            if (op >= 0x10 && op <= 0x1F) return fOk | fModRM;      // includes the 0F 1F multi-byte nop
            if (op >= 0x28 && op <= 0x2F) return fOk | fModRM;
            if (op >= 0x40 && op <= 0x4F) return fOk | fModRM;
            if (op >= 0x50 && op <= 0x6F) return fOk | fModRM;
            if (op >= 0x70 && op <= 0x73) return fOk | fModRM | fImm8;
            if (op >= 0x74 && op <= 0x76) return fOk | fModRM;
            if (op >= 0x90 && op <= 0x9F) return fOk | fModRM;
            if (op >= 0xC8 && op <= 0xCF) return fOk;               // bswap
            if (op >= 0xD0 && op <= 0xFE) return fOk | fModRM;
            switch (op) {
                case 0x05: case 0x0B: case 0xA2: return fOk;
                case 0x7E: case 0x7F: case 0xA3: case 0xA5: case 0xAB: case 0xAD: case 0xAF:
                case 0xB0: case 0xB1: case 0xB3: case 0xB6: case 0xB7: case 0xBB: case 0xBC:
                case 0xBD: case 0xBE: case 0xBF: case 0xC0: case 0xC1:
                    return fOk | fModRM;
                case 0xA4: case 0xAC: case 0xBA: case 0xC2: case 0xC4: case 0xC5: case 0xC6:
                    return fOk | fModRM | fImm8;
                default: return 0;
            }
        }
    } // namespace detail

    struct Decoded {
        std::size_t len = 0;        // 0 = unknown encoding or truncated: nothing else is valid
        std::size_t maskOff = 0;    // offset (within the instruction) of the wildcarded field
        std::size_t maskLen = 0;    // 0 or 4
        bool callOrJmp32 = false;   // E8 / E9: the wildcarded field is a direct call/jump rel32
    };

    [[nodiscard]] inline Decoded DecodeOne(std::span<const std::uint8_t> c) noexcept
    {
        using namespace detail;
        Decoded d;
        std::size_t pos = 0;

        bool op16 = false;
        while (pos < c.size() && pos < 4) {
            const std::uint8_t b = c[pos];
            if (b == 0x66) op16 = true;
            else if (b != 0x67 && b != 0xF0 && b != 0xF2 && b != 0xF3 && b != 0x2E && b != 0x3E &&
                     b != 0x26 && b != 0x36 && b != 0x64 && b != 0x65)
                break;
            ++pos;
        }
        bool rexW = false;
        if (pos < c.size() && (c[pos] & 0xF0) == 0x40) {
            rexW = (c[pos] & 8) != 0;
            ++pos;
        }
        if (pos >= c.size()) return {};

        const std::uint8_t op = c[pos++];
        bool twoByte = false;
        int fl = 0;
        if (op == 0x0F) {
            if (pos >= c.size()) return {};
            fl = TwoByte(c[pos++]);
            twoByte = true;
        } else {
            fl = OneByte(op);
        }
        if (fl == 0) return {};

        std::size_t modrmReg = 0;
        if (fl & fModRM) {
            if (pos >= c.size()) return {};
            const std::uint8_t m = c[pos++];
            const unsigned mod = static_cast<unsigned>(m >> 6);
            const unsigned rm = static_cast<unsigned>(m & 7);
            modrmReg = static_cast<std::size_t>((m >> 3) & 7);
            std::size_t disp = 0;
            if (mod != 3 && rm == 4) {                       // SIB follows
                if (pos >= c.size()) return {};
                const std::uint8_t sib = c[pos++];
                if (mod == 0 && (sib & 7) == 5) disp = 4;    // no base: disp32, not RIP-relative
            }
            if (mod == 1) {
                disp = 1;
            } else if (mod == 2) {
                disp = 4;
            } else if (mod == 0 && rm == 5) {                // RIP-relative disp32
                disp = 4;
                d.maskOff = pos;
                d.maskLen = 4;
            }
            if (pos + disp > c.size()) return {};
            pos += disp;
        }

        std::size_t imm = 0;
        if (fl & fImm8) imm = 1;
        else if (fl & fImmZ) imm = op16 ? 2 : 4;
        else if (fl & fImmV) imm = rexW ? 8 : (op16 ? 2 : 4);
        else if (fl & fImm16) imm = 2;
        else if ((fl & fGrp3b) && modrmReg < 2) imm = 1;
        else if ((fl & fGrp3z) && modrmReg < 2) imm = op16 ? 2 : 4;
        else if (fl & fRel8) imm = 1;
        else if (fl & fRel32) {
            imm = 4;
            d.maskOff = pos;
            d.maskLen = 4;
            d.callOrJmp32 = !twoByte && (op == 0xE8 || op == 0xE9);
        }
        if (pos + imm > c.size()) return {};
        pos += imm;
        if (pos > 15) return {};                             // longer than any x86 instruction
        d.len = pos;
        return d;
    }

    struct MaskedPrefix {
        std::vector<std::uint8_t> bytes;        // wildcarded positions hold 0
        std::vector<std::uint8_t> mask;         // 0xFF fixed, 0x00 wildcard; same size as bytes
        std::vector<std::uint32_t> callTargets; // RVAs of the direct E8/E9 targets in the prefix
        bool complete = false;                  // false: stopped at an encoding DecodeOne does not know
    };

    // Masks the first `maxLen` bytes of `code` (whole instructions: it may run a little
    // over). `baseRva` is the RVA of code[0], used to turn rel32 into call targets.
    // Pass the function's own bytes (bounded by its .pdata size) so padding after it is
    // never fingerprinted.
    [[nodiscard]] inline MaskedPrefix MaskPrefix(std::span<const std::uint8_t> code, std::uint32_t baseRva,
                                                 std::size_t maxLen = kPrefixLen)
    {
        MaskedPrefix out;
        out.complete = true;
        std::size_t pos = 0;
        while (pos < maxLen && pos < code.size()) {
            const Decoded d = DecodeOne(code.subspan(pos));
            if (d.len == 0) {
                out.complete = false;
                break;
            }
            for (std::size_t i = 0; i < d.len; ++i) {
                const bool wild = d.maskLen != 0 && i >= d.maskOff && i < d.maskOff + d.maskLen;
                out.bytes.push_back(wild ? std::uint8_t{0} : code[pos + i]);
                out.mask.push_back(wild ? std::uint8_t{0x00} : std::uint8_t{0xFF});
            }
            if (d.callOrJmp32) {
                const std::size_t f = pos + d.maskOff;
                const std::uint32_t rel = static_cast<std::uint32_t>(code[f]) |
                                          (static_cast<std::uint32_t>(code[f + 1]) << 8) |
                                          (static_cast<std::uint32_t>(code[f + 2]) << 16) |
                                          (static_cast<std::uint32_t>(code[f + 3]) << 24);
                out.callTargets.push_back(baseRva + static_cast<std::uint32_t>(pos + d.len) + rel);
            }
            pos += d.len;
        }
        return out;
    }

    [[nodiscard]] inline std::size_t CountFixed(const std::vector<std::uint8_t>& mask) noexcept
    {
        return static_cast<std::size_t>(std::count_if(mask.begin(), mask.end(),
                                                      [](std::uint8_t m) { return m != 0; }));
    }

    // ── image view (.text and .pdata) ────────────────────────────────────────────

    struct RuntimeFunction {
        std::uint32_t begin = 0;    // RVA, inclusive
        std::uint32_t end = 0;      // RVA, exclusive
    };

    // Parses a raw .pdata section: 12-byte RUNTIME_FUNCTION entries (begin, end, unwind).
    // Entries with begin == 0 or begin >= end (padding, terminators, garbage) and a
    // trailing partial entry are dropped. Sorted by begin; on a duplicate begin the first
    // is kept. Chained unwind info is not followed: each entry stands on its own.
    [[nodiscard]] inline std::vector<RuntimeFunction> ParsePdata(std::span<const std::uint8_t> raw)
    {
        constexpr std::size_t kEntry = 12;
        const auto rd = [&raw](std::size_t off) {
            return static_cast<std::uint32_t>(raw[off]) |
                   (static_cast<std::uint32_t>(raw[off + 1]) << 8) |
                   (static_cast<std::uint32_t>(raw[off + 2]) << 16) |
                   (static_cast<std::uint32_t>(raw[off + 3]) << 24);
        };
        std::vector<RuntimeFunction> out;
        out.reserve(raw.size() / kEntry);
        for (std::size_t off = 0; off + kEntry <= raw.size(); off += kEntry) {
            const RuntimeFunction f{ rd(off), rd(off + 4) };
            if (f.begin == 0 || f.begin >= f.end) continue;
            out.push_back(f);
        }
        std::stable_sort(out.begin(), out.end(),
                         [](const RuntimeFunction& a, const RuntimeFunction& b) { return a.begin < b.begin; });
        out.erase(std::unique(out.begin(), out.end(),
                              [](const RuntimeFunction& a, const RuntimeFunction& b) { return a.begin == b.begin; }),
                  out.end());
        return out;
    }

    // `pdata` must be sorted by begin (ParsePdata does it).
    [[nodiscard]] inline std::optional<RuntimeFunction> FunctionBeginningAt(
        const std::vector<RuntimeFunction>& pdata, std::uint32_t rva)
    {
        const auto it = std::lower_bound(pdata.begin(), pdata.end(), rva,
                                         [](const RuntimeFunction& f, std::uint32_t v) { return f.begin < v; });
        if (it == pdata.end() || it->begin != rva) return std::nullopt;
        return *it;
    }

    struct ImageView {
        std::uint32_t textRva = 0;                  // RVA of text[0]
        std::span<const std::uint8_t> text;         // the .text section
        std::vector<RuntimeFunction> pdata;         // sorted by begin
    };

    // Up to `maxLen` bytes of .text starting at `rva`; empty when `rva` is outside it.
    [[nodiscard]] inline std::span<const std::uint8_t> CodeAt(const ImageView& img, std::uint32_t rva,
                                                              std::size_t maxLen)
    {
        if (rva < img.textRva) return {};
        const std::size_t off = static_cast<std::size_t>(rva - img.textRva);
        if (off >= img.text.size()) return {};
        return img.text.subspan(off, std::min(maxLen, img.text.size() - off));
    }

    // ── fingerprint, record and match ────────────────────────────────────────────

    struct NamedRva {
        std::string name;
        std::uint32_t rva = 0;
    };

    // What is recorded about one function in a build that worked.
    struct FunctionFingerprint {
        std::string name;
        std::uint32_t rva = 0;                  // in the recorded build
        bool hasPdata = false;                  // a .pdata entry begins at rva
        std::uint32_t funcSize = 0;             // from that entry; 0 when !hasPdata
        std::vector<std::uint8_t> bytes;        // masked prefix (see MaskPrefix)
        std::vector<std::uint8_t> mask;
        std::vector<std::string> calls;         // known functions called inside the prefix; sorted, unique
        std::uint32_t matchCountOld = 0;        // matches of the prefix in the recorded build (1 = unique)
        bool decodeComplete = false;
    };

    struct ScanResult {
        std::size_t count = 0;                  // exact
        std::vector<std::uint32_t> rvas;        // the first kMaxCandidates
    };

    // Every offset of .text where the masked pattern matches. A pattern with no fixed
    // byte matches everywhere and is refused (empty result).
    [[nodiscard]] inline ScanResult ScanText(const ImageView& img, const std::vector<std::uint8_t>& bytes,
                                             const std::vector<std::uint8_t>& mask)
    {
        ScanResult r;
        const std::size_t n = bytes.size();
        if (n == 0 || n != mask.size() || img.text.size() < n) return r;
        std::size_t f0 = 0;
        while (f0 < n && mask[f0] == 0) ++f0;
        if (f0 == n) return r;
        const std::size_t last = img.text.size() - n;
        for (std::size_t i = 0; i <= last; ++i) {
            if ((img.text[i + f0] & mask[f0]) != (bytes[f0] & mask[f0])) continue;
            bool ok = true;
            for (std::size_t k = 0; k < n && ok; ++k)
                ok = (img.text[i + k] & mask[k]) == (bytes[k] & mask[k]);
            if (!ok) continue;
            ++r.count;
            if (r.rvas.size() < kMaxCandidates) r.rvas.push_back(img.textRva + static_cast<std::uint32_t>(i));
        }
        return r;
    }

    // Names, from `known`, of the functions that `targets` point at. Sorted, unique,
    // `self` excluded (a recursive call must not count as an anchor).
    [[nodiscard]] inline std::vector<std::string> NamesOfTargets(const std::vector<std::uint32_t>& targets,
                                                                 const std::vector<NamedRva>& known,
                                                                 std::string_view self)
    {
        std::vector<std::string> out;
        for (const std::uint32_t t : targets)
            for (const NamedRva& k : known)
                if (k.rva == t && k.name != self) out.push_back(k.name);
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
        return out;
    }

    // Records one function of the image that is running. `known` is every resolved
    // function of that image (name, rva): the ones called inside the prefix become the
    // anchors. nullopt when `rva` is outside .text.
    [[nodiscard]] inline std::optional<FunctionFingerprint> BuildFingerprint(
        const ImageView& img, std::string name, std::uint32_t rva, const std::vector<NamedRva>& known)
    {
        FunctionFingerprint fp;
        const std::span<const std::uint8_t> probe = CodeAt(img, rva, 1);
        if (probe.empty()) return std::nullopt;

        const std::optional<RuntimeFunction> fn = FunctionBeginningAt(img.pdata, rva);
        fp.hasPdata = fn.has_value();
        fp.funcSize = fn ? fn->end - fn->begin : 0;
        // With a .pdata entry the function's own bytes bound the prefix. Without one the
        // end is unknown: take the plain prefix; the result is "needs manual" anyway.
        const std::size_t limit = fn ? static_cast<std::size_t>(fp.funcSize) : kPrefixLen + 15;
        const MaskedPrefix mp = MaskPrefix(CodeAt(img, rva, limit), rva, kPrefixLen);

        fp.name = std::move(name);
        fp.rva = rva;
        fp.bytes = mp.bytes;
        fp.mask = mp.mask;
        fp.decodeComplete = mp.complete;
        fp.calls = NamesOfTargets(mp.callTargets, known, fp.name);
        fp.matchCountOld = static_cast<std::uint32_t>(ScanText(img, fp.bytes, fp.mask).count);
        return fp;
    }

    enum class Verdict {
        Migrated,
        Ambiguous,      // the masked prefix matches more than once
        NotFound,       // no match
        SizeMismatch,   // unique match at a .pdata begin, size more than 25% off
        CallsMismatch,  // unique match, but it calls different known functions
        NeedsManual,    // see ManualReason; a human decides
    };

    enum class ManualReason {
        None,
        WeakFingerprint,    // fewer than kMinFixedBytes fixed bytes
        NoOldPdata,         // the recorded function had no .pdata entry: size cannot be checked
        NotInPdata,         // the match does not start at a .pdata function begin
    };

    struct MigrationResult {
        Verdict verdict = Verdict::NotFound;
        ManualReason reason = ManualReason::None;
        std::uint32_t rva = 0;                      // the single match, when there is one
        std::uint32_t newSize = 0;                  // from .pdata, when checked
        std::size_t matchCount = 0;
        std::vector<std::uint32_t> candidates;      // up to kMaxCandidates, for a human to look at
        std::size_t anchorsUnverified = 0;          // recorded callees that were not migrated yet
    };

    [[nodiscard]] constexpr std::string_view ToString(Verdict v) noexcept
    {
        switch (v) {
            case Verdict::Migrated:      return "migrated";
            case Verdict::Ambiguous:     return "ambiguous";
            case Verdict::NotFound:      return "not found";
            case Verdict::SizeMismatch:  return "size mismatch";
            case Verdict::CallsMismatch: return "calls mismatch";
            case Verdict::NeedsManual:   return "needs manual";
        }
        return "unknown";
    }

    [[nodiscard]] constexpr std::string_view ToString(ManualReason r) noexcept
    {
        switch (r) {
            case ManualReason::None:            return "";
            case ManualReason::WeakFingerprint: return "fingerprint too short";
            case ManualReason::NoOldPdata:      return "no .pdata entry in the recorded build";
            case ManualReason::NotInPdata:      return "match is not at a .pdata function begin";
        }
        return "unknown";
    }

    // One function against one image. `knownNew` is what has already been migrated in
    // `img` (name, new rva); it decides the anchor check.
    [[nodiscard]] inline MigrationResult MigrateOne(const ImageView& img, const FunctionFingerprint& fp,
                                                    const std::vector<NamedRva>& knownNew)
    {
        MigrationResult r;
        if (fp.bytes.size() != fp.mask.size() || CountFixed(fp.mask) < kMinFixedBytes) {
            r.verdict = Verdict::NeedsManual;
            r.reason = ManualReason::WeakFingerprint;
            return r;
        }

        const ScanResult s = ScanText(img, fp.bytes, fp.mask);
        r.matchCount = s.count;
        r.candidates = s.rvas;
        if (s.count == 0) {
            r.verdict = Verdict::NotFound;
            return r;
        }
        if (s.count > 1) {
            r.verdict = Verdict::Ambiguous;
            return r;
        }
        r.rva = s.rvas.front();

        if (!fp.hasPdata || fp.funcSize == 0) {
            r.verdict = Verdict::NeedsManual;
            r.reason = ManualReason::NoOldPdata;
            return r;
        }
        const std::optional<RuntimeFunction> fn = FunctionBeginningAt(img.pdata, r.rva);
        if (!fn) {
            r.verdict = Verdict::NeedsManual;
            r.reason = ManualReason::NotInPdata;
            return r;
        }

        r.newSize = fn->end - fn->begin;
        const std::uint64_t oldSize = fp.funcSize;
        const std::uint64_t diff = r.newSize > oldSize ? r.newSize - oldSize : oldSize - r.newSize;
        if (diff * 100 > oldSize * kSizeTolerancePercent) {
            r.verdict = Verdict::SizeMismatch;
            return r;
        }

        // Anchors: the functions already migrated that the recorded one called must be
        // exactly the ones the candidate calls. Callees not migrated yet cannot be
        // checked; they are counted, not trusted.
        const MaskedPrefix cand = MaskPrefix(CodeAt(img, r.rva, r.newSize), r.rva, fp.bytes.size());
        const std::vector<std::string> observed = NamesOfTargets(cand.callTargets, knownNew, fp.name);
        std::vector<std::string> expected;
        for (const std::string& callee : fp.calls) {
            const bool isKnown = std::any_of(knownNew.begin(), knownNew.end(), [&](const NamedRva& k) {
                return k.name == callee && k.name != fp.name;
            });
            if (isKnown) expected.push_back(callee);
            else ++r.anchorsUnverified;
        }
        std::sort(expected.begin(), expected.end());
        if (expected != observed) {
            r.verdict = Verdict::CallsMismatch;
            return r;
        }

        r.verdict = Verdict::Migrated;
        return r;
    }

    struct NamedMigration {
        std::string name;
        MigrationResult result;
    };

    // Every fingerprint against one image, in two phases so the order of `fps` does not
    // matter. Phase 1 migrates each function on its own (anchors not yet known).
    // Phase 2 re-checks every migrated function against the full migrated set and demotes
    // the ones whose callees disagree. A function wrongly migrated in phase 1 can make a
    // correct neighbour look divergent in phase 2: that errs towards "not migrated",
    // never towards accepting more.
    [[nodiscard]] inline std::vector<NamedMigration> MigrateAll(const ImageView& img,
                                                                const std::vector<FunctionFingerprint>& fps)
    {
        std::vector<NamedMigration> out(fps.size());
        std::vector<NamedRva> known;
        for (std::size_t i = 0; i < fps.size(); ++i) {
            out[i].name = fps[i].name;
            out[i].result = MigrateOne(img, fps[i], {});
            if (out[i].result.verdict == Verdict::Migrated)
                known.push_back({ fps[i].name, out[i].result.rva });
        }
        for (std::size_t i = 0; i < fps.size(); ++i) {
            if (out[i].result.verdict != Verdict::Migrated) continue;
            MigrationResult again = MigrateOne(img, fps[i], known);
            if (again.verdict != Verdict::Migrated) out[i].result = std::move(again);
            else out[i].result.anchorsUnverified = again.anchorsUnverified;
        }
        return out;
    }

} // namespace PatternMigrateLogic
