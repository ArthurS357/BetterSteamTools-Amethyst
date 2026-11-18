// Unit tests for PatternRecorderLogic (v1.5.0, pillar 4): when to record, the PE layout
// parser (including forged and truncated headers), recording a set of functions, and
// the text of the fingerprint file. Header-only logic on synthetic images; no disk, no
// Windows API.

#include "Utils/SteamMetadata/PatternRecorderLogic.h"
#include "synthetic_image.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

using namespace PatternRecorderLogic;
using namespace SyntheticImage;
using PatternMigrateLogic::NamedRva;

namespace {

    constexpr std::uint32_t kSeedF = 0xF0F0F0F0;
    constexpr std::uint32_t kSeedA = 0xA0A0A0A0;

    const std::string kSha(64, 'a');

    // ── Decide ───────────────────────────────────────────────────────

    TEST(PatternRecorderDecide, RecordsOnlyWhenEnabledCompleteNonEmptyAndNotYetRecorded) {
        DecisionInputs in;
        in.enabled = true;
        in.resolvedCount = 12;
        EXPECT_EQ(Decide(in), Decision::Record);

        in.fileExists = true;
        EXPECT_EQ(Decide(in), Decision::AlreadyRecorded);
        in.fileExists = false;

        in.resolvedCount = 0;
        EXPECT_EQ(Decide(in), Decision::NothingResolved);
        in.resolvedCount = 12;

        in.requiredMisses = 1;
        EXPECT_EQ(Decide(in), Decision::RequiredMissing);
        in.requiredMisses = 0;

        in.enabled = false;
        EXPECT_EQ(Decide(in), Decision::Disabled);
    }

    TEST(PatternRecorderDecide, DisabledWinsOverEveryOtherReason) {
        DecisionInputs in;                     // enabled = false by default: the opt-in
        in.requiredMisses = 3;
        in.resolvedCount = 0;
        in.fileExists = true;
        EXPECT_EQ(Decide(in), Decision::Disabled);
    }

    TEST(PatternRecorderDecide, ARequiredMissBeatsAnExistingFile) {
        DecisionInputs in;
        in.enabled = true;
        in.requiredMisses = 1;
        in.resolvedCount = 5;
        in.fileExists = true;
        EXPECT_EQ(Decide(in), Decision::RequiredMissing);
    }

    TEST(PatternRecorderDecide, EveryDecisionHasALabel) {
        std::vector<std::string> labels;
        for (const Decision d : { Decision::Record, Decision::Disabled, Decision::RequiredMissing,
                                  Decision::NothingResolved, Decision::AlreadyRecorded })
            labels.emplace_back(ToString(d));
        for (std::size_t i = 0; i < labels.size(); ++i) {
            EXPECT_FALSE(labels[i].empty());
            for (std::size_t j = i + 1; j < labels.size(); ++j)
                EXPECT_NE(labels[i], labels[j]);                     // distinct: they are what the log shows
        }
    }

    // ── path-segment checks ──────────────────────────────────────────

    TEST(PatternRecorderNames, ComponentMustBeALowerCaseIdentifier) {
        EXPECT_TRUE(IsSafeComponent("steamclient"));
        EXPECT_TRUE(IsSafeComponent("steamui"));
        EXPECT_TRUE(IsSafeComponent("a_1"));
        EXPECT_FALSE(IsSafeComponent(""));
        EXPECT_FALSE(IsSafeComponent("../x"));
        EXPECT_FALSE(IsSafeComponent("a/b"));
        EXPECT_FALSE(IsSafeComponent("a\\b"));
        EXPECT_FALSE(IsSafeComponent("Steam"));
        EXPECT_FALSE(IsSafeComponent("a b"));
        EXPECT_FALSE(IsSafeComponent(std::string(33, 'a')));
    }

    TEST(PatternRecorderNames, Sha256MustBeExactly64LowerCaseHexDigits) {
        EXPECT_TRUE(IsSha256Hex(kSha));
        EXPECT_TRUE(IsSha256Hex("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"));
        EXPECT_FALSE(IsSha256Hex(""));
        EXPECT_FALSE(IsSha256Hex(std::string(63, 'a')));
        EXPECT_FALSE(IsSha256Hex(std::string(65, 'a')));
        EXPECT_FALSE(IsSha256Hex(std::string(64, 'A')));              // upper case
        EXPECT_FALSE(IsSha256Hex(std::string(63, 'a') + "g"));
        EXPECT_FALSE(IsSha256Hex(std::string(63, 'a') + "/"));
        EXPECT_FALSE(IsSha256Hex(std::string(60, 'a') + "..\\x"));
    }

    // ── ParsePeLayout ────────────────────────────────────────────────

    Img TwoFunctions() {
        Img img;
        img.Add(Func(kSeedA, 100));
        img.Add(Func(kSeedF, 120));
        return img;
    }

    TEST(PatternRecorderPe, FindsTextAndTheExceptionDirectoryInTheFile) {
        const Img img = TwoFunctions();
        const Bytes file = BuildPeFile(img);
        const auto layout = ParsePeLayout(file, file.size());
        ASSERT_TRUE(layout.has_value());

        EXPECT_EQ(layout->text.rva, kTextRva);
        EXPECT_EQ(layout->text.fileOffset, kRawStart);
        EXPECT_EQ(layout->text.size, img.text.size());

        EXPECT_EQ(layout->pdata.rva, PdataRvaOf(img));
        EXPECT_EQ(layout->pdata.fileOffset, kRawStart + AlignUp(img.text.size(), 0x200));
        EXPECT_EQ(layout->pdata.size, img.pdata.size() * 12);
    }

    TEST(PatternRecorderPe, OnlyTheHeadersAreNeeded) {
        const Bytes file = BuildPeFile(TwoFunctions());
        const std::span<const std::uint8_t> head(file.data(), kHeadersEnd);
        EXPECT_TRUE(ParsePeLayout(head, file.size()).has_value());
    }

    TEST(PatternRecorderPe, EveryTruncationOfTheHeadersIsRefused) {
        const Bytes file = BuildPeFile(TwoFunctions());
        for (std::size_t n = 0; n < kHeadersEnd; ++n)
            EXPECT_FALSE(ParsePeLayout(std::span<const std::uint8_t>(file.data(), n), file.size()).has_value())
                << "first " << n << " bytes";
    }

    TEST(PatternRecorderPe, NotAPeX64ImageIsRefused) {
        const Bytes good = BuildPeFile(TwoFunctions());
        const auto refuses = [&good](auto&& tamper, const char* what) {
            Bytes f = good;
            tamper(f);
            EXPECT_FALSE(ParsePeLayout(f, f.size()).has_value()) << what;
        };
        refuses([](Bytes& f) { f[0] = 'X'; }, "no MZ");
        refuses([](Bytes& f) { Put32(f, 0x3C, 0x10); }, "e_lfanew inside the DOS header");
        refuses([](Bytes& f) { Put32(f, 0x3C, 0x00FFFFF0); }, "e_lfanew beyond the headers");
        refuses([](Bytes& f) { Put32(f, kPeOffset, 0x00004551); }, "no PE signature");
        refuses([](Bytes& f) { Put16(f, kPeOffset + 4, 0x014C); }, "x86 machine");
        refuses([](Bytes& f) { Put16(f, kOptOffset, 0x10B); }, "PE32, not PE32+");
        refuses([](Bytes& f) { Put16(f, kPeOffset + 6, 0); }, "no sections");
        refuses([](Bytes& f) { Put16(f, kPeOffset + 6, 97); }, "absurd section count");
        refuses([](Bytes& f) { Put32(f, kOptOffset + 108, 3); }, "no exception directory entry");
        refuses([](Bytes& f) { Put16(f, kPeOffset + 20, 100); }, "optional header too small for its directories");
        refuses([](Bytes& f) { f[kSectionTable] = '.'; f[kSectionTable + 1] = 'x'; }, "no .text");
    }

    TEST(PatternRecorderPe, RangesThatDoNotFitTheFileAreRefused) {
        const Bytes file = BuildPeFile(TwoFunctions());

        // The file is shorter than the section data it claims.
        EXPECT_FALSE(ParsePeLayout(file, kRawStart + 16).has_value());
        EXPECT_FALSE(ParsePeLayout(file, kRawStart + AlignUp(TwoFunctions().text.size(), 0x200)).has_value());  // .pdata cut off

        // .pdata claims more than its section has.
        Bytes f = file;
        Put32(f, kOptOffset + 112 + 3 * 8 + 4, 0x00100000);
        EXPECT_FALSE(ParsePeLayout(f, f.size()).has_value());

        // .pdata RVA points outside every section.
        f = file;
        Put32(f, kOptOffset + 112 + 3 * 8, 0x00700000);
        EXPECT_FALSE(ParsePeLayout(f, f.size()).has_value());

        // An empty exception directory.
        f = file;
        Put32(f, kOptOffset + 112 + 3 * 8 + 4, 0);
        EXPECT_FALSE(ParsePeLayout(f, f.size()).has_value());
    }

    TEST(PatternRecorderPe, FileSizeBoundaryIsExact) {
        const Img img = TwoFunctions();
        const Bytes file = BuildPeFile(img);
        const std::uint64_t textRaw = AlignUp(img.text.size(), 0x200);
        const std::uint64_t pdataEnd = kRawStart + textRaw + img.pdata.size() * 12;   // last byte actually needed

        EXPECT_TRUE(ParsePeLayout(file, pdataEnd).has_value());
        EXPECT_FALSE(ParsePeLayout(file, pdataEnd - 1).has_value());

        const std::uint64_t textEnd = kRawStart + img.text.size();                    // .text alone
        EXPECT_FALSE(ParsePeLayout(file, textEnd).has_value());                       // .pdata still does not fit
    }

    TEST(PatternRecorderPe, OffsetsAndCountsNearTheTypeLimitsDoNotWrapIntoAcceptance) {
        const Bytes file = BuildPeFile(TwoFunctions());
        const auto refuses = [&file](auto&& tamper, std::uint64_t fileSize, const char* what) {
            Bytes f = file;
            tamper(f);
            EXPECT_FALSE(ParsePeLayout(f, fileSize).has_value()) << what;
        };
        refuses([](Bytes& f) { Put32(f, kSectionTable + 20, 0xFFFFFFF0u); }, file.size(), ".text rawOffset near 2^32");
        refuses([](Bytes& f) { Put32(f, kSectionTable + 40 + 20, 0xFFFFFFF0u); }, file.size(), ".pdata rawOffset near 2^32");
        refuses([](Bytes& f) { Put32(f, kSectionTable + 12, 0xFFFFFFF0u); }, file.size(), ".text VA near 2^32");
        refuses([](Bytes& f) { Put32(f, kOptOffset + 112 + 3 * 8, 0xFFFFFFF0u); }, file.size(), "exception RVA near 2^32");
        refuses([](Bytes& f) { Put32(f, kOptOffset + 112 + 3 * 8 + 4, 0xFFFFFFFFu); }, file.size(), "exception size 4 GB");
        refuses([](Bytes& f) { Put32(f, kOptOffset + 108, 0xFFFFFFFFu); }, file.size(), "4 billion data directories");
        refuses([](Bytes& f) { Put32(f, 0x3C, 0xFFFFFFF0u); }, 1ull << 33, "e_lfanew near 2^32 in a huge file");
        refuses([](Bytes& f) { Put32(f, kSectionTable + 16, 0xFFFFFFFFu); Put32(f, kSectionTable + 8, 0xFFFFFFFFu); },
                1ull << 33, ".text of 4 GB");
    }

    TEST(PatternRecorderPe, SizesAboveTheCapsAreRefusedNotAllocated) {
        const Bytes file = BuildPeFile(TwoFunctions());
        constexpr std::uint64_t kBigFile = 1ull << 33;

        Bytes f = file;                                             // .text of 256 MB
        Put32(f, kSectionTable + 8, 0x10000000);
        Put32(f, kSectionTable + 16, 0x10000000);
        EXPECT_FALSE(ParsePeLayout(f, kBigFile).has_value());

        f = file;                                                   // .pdata of 128 MB
        Put32(f, kSectionTable + 40 + 8, 0x08000000);
        Put32(f, kSectionTable + 40 + 16, 0x08000000);
        Put32(f, kOptOffset + 112 + 3 * 8 + 4, 0x08000000);
        EXPECT_FALSE(ParsePeLayout(f, kBigFile).has_value());
    }

    // ── RecordFunctions ──────────────────────────────────────────────

    TEST(PatternRecorderRecord, FingerprintsEveryResolvedFunctionSortedByName) {
        Img img;
        const std::uint32_t a = img.Add(Func(kSeedA, 100));
        const std::uint32_t f = img.Add(Func(kSeedF, 120));
        img.PatchCall(f, a);

        const RecordedSet set = RecordFunctions(img.View(), { { "Zeta", f }, { "Alpha", a } });
        ASSERT_EQ(set.fps.size(), 2u);
        EXPECT_TRUE(set.skipped.empty());
        EXPECT_EQ(set.fps[0].name, "Alpha");
        EXPECT_EQ(set.fps[1].name, "Zeta");
        EXPECT_EQ(set.fps[1].rva, f);
        EXPECT_EQ(set.fps[1].calls, std::vector<std::string>{ "Alpha" });   // anchor between two resolved functions
        EXPECT_TRUE(set.fps[0].calls.empty());
    }

    TEST(PatternRecorderRecord, ADuplicateNameKeepsTheFirstAndAnRvaOutsideTextIsSkipped) {
        Img img;
        const std::uint32_t a = img.Add(Func(kSeedA, 100));
        const std::uint32_t f = img.Add(Func(kSeedF, 120));

        const RecordedSet set = RecordFunctions(img.View(), { { "F", f }, { "A", a }, { "F", a }, { "Gone", 0x10 } });
        ASSERT_EQ(set.fps.size(), 2u);
        EXPECT_EQ(set.fps[1].name, "F");
        EXPECT_EQ(set.fps[1].rva, f);                       // not the later duplicate
        EXPECT_EQ(set.skipped, std::vector<std::string>{ "Gone" });
    }

    TEST(PatternRecorderRecord, TheInputOrderDoesNotChangeTheResult) {
        Img img;
        const std::uint32_t a = img.Add(Func(kSeedA, 100));
        const std::uint32_t f = img.Add(Func(kSeedF, 120));
        const std::string one = SerializeFingerprints("steamclient", kSha,
                                                      RecordFunctions(img.View(), { { "A", a }, { "F", f } }));
        const std::string two = SerializeFingerprints("steamclient", kSha,
                                                      RecordFunctions(img.View(), { { "F", f }, { "A", a } }));
        EXPECT_EQ(one, two);
    }

    // ── file text ────────────────────────────────────────────────────

    TEST(PatternRecorderText, QuotingEscapesWhatBreaksATomlString) {
        EXPECT_EQ(TomlQuote("Plain"), "\"Plain\"");
        EXPECT_EQ(TomlQuote("a\"b"), "\"a\\\"b\"");
        EXPECT_EQ(TomlQuote("a\\b"), "\"a\\\\b\"");
        EXPECT_EQ(TomlQuote(std::string("a\nb")), "\"a\\u000Ab\"");
        EXPECT_EQ(TomlQuote(std::string("a") + '\0' + "b"), "\"a\\u0000b\"");
        EXPECT_EQ(TomlQuote("\x7F"), "\"\\u007F\"");
        EXPECT_EQ(TomlQuote(""), "\"\"");
    }

    TEST(PatternRecorderText, SigUsesThePatternTomlNotationWithWildcards) {
        EXPECT_EQ(SigString({ 0x48, 0x00, 0xAB }, { 0xFF, 0x00, 0xFF }), "48 ?? AB");
        EXPECT_EQ(SigString({}, {}), "");
        EXPECT_EQ(SigString({ 0x0F }, { 0xFF }), "0F");
    }

    TEST(PatternRecorderText, RvaIsAnUpperCaseHexString) {
        EXPECT_EQ(HexRva(0), "\"0x0\"");
        EXPECT_EQ(HexRva(0x4D75D0), "\"0x4D75D0\"");
        EXPECT_EQ(HexRva(0xFFFFFFFF), "\"0xFFFFFFFF\"");
    }

    TEST(PatternRecorderText, TheFileCarriesTheKeysAFutureToolReads) {
        Img img;
        const std::uint32_t a = img.Add(Func(kSeedA, 100));
        const std::uint32_t f = img.Add(Func(kSeedF, 120));
        img.PatchCall(f, a);
        const std::string text = SerializeFingerprints(
            "steamclient", kSha, RecordFunctions(img.View(), { { "A", a }, { "F", f }, { "Gone", 0x10 } }));

        const auto has = [&text](const std::string& s) { return text.find(s) != std::string::npos; };
        EXPECT_TRUE(has("version = 1\n"));
        EXPECT_TRUE(has("component = \"steamclient\"\n"));
        EXPECT_TRUE(has("sha256 = \"" + kSha + "\"\n"));
        EXPECT_TRUE(has("prefix_len = 64\n"));
        EXPECT_TRUE(has("skipped = [\"Gone\"]\n"));
        EXPECT_TRUE(has("\n[[function]]\nname = \"F\"\n"));
        EXPECT_TRUE(has("rva = \"0x1070\"\n"));
        EXPECT_TRUE(has("pdata = true\n"));
        EXPECT_TRUE(has("size = 120\n"));
        EXPECT_TRUE(has("complete = true\n"));
        EXPECT_TRUE(has("matches = 1\n"));
        EXPECT_TRUE(has("calls = [\"A\"]\n"));
        // the masked prefix: call rel32 and RIP-relative disp32 are wildcards, the seed is not
        EXPECT_TRUE(has("B8 F0 F0 F0 F0 E8 ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? 85 C0 74 02"));
    }

    TEST(PatternRecorderText, ItHoldsNothingAboutTheMachine) {
        Img img;
        const std::uint32_t f = img.Add(Func(kSeedF, 100));
        const std::string text = SerializeFingerprints("steamui", kSha, RecordFunctions(img.View(), { { "F", f } }));
        for (const char* needle : { ":\\", "C:", "D:", "Users", "steam.exe", ".dll", "@" })
            EXPECT_EQ(text.find(needle), std::string::npos) << needle;
    }

} // namespace
