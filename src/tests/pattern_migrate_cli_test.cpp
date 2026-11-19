// Tests for PatternMigrateCliLogic (v1.5.0, pillar 4): what tools/pattern_migrate does
// around the engine -- arguments, the fingerprint file, the run against a DLL image, the
// candidate TOML and report, and where the output may go. Synthetic images only; the one
// file write goes to a temp folder. Runs on Linux too (no Windows header, no toml++).

#include "Utils/Config/CompatAliasLogic.h"
#include "Utils/SteamMetadata/PatternMigrateCliLogic.h"
#include "synthetic_image.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace PatternMigrateCliLogic;
using namespace SyntheticImage;
using PatternMigrateLogic::NamedRva;
using PatternMigrateLogic::Verdict;

namespace {

    constexpr std::uint32_t kSeedF = 0xF0F0F0F0;
    constexpr std::uint32_t kSeedA = 0xA0A0A0A0;
    constexpr std::uint32_t kSeedOther = 0x11111111;

    // A fresh folder under the system temp dir, removed on destruction.
    class TempDir {
    public:
        TempDir() {
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            path_ = fs::temp_directory_path() / ("amethyst_migrate_cli_test_" + std::to_string(stamp));
            fs::create_directories(path_);
        }
        ~TempDir() { std::error_code ec; fs::remove_all(path_, ec); }
        TempDir(const TempDir&) = delete;
        TempDir& operator=(const TempDir&) = delete;
        const fs::path& Path() const { return path_; }
    private:
        fs::path path_;
    };

    void Touch(const fs::path& p, const std::string& body = "x") {
        fs::create_directories(p.parent_path());
        std::ofstream(p, std::ios::binary) << body;
    }

    ParsedArgs Parse(std::vector<std::string> a) {
        std::vector<fs::path> v(a.begin(), a.end());
        return ParseArgs(v);
    }

    // The old build: A, then F which calls A. Returns the fingerprint file text the recorder
    // would write for it (RecordFunctions + SerializeFingerprints, the real pair).
    std::string OldFingerprintText(std::vector<NamedRva>* resolvedOut = nullptr) {
        Img old;
        const std::uint32_t a = old.Add(Func(kSeedA, 100));
        const std::uint32_t f = old.Add(Func(kSeedF, 120));
        old.PatchCall(f, a);
        std::vector<NamedRva> resolved = { { "BuildDepotDependency", f }, { "GetAppByID", a } };
        if (resolvedOut) *resolvedOut = resolved;
        const auto set = PatternRecorderLogic::RecordFunctions(old.View(), resolved);
        return PatternRecorderLogic::SerializeFingerprints("steamclient", std::string(64, 'a'), set);
    }

    // The new build: an unrelated function first (everything moves), F 10 bytes longer.
    struct NewBuild {
        Img img;
        std::uint32_t a = 0, f = 0;
    };

    NewBuild MakeNewBuild(bool withA = true) {
        NewBuild n;
        n.img.Add(Func(kSeedOther, 80));
        n.a = n.img.Add(Func(withA ? kSeedA : 0x22222222, 100));
        n.f = n.img.Add(Func(kSeedF, 130));
        n.img.PatchCall(n.f, n.a);
        return n;
    }

    FingerprintFile ParseOk(const std::string& text) {
        std::string err;
        const auto fp = ParseFingerprintFile(text, err);
        EXPECT_TRUE(fp.has_value()) << err;
        return fp.value_or(FingerprintFile{});
    }

    // ── arguments ────────────────────────────────────────────────────

    TEST(PatternMigrateCliArgs, AcceptsTheFourFlagsAndForce) {
        const ParsedArgs p = Parse({ "--fingerprint", "fp.toml", "--dll", "new.dll", "--out", "o.toml",
                                     "--required", "A,B", "--force" });
        ASSERT_TRUE(p.error.empty()) << p.error;
        EXPECT_EQ(p.options.fingerprint, fs::path("fp.toml"));
        EXPECT_EQ(p.options.dll, fs::path("new.dll"));
        EXPECT_EQ(p.options.out, fs::path("o.toml"));
        EXPECT_EQ(p.options.required, "A,B");
        EXPECT_TRUE(p.options.force);
    }

    TEST(PatternMigrateCliArgs, RequiredAndForceAreOptional) {
        const ParsedArgs p = Parse({ "--dll", "d", "--out", "o", "--fingerprint", "f" });
        ASSERT_TRUE(p.error.empty()) << p.error;
        EXPECT_TRUE(p.options.required.empty());
        EXPECT_FALSE(p.options.force);
    }

    TEST(PatternMigrateCliArgs, EachMissingFlagIsNamed) {
        EXPECT_NE(Parse({ "--dll", "d", "--out", "o" }).error.find("--fingerprint"), std::string::npos);
        EXPECT_NE(Parse({ "--fingerprint", "f", "--out", "o" }).error.find("--dll"), std::string::npos);
        EXPECT_NE(Parse({ "--fingerprint", "f", "--dll", "d" }).error.find("--out"), std::string::npos);
        EXPECT_FALSE(Parse({}).error.empty());
    }

    TEST(PatternMigrateCliArgs, RejectsUnknownDuplicateAndForgottenValues) {
        EXPECT_NE(Parse({ "--nope" }).error.find("unknown argument"), std::string::npos);
        EXPECT_NE(Parse({ "--dll", "a", "--dll", "b" }).error.find("twice"), std::string::npos);
        // a value that looks like a flag is a forgotten value, not a path
        EXPECT_NE(Parse({ "--out", "--force" }).error.find("needs a value"), std::string::npos);
        EXPECT_NE(Parse({ "--dll" }).error.find("needs a value"), std::string::npos);
        // a stray positional argument is unknown too
        EXPECT_NE(Parse({ "--fingerprint", "f", "--dll", "d", "--out", "o", "extra" }).error.find("unknown"),
                  std::string::npos);
    }

    TEST(PatternMigrateCliArgs, HelpNeedsNothingElse) {
        const ParsedArgs p = Parse({ "--help" });
        EXPECT_TRUE(p.error.empty());
        EXPECT_TRUE(p.options.help);
        EXPECT_TRUE(Parse({ "-h" }).options.help);
    }

    // ── required ─────────────────────────────────────────────────────

    TEST(PatternMigrateCliRequired, ListIsSortedUniqueAndTrimmed) {
        const auto r = ParseRequiredList(" B, A ,B,C_1");
        ASSERT_TRUE(r.has_value());
        EXPECT_EQ(*r, (std::vector<std::string>{ "A", "B", "C_1" }));
    }

    TEST(PatternMigrateCliRequired, ListRejectsEmptyEntriesAndOddNames) {
        EXPECT_FALSE(ParseRequiredList("").has_value());
        EXPECT_FALSE(ParseRequiredList("A,,B").has_value());
        EXPECT_FALSE(ParseRequiredList("A,B C").has_value());
        EXPECT_FALSE(ParseRequiredList("A]\n[Evil").has_value());
    }

    TEST(PatternMigrateCliRequired, OldPatternTomlGivesItsTableNames) {
        const std::string toml =
            "# old\n[BBuildAndAsyncSendFrame]\nrva = \"0x1A2B30\"\n\n[BuildDepotDependency]\nsig = \"40 53\"\n";
        const auto r = ParseRequiredFromPatternToml(toml);
        ASSERT_TRUE(r.has_value());
        EXPECT_EQ(*r, (std::vector<std::string>{ "BBuildAndAsyncSendFrame", "BuildDepotDependency" }));
        EXPECT_FALSE(ParseRequiredFromPatternToml("rva = \"0x1\"\n").has_value());
        EXPECT_FALSE(ParseRequiredFromPatternToml("[bad name]\n").has_value());
    }

    // ── fingerprint file ─────────────────────────────────────────────

    TEST(PatternMigrateCliFingerprint, RoundTripsWhatTheRecorderWrites) {
        Img old;
        const std::uint32_t a = old.Add(Func(kSeedA, 100));
        const std::uint32_t f = old.Add(Func(kSeedF, 120));
        old.PatchCall(f, a);
        const auto set = PatternRecorderLogic::RecordFunctions(
            old.View(), { { "BuildDepotDependency", f }, { "GetAppByID", a } });
        const FingerprintFile fp = ParseOk(PatternRecorderLogic::SerializeFingerprints("steamclient", std::string(64, 'a'), set));

        EXPECT_EQ(fp.component, "steamclient");
        EXPECT_EQ(fp.sha256, std::string(64, 'a'));
        ASSERT_EQ(fp.fps.size(), set.fps.size());
        for (std::size_t i = 0; i < fp.fps.size(); ++i) {
            const auto& got = fp.fps[i];
            const auto& want = set.fps[i];
            EXPECT_EQ(got.name, want.name);
            EXPECT_EQ(got.rva, want.rva);
            EXPECT_EQ(got.hasPdata, want.hasPdata);
            EXPECT_EQ(got.funcSize, want.funcSize);
            EXPECT_EQ(got.decodeComplete, want.decodeComplete);
            EXPECT_EQ(got.matchCountOld, want.matchCountOld);
            EXPECT_EQ(got.bytes, want.bytes);
            EXPECT_EQ(got.mask, want.mask);
            EXPECT_EQ(got.calls, want.calls);
        }
    }

    TEST(PatternMigrateCliFingerprint, ReadsTheSkippedList) {
        const std::string text = OldFingerprintText();
        std::string with = text;
        with.insert(with.find("\n[[function]]"), "\nskipped = [\"Lost\", \"Gone\"]");
        EXPECT_EQ(ParseOk(with).skipped, (std::vector<std::string>{ "Gone", "Lost" }));   // sorted
    }

    TEST(PatternMigrateCliFingerprint, RejectsWhatItCannotReadInsteadOfGuessing) {
        const std::string good = OldFingerprintText();
        const auto bad = [&](const std::string& text, const char* needle) {
            std::string err;
            EXPECT_FALSE(ParseFingerprintFile(text, err).has_value()) << needle;
            EXPECT_NE(err.find(needle), std::string::npos) << err;
        };
        const auto replace = [&](std::string from, const std::string& to) {
            std::string t = good;
            const std::size_t at = t.find(from);
            EXPECT_NE(at, std::string::npos) << from;
            t.replace(at, from.size(), to);
            return t;
        };

        bad("", "no version");
        bad("version = 2\n[[function]]\nname = \"A\"\n", "unsupported version");
        bad(replace("version = 1", "version = one"), "unsupported version");
        bad(replace("sig = \"", "sig = \"ZZ "), "bad sig");
        bad(replace("rva = \"0x", "rva = \"0xQ"), "bad rva");
        bad(replace("name = \"BuildDepotDependency\"", "name = \"Build Depot\""), "valid name");
        bad(replace("pdata = true", "pdata = yes"), "pdata");
        bad(replace("size = 120", "size = -1"), "size");
        bad(replace("calls = [\"GetAppByID\"]", "calls = [GetAppByID]"), "calls");
        bad(good + "this is not toml\n", "key = value");
        bad("version = 1\ncomponent = \"x\"\n", "no function");
        // a function without a signature, and a name given twice
        bad("version = 1\n[[function]]\nname = \"A\"\nrva = \"0x10\"\n", "needs rva and sig");
        bad(good + "\n[[function]]\nname = \"GetAppByID\"\nrva = \"0x10\"\nsig = \"48 89\"\n", "twice");
    }

    TEST(PatternMigrateCliFingerprint, SigTooLongIsRefused) {
        std::string sig;
        for (int i = 0; i < 200; ++i) sig += "90 ";
        sig.pop_back();
        std::string err;
        EXPECT_FALSE(ParseFingerprintFile(OldFingerprintText().substr(0, OldFingerprintText().find("\n[[function]]")) +
                                          "\n[[function]]\nname = \"A\"\nrva = \"0x10\"\nsig = \"" + sig + "\"\n", err));
        EXPECT_NE(err.find("bad sig"), std::string::npos) << err;
    }

    TEST(PatternMigrateCliFingerprint, QuotedStringsAreUnescaped) {
        // TomlQuote writes \\ \" and \u00XX; ParseQuoted is its inverse.
        EXPECT_EQ(detail::ParseQuoted("\"a\\\"b\\\\c\\u0041\""), "a\"b\\cA");
        EXPECT_EQ(detail::ParseQuoted("\"\\u001B\""), "\x1B");
        EXPECT_FALSE(detail::ParseQuoted("\"cut\\u00\"").has_value());      // truncated escape
        EXPECT_FALSE(detail::ParseQuoted("\"bad\\n\"").has_value());        // an escape TomlQuote never writes
        EXPECT_FALSE(detail::ParseQuoted("\"a\"b\"").has_value());          // an unescaped quote inside
    }

    TEST(PatternMigrateCliFingerprint, FreeTextFieldsAreValidated) {
        const std::string good = OldFingerprintText();
        const auto bad = [&](const std::string& from, const std::string& to, const char* needle) {
            std::string t = good;
            const std::size_t at = t.find(from);
            ASSERT_NE(at, std::string::npos) << from;
            t.replace(at, from.size(), to);
            std::string err;
            EXPECT_FALSE(ParseFingerprintFile(t, err).has_value()) << to;
            EXPECT_NE(err.find(needle), std::string::npos) << err;
        };
        // a control byte (a terminal escape) never reaches the console through these fields
        bad("component = \"steamclient\"", "component = \"steam\\u001B[2Jclient\"", "component");
        bad(std::string(64, 'a'), std::string(63, 'a') + "G", "sha256");
        bad(std::string(64, 'a'), std::string(10, 'a'), "sha256");
        bad("calls = [\"GetAppByID\"]", "calls = [\"Get App\\u001B\"]", "calls");

        std::string withSkipped = good;
        withSkipped.insert(withSkipped.find("\n[[function]]"), "\nskipped = [\"ok\", \"\\u001B[2J\"]");
        std::string err;
        EXPECT_FALSE(ParseFingerprintFile(withSkipped, err).has_value());
        EXPECT_NE(err.find("skipped"), std::string::npos) << err;
    }

    TEST(PatternMigrateCliFingerprint, ListsAndFunctionCountAreCapped) {
        const std::string good = OldFingerprintText();
        std::string items = "\"A\"";
        for (std::size_t i = 0; i < kMaxListItems; ++i) items += ",\"N" + std::to_string(i) + "\"";   // kMaxListItems + 1 in all
        std::string t = good;
        t.replace(t.find("calls = [\"GetAppByID\"]"), 22, "calls = [" + items + "]");
        std::string err;
        EXPECT_FALSE(ParseFingerprintFile(t, err).has_value());
        EXPECT_NE(err.find("calls"), std::string::npos) << err;

        std::string many = good.substr(0, good.find("\n[[function]]"));
        for (std::size_t i = 0; i <= kMaxFunctions; ++i)
            many += "\n[[function]]\nname = \"F" + std::to_string(i) + "\"\nrva = \"0x10\"\nsig = \"48 89 5C\"\n";
        EXPECT_FALSE(ParseFingerprintFile(many, err).has_value());
        EXPECT_NE(err.find("too many"), std::string::npos) << err;
    }

    TEST(PatternMigrateCliFingerprint, BomCrlfAndDuplicateKeys) {
        const std::string good = OldFingerprintText();
        // PowerShell 5 writes a BOM; Windows editors write CRLF
        std::string crlf;
        for (const char c : good) { if (c == '\n') crlf += '\r'; crlf += c; }
        EXPECT_EQ(ParseOk("\xEF\xBB\xBF" + crlf).fps.size(), 2u);

        std::string t = good;
        t.insert(t.find("\n[[function]]"), "\nsha256 = \"" + std::string(64, 'b') + "\"");
        std::string err;
        EXPECT_FALSE(ParseFingerprintFile(t, err).has_value());
        EXPECT_NE(err.find("given twice"), std::string::npos) << err;

        t = good;
        t.replace(t.find("rva = "), 6, "rva = \"0x10\"\nrva = ");
        EXPECT_FALSE(ParseFingerprintFile(t, err).has_value());
        EXPECT_NE(err.find("given twice"), std::string::npos) << err;
    }

    // ── the DLL ──────────────────────────────────────────────────────

    TEST(PatternMigrateCliImage, ViewOfASyntheticPe) {
        const NewBuild n = MakeNewBuild();
        const Bytes file = BuildPeFile(n.img);
        std::string err;
        const auto view = MakeImageView(file, err);
        ASSERT_TRUE(view.has_value()) << err;
        EXPECT_EQ(view->textRva, kTextRva);
        EXPECT_EQ(view->text.size(), n.img.text.size());
        EXPECT_EQ(view->pdata.size(), n.img.pdata.size());
    }

    TEST(PatternMigrateCliImage, GarbageAndTruncatedImagesAreRefused) {
        std::string err;
        EXPECT_FALSE(MakeImageView(Bytes{}, err).has_value());
        const Bytes junk(4096, 0x41);
        EXPECT_FALSE(MakeImageView(junk, err).has_value());
        EXPECT_FALSE(err.empty());
        Bytes cut = BuildPeFile(MakeNewBuild().img);
        cut.resize(0x300);                                           // headers only, sections cut off
        EXPECT_FALSE(MakeImageView(cut, err).has_value());
    }

    // ── run, TOML, report ────────────────────────────────────────────

    TEST(PatternMigrateCliRun, MigratesBothFunctionsToTheirNewRvas) {
        const FingerprintFile fp = ParseOk(OldFingerprintText());
        const NewBuild n = MakeNewBuild();
        const RunResult run = RunMigration(n.img.View(), fp, { "BuildDepotDependency" });

        EXPECT_EQ(run.migrated, 2u);
        EXPECT_TRUE(run.requiredFailed.empty());
        EXPECT_EQ(ExitCodeFor(run), 0);
        for (const auto& m : run.results) {
            EXPECT_EQ(m.result.verdict, Verdict::Migrated) << m.name;
            EXPECT_EQ(m.result.rva, m.name == "GetAppByID" ? n.a : n.f) << m.name;
        }
    }

    TEST(PatternMigrateCliRun, CandidateTomlHasRvaAndSigPerMigratedFunction) {
        const FingerprintFile fp = ParseOk(OldFingerprintText());
        const NewBuild n = MakeNewBuild();
        const std::string toml = RenderCandidateToml(fp, RunMigration(n.img.View(), fp, {}));

        EXPECT_NE(toml.find("[BuildDepotDependency]\nrva = " + PatternRecorderLogic::HexRva(n.f)), std::string::npos) << toml;
        EXPECT_NE(toml.find("[GetAppByID]\nrva = " + PatternRecorderLogic::HexRva(n.a)), std::string::npos) << toml;
        EXPECT_NE(toml.find("sig = \"48 89 5C 24 08 48 89 74 24 10 57"), std::string::npos) << toml;
        EXPECT_NE(toml.find("??"), std::string::npos) << "the call/RIP displacements stay wildcards";
        EXPECT_NE(toml.find("REVIEW IT BEFORE USE"), std::string::npos);
    }

    TEST(PatternMigrateCliRun, WhatIsNotMigratedIsACommentNotATable) {
        const FingerprintFile fp = ParseOk(OldFingerprintText());
        const NewBuild n = MakeNewBuild(/*withA=*/false);           // GetAppByID is gone
        const RunResult run = RunMigration(n.img.View(), fp, { "GetAppByID" });
        const std::string toml = RenderCandidateToml(fp, run);

        EXPECT_EQ(toml.find("[GetAppByID]"), std::string::npos) << toml;
        EXPECT_NE(toml.find("# GetAppByID: not found, not written"), std::string::npos) << toml;
        EXPECT_EQ(run.requiredFailed, (std::vector<std::string>{ "GetAppByID" }));
        EXPECT_EQ(ExitCodeFor(run), 2) << "a required function that is not migrated fails the run";
        EXPECT_NE(RenderReport(fp, run).find("REQUIRED NOT MIGRATED: GetAppByID"), std::string::npos);
    }

    TEST(PatternMigrateCliRun, ARequiredNameAbsentFromTheFingerprintFails) {
        const FingerprintFile fp = ParseOk(OldFingerprintText());
        const RunResult run = RunMigration(MakeNewBuild().img.View(), fp, { "NeverRecorded" });
        EXPECT_EQ(run.requiredFailed, (std::vector<std::string>{ "NeverRecorded" }));
        EXPECT_EQ(ExitCodeFor(run), 2);
    }

    TEST(PatternMigrateCliRun, NothingMigratedIsAFailureEvenWithNoRequired) {
        const FingerprintFile fp = ParseOk(OldFingerprintText());
        Img other;
        other.Add(Func(kSeedOther, 90));
        const RunResult run = RunMigration(other.View(), fp, {});
        EXPECT_EQ(run.migrated, 0u);
        EXPECT_EQ(ExitCodeFor(run), 2);
    }

    TEST(PatternMigrateCliRun, ReportNamesEachVerdictAndTheSummary) {
        const FingerprintFile fp = ParseOk(OldFingerprintText());
        const NewBuild n = MakeNewBuild();
        const std::string report = RenderReport(fp, RunMigration(n.img.View(), fp, {}));
        EXPECT_NE(report.find("[migrated] BuildDepotDependency: 0x1070 -> " + detail::Hex(n.f)), std::string::npos) << report;
        EXPECT_NE(report.find("size 120 -> 130"), std::string::npos) << report;
        EXPECT_NE(report.find("2 of 2 migrated"), std::string::npos) << report;

        // a same-seed function 60% longer: size mismatch
        Img big;
        big.Add(Func(kSeedA, 160));
        const std::string mismatch = RenderReport(fp, RunMigration(big.View(), fp, {}));
        EXPECT_NE(mismatch.find("[size mismatch] GetAppByID"), std::string::npos) << mismatch;
        EXPECT_NE(mismatch.find("recorded 100"), std::string::npos) << mismatch;
    }

    TEST(PatternMigrateCliRun, SkippedFunctionsAreReported) {
        FingerprintFile fp = ParseOk(OldFingerprintText());
        fp.skipped = { "Lost" };
        EXPECT_NE(RenderReport(fp, RunMigration(MakeNewBuild().img.View(), fp, {})).find("[skipped] Lost"), std::string::npos);
    }

    // ── output ───────────────────────────────────────────────────────

    TEST(PatternMigrateCliOutput, TheCandidateIsWrittenAndReadBackIdentical) {
        TempDir dir;
        const FingerprintFile fp = ParseOk(OldFingerprintText());
        const std::string body = RenderCandidateToml(fp, RunMigration(MakeNewBuild().img.View(), fp, {}));
        const fs::path out = dir.Path() / "candidate.toml";

        ASSERT_FALSE(CompatAliasLogic::detail::WriteBytesViaTemp(body, out, CompatAliasLogic::detail::MakeTempTag("t")));
        std::ifstream in(out, std::ios::binary);
        const std::string back((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        EXPECT_EQ(back, body);
        // and only the result is left in the folder: no temp file
        std::size_t files = 0;
        for (const auto& e : fs::directory_iterator(dir.Path())) { (void)e; ++files; }
        EXPECT_EQ(files, 1u);
    }

    TEST(PatternMigrateCliOutput, AFreshPathInAPlainFolderIsAllowed) {
        TempDir dir;
        const std::vector<fs::path> in = { dir.Path() / "d.dll", dir.Path() / "f.toml" };
        EXPECT_EQ(RefuseOutput(dir.Path() / "o.toml", in, false), "");
    }

    TEST(PatternMigrateCliOutput, NeverInsideASteamFolder) {
        TempDir dir;
        Touch(dir.Path() / "Steam" / "steam.exe");
        const std::vector<fs::path> in = { dir.Path() / "d.dll", dir.Path() / "f.toml" };
        EXPECT_NE(RefuseOutput(dir.Path() / "Steam" / "o.toml", in, true).find("Steam folder"), std::string::npos);
        // a subfolder of Steam counts, even one that does not exist yet, and --force does not change that
        EXPECT_NE(RefuseOutput(dir.Path() / "Steam" / "amethysttool" / "pattern" / "o.toml", in, true).find("Steam folder"),
                  std::string::npos);
        // ".." cannot climb out of the check: this resolves to inside Steam
        EXPECT_NE(RefuseOutput(dir.Path() / "mine" / ".." / "Steam" / "o.toml", in, true).find("Steam folder"),
                  std::string::npos);
        // a sibling of Steam is fine
        EXPECT_EQ(RefuseOutput(dir.Path() / "mine" / "o.toml", in, false), "");
    }

    TEST(PatternMigrateCliOutput, ARelativePathIsResolvedBeforeTheCheck) {
        TempDir dir;
        Touch(dir.Path() / "Steam" / "steam.exe");
        const fs::path old = fs::current_path();
        fs::current_path(dir.Path() / "Steam");
        const std::string why = RefuseOutput("o.toml", {}, false);
        fs::current_path(old);
        EXPECT_NE(why.find("Steam folder"), std::string::npos) << why;
    }

    TEST(PatternMigrateCliOutput, NotOverAnExistingFileWithoutForceAndNotOverAnInput) {
        TempDir dir;
        const fs::path dll = dir.Path() / "d.dll", fp = dir.Path() / "f.toml", old = dir.Path() / "old.toml";
        const fs::path out = dir.Path() / "o.toml";
        Touch(dll);
        Touch(fp);
        Touch(old);
        Touch(out);
        const std::vector<fs::path> in = { dll, fp, old };    // old = a file given to --required
        EXPECT_NE(RefuseOutput(out, in, false).find("already exists"), std::string::npos);
        EXPECT_EQ(RefuseOutput(out, in, true), "");
        for (const fs::path& input : in)
            EXPECT_NE(RefuseOutput(input, in, true).find("same file"), std::string::npos) << Narrow(input);
        // the same file reached through a ".." path
        EXPECT_NE(RefuseOutput(dir.Path() / "x" / ".." / "old.toml", in, true).find("same file"), std::string::npos);
    }

    TEST(PatternMigrateCliOutput, NotOntoADirectoryOrAnEmptyFileName) {
        TempDir dir;
        fs::create_directories(dir.Path() / "sub");
        EXPECT_NE(RefuseOutput(dir.Path() / "sub", {}, true).find("directory"), std::string::npos);
        EXPECT_NE(RefuseOutput(dir.Path() / "sub" / "", {}, true), "");       // a trailing separator
    }

    // ── non-ASCII paths ──────────────────────────────────────────────

    TEST(PatternMigrateCliArgs, PathsOutsideTheAnsiCodePageDoNotThrow) {
        // Japanese and an emoji: not representable in a Latin-1 ANSI code page. path::string()
        // would throw on Windows there; the parser and Narrow must not.
        const fs::path dir = fs::path(u8"C:\\tmp\\\u65E5\u672C\U0001F600");
        const std::vector<fs::path> args = { "--fingerprint", dir / "f.toml", "--dll", dir / "d.dll", "--out",
                                             dir / "o.toml", "--required", dir / "old.toml" };
        ParsedArgs p;
        ASSERT_NO_THROW(p = ParseArgs(args));
        EXPECT_TRUE(p.error.empty()) << p.error;
        EXPECT_EQ(p.options.required, dir / "old.toml");
        EXPECT_NO_THROW((void)Narrow(p.options.out));
        EXPECT_NE(Narrow(p.options.out).find("\xE6\x97\xA5"), std::string::npos);     // the UTF-8 of U+65E5
    }

} // namespace
