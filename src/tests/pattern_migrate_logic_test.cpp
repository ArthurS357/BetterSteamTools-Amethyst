// Unit tests for PatternMigrateLogic (v1.5.0, pillar 4): the x64 length decoder and
// masking, .pdata parsing, and the migration rules (unique masked match, at a .pdata
// begin, size within 25%, same known callees). Everything runs on hand-assembled byte
// arrays standing in for .text and .pdata; no Windows API, no real Steam DLL.

#include "Utils/SteamMetadata/PatternMigrateLogic.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

using namespace PatternMigrateLogic;

namespace {

    using Bytes = std::vector<std::uint8_t>;

    constexpr std::uint32_t kTextRva = 0x1000;

    void Put32(Bytes& b, std::size_t off, std::uint32_t v) {
        for (std::size_t i = 0; i < 4; ++i)
            b[off + i] = static_cast<std::uint8_t>(v >> (8 * i));
    }

    // ── a synthetic function ─────────────────────────────────────────
    //
    //   0  48 89 5C 24 08        mov [rsp+8], rbx
    //   5  48 89 74 24 10        mov [rsp+10h], rsi
    //  10  57                    push rdi
    //  11  48 83 EC 20           sub rsp, 20h
    //  15  B8 <seed>             mov eax, seed          (imm32 at 16, fixed)
    //  20  E8 <rel32>            call ...               (rel32 at 21, wildcard)
    //  25  48 8B 05 <disp32>     mov rax, [rip+disp]    (disp32 at 28, wildcard)
    //  32  85 C0                 test eax, eax
    //  34  74 02                 jz +2
    //  36  90 x30                nop padding            -> head = 66 bytes
    //  66  90 x(size-67), C3     tail                   -> size = 66 + tail + 1
    //
    // The first 64 bytes are the fingerprint prefix; 8 of them are wildcards.
    constexpr std::size_t kCallOff = 20;
    constexpr std::size_t kRipDispOff = 28;
    constexpr std::size_t kHeadLen = 66;

    Bytes Func(std::uint32_t seed, std::size_t size) {
        Bytes b = { 0x48, 0x89, 0x5C, 0x24, 0x08,
                    0x48, 0x89, 0x74, 0x24, 0x10,
                    0x57,
                    0x48, 0x83, 0xEC, 0x20,
                    0xB8, 0, 0, 0, 0,
                    0xE8, 0, 0, 0, 0,
                    0x48, 0x8B, 0x05, 0, 0, 0, 0,
                    0x85, 0xC0,
                    0x74, 0x02 };
        Put32(b, 16, seed);
        b.resize(size, 0x90);
        b[size - 1] = 0xC3;
        return b;
    }

    struct Img {
        Bytes text;
        std::vector<RuntimeFunction> pdata;

        // Places `code` at the next 16-byte boundary (int3 padding between functions)
        // and, unless told otherwise, gives it a .pdata entry covering exactly it.
        std::uint32_t Add(const Bytes& code, bool inPdata = true) {
            while (text.size() % 16 != 0) text.push_back(0xCC);
            const std::uint32_t rva = kTextRva + static_cast<std::uint32_t>(text.size());
            text.insert(text.end(), code.begin(), code.end());
            if (inPdata)
                pdata.push_back({ rva, rva + static_cast<std::uint32_t>(code.size()) });
            return rva;
        }

        void PatchCall(std::uint32_t funcRva, std::uint32_t target) {
            const std::size_t off = funcRva - kTextRva + kCallOff + 1;
            Put32(text, off, target - (funcRva + static_cast<std::uint32_t>(kCallOff) + 5));
        }

        void PatchRip(std::uint32_t funcRva, std::uint32_t disp) {
            Put32(text, funcRva - kTextRva + kRipDispOff, disp);
        }

        ImageView View() const {
            ImageView v;
            v.textRva = kTextRva;
            v.text = std::span<const std::uint8_t>(text);
            v.pdata = pdata;
            return v;
        }
    };

    constexpr std::uint32_t kSeedF = 0xF0F0F0F0;
    constexpr std::uint32_t kSeedA = 0xA0A0A0A0;
    constexpr std::uint32_t kSeedB = 0xB0B0B0B0;

    // ── DecodeOne ────────────────────────────────────────────────────

    struct DecodeCase {
        Bytes code;
        std::size_t len;
        std::size_t maskOff;
        std::size_t maskLen;
    };

    TEST(PatternMigrateDecode, LengthsAndMaskedFieldsOfTheEncodingsMsvcEmits) {
        const std::vector<DecodeCase> cases = {
            { { 0x48, 0x89, 0x5C, 0x24, 0x08 }, 5, 0, 0 },                    // mov [rsp+8], rbx (SIB + disp8)
            { { 0x48, 0x83, 0xEC, 0x28 }, 4, 0, 0 },                          // sub rsp, 28h
            { { 0x40, 0x53 }, 2, 0, 0 },                                      // push rbx (bare REX)
            { { 0x41, 0x54 }, 2, 0, 0 },                                      // push r12
            { { 0xE8, 0x10, 0x00, 0x00, 0x00 }, 5, 1, 4 },                    // call rel32
            { { 0xE9, 0x10, 0x00, 0x00, 0x00 }, 5, 1, 4 },                    // jmp rel32
            { { 0x0F, 0x84, 0xFF, 0x00, 0x00, 0x00 }, 6, 2, 4 },              // jz rel32
            { { 0x74, 0x05 }, 2, 0, 0 },                                      // jz rel8: kept
            { { 0x48, 0x8B, 0x05, 0x78, 0x56, 0x34, 0x12 }, 7, 3, 4 },        // mov rax, [rip+d]
            { { 0x48, 0x8D, 0x0D, 0x78, 0x56, 0x34, 0x12 }, 7, 3, 4 },        // lea rcx, [rip+d]
            { { 0xFF, 0x15, 0x00, 0x10, 0x00, 0x00 }, 6, 2, 4 },              // call [rip+d]
            { { 0xF3, 0x0F, 0x10, 0x05, 1, 2, 3, 4 }, 8, 4, 4 },              // movss xmm0, [rip+d]
            { { 0x8B, 0x04, 0x85, 0x00, 0x10, 0x00, 0x00 }, 7, 0, 0 },        // mov eax, [rax*4+disp32]: not RIP-relative
            { { 0xB8, 0x01, 0x00, 0x00, 0x00 }, 5, 0, 0 },                    // mov eax, 1
            { { 0x48, 0xB8, 1, 2, 3, 4, 5, 6, 7, 8 }, 10, 0, 0 },             // mov rax, imm64
            { { 0x66, 0xB8, 0x34, 0x12 }, 4, 0, 0 },                          // mov ax, imm16
            { { 0xC7, 0x44, 0x24, 0x20, 0, 0, 0, 0 }, 8, 0, 0 },              // mov dword [rsp+20h], 0
            { { 0x81, 0xC1, 0x34, 0x12, 0x00, 0x00 }, 6, 0, 0 },              // add ecx, imm32
            { { 0x66, 0x81, 0xC1, 0x34, 0x12 }, 5, 0, 0 },                    // add cx, imm16
            { { 0x6B, 0xC0, 0x03 }, 3, 0, 0 },                                // imul eax, eax, 3
            { { 0x69, 0xC0, 1, 2, 3, 4 }, 6, 0, 0 },                          // imul eax, eax, imm32
            { { 0xF6, 0xC1, 0x01 }, 3, 0, 0 },                                // test cl, 1   (/0 has an imm8)
            { { 0xF6, 0xD1 }, 2, 0, 0 },                                      // not cl       (/2 has none)
            { { 0xF7, 0xC1, 1, 2, 3, 4 }, 6, 0, 0 },                          // test ecx, imm32
            { { 0xF7, 0xD9 }, 2, 0, 0 },                                      // neg ecx
            { { 0x0F, 0xB6, 0xC1 }, 3, 0, 0 },                                // movzx eax, cl
            { { 0x0F, 0x1F, 0x44, 0x00, 0x00 }, 5, 0, 0 },                    // nop dword [rax+rax]
            { { 0x66, 0x0F, 0x1F, 0x84, 0, 0, 0, 0, 0 }, 9, 0, 0 },           // 66 nop word [rax+rax+disp32]
            { { 0xF3, 0x0F, 0x1E, 0xFA }, 4, 0, 0 },                          // endbr64
            { { 0xC2, 0x08, 0x00 }, 3, 0, 0 },                                // ret 8
            { { 0xC3 }, 1, 0, 0 },
            { { 0xCC }, 1, 0, 0 },
            { { 0x0F, 0x05 }, 2, 0, 0 },                                      // syscall
        };
        for (const DecodeCase& c : cases) {
            const Decoded d = DecodeOne(c.code);
            EXPECT_EQ(d.len, c.len) << "first byte 0x" << std::hex << int{ c.code[0] };
            EXPECT_EQ(d.maskLen, c.maskLen) << "first byte 0x" << std::hex << int{ c.code[0] };
            if (c.maskLen != 0)
                EXPECT_EQ(d.maskOff, c.maskOff) << "first byte 0x" << std::hex << int{ c.code[0] };

            // The length comes from the encoding, not from where the buffer ends.
            Bytes more = c.code;
            more.push_back(0x90);
            more.push_back(0x90);
            EXPECT_EQ(DecodeOne(more).len, c.len) << "first byte 0x" << std::hex << int{ c.code[0] };
        }
    }

    TEST(PatternMigrateDecode, OnlyDirectCallAndJmpAreMarkedAsCalls) {
        EXPECT_TRUE(DecodeOne(Bytes{ 0xE8, 0, 0, 0, 0 }).callOrJmp32);
        EXPECT_TRUE(DecodeOne(Bytes{ 0xE9, 0, 0, 0, 0 }).callOrJmp32);
        EXPECT_FALSE(DecodeOne(Bytes{ 0x0F, 0x84, 0, 0, 0, 0 }).callOrJmp32);          // jcc
        EXPECT_FALSE(DecodeOne(Bytes{ 0xFF, 0x15, 0, 0, 0, 0 }).callOrJmp32);          // indirect
        EXPECT_FALSE(DecodeOne(Bytes{ 0x48, 0x8B, 0x05, 0, 0, 0, 0 }).callOrJmp32);
    }

    TEST(PatternMigrateDecode, UnknownOrTruncatedEncodingsYieldNoLength) {
        const std::vector<Bytes> bad = {
            {},
            { 0x48 },                       // REX with nothing after it
            { 0xE8 },                       // call, rel32 missing
            { 0xE8, 0x01, 0x02, 0x03 },     // call, rel32 one byte short
            { 0x48, 0x8B, 0x05, 1, 2 },     // RIP-relative disp32 cut short
            { 0x48, 0x89, 0x5C },           // SIB missing
            { 0x06 },                       // invalid in 64-bit mode
            { 0x0F, 0x38, 0x00, 0xC1 },     // three-byte opcode: not covered
            { 0x0F },                       // escape alone
            { 0xD6 },                       // salc: invalid in 64-bit mode
        };
        for (const Bytes& b : bad)
            EXPECT_EQ(DecodeOne(b).len, 0u) << "size " << b.size();
    }

    // ── MaskPrefix ───────────────────────────────────────────────────

    TEST(PatternMigrateMask, WildcardsCallAndRipDisplacementsAndNothingElse) {
        const Bytes code = Func(kSeedF, 80);
        const MaskedPrefix mp = MaskPrefix(code, kTextRva);

        ASSERT_EQ(mp.bytes.size(), kPrefixLen);
        ASSERT_EQ(mp.mask.size(), kPrefixLen);
        EXPECT_TRUE(mp.complete);
        EXPECT_EQ(CountFixed(mp.mask), kPrefixLen - 8);
        for (std::size_t i = 0; i < kPrefixLen; ++i) {
            const bool wild = (i >= kCallOff + 1 && i < kCallOff + 5) || (i >= kRipDispOff && i < kRipDispOff + 4);
            EXPECT_EQ(mp.mask[i], wild ? 0x00 : 0xFF) << "offset " << i;
            if (!wild) EXPECT_EQ(mp.bytes[i], code[i]) << "offset " << i;
            else EXPECT_EQ(mp.bytes[i], 0) << "offset " << i;
        }
    }

    TEST(PatternMigrateMask, RelativeFieldsDoNotChangeTheMaskedPrefix) {
        Img a, b;
        const std::uint32_t fa = a.Add(Func(kSeedF, 80));
        b.Add(Func(kSeedA, 90));                        // something in front: different rva
        const std::uint32_t fb = b.Add(Func(kSeedF, 80));
        a.PatchCall(fa, 0x1234);  a.PatchRip(fa, 0x1111);
        b.PatchCall(fb, 0x4321);  b.PatchRip(fb, 0x7777);

        const MaskedPrefix ma = MaskPrefix(CodeAt(a.View(), fa, 80), fa);
        const MaskedPrefix mb = MaskPrefix(CodeAt(b.View(), fb, 80), fb);
        EXPECT_EQ(ma.bytes, mb.bytes);
        EXPECT_EQ(ma.mask, mb.mask);
    }

    TEST(PatternMigrateMask, CallTargetsAreAbsoluteRvasForwardAndBackward) {
        // call +0x10 and, right after it, jmp -0x20, at base 0x2000.
        const Bytes code = { 0xE8, 0x10, 0x00, 0x00, 0x00,
                             0xE9, 0xE0, 0xFF, 0xFF, 0xFF };
        const MaskedPrefix mp = MaskPrefix(code, 0x2000);
        ASSERT_EQ(mp.callTargets.size(), 2u);
        EXPECT_EQ(mp.callTargets[0], 0x2000u + 5 + 0x10);
        EXPECT_EQ(mp.callTargets[1], 0x2000u + 10 - 0x20);
    }

    TEST(PatternMigrateMask, StopsBeforeAnEncodingItDoesNotKnow) {
        Bytes code(40, 0x90);
        code[12] = 0x06;                                    // invalid in 64-bit mode
        const MaskedPrefix mp = MaskPrefix(code, kTextRva);
        EXPECT_FALSE(mp.complete);
        EXPECT_EQ(mp.bytes.size(), 12u);                    // never guesses past it
        EXPECT_EQ(mp.mask.size(), 12u);
    }

    TEST(PatternMigrateMask, ShortFunctionGivesAShortCompletePrefix) {
        const Bytes head = Func(kSeedF, 80);
        const Bytes code(head.begin(), head.begin() + 36);   // ends on an instruction boundary
        const MaskedPrefix mp = MaskPrefix(code, kTextRva);
        EXPECT_TRUE(mp.complete);
        EXPECT_EQ(mp.bytes.size(), 36u);
    }

    // ── .pdata ───────────────────────────────────────────────────────

    TEST(PatternMigratePdata, ParsesSortsAndDropsGarbage) {
        Bytes raw(12 * 5 + 5, 0);                            // 5 entries + a partial one
        const auto entry = [&raw](std::size_t i, std::uint32_t b, std::uint32_t e) {
            Put32(raw, i * 12, b);
            Put32(raw, i * 12 + 4, e);
            Put32(raw, i * 12 + 8, 0x5000);                  // unwind info: ignored
        };
        entry(0, 0x2000, 0x2040);
        entry(1, 0, 0);                                      // padding
        entry(2, 0x1000, 0x1010);
        entry(3, 0x3000, 0x3000);                            // empty range
        entry(4, 0x4000, 0x3000);                            // inverted range
        raw[60] = 0xFF;                                      // partial entry: ignored

        const std::vector<RuntimeFunction> p = ParsePdata(raw);
        ASSERT_EQ(p.size(), 2u);
        EXPECT_EQ(p[0].begin, 0x1000u);
        EXPECT_EQ(p[0].end, 0x1010u);
        EXPECT_EQ(p[1].begin, 0x2000u);
        EXPECT_EQ(p[1].end, 0x2040u);
    }

    TEST(PatternMigratePdata, FunctionBeginningAtMatchesOnlyAnExactBegin) {
        const std::vector<RuntimeFunction> p = { { 0x1000, 0x1010 }, { 0x2000, 0x2040 } };
        ASSERT_TRUE(FunctionBeginningAt(p, 0x2000).has_value());
        EXPECT_EQ(FunctionBeginningAt(p, 0x2000)->end, 0x2040u);
        EXPECT_FALSE(FunctionBeginningAt(p, 0x2001).has_value());
        EXPECT_FALSE(FunctionBeginningAt(p, 0x0FFF).has_value());
        EXPECT_FALSE(FunctionBeginningAt(p, 0x3000).has_value());
        EXPECT_FALSE(FunctionBeginningAt({}, 0x1000).has_value());
    }

    // ── BuildFingerprint ─────────────────────────────────────────────

    TEST(PatternMigrateFingerprint, RecordsPrefixSizeAnchorsAndUniqueness) {
        Img old;
        const std::uint32_t a = old.Add(Func(kSeedA, 100));
        const std::uint32_t f = old.Add(Func(kSeedF, 120));
        old.PatchCall(f, a);

        const std::vector<NamedRva> known = { { "A", a }, { "F", f } };
        const auto fp = BuildFingerprint(old.View(), "F", f, known);
        ASSERT_TRUE(fp.has_value());
        EXPECT_EQ(fp->name, "F");
        EXPECT_EQ(fp->rva, f);
        EXPECT_TRUE(fp->hasPdata);
        EXPECT_EQ(fp->funcSize, 120u);
        EXPECT_EQ(fp->bytes.size(), kPrefixLen);
        EXPECT_EQ(CountFixed(fp->mask), kPrefixLen - 8);
        EXPECT_TRUE(fp->decodeComplete);
        EXPECT_EQ(fp->matchCountOld, 1u);
        EXPECT_EQ(fp->calls, std::vector<std::string>{ "A" });
    }

    TEST(PatternMigrateFingerprint, ACallToItselfOrToAnUnknownPlaceIsNotAnAnchor) {
        Img old;
        const std::uint32_t f = old.Add(Func(kSeedF, 120));
        old.PatchCall(f, f);                                 // recursion
        const auto self = BuildFingerprint(old.View(), "F", f, { { "F", f } });
        ASSERT_TRUE(self.has_value());
        EXPECT_TRUE(self->calls.empty());

        old.PatchCall(f, 0x1ABC);                            // to an address nobody named
        const auto unknown = BuildFingerprint(old.View(), "F", f, { { "F", f } });
        ASSERT_TRUE(unknown.has_value());
        EXPECT_TRUE(unknown->calls.empty());
    }

    TEST(PatternMigrateFingerprint, ABuildWithoutAPdataEntryIsRecordedAsSuch) {
        Img old;
        const std::uint32_t f = old.Add(Func(kSeedF, 120), /*inPdata=*/false);
        const auto fp = BuildFingerprint(old.View(), "F", f, {});
        ASSERT_TRUE(fp.has_value());
        EXPECT_FALSE(fp->hasPdata);
        EXPECT_EQ(fp->funcSize, 0u);
        EXPECT_FALSE(fp->bytes.empty());
    }

    TEST(PatternMigrateFingerprint, RvaOutsideTextIsRefused) {
        Img old;
        old.Add(Func(kSeedF, 120));
        EXPECT_FALSE(BuildFingerprint(old.View(), "X", 0x10, {}).has_value());
        EXPECT_FALSE(BuildFingerprint(old.View(), "X", kTextRva + 0x10000, {}).has_value());
    }

    TEST(PatternMigrateFingerprint, ThePrefixIsBoundedByTheFunctionSize) {
        Img old;
        const Bytes head = Func(kSeedF, 80);
        const std::uint32_t f = old.Add(Bytes(head.begin(), head.begin() + 36));    // 36-byte function
        old.Add(Func(kSeedA, 80));                                                  // code right after it
        const auto fp = BuildFingerprint(old.View(), "F", f, {});
        ASSERT_TRUE(fp.has_value());
        EXPECT_EQ(fp->bytes.size(), 36u);
        EXPECT_EQ(fp->funcSize, 36u);
    }

    // ── MigrateOne ───────────────────────────────────────────────────

    // Old build: A and F (F calls A). Returns F's fingerprint.
    FunctionFingerprint RecordF(std::size_t fSize = 100) {
        Img old;
        const std::uint32_t a = old.Add(Func(kSeedA, 100));
        const std::uint32_t f = old.Add(Func(kSeedF, fSize));
        old.PatchCall(f, a);
        old.PatchRip(f, 0x1234);
        return BuildFingerprint(old.View(), "F", f, { { "A", a }, { "F", f } }).value();
    }

    TEST(PatternMigrateOne, FindsTheFunctionAfterItMovedAndItsRelativeFieldsChanged) {
        const FunctionFingerprint fp = RecordF();

        Img nw;
        nw.Add(Func(0x01010101, 70));                        // new code in front: everything shifts
        const std::uint32_t a = nw.Add(Func(kSeedA, 100));
        const std::uint32_t f = nw.Add(Func(kSeedF, 100));
        nw.PatchCall(f, a);
        nw.PatchRip(f, 0x9999);

        const MigrationResult r = MigrateOne(nw.View(), fp, { { "A", a } });
        EXPECT_EQ(r.verdict, Verdict::Migrated);
        EXPECT_EQ(r.rva, f);
        EXPECT_EQ(r.matchCount, 1u);
        EXPECT_EQ(r.newSize, 100u);
        EXPECT_EQ(r.anchorsUnverified, 0u);
    }

    TEST(PatternMigrateOne, TheSameImageGivesBackTheSameRva) {
        Img img;
        const std::uint32_t a = img.Add(Func(kSeedA, 100));
        const std::uint32_t f = img.Add(Func(kSeedF, 100));
        img.PatchCall(f, a);
        const auto fp = BuildFingerprint(img.View(), "F", f, { { "A", a }, { "F", f } }).value();

        const MigrationResult r = MigrateOne(img.View(), fp, { { "A", a } });
        EXPECT_EQ(r.verdict, Verdict::Migrated);
        EXPECT_EQ(r.rva, f);
    }

    TEST(PatternMigrateOne, SizeIsAcceptedUpToExactly25PercentEitherWay) {
        // Recorded size 100: +25 and -25 pass, +26 and -26 do not.
        const FunctionFingerprint fp = RecordF(100);
        const struct { std::size_t size; Verdict want; } cases[] = {
            { 125, Verdict::Migrated },
            { 126, Verdict::SizeMismatch },
            { 75, Verdict::Migrated },
            { 74, Verdict::SizeMismatch },
            { 100, Verdict::Migrated },
        };
        for (const auto& c : cases) {
            Img nw;
            const std::uint32_t a = nw.Add(Func(kSeedA, 100));
            const std::uint32_t f = nw.Add(Func(kSeedF, c.size));
            nw.PatchCall(f, a);
            const MigrationResult r = MigrateOne(nw.View(), fp, { { "A", a } });
            EXPECT_EQ(r.verdict, c.want) << "new size " << c.size;
            EXPECT_EQ(r.newSize, static_cast<std::uint32_t>(c.size));
            EXPECT_EQ(r.rva, f);
        }
    }

    TEST(PatternMigrateOne, AMatchOutsidePdataNeedsAHuman) {
        const FunctionFingerprint fp = RecordF();

        Img none;                                                    // no .pdata entry at all
        const std::uint32_t f1 = none.Add(Func(kSeedF, 100), /*inPdata=*/false);
        MigrationResult r = MigrateOne(none.View(), fp, {});
        EXPECT_EQ(r.verdict, Verdict::NeedsManual);
        EXPECT_EQ(r.reason, ManualReason::NotInPdata);
        EXPECT_EQ(r.rva, f1);                                        // still shown for the maintainer
        ASSERT_EQ(r.candidates.size(), 1u);

        Img inside;                                                  // inside a function, not at its begin
        inside.Add(Func(kSeedA, 100));
        const std::uint32_t f2 = inside.Add(Func(kSeedF, 100), /*inPdata=*/false);
        inside.pdata.push_back({ f2 - 16, f2 + 100 });
        r = MigrateOne(inside.View(), fp, {});
        EXPECT_EQ(r.verdict, Verdict::NeedsManual);
        EXPECT_EQ(r.reason, ManualReason::NotInPdata);
    }

    TEST(PatternMigrateOne, ALeafWithoutRecordedPdataIsNeverAutoAccepted) {
        Img old;
        const std::uint32_t f = old.Add(Func(kSeedF, 100), /*inPdata=*/false);
        const auto fp = BuildFingerprint(old.View(), "F", f, {}).value();

        Img nw;
        const std::uint32_t g = nw.Add(Func(kSeedF, 100));           // new build does have an entry
        const MigrationResult r = MigrateOne(nw.View(), fp, {});
        EXPECT_EQ(r.verdict, Verdict::NeedsManual);
        EXPECT_EQ(r.reason, ManualReason::NoOldPdata);
        EXPECT_EQ(r.rva, g);
    }

    TEST(PatternMigrateOne, AFingerprintWithTooFewFixedBytesIsNotSearchedFor) {
        Img old;
        const std::uint32_t t = old.Add(Bytes{ 0x33, 0xC0, 0xC3 });  // xor eax, eax; ret
        const auto fp = BuildFingerprint(old.View(), "T", t, {}).value();
        ASSERT_LT(CountFixed(fp.mask), kMinFixedBytes);

        const MigrationResult r = MigrateOne(old.View(), fp, {});
        EXPECT_EQ(r.verdict, Verdict::NeedsManual);
        EXPECT_EQ(r.reason, ManualReason::WeakFingerprint);
        EXPECT_EQ(r.matchCount, 0u);
    }

    TEST(PatternMigrateOne, TwoMatchesAreAmbiguousAndBothAreListed) {
        const FunctionFingerprint fp = RecordF();
        Img nw;
        const std::uint32_t a = nw.Add(Func(kSeedA, 100));
        const std::uint32_t f1 = nw.Add(Func(kSeedF, 100));
        const std::uint32_t f2 = nw.Add(Func(kSeedF, 100));
        nw.PatchCall(f1, a);
        nw.PatchCall(f2, a);

        const MigrationResult r = MigrateOne(nw.View(), fp, { { "A", a } });
        EXPECT_EQ(r.verdict, Verdict::Ambiguous);
        EXPECT_EQ(r.matchCount, 2u);
        EXPECT_EQ(r.candidates, (std::vector<std::uint32_t>{ f1, f2 }));
    }

    TEST(PatternMigrateOne, CandidateListIsCappedButTheCountIsExact) {
        const FunctionFingerprint fp = RecordF();
        Img nw;
        for (int i = 0; i < 20; ++i) nw.Add(Func(kSeedF, 100));
        const MigrationResult r = MigrateOne(nw.View(), fp, {});
        EXPECT_EQ(r.verdict, Verdict::Ambiguous);
        EXPECT_EQ(r.matchCount, 20u);
        EXPECT_EQ(r.candidates.size(), kMaxCandidates);
    }

    TEST(PatternMigrateOne, NothingMatchingIsNotFound) {
        const FunctionFingerprint fp = RecordF();
        Img nw;
        nw.Add(Func(kSeedA, 100));
        nw.Add(Func(kSeedB, 100));
        const MigrationResult r = MigrateOne(nw.View(), fp, {});
        EXPECT_EQ(r.verdict, Verdict::NotFound);
        EXPECT_EQ(r.matchCount, 0u);
        EXPECT_TRUE(r.candidates.empty());
    }

    TEST(PatternMigrateOne, ADifferentCalleeRejectsAnOtherwisePerfectMatch) {
        const FunctionFingerprint fp = RecordF();                    // F called A
        Img nw;
        const std::uint32_t a = nw.Add(Func(kSeedA, 100));
        const std::uint32_t b = nw.Add(Func(kSeedB, 100));
        const std::uint32_t g = nw.Add(Func(kSeedF, 100));           // same bytes, same size, calls B
        nw.PatchCall(g, b);

        MigrationResult r = MigrateOne(nw.View(), fp, { { "A", a }, { "B", b } });
        EXPECT_EQ(r.verdict, Verdict::CallsMismatch);
        EXPECT_EQ(r.rva, g);

        // Even with A not migrated yet, a call to some OTHER known function is divergent.
        r = MigrateOne(nw.View(), fp, { { "B", b } });
        EXPECT_EQ(r.verdict, Verdict::CallsMismatch);
    }

    TEST(PatternMigrateOne, TheRightCalleeIsAccepted) {
        const FunctionFingerprint fp = RecordF();
        Img nw;
        const std::uint32_t a = nw.Add(Func(kSeedA, 100));
        const std::uint32_t b = nw.Add(Func(kSeedB, 100));
        const std::uint32_t f = nw.Add(Func(kSeedF, 100));
        nw.PatchCall(f, a);
        const MigrationResult r = MigrateOne(nw.View(), fp, { { "A", a }, { "B", b } });
        EXPECT_EQ(r.verdict, Verdict::Migrated);
    }

    TEST(PatternMigrateOne, CalleesNotMigratedYetAreCountedNotTrusted) {
        const FunctionFingerprint fp = RecordF();                    // F called A
        Img nw;
        const std::uint32_t f = nw.Add(Func(kSeedF, 100));           // calls nothing known
        const MigrationResult r = MigrateOne(nw.View(), fp, {});
        EXPECT_EQ(r.verdict, Verdict::Migrated);
        EXPECT_EQ(r.anchorsUnverified, 1u);
        EXPECT_EQ(r.rva, f);
    }

    // ── MigrateAll ───────────────────────────────────────────────────

    struct OldWorld {
        std::vector<FunctionFingerprint> fps;                        // F first on purpose: order must not matter
    };

    OldWorld RecordWorld(bool recursiveF = false) {
        Img old;
        const std::uint32_t a = old.Add(Func(kSeedA, 100));
        const std::uint32_t b = old.Add(Func(kSeedB, 100));
        const std::uint32_t f = old.Add(Func(kSeedF, 100));
        old.PatchCall(f, recursiveF ? f : a);
        const std::vector<NamedRva> known = { { "A", a }, { "B", b }, { "F", f } };
        OldWorld w;
        w.fps.push_back(BuildFingerprint(old.View(), "F", f, known).value());
        w.fps.push_back(BuildFingerprint(old.View(), "A", a, known).value());
        w.fps.push_back(BuildFingerprint(old.View(), "B", b, known).value());
        return w;
    }

    const NamedMigration& Find(const std::vector<NamedMigration>& v, const std::string& name) {
        for (const NamedMigration& m : v)
            if (m.name == name) return m;
        ADD_FAILURE() << "no result for " << name;
        return v.front();
    }

    TEST(PatternMigrateAll, MigratesAChainWhateverTheOrder) {
        const OldWorld w = RecordWorld();
        Img nw;
        nw.Add(Func(0x01010101, 90));
        const std::uint32_t b = nw.Add(Func(kSeedB, 100));
        const std::uint32_t a = nw.Add(Func(kSeedA, 100));
        const std::uint32_t f = nw.Add(Func(kSeedF, 100));
        nw.PatchCall(f, a);

        const auto res = MigrateAll(nw.View(), w.fps);
        ASSERT_EQ(res.size(), 3u);
        EXPECT_EQ(Find(res, "F").result.verdict, Verdict::Migrated);
        EXPECT_EQ(Find(res, "F").result.rva, f);
        EXPECT_EQ(Find(res, "F").result.anchorsUnverified, 0u);      // verified in the second phase
        EXPECT_EQ(Find(res, "A").result.rva, a);
        EXPECT_EQ(Find(res, "B").result.rva, b);
    }

    TEST(PatternMigrateAll, TheSecondPhaseDemotesAFunctionWhoseCalleesDisagree) {
        const OldWorld w = RecordWorld();                            // F called A
        Img nw;
        const std::uint32_t a = nw.Add(Func(kSeedA, 100));
        const std::uint32_t b = nw.Add(Func(kSeedB, 100));
        const std::uint32_t f = nw.Add(Func(kSeedF, 100));
        nw.PatchCall(f, b);                                          // calls B instead

        const auto res = MigrateAll(nw.View(), w.fps);
        EXPECT_EQ(Find(res, "F").result.verdict, Verdict::CallsMismatch);
        EXPECT_EQ(Find(res, "F").result.rva, f);
        EXPECT_EQ(Find(res, "A").result.verdict, Verdict::Migrated);
        EXPECT_EQ(Find(res, "A").result.rva, a);
        EXPECT_EQ(Find(res, "B").result.verdict, Verdict::Migrated);
    }

    TEST(PatternMigrateAll, ARecursiveFunctionIsNotItsOwnAnchor) {
        const OldWorld w = RecordWorld(/*recursiveF=*/true);
        Img nw;
        nw.Add(Func(kSeedA, 100));
        nw.Add(Func(kSeedB, 100));
        const std::uint32_t f = nw.Add(Func(kSeedF, 100));
        nw.PatchCall(f, f);

        const auto res = MigrateAll(nw.View(), w.fps);
        EXPECT_EQ(Find(res, "F").result.verdict, Verdict::Migrated);
        EXPECT_EQ(Find(res, "F").result.rva, f);
    }

    TEST(PatternMigrateAll, AFunctionTheNewBuildLostDoesNotBreakTheOthers) {
        const OldWorld w = RecordWorld();
        Img nw;
        const std::uint32_t a = nw.Add(Func(kSeedA, 100));
        nw.Add(Func(kSeedB, 100));                                   // F is gone
        const auto res = MigrateAll(nw.View(), w.fps);
        EXPECT_EQ(Find(res, "F").result.verdict, Verdict::NotFound);
        EXPECT_EQ(Find(res, "A").result.verdict, Verdict::Migrated);
        EXPECT_EQ(Find(res, "A").result.rva, a);
    }

    // ── the failure this engine exists to catch ──────────────────────

    TEST(PatternMigrateRegression, ASameSizedNeighbourWithOtherCalleesIsNotAccepted) {
        // The measured error of carrying signatures over (see the roadmap): the recorded
        // function is gone, an unrelated one has the same masked prefix, sits at a .pdata
        // begin and is within 25% of the size. Without the callee check it is accepted.
        const FunctionFingerprint fp = RecordF(76);

        Img nw;
        const std::uint32_t a = nw.Add(Func(kSeedA, 100));
        const std::uint32_t b = nw.Add(Func(kSeedB, 100));
        const std::uint32_t impostor = nw.Add(Func(kSeedF, 79));
        nw.PatchCall(impostor, b);

        // With no anchor information (nothing migrated yet, nothing recorded) the
        // impostor passes every other check: this is the silent error being measured.
        FunctionFingerprint noAnchors = fp;
        noAnchors.calls.clear();
        EXPECT_EQ(MigrateOne(nw.View(), noAnchors, {}).verdict, Verdict::Migrated);

        const MigrationResult r = MigrateOne(nw.View(), fp, { { "A", a }, { "B", b } });
        EXPECT_EQ(r.verdict, Verdict::CallsMismatch);
        EXPECT_EQ(r.rva, impostor);
    }

    // ── names ────────────────────────────────────────────────────────

    TEST(PatternMigrateNames, VerdictAndReasonLabelsAreStable) {
        EXPECT_EQ(ToString(Verdict::Migrated), "migrated");
        EXPECT_EQ(ToString(Verdict::Ambiguous), "ambiguous");
        EXPECT_EQ(ToString(Verdict::NotFound), "not found");
        EXPECT_EQ(ToString(Verdict::SizeMismatch), "size mismatch");
        EXPECT_EQ(ToString(Verdict::CallsMismatch), "calls mismatch");
        EXPECT_EQ(ToString(Verdict::NeedsManual), "needs manual");
        EXPECT_EQ(ToString(ManualReason::None), "");
        EXPECT_NE(ToString(ManualReason::NotInPdata), "");
        EXPECT_NE(ToString(ManualReason::NoOldPdata), "");
        EXPECT_NE(ToString(ManualReason::WeakFingerprint), "");
    }

    TEST(PatternMigrateConstants, MatchTheRoadmap) {
        static_assert(kPrefixLen == 64);
        static_assert(kSizeTolerancePercent == 25);
        static_assert(kMinFixedBytes == 16);
        static_assert(kHeadLen > kPrefixLen);        // the synthetic head covers the whole prefix
        SUCCEED();
    }

} // namespace
