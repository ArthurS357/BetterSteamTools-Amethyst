// Unit tests for VersionPinLogic.h -- the appmanifest reader and pin writer behind
// tools/version_pin (v1.5.0). Pure logic plus <filesystem>, so it also runs in the
// Linux logic-test job under ASan + UBSan.
//
// The .acf below follows the layout Steam writes (checked against real manifests):
// tabs, CRLF, "\\" escapes in values, an InstalledDepots block whose DLC entries
// carry an extra dlcappid, and a SharedDepots block that maps a depot to another app.

#include "Utils/SteamMetadata/VersionPinLogic.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace VersionPinLogic;
namespace fs = std::filesystem;

namespace {

    // LF text -> CRLF, as Steam writes it on Windows.
    std::string Crlf(std::string_view lf) {
        std::string out;
        for (char c : lf) {
            if (c == '\n') out.push_back('\r');
            out.push_back(c);
        }
        return out;
    }

    const std::string kSampleLf =
        "\"AppState\"\n"
        "{\n"
        "\t\"appid\"\t\t\"3357650\"\n"
        "\t\"Universe\"\t\t\"1\"\n"
        "\t\"LauncherPath\"\t\t\"C:\\\\steam\\\\steam.exe\"\n"
        "\t\"name\"\t\t\"Sample Game\"\n"
        "\t\"buildid\"\t\t\"20987654\"\n"
        "\t\"InstalledDepots\"\n"
        "\t{\n"
        "\t\t\"3357652\"\n"
        "\t\t{\n"
        "\t\t\t\"manifest\"\t\t\"2727486347555616762\"\n"
        "\t\t\t\"size\"\t\t\"877605834\"\n"
        "\t\t}\n"
        "\t\t\"3357651\"\n"
        "\t\t{\n"
        "\t\t\t\"manifest\"\t\t\"7378614952147005398\"\n"
        "\t\t\t\"size\"\t\t\"35981673740\"\n"
        "\t\t}\n"
        "\t\t\"3859920\"\n"
        "\t\t{\n"
        "\t\t\t\"manifest\"\t\t\"4731286747379700304\"\n"
        "\t\t\t\"size\"\t\t\"90058268\"\n"
        "\t\t\t\"dlcappid\"\t\t\"3859920\"\n"
        "\t\t}\n"
        "\t}\n"
        "\t\"SharedDepots\"\n"
        "\t{\n"
        "\t\t\"228984\"\t\t\"228980\"\n"
        "\t}\n"
        "\t\"UserConfig\"\n"
        "\t{\n"
        "\t\t\"language\"\t\t\"english\"\n"
        "\t}\n"
        "}\n";

    const std::string kSample = Crlf(kSampleLf);

    // A one-block appmanifest with the given InstalledDepots body (the depot entries).
    std::string AcfWith(std::string_view depots, std::string_view appid = "\"appid\" \"100\"") {
        return "\"AppState\" { " + std::string(appid) + " \"InstalledDepots\" { " + std::string(depots) + " } }";
    }

    ExtractResult Extract(std::string_view text) {
        const KvParse kv = ParseKv(text);
        EXPECT_TRUE(kv.ok) << kv.error;
        return ExtractPins(kv.nodes);
    }

    bool Contains(const std::string& haystack, std::string_view needle) { return haystack.find(needle) != std::string::npos; }

    std::vector<std::string> Lines(const std::string& text) {
        std::vector<std::string> lines;
        std::istringstream in(text);
        for (std::string l; std::getline(in, l);) lines.push_back(l);
        return lines;
    }

} // namespace

// ── KeyValues parser ────────────────────────────────────────────

TEST(VersionPinKv, ParsesNestedBlocksAndValues) {
    const KvParse kv = ParseKv("\"a\" { \"b\" \"1\" \"c\" { \"d\" \"2\" } }");
    ASSERT_TRUE(kv.ok) << kv.error;
    ASSERT_EQ(kv.nodes.size(), 1u);
    const KvNode& a = kv.nodes[0];
    EXPECT_TRUE(a.isBlock);
    ASSERT_EQ(a.children.size(), 2u);
    EXPECT_EQ(a.children[0].key, "b");
    EXPECT_EQ(a.children[0].value, "1");
    EXPECT_EQ(a.children[1].children[0].value, "2");
}

TEST(VersionPinKv, ToleratesBomCrlfTabsAndComments) {
    const std::string text = "\xEF\xBB\xBF// header comment\r\n\"a\"\t{\r\n\t// inside\r\n\t\"b\"\t\t\"1\" // trailing\r\n}\r\n";
    const KvParse kv = ParseKv(text);
    ASSERT_TRUE(kv.ok) << kv.error;
    EXPECT_EQ(kv.nodes[0].children[0].value, "1");
}

TEST(VersionPinKv, SlashesInsideAStringAreNotAComment) {
    const KvParse kv = ParseKv("\"url\" \"http://example.com//x\" \"k\" \"v\"");
    ASSERT_TRUE(kv.ok) << kv.error;
    ASSERT_EQ(kv.nodes.size(), 2u);
    EXPECT_EQ(kv.nodes[0].value, "http://example.com//x");
}

TEST(VersionPinKv, DecodesEscapesAndKeepsUnknownOnes) {
    const KvParse kv = ParseKv(R"("p" "C:\\steam\\x" "q" "say \"hi\"" "n" "a\nb\tc" "u" "\q")");
    ASSERT_TRUE(kv.ok) << kv.error;
    EXPECT_EQ(kv.nodes[0].value, "C:\\steam\\x");
    EXPECT_EQ(kv.nodes[1].value, "say \"hi\"");
    EXPECT_EQ(kv.nodes[2].value, "a\nb\tc");
    EXPECT_EQ(kv.nodes[3].value, "\\q");
}

TEST(VersionPinKv, AcceptsBareTokensAndEmptyBlocks) {
    const KvParse kv = ParseKv("key value other { } last { \"x\" y }");
    ASSERT_TRUE(kv.ok) << kv.error;
    ASSERT_EQ(kv.nodes.size(), 3u);
    EXPECT_EQ(kv.nodes[0].value, "value");
    EXPECT_TRUE(kv.nodes[1].isBlock);
    EXPECT_TRUE(kv.nodes[1].children.empty());
    EXPECT_EQ(kv.nodes[2].children[0].value, "y");
}

TEST(VersionPinKv, AStringMayContainANewline) {
    const KvParse kv = ParseKv("\"a\" \"line1\nline2\"");
    ASSERT_TRUE(kv.ok) << kv.error;
    EXPECT_EQ(kv.nodes[0].value, "line1\nline2");
}

TEST(VersionPinKv, EmptyAndCommentOnlyInputParseToNothing) {
    EXPECT_TRUE(ParseKv("").ok);
    EXPECT_TRUE(ParseKv("").nodes.empty());
    EXPECT_TRUE(ParseKv("  // nothing here\r\n").nodes.empty());
}

TEST(VersionPinKv, KeysAreMatchedCaseInsensitively) {
    const KvParse kv = ParseKv("\"InstalledDepots\" { }");
    ASSERT_TRUE(kv.ok);
    EXPECT_NE(FindChild(kv.nodes, "installeddepots"), nullptr);
    EXPECT_EQ(FindChild(kv.nodes, "missing"), nullptr);
}

TEST(VersionPinKv, MalformedInputIsAnErrorWithAnOffset) {
    const std::vector<std::pair<std::string, std::string>> cases = {
        { "\"a\" \"unterminated",       "unterminated string" },
        { "\"a\" {",                    "end of file inside a block" },
        { "\"a\" { \"b\" \"1\"",        "end of file inside a block" },
        { "}",                          "unmatched '}'" },
        { "\"a\" \"1\" }",              "unmatched '}'" },
        { "{ \"a\" \"1\" }",            "'{' without a key" },
        { "\"a\"",                      "has no value" },
        { "\"a\" { \"b\" }",            "has no value" },
        { "\"a\" \"\\",                 "unterminated string" },
    };
    for (const auto& [text, expected] : cases) {
        const KvParse kv = ParseKv(text);
        EXPECT_FALSE(kv.ok) << text;
        EXPECT_TRUE(Contains(kv.error, expected)) << text << " -> " << kv.error;
        EXPECT_TRUE(Contains(kv.error, "at byte")) << kv.error;
        EXPECT_TRUE(kv.nodes.empty()) << "no partial result for: " << text;
    }
}

TEST(VersionPinKv, ErrorTextNeverEchoesControlCharacters) {
    const KvParse kv = ParseKv("\"bad\x01key\x1b[31m\"");
    ASSERT_FALSE(kv.ok);
    for (char c : kv.error) EXPECT_TRUE(c >= 0x20 && c < 0x7F) << static_cast<int>(c);
}

TEST(VersionPinKv, NestingIsBoundedWithoutOverflowingTheStack) {
    std::string deep;
    for (int i = 0; i < kMaxNesting; ++i) deep += "\"k\" { ";
    deep += "\"v\" \"1\" ";
    for (int i = 0; i < kMaxNesting; ++i) deep += "} ";
    EXPECT_TRUE(ParseKv(deep).ok);

    std::string tooDeep;
    for (int i = 0; i < kMaxNesting + 1; ++i) tooDeep += "\"k\" { ";
    const KvParse kv = ParseKv(tooDeep);
    EXPECT_FALSE(kv.ok);
    EXPECT_TRUE(Contains(kv.error, "nesting deeper"));

    std::string huge;                // would recurse 100k deep without the bound
    for (int i = 0; i < 100000; ++i) huge += "\"k\"{";
    EXPECT_FALSE(ParseKv(huge).ok);
}

TEST(VersionPinKv, AnOversizedInputIsRefused) {
    const std::string big(static_cast<std::size_t>(kMaxAcfBytes) + 1, ' ');
    const KvParse kv = ParseKv(big);
    EXPECT_FALSE(kv.ok);
    EXPECT_TRUE(Contains(kv.error, "larger than"));
}

TEST(VersionPinKv, EveryTruncationOfARealManifestIsAnError) {
    const std::size_t complete = kSample.rfind('}') + 1;   // the closing brace of AppState
    ASSERT_GT(complete, 100u);
    for (std::size_t len = 1; len < complete; ++len) {
        const KvParse kv = ParseKv(std::string_view(kSample).substr(0, len));
        ASSERT_FALSE(kv.ok) << "a " << len << "-byte prefix of " << complete << " parsed as complete";
    }
    // From the last brace on, only trailing whitespace is missing.
    for (std::size_t len = complete; len <= kSample.size(); ++len)
        ASSERT_TRUE(ParseKv(std::string_view(kSample).substr(0, len)).ok) << len;
}

// ── InstalledDepots extraction ──────────────────────────────────

TEST(VersionPinExtract, ReadsTheInstalledDepotsOfARealLayout) {
    for (const std::string& text : { kSampleLf, kSample }) {   // LF and CRLF
        const ExtractResult r = Extract(text);
        ASSERT_TRUE(r.ok) << r.error;
        EXPECT_EQ(r.pins.appId, 3357650u);
        EXPECT_EQ(r.pins.name, "Sample Game");
        ASSERT_EQ(r.pins.depots.size(), 3u);
        // Sorted by depot id, whatever the order in the file.
        EXPECT_EQ(r.pins.depots[0].depotId, 3357651u);
        EXPECT_EQ(r.pins.depots[0].manifestId, 7378614952147005398ull);
        EXPECT_EQ(r.pins.depots[1].depotId, 3357652u);
        EXPECT_EQ(r.pins.depots[2].depotId, 3859920u);   // the DLC depot is pinned too
        EXPECT_EQ(r.pins.skipped, 0u);
    }
}

TEST(VersionPinExtract, SharedDepotsAreNotPins) {
    const ExtractResult r = Extract(kSampleLf);
    ASSERT_TRUE(r.ok);
    for (const DepotPin& d : r.pins.depots) EXPECT_NE(d.depotId, 228984u);
}

TEST(VersionPinExtract, ABlockThatIsNotAnAppManifestIsRefused) {
    const ExtractResult r = Extract("\"libraryfolders\" { \"0\" { \"path\" \"D:\\\\steam\" } }");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(Contains(r.error, "no AppState"));
    EXPECT_FALSE(Extract("").ok);
}

TEST(VersionPinExtract, AppidMustBeAPositiveNumber) {
    for (const char* appid : { "\"appid\" \"0\"", "\"appid\" \"abc\"", "\"appid\" \"-5\"", "\"appid\" \"4294967296\"", "\"appid\" { }", "" }) {
        const ExtractResult r = Extract(AcfWith("\"1\" { \"manifest\" \"2\" }", appid));
        EXPECT_FALSE(r.ok) << appid;
        EXPECT_TRUE(Contains(r.error, "appid")) << r.error;
    }
}

TEST(VersionPinExtract, NoInstalledDepotsBlockIsClear) {
    const ExtractResult r = Extract("\"AppState\" { \"appid\" \"100\" \"name\" \"x\" }");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(Contains(r.error, "no InstalledDepots"));
}

TEST(VersionPinExtract, AnEmptyInstalledDepotsBlockIsClear) {
    const ExtractResult r = Extract(AcfWith(""));
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(Contains(r.error, "InstalledDepots is empty"));
}

TEST(VersionPinExtract, UnusableEntriesAreSkippedAndCounted) {
    const ExtractResult r = Extract(AcfWith(
        "\"10\" { \"manifest\" \"111\" } "
        "\"abc\" { \"manifest\" \"222\" } "           // depot id is not a number
        "\"11\" { \"manifest\" \"12x\" } "            // manifest is not a number
        "\"12\" { \"manifest\" \"0\" } "              // 0 is no manifest
        "\"13\" { \"size\" \"5\" } "                  // no manifest key
        "\"14\" \"loose\" "                           // not a block
        "\"4294967296\" { \"manifest\" \"1\" } "      // depot id over 32 bits
        "\"15\" { \"manifest\" \"18446744073709551616\" } "   // manifest over 64 bits
        "\"16\" { \"manifest\" \"-3\" }"));
    ASSERT_TRUE(r.ok) << r.error;
    ASSERT_EQ(r.pins.depots.size(), 1u);
    EXPECT_EQ(r.pins.depots[0].depotId, 10u);
    EXPECT_EQ(r.pins.skipped, 8u);
    EXPECT_EQ(r.pins.skippedNotes.size(), 8u);
}

TEST(VersionPinExtract, EverythingUnusableIsAnError) {
    const ExtractResult r = Extract(AcfWith("\"10\" { \"manifest\" \"x\" } \"11\" { }"));
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(Contains(r.error, "no usable depot (2 skipped)"));
}

TEST(VersionPinExtract, ADuplicatedDepotKeepsTheFirstEntry) {
    const ExtractResult r = Extract(AcfWith("\"10\" { \"manifest\" \"111\" } \"10\" { \"manifest\" \"222\" }"));
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.pins.depots.size(), 1u);
    EXPECT_EQ(r.pins.depots[0].manifestId, 111u);
    EXPECT_EQ(r.pins.skipped, 1u);
}

TEST(VersionPinExtract, TheLargestManifestIdSurvives) {
    const ExtractResult r = Extract(AcfWith("\"4294967295\" { \"manifest\" \"18446744073709551615\" }"));
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(r.pins.depots[0].depotId, 4294967295u);
    EXPECT_EQ(r.pins.depots[0].manifestId, 18446744073709551615ull);
}

TEST(VersionPinExtract, KeyCaseDoesNotMatter) {
    const ExtractResult r = Extract("\"appstate\" { \"APPID\" \"7\" \"installeddepots\" { \"1\" { \"MANIFEST\" \"9\" } } }");
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(r.pins.appId, 7u);
}

TEST(VersionPinExtract, SkippedNotesAreBounded) {
    std::string depots;
    for (int i = 0; i < 100; ++i) depots += "\"x" + std::to_string(i) + "\" { } ";
    depots += "\"5\" { \"manifest\" \"6\" }";
    const ExtractResult r = Extract(AcfWith(depots));
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.pins.skipped, 100u);
    EXPECT_EQ(r.pins.skippedNotes.size(), kMaxNotes);
}

// ── Lua output ──────────────────────────────────────────────────

TEST(VersionPinLua, WritesExactlyWhatTheEngineParses) {
    AppPins pins;
    pins.appId = 100;
    pins.name = "Test Game";
    pins.depots = { { 101, 5173085471687919135ull }, { 102, 18446744073709551615ull } };
    EXPECT_EQ(BuildPinLua(pins),
              "-- AmethystTool version_pin: generated file, safe to delete.\n"
              "-- Pins the installed manifest of each depot of app 100 (Test Game).\n"
              "-- Source: appmanifest_100.acf. Delete this file to follow Steam's updates again.\n"
              "-- If another .lua file here also calls setManifestid for the same depot, the one parsed last wins.\n"
              "setManifestid(101, \"5173085471687919135\")\n"
              "setManifestid(102, \"18446744073709551615\")\n");
}

TEST(VersionPinLua, StartsWithTheMarkerAndHasOneCallPerDepot) {
    const ExtractResult r = Extract(kSampleLf);
    ASSERT_TRUE(r.ok);
    const std::string lua = BuildPinLua(r.pins);
    EXPECT_TRUE(lua.starts_with(kMarker));
    EXPECT_TRUE(IsGeneratedPinFile(lua));
    EXPECT_EQ(std::ranges::count_if(Lines(lua), [](const std::string& l) { return l.starts_with("setManifestid("); }), 3);
}

TEST(VersionPinLua, AHostileGameNameCannotAddAStatement) {
    AppPins pins;
    pins.appId = 100;
    pins.depots = { { 1, 2 } };
    for (const char* name : { "X\nos.execute('calc')\n--", "X\r\nio.popen('calc')", "]]os.exit()--[[", "a\"b'c\\d", "\x01\x1b[2J",
                              "\xE3\x82\xB2\xE3\x83\xBC\xE3\x83\xA0" }) {
        pins.name = name;
        const std::string lua = BuildPinLua(pins);
        for (const std::string& line : Lines(lua)) {
            const bool comment = line.starts_with("--");
            const bool call = line.starts_with("setManifestid(");
            EXPECT_TRUE(comment || call) << "unexpected line for name " << name << ": " << line;
            if (comment) EXPECT_EQ(line.find('\r'), std::string::npos) << line;
        }
        EXPECT_EQ(Lines(lua).size(), 5u) << name;   // marker, 2 info comments, note, 1 call... never more
    }
}

TEST(VersionPinLua, SafeCommentKeepsAFixedAlphabetAndLength) {
    EXPECT_EQ(SafeComment("Half-Life 2: Episode One"), "Half-Life 2: Episode One");
    EXPECT_EQ(SafeComment("a\nb"), "a?b");
    EXPECT_EQ(SafeComment("[[x]]"), "??x??");
    EXPECT_EQ(SafeComment(std::string(500, 'a')).size(), 64u);
    EXPECT_EQ(SafeComment(""), "");
}

TEST(VersionPinLua, NoNameMeansNoParentheses) {
    AppPins pins;
    pins.appId = 7;
    pins.depots = { { 1, 2 } };
    EXPECT_FALSE(Contains(BuildPinLua(pins), " ("));   // "app 7 (Name)" is the only place a space meets a parenthesis
}

// ── Ownership marker ────────────────────────────────────────────

TEST(VersionPinOwnership, OnlyTheMarkerMakesAFileOurs) {
    EXPECT_TRUE(IsGeneratedPinFile(std::string(kMarker) + "\nsetManifestid(1, \"2\")\n"));
    EXPECT_TRUE(IsGeneratedPinFile("\xEF\xBB\xBF" + std::string(kMarker) + "\r\n"));
    EXPECT_FALSE(IsGeneratedPinFile(""));
    EXPECT_FALSE(IsGeneratedPinFile("addappid(1)\n"));
    EXPECT_FALSE(IsGeneratedPinFile("\n" + std::string(kMarker)));            // not the first line
    EXPECT_FALSE(IsGeneratedPinFile("-- Generated with Luie\nsetManifestid(1, \"2\")\n"));
}

// ── Conflicts with other .lua files ─────────────────────────────

TEST(VersionPinConflicts, ADifferentGidForTheSameDepotIsReported) {
    const std::vector<DepotPin> pins = { { 101, 1000 }, { 102, 2000 } };
    const auto found = FindConflicts("addappid(101, 1, \"abc\")\nsetManifestid(101, \"999\", 5)\nsetManifestid(102, \"2000\")\n", pins);
    ASSERT_EQ(found.size(), 1u);
    EXPECT_EQ(found[0].depotId, 101u);
    EXPECT_EQ(found[0].otherManifestId, 999u);
}

TEST(VersionPinConflicts, TheSameGidOrAnotherDepotIsNotAConflict) {
    const std::vector<DepotPin> pins = { { 101, 1000 } };
    EXPECT_TRUE(FindConflicts("setManifestid(101, \"1000\")\n", pins).empty());
    EXPECT_TRUE(FindConflicts("setManifestid(555, \"1\")\n", pins).empty());
    EXPECT_TRUE(FindConflicts("", pins).empty());
}

TEST(VersionPinConflicts, MatchesLikeTheLuaEngineDoes) {
    const std::vector<DepotPin> pins = { { 1, 10 } };
    EXPECT_EQ(FindConflicts("SETMANIFESTID ( 1 , '11' )", pins).size(), 1u);          // case-insensitive, spaces, single quotes
    EXPECT_EQ(FindConflicts("x = 1; setManifestid(1,\"12\")", pins).size(), 1u);       // not only at the line start
    EXPECT_EQ(FindConflicts("setManifestid(1,\"13\") setManifestid(1,\"14\")", pins).size(), 2u);
    EXPECT_EQ(FindConflicts("setManifestid(1, \"15\")\r\nsetManifestid(1, \"16\")\r\n", pins).size(), 2u);
}

TEST(VersionPinConflicts, ABareNumberGidIsACallToo) {
    // lua_setManifestid takes anything lua_isstring accepts, and that includes numbers.
    const std::vector<DepotPin> pins = { { 1, 10 } };
    ASSERT_EQ(FindConflicts("setManifestid(1, 11)", pins).size(), 1u);
    EXPECT_EQ(FindConflicts("setManifestid(1, 11)", pins)[0].otherManifestId, 11u);
    EXPECT_EQ(FindConflicts("setManifestid(1, 12, 5000)", pins).size(), 1u);    // with the ignored size
    EXPECT_EQ(FindConflicts("setManifestid(1, \"13\", 5000)", pins).size(), 1u);
    EXPECT_TRUE(FindConflicts("setManifestid(1, 10)", pins).empty());           // same gid
}

TEST(VersionPinConflicts, CommentsAndMalformedCallsAreIgnored) {
    const std::vector<DepotPin> pins = { { 1, 10 } };
    for (const char* text : { "-- setManifestid(1, \"11\")", "setManifestid(1, \"11\"", "setManifestid(1, 11", "setManifestid(x, \"11\")",
                              "setManifestid(1, \"11')", "setManifestid 1, \"11\"", "setManifestid(\"1\", \"11\")",
                              "setManifestid(1, \"\")", "setManifestid(1, \"1a\")", "setManifestid(1, 11x)",
                              "setManifestid(1, \"11\" 5)", "setManifestid(1,", "setManifestid(" })
        EXPECT_TRUE(FindConflicts(text, pins).empty()) << text;
}

// ── Numbers ─────────────────────────────────────────────────────

TEST(VersionPinNumbers, DecimalDigitsOnlyAndNoOverflow) {
    EXPECT_EQ(ParseDecimal<std::uint32_t>("5"), 5u);
    EXPECT_EQ(ParseDecimal<std::uint32_t>("007"), 7u);                      // leading zeros are the same number
    EXPECT_EQ(ParseDecimal<std::uint32_t>("4294967295"), 4294967295u);
    EXPECT_EQ(ParseDecimal<std::uint64_t>("18446744073709551615"), 18446744073709551615ull);
    for (const char* bad : { "", "+5", "-5", "-0", " 5", "5 ", "5x", "0x10", "1.5", "4294967296" })
        EXPECT_FALSE(ParseDecimal<std::uint32_t>(bad).has_value()) << bad;
    EXPECT_FALSE(ParseDecimal<std::uint64_t>("18446744073709551616").has_value());
}

// ── What to do with the target file ─────────────────────────────

TEST(VersionPinDecide, TruthTable) {
    EXPECT_EQ(DecideWrite(false, false, false, false), WriteAction::Create);
    EXPECT_EQ(DecideWrite(false, false, false, true),  WriteAction::Create);
    EXPECT_EQ(DecideWrite(true, true, true, false),    WriteAction::UpToDate);   // nothing to change, no --force needed
    EXPECT_EQ(DecideWrite(true, true, true, true),     WriteAction::UpToDate);
    EXPECT_EQ(DecideWrite(true, true, false, false),   WriteAction::RefuseOwn);
    EXPECT_EQ(DecideWrite(true, true, false, true),    WriteAction::Replace);
    EXPECT_EQ(DecideWrite(true, false, false, false),  WriteAction::RefuseForeign);
    EXPECT_EQ(DecideWrite(true, false, false, true),   WriteAction::Replace);
}

// ── The whole job on disk ───────────────────────────────────────

class VersionPinDisk : public ::testing::Test {
protected:
    void SetUp() override {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        root_ = fs::temp_directory_path() / (std::string("amethyst_versionpin_") + info->name());
        fs::remove_all(root_);
        steamapps_ = root_ / "steamapps";
        out_ = root_ / "stplug-in";
        fs::create_directories(steamapps_);
        fs::create_directories(out_);
        opt_.outDir = out_;
        opt_.processTag = "t";
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(root_, ec);
    }

    fs::path WriteAcf(std::string_view text, const std::string& name = "appmanifest_3357650.acf") {
        const fs::path p = steamapps_ / name;
        std::ofstream(p, std::ios::binary | std::ios::trunc) << text;
        return p;
    }
    void WriteFile(const fs::path& p, std::string_view text) { std::ofstream(p, std::ios::binary | std::ios::trunc) << text; }
    std::string ReadFile(const fs::path& p) { return ReadSmallFile(p, 1 << 20).value_or("<unreadable>"); }

    std::vector<std::string> OutDirNames() {
        std::vector<std::string> names;
        for (const auto& e : fs::directory_iterator(out_)) names.push_back(e.path().filename().string());
        std::ranges::sort(names);
        return names;
    }

    fs::path root_, steamapps_, out_;
    PinOptions opt_;
};

TEST_F(VersionPinDisk, WritesThePinFileForARealLayout) {
    const PinReport r = PinAcf(WriteAcf(kSample), opt_);
    ASSERT_EQ(r.status, PinStatus::Written) << r.message;
    EXPECT_EQ(r.appId, 3357650u);
    EXPECT_EQ(r.depots, 3u);
    EXPECT_EQ(r.file, out_ / "3357650.pin.lua");
    const std::string onDisk = ReadFile(r.file);
    EXPECT_EQ(onDisk, r.lua);
    EXPECT_TRUE(Contains(onDisk, "setManifestid(3357651, \"7378614952147005398\")\n"));
    EXPECT_EQ(OutDirNames(), std::vector<std::string>{ "3357650.pin.lua" });   // no temp file left behind
}

TEST_F(VersionPinDisk, RunningAgainOnTheSameVersionChangesNothing) {
    const fs::path acf = WriteAcf(kSample);
    ASSERT_EQ(PinAcf(acf, opt_).status, PinStatus::Written);
    const PinReport again = PinAcf(acf, opt_);
    EXPECT_EQ(again.status, PinStatus::UpToDate) << again.message;
    EXPECT_EQ(OutDirNames().size(), 1u);
}

TEST_F(VersionPinDisk, ANewerInstalledVersionIsNotPinnedOverTheOldOneWithoutForce) {
    const fs::path acf = WriteAcf(kSample);
    const PinReport first = PinAcf(acf, opt_);
    ASSERT_EQ(first.status, PinStatus::Written);

    std::string updated = kSample;
    updated.replace(updated.find("7378614952147005398"), 19, "1111111111111111111");
    WriteAcf(updated);

    const PinReport refused = PinAcf(acf, opt_);
    EXPECT_EQ(refused.status, PinStatus::Refused);
    EXPECT_TRUE(Contains(refused.message, "--force"));
    EXPECT_EQ(ReadFile(first.file), first.lua);   // untouched

    opt_.force = true;
    const PinReport replaced = PinAcf(acf, opt_);
    EXPECT_EQ(replaced.status, PinStatus::Written);
    EXPECT_EQ(replaced.message, "replaced");
    EXPECT_TRUE(Contains(ReadFile(first.file), "\"1111111111111111111\""));
}

TEST_F(VersionPinDisk, AFileWeDidNotMakeIsNeverOverwrittenWithoutForce) {
    WriteFile(out_ / "3357650.pin.lua", "addappid(3357650)\n-- mine\n");
    const fs::path acf = WriteAcf(kSample);

    const PinReport refused = PinAcf(acf, opt_);
    EXPECT_EQ(refused.status, PinStatus::Refused);
    EXPECT_TRUE(Contains(refused.message, "not made by version_pin"));
    EXPECT_EQ(ReadFile(out_ / "3357650.pin.lua"), "addappid(3357650)\n-- mine\n");

    opt_.force = true;
    EXPECT_EQ(PinAcf(acf, opt_).status, PinStatus::Written);
    EXPECT_TRUE(IsGeneratedPinFile(ReadFile(out_ / "3357650.pin.lua")));
}

TEST_F(VersionPinDisk, DryRunWritesNothingAndShowsTheLua) {
    opt_.dryRun = true;
    const PinReport r = PinAcf(WriteAcf(kSample), opt_);
    EXPECT_EQ(r.status, PinStatus::DryRun);
    EXPECT_TRUE(Contains(r.lua, "setManifestid(3357652, "));
    EXPECT_TRUE(OutDirNames().empty());
}

TEST_F(VersionPinDisk, DryRunStillReportsARefusal) {
    WriteFile(out_ / "3357650.pin.lua", "not ours");
    opt_.dryRun = true;
    EXPECT_EQ(PinAcf(WriteAcf(kSample), opt_).status, PinStatus::Refused);
}

TEST_F(VersionPinDisk, AMissingManifestIsAClearError) {
    const PinReport r = PinAcf(steamapps_ / "appmanifest_999.acf", opt_);
    EXPECT_EQ(r.status, PinStatus::Failed);
    EXPECT_TRUE(Contains(r.message, "appmanifest_999.acf"));
    EXPECT_TRUE(OutDirNames().empty());
}

TEST_F(VersionPinDisk, AMalformedManifestIsAClearError) {
    const PinReport r = PinAcf(WriteAcf("\"AppState\" { \"appid\" \"1\" "), opt_);
    EXPECT_EQ(r.status, PinStatus::Failed);
    EXPECT_TRUE(Contains(r.message, "malformed"));
    EXPECT_TRUE(OutDirNames().empty());
}

TEST_F(VersionPinDisk, EmptyInstalledDepotsIsAClearError) {
    const PinReport r = PinAcf(WriteAcf(AcfWith("")), opt_);
    EXPECT_EQ(r.status, PinStatus::Failed);
    EXPECT_TRUE(Contains(r.message, "InstalledDepots is empty"));
    EXPECT_TRUE(OutDirNames().empty());
}

TEST_F(VersionPinDisk, ATruncatedManifestNeverWritesAnything) {
    const std::size_t complete = kSample.rfind('}') + 1;
    for (std::size_t len = 0; len < complete; len += 11) {
        const PinReport r = PinAcf(WriteAcf(std::string_view(kSample).substr(0, len)), opt_);
        ASSERT_EQ(r.status, PinStatus::Failed) << len << ": " << r.message;
    }
    EXPECT_TRUE(OutDirNames().empty());
}

TEST_F(VersionPinDisk, ABadOutputDirectoryIsAClearError) {
    opt_.outDir = root_ / "no" / "such" / "dir";
    const PinReport r = PinAcf(WriteAcf(kSample), opt_);
    EXPECT_EQ(r.status, PinStatus::Failed);
    EXPECT_TRUE(Contains(r.message, "output directory not found"));
}

TEST_F(VersionPinDisk, AnUnwritableTargetLeavesNoTempFileAndKeepsTheOriginal) {
    // A directory holds the target name: the rename must fail and clean up after itself.
    fs::create_directories(out_ / "3357650.pin.lua");
    opt_.force = true;
    const PinReport r = PinAcf(WriteAcf(kSample), opt_);
    EXPECT_EQ(r.status, PinStatus::Failed);
    EXPECT_TRUE(Contains(r.message, "could not write"));
    EXPECT_EQ(OutDirNames(), std::vector<std::string>{ "3357650.pin.lua" });   // still the directory, no *.amethyst-tmp
    EXPECT_TRUE(fs::is_directory(out_ / "3357650.pin.lua"));
}

TEST_F(VersionPinDisk, WarnsWhenAnotherLuaPinsTheSameDepotElsewhere) {
    WriteFile(out_ / "3357650.lua", "addappid(3357650)\nsetManifestid(3357651, \"42\")\nsetManifestid(3357652, \"2727486347555616762\")\n");
    WriteFile(out_ / "readme.txt", "setManifestid(3357651, \"43\")");   // not a .lua: not scanned
    const PinReport r = PinAcf(WriteAcf(kSample), opt_);
    ASSERT_EQ(r.status, PinStatus::Written);
    ASSERT_EQ(r.warnings.size(), 1u);
    EXPECT_TRUE(Contains(r.warnings[0], "depot 3357651 is also pinned to 42 in 3357650.lua"));
}

TEST_F(VersionPinDisk, ItsOwnPreviousFileIsNotAConflict) {
    const fs::path acf = WriteAcf(kSample);
    ASSERT_EQ(PinAcf(acf, opt_).status, PinStatus::Written);

    // The installed version moves on, so the old pin file now disagrees with the new gids:
    // only leaving the target out of the scan keeps it from being reported against itself.
    std::string updated = kSample;
    updated.replace(updated.find("7378614952147005398"), 19, "1111111111111111111");
    WriteAcf(updated);
    opt_.force = true;
    const PinReport r = PinAcf(acf, opt_);
    EXPECT_EQ(r.status, PinStatus::Written);
    EXPECT_TRUE(r.warnings.empty());
}

TEST_F(VersionPinDisk, ConflictWarningsAreCapped) {
    std::string many;
    for (int i = 0; i < 40; ++i) many += "setManifestid(3357651, \"" + std::to_string(100 + i) + "\")\n";
    WriteFile(out_ / "other.lua", many);
    const PinReport r = PinAcf(WriteAcf(kSample), opt_);
    ASSERT_EQ(r.status, PinStatus::Written);
    EXPECT_EQ(r.warnings.size(), kMaxNotes);
}

TEST_F(VersionPinDisk, APartialPinIsRefusedBecauseTheSkippedDepotWouldStillUpdate) {
    const fs::path acf = WriteAcf(AcfWith("\"10\" { \"manifest\" \"5\" } \"x\" { \"manifest\" \"6\" }"), "appmanifest_100.acf");
    const PinReport r = PinAcf(acf, opt_);
    EXPECT_EQ(r.status, PinStatus::Failed);
    EXPECT_TRUE(Contains(r.message, "1 InstalledDepots entries are unusable"));
    EXPECT_TRUE(Contains(r.message, "refusing a partial pin"));
    EXPECT_TRUE(OutDirNames().empty());
}

TEST_F(VersionPinDisk, ADuplicatedDepotIsAlsoRefused) {
    const fs::path acf = WriteAcf(AcfWith("\"10\" { \"manifest\" \"5\" } \"10\" { \"manifest\" \"6\" }"), "appmanifest_100.acf");
    EXPECT_EQ(PinAcf(acf, opt_).status, PinStatus::Failed);
}

TEST_F(VersionPinDisk, AManifestUnderAnotherAppsNameIsRefused) {
    const fs::path acf = WriteAcf(kSample, "appmanifest_999.acf");   // the content says 3357650
    const PinReport r = PinAcf(acf, opt_);
    EXPECT_EQ(r.status, PinStatus::Failed);
    EXPECT_TRUE(Contains(r.message, "file name says app 999 but the manifest says app 3357650"));
    EXPECT_TRUE(OutDirNames().empty());
}

TEST_F(VersionPinDisk, AFileNameOutsideTheSteamPatternIsNotChecked) {
    EXPECT_EQ(PinAcf(WriteAcf(kSample, "my copy.acf"), opt_).status, PinStatus::Written);
}

TEST_F(VersionPinDisk, ASymlinkAtTheTargetIsReplacedNotFollowed) {
    const fs::path elsewhere = root_ / "elsewhere.txt";
    WriteFile(elsewhere, "keep me");
    std::error_code ec;
    fs::create_symlink(elsewhere, out_ / "3357650.pin.lua", ec);
    if (ec) GTEST_SKIP() << "cannot create a symlink here: " << ec.message();   // Windows without the privilege

    const fs::path acf = WriteAcf(kSample);
    EXPECT_EQ(PinAcf(acf, opt_).status, PinStatus::Refused);                      // foreign, not forced
    opt_.force = true;
    EXPECT_EQ(PinAcf(acf, opt_).status, PinStatus::Written);
    EXPECT_EQ(ReadFile(elsewhere), "keep me");                                    // the file behind the link is untouched
    EXPECT_FALSE(fs::is_symlink(out_ / "3357650.pin.lua"));
    EXPECT_TRUE(IsGeneratedPinFile(ReadFile(out_ / "3357650.pin.lua")));
}

TEST_F(VersionPinDisk, ListsOnlyAppManifestsByName) {
    WriteAcf(kSample, "appmanifest_2.acf");
    WriteAcf(kSample, "appmanifest_1.acf");
    WriteAcf(kSample, "appmanifest_3.txt");
    WriteAcf(kSample, "other_4.acf");
    fs::create_directories(steamapps_ / "appmanifest_5.acf");   // a directory with a matching name
    std::error_code ec;
    const auto files = ListAcfFiles(steamapps_, ec);
    ASSERT_FALSE(ec);
    ASSERT_EQ(files.size(), 2u);
    EXPECT_EQ(files[0].filename().string(), "appmanifest_1.acf");
    EXPECT_EQ(files[1].filename().string(), "appmanifest_2.acf");
}

TEST_F(VersionPinDisk, AnUnreadableLibraryIsReportedNotThrown) {
    std::error_code ec;
    EXPECT_TRUE(ListAcfFiles(root_ / "missing", ec).empty());
    EXPECT_TRUE(static_cast<bool>(ec));
}

TEST(VersionPinFiles, ReadSmallFileHonoursTheCap) {
    // Unique per run: Debug and Release test binaries can run at the same time.
    const fs::path p = fs::temp_directory_path() /
        ("amethyst_versionpin_cap_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".txt");
    std::ofstream(p, std::ios::binary | std::ios::trunc) << "12345";
    EXPECT_EQ(ReadSmallFile(p, 5).value_or("<none>"), "12345");
    EXPECT_FALSE(ReadSmallFile(p, 4).has_value());
    EXPECT_FALSE(ReadSmallFile(fs::temp_directory_path(), 100).has_value());   // a directory
    std::error_code ec;
    fs::remove(p, ec);
    EXPECT_FALSE(ReadSmallFile(p, 100).has_value());                           // gone
}
