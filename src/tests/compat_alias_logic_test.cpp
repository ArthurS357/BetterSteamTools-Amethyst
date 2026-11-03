// Unit tests for CompatAliasLogic -- the [compatibility] denuvo_aliases feature
// (v1.3.5) that drops OpenSteamTool.dll / opensteamtool.toml copies next to
// steam.exe so external activation tooling (LuaTools Validator) recognises the
// engine.
//
// Contract per CompatAliasLogic.h: do nothing when disabled; create an alias
// that is absent; refresh one that is ours but stale; NEVER touch a file under
// the upstream name that we did not write (a real OpenSteamTool install, or a
// hand-renamed config); every filesystem error is non-fatal and leaves no
// half-written alias behind.
//
// Categories mirror the other suites: pure policy (Decide), content
// predicates, then file-based behaviour in a unique temp directory per test
// (including a non-ASCII Steam path built through Utf8ToPath).

#include "Utils/Config/CompatAliasLogic.h"
#include "OSTPlatform/include/Encoding.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using CompatAliasLogic::Decision;

namespace {

    inline std::string u8_to_string(const char8_t* s) {
        return std::string(reinterpret_cast<const char*>(s));
    }

    // Stand-ins for the real binaries. Embedded NULs on purpose: the marker
    // search must work on arbitrary binary data, not C strings.
    const std::string kOurDll =
        std::string("MZ\x90\0\0fake-pe", 12) + "%s\\amethysttool.toml" + std::string("\0\0v1.3.5", 8);
    const std::string kOurOlderDll =
        std::string("MZ\x90\0\0fake-pe", 12) + "%s\\amethysttool.toml" + std::string("\0\0v1.3.4", 8);
    const std::string kUpstreamDll =
        std::string("MZ\x90\0\0fake-pe", 12) + "%s\\opensteamtool.toml" + std::string("\0\0upstream", 10);
    const std::string kToml = "[log]\nlevel = \"info\"\n\n[compatibility]\ndenuvo_aliases = true\n";

    // ---------------------------------------------------------------------
    // Pure policy
    // ---------------------------------------------------------------------
    TEST(CompatAliasDecide, DisabledWinsOverEverything) {
        EXPECT_EQ(CompatAliasLogic::Decide(false, true, false, false, false), Decision::Disabled);
        EXPECT_EQ(CompatAliasLogic::Decide(false, true, true, true, false), Decision::Disabled);
        EXPECT_EQ(CompatAliasLogic::Decide(false, false, true, false, false), Decision::Disabled);
    }

    TEST(CompatAliasDecide, SourceMissingBeforeAnyAliasCheck) {
        EXPECT_EQ(CompatAliasLogic::Decide(true, false, false, false, false), Decision::SourceMissing);
        EXPECT_EQ(CompatAliasLogic::Decide(true, false, true, true, true), Decision::SourceMissing);
    }

    TEST(CompatAliasDecide, CreatesWhenAliasAbsent) {
        EXPECT_EQ(CompatAliasLogic::Decide(true, true, false, false, false), Decision::Create);
    }

    TEST(CompatAliasDecide, ForeignFileIsNeverOverwritten) {
        // Present and not ours: even "matches" cannot turn it into a write.
        EXPECT_EQ(CompatAliasLogic::Decide(true, true, true, false, false), Decision::Foreign);
        EXPECT_EQ(CompatAliasLogic::Decide(true, true, true, false, true), Decision::Foreign);
    }

    TEST(CompatAliasDecide, OwnAliasIsRefreshedOrLeftAlone) {
        EXPECT_EQ(CompatAliasLogic::Decide(true, true, true, true, false), Decision::Refresh);
        EXPECT_EQ(CompatAliasLogic::Decide(true, true, true, true, true), Decision::UpToDate);
    }

    TEST(CompatAliasDecide, OnlyCreateAndRefreshWrite) {
        EXPECT_TRUE(CompatAliasLogic::ShouldWriteAlias(Decision::Create));
        EXPECT_TRUE(CompatAliasLogic::ShouldWriteAlias(Decision::Refresh));
        EXPECT_FALSE(CompatAliasLogic::ShouldWriteAlias(Decision::Disabled));
        EXPECT_FALSE(CompatAliasLogic::ShouldWriteAlias(Decision::SourceMissing));
        EXPECT_FALSE(CompatAliasLogic::ShouldWriteAlias(Decision::UpToDate));
        EXPECT_FALSE(CompatAliasLogic::ShouldWriteAlias(Decision::Foreign));
    }

    // ---------------------------------------------------------------------
    // Content predicates
    // ---------------------------------------------------------------------
    TEST(CompatAliasContent, MarkerFoundInsideBinaryData) {
        EXPECT_TRUE(CompatAliasLogic::LooksLikeAmethystDll(kOurDll));
        EXPECT_TRUE(CompatAliasLogic::LooksLikeAmethystDll(kOurOlderDll));
    }

    TEST(CompatAliasContent, UpstreamDllIsNotOurs) {
        EXPECT_FALSE(CompatAliasLogic::LooksLikeAmethystDll(kUpstreamDll));
        EXPECT_FALSE(CompatAliasLogic::LooksLikeAmethystDll(""));
        // Case matters: the marker is the exact literal compiled into the DLL.
        EXPECT_FALSE(CompatAliasLogic::LooksLikeAmethystDll("AMETHYSTTOOL.TOML"));
    }

    TEST(CompatAliasContent, TomlAliasIsHeaderPlusVerbatimSource) {
        const std::string alias = CompatAliasLogic::BuildTomlAlias(kToml);
        EXPECT_TRUE(CompatAliasLogic::IsGeneratedTomlAlias(alias));
        EXPECT_EQ(alias.substr(CompatAliasLogic::kTomlAliasHeader.size()), kToml);
    }

    TEST(CompatAliasContent, HandWrittenTomlIsNotOurs) {
        EXPECT_FALSE(CompatAliasLogic::IsGeneratedTomlAlias(kToml));
        EXPECT_FALSE(CompatAliasLogic::IsGeneratedTomlAlias(""));
        // Header must be at offset 0 -- a user comment above it means the user owns the file.
        EXPECT_FALSE(CompatAliasLogic::IsGeneratedTomlAlias(
            "# mine\n" + std::string(CompatAliasLogic::kTomlAliasHeader)));
        // Truncated header is not enough.
        EXPECT_FALSE(CompatAliasLogic::IsGeneratedTomlAlias(
            CompatAliasLogic::kTomlAliasHeader.substr(0, 20)));
    }

    // ---------------------------------------------------------------------
    // File-based behaviour
    // ---------------------------------------------------------------------
    class CompatAliasFileTest : public ::testing::Test {
    protected:
        void SetUp() override {
            dir_ = fs::temp_directory_path() /
                   ("amethyst_compat_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
            fs::remove_all(dir_);
            fs::create_directories(dir_);
        }
        void TearDown() override {
            std::error_code ec;
            fs::remove_all(dir_, ec);
        }

        static void Write(const fs::path& p, const std::string& contents) {
            std::ofstream out(p, std::ios::binary | std::ios::trunc);
            out << contents;
        }
        static std::string Read(const fs::path& p) {
            std::ifstream in(p, std::ios::binary);
            return std::string((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
        }

        // Every "*.amethyst-tmp" currently in the Steam dir (any alias, any tag).
        std::vector<fs::path> TempFiles() const {
            std::vector<fs::path> out;
            for (const auto& e : fs::directory_iterator(dir_))
                if (e.path().extension() == ".amethyst-tmp") out.push_back(e.path());
            return out;
        }

        // Backdates a file's mtime (a temp left by a process that died `age` ago).
        static void Age(const fs::path& p, std::chrono::hours age) {
            fs::last_write_time(p, fs::file_time_type::clock::now() - age);
        }

        fs::path SourceDll() const  { return dir_ / "AmethystTool.dll"; }
        fs::path AliasDll() const   { return dir_ / "OpenSteamTool.dll"; }
        fs::path SourceToml() const { return dir_ / "amethysttool.toml"; }
        fs::path AliasToml() const  { return dir_ / "opensteamtool.toml"; }

        void WriteSources() const {
            Write(SourceDll(), kOurDll);
            Write(SourceToml(), kToml);
        }

        fs::path dir_;
    };

    TEST_F(CompatAliasFileTest, DisabledPerformsNoIo) {
        WriteSources();
        const auto r = CompatAliasLogic::Apply(false, dir_);
        EXPECT_EQ(r.dll.decision, Decision::Disabled);
        EXPECT_EQ(r.toml.decision, Decision::Disabled);
        EXPECT_FALSE(fs::exists(AliasDll()));
        EXPECT_FALSE(fs::exists(AliasToml()));
    }

    TEST_F(CompatAliasFileTest, CreatesBothAliases) {
        WriteSources();
        const auto r = CompatAliasLogic::Apply(true, dir_);

        EXPECT_EQ(r.dll.decision, Decision::Create);
        EXPECT_TRUE(r.dll.written);
        EXPECT_FALSE(r.dll.error);
        EXPECT_EQ(Read(AliasDll()), kOurDll);

        EXPECT_EQ(r.toml.decision, Decision::Create);
        EXPECT_TRUE(r.toml.written);
        EXPECT_EQ(Read(AliasToml()), CompatAliasLogic::BuildTomlAlias(kToml));

        // Sources untouched, no temp files left behind.
        EXPECT_EQ(Read(SourceDll()), kOurDll);
        EXPECT_EQ(Read(SourceToml()), kToml);
        EXPECT_TRUE(TempFiles().empty());
    }

    TEST_F(CompatAliasFileTest, SecondRunIsUpToDateAndWritesNothing) {
        WriteSources();
        (void)CompatAliasLogic::Apply(true, dir_);
        const auto r = CompatAliasLogic::Apply(true, dir_);
        EXPECT_EQ(r.dll.decision, Decision::UpToDate);
        EXPECT_FALSE(r.dll.written);
        EXPECT_EQ(r.toml.decision, Decision::UpToDate);
        EXPECT_FALSE(r.toml.written);
    }

    TEST_F(CompatAliasFileTest, RealUpstreamInstallIsLeftUntouched) {
        WriteSources();
        Write(AliasDll(), kUpstreamDll);
        const std::string userToml = "[manifest]\nurl = \"opensteamtool\"\n";
        Write(AliasToml(), userToml);

        const auto r = CompatAliasLogic::Apply(true, dir_);

        EXPECT_EQ(r.dll.decision, Decision::Foreign);
        EXPECT_FALSE(r.dll.written);
        EXPECT_EQ(Read(AliasDll()), kUpstreamDll);
        EXPECT_EQ(r.toml.decision, Decision::Foreign);
        EXPECT_FALSE(r.toml.written);
        EXPECT_EQ(Read(AliasToml()), userToml);
    }

    TEST_F(CompatAliasFileTest, StaleOwnCopyIsRefreshedAfterUpdate) {
        WriteSources();
        // e.g. the user's manual rename of a v1.3.4 AmethystTool.dll.
        Write(AliasDll(), kOurOlderDll);
        Write(AliasToml(), CompatAliasLogic::BuildTomlAlias("[log]\nlevel = \"warn\"\n"));

        const auto r = CompatAliasLogic::Apply(true, dir_);

        EXPECT_EQ(r.dll.decision, Decision::Refresh);
        EXPECT_TRUE(r.dll.written);
        EXPECT_EQ(Read(AliasDll()), kOurDll);
        EXPECT_EQ(r.toml.decision, Decision::Refresh);
        EXPECT_TRUE(r.toml.written);
        EXPECT_EQ(Read(AliasToml()), CompatAliasLogic::BuildTomlAlias(kToml));
    }

    TEST_F(CompatAliasFileTest, MissingSourceDllDoesNotBlockTomlAlias) {
        Write(SourceToml(), kToml);   // no AmethystTool.dll

        const auto r = CompatAliasLogic::Apply(true, dir_);

        EXPECT_EQ(r.dll.decision, Decision::SourceMissing);
        EXPECT_FALSE(fs::exists(AliasDll()));
        EXPECT_EQ(r.toml.decision, Decision::Create);
        EXPECT_TRUE(fs::exists(AliasToml()));
    }

    TEST_F(CompatAliasFileTest, DirectoryUnderAliasNameIsForeign) {
        WriteSources();
        fs::create_directories(AliasDll());

        const auto r = CompatAliasLogic::Apply(true, dir_);

        EXPECT_EQ(r.dll.decision, Decision::Foreign);
        EXPECT_FALSE(r.dll.written);
        EXPECT_TRUE(fs::is_directory(AliasDll()));
    }

    // Fail-safe: the write itself fails (a directory squats on our temp name).
    // Must not throw, must report the error, must leave no alias behind -- and
    // the TOML alias is still written independently.
    // ── Write failures (deterministic, via the exposed building blocks) ──────
    //
    // Apply() itself can no longer be steered into a write failure: each call's
    // temp name is unique, so there is nothing fixed to block. The two failure
    // points are the temp write and the final rename; both are driven directly.

    TEST_F(CompatAliasFileTest, TempWriteFailureIsReportedAndLeavesNothing) {
        // The directory that should hold the temp does not exist.
        const fs::path alias = dir_ / "missing-subdir" / "OpenSteamTool.dll";
        std::error_code ec;
        EXPECT_NO_THROW(ec = CompatAliasLogic::detail::WriteBytesViaTemp(kOurDll, alias, "t1"));
        EXPECT_TRUE(static_cast<bool>(ec));
        EXPECT_FALSE(fs::exists(alias));
        EXPECT_FALSE(fs::exists(dir_ / "missing-subdir"));
    }

    TEST_F(CompatAliasFileTest, RenameFailureIsReportedRemovesTheTempAndKeepsTheAlias) {
        // A directory squatting on the alias name makes the final rename fail
        // (Apply never gets this far -- Decide calls it Foreign -- but a directory
        // can appear between Decide and the rename).
        fs::create_directories(AliasDll());
        Write(AliasDll() / "keep.txt", "untouched");

        std::error_code ec;
        EXPECT_NO_THROW(ec = CompatAliasLogic::detail::WriteBytesViaTemp(kOurDll, AliasDll(), "t2"));
        EXPECT_TRUE(static_cast<bool>(ec));
        EXPECT_TRUE(TempFiles().empty());                       // the written temp was cleaned up
        EXPECT_TRUE(fs::is_directory(AliasDll()));
        EXPECT_EQ(Read(AliasDll() / "keep.txt"), "untouched");
    }

    TEST_F(CompatAliasFileTest, SuccessfulWriteReplacesAnExistingOwnAliasAtomically) {
        Write(AliasDll(), kOurOlderDll);
        const auto ec = CompatAliasLogic::detail::WriteBytesViaTemp(kOurDll, AliasDll(), "t3");
        EXPECT_FALSE(static_cast<bool>(ec));
        EXPECT_EQ(Read(AliasDll()), kOurDll);
        EXPECT_TRUE(TempFiles().empty());
    }

    TEST_F(CompatAliasFileTest, EmptyPayloadIsWrittenAsAnEmptyFile) {
        const auto ec = CompatAliasLogic::detail::WriteBytesViaTemp("", AliasToml(), "t4");
        EXPECT_FALSE(static_cast<bool>(ec));
        ASSERT_TRUE(fs::exists(AliasToml()));
        EXPECT_EQ(fs::file_size(AliasToml()), 0u);
    }

    // ── Temp names ───────────────────────────────────────────────────────────

    TEST(CompatAliasTempNames, TagsAreUniqueAcrossCallsEvenWithTheSameProcessTag) {
        std::vector<std::string> seen;
        for (int i = 0; i < 1000; ++i) seen.push_back(CompatAliasLogic::detail::MakeTempTag("4242"));
        std::sort(seen.begin(), seen.end());
        EXPECT_EQ(std::adjacent_find(seen.begin(), seen.end()), seen.end());
    }

    TEST(CompatAliasTempNames, ProcessTagIsSanitisedAndBoundedSoItCannotEscapeTheDirectory) {
        const std::string hostile = CompatAliasLogic::detail::MakeTempTag("../../evil\\name:with*bad?chars");
        EXPECT_EQ(hostile.find_first_of("/\\:*?\"<>|"), std::string::npos);
        EXPECT_EQ(hostile.find(".."), std::string::npos);
        const std::string longTag = CompatAliasLogic::detail::MakeTempTag(std::string(500, 'a'));
        EXPECT_LT(longTag.size(), 100u);

        const fs::path tmp = CompatAliasLogic::detail::TempPathFor(fs::path("steam") / "OpenSteamTool.dll", hostile);
        EXPECT_EQ(tmp.parent_path(), fs::path("steam"));          // still a sibling of the alias
        EXPECT_EQ(tmp.extension(), ".amethyst-tmp");
    }

    TEST(CompatAliasTempNames, EmptyProcessTagStillProducesAUsableTag) {
        const std::string tag = CompatAliasLogic::detail::MakeTempTag("");
        EXPECT_FALSE(tag.empty());
        EXPECT_NE(tag.front(), '-');
    }

    // ── Concurrent writers (two steam.exe, or two threads) ──────────────────
    //
    // Regression for the shared-temp-name design: with one fixed temp name per
    // alias, concurrent Apply() calls renamed each other's half-written temp into
    // place (an observer saw a partial alias) and failed when their own temp had
    // already been moved. Each call now owns its temp, so the alias is only ever
    // absent or complete.
    //
    // The observer only stat()s the alias -- it never keeps a handle open, because
    // on Windows an open handle (std::ifstream has no FILE_SHARE_DELETE) would make
    // the writers' rename fail for a reason that has nothing to do with the code
    // under test. For the same reason "every call succeeds" is asserted on POSIX
    // only, where rename-over is atomic by specification; on Windows two renames
    // onto one target can transiently collide, which Apply() reports (and the next
    // launch retries) rather than corrupts: what must hold everywhere is that at
    // least one write lands, nothing is ever partial, and nothing is left behind.

    TEST_F(CompatAliasFileTest, ConcurrentApplyNeverExposesAPartialAliasOrLeavesTemps) {
        // A big-enough "DLL" that a write takes real time, marker included.
        const std::string bigDll = kOurDll + std::string(1024 * 1024, 'x');
        Write(SourceDll(), bigDll);
        Write(SourceToml(), kToml);

        constexpr int kRounds  = 4;
        constexpr int kThreads = 8;
        for (int round = 0; round < kRounds; ++round) {
            std::error_code ignored;
            fs::remove(AliasDll(), ignored);
            fs::remove(AliasToml(), ignored);

            std::atomic<bool> stop{ false };
            std::atomic<int> partialObservations{ 0 };
            std::thread observer([&] {
                while (!stop.load()) {
                    std::error_code e;
                    const auto size = fs::file_size(AliasDll(), e);
                    if (!e && size != bigDll.size()) ++partialObservations;
                }
            });

            std::atomic<int> failures{ 0 };
            std::atomic<int> successes{ 0 };
            std::vector<std::thread> writers;
            for (int i = 0; i < kThreads; ++i)
                writers.emplace_back([&] {
                    const auto r = CompatAliasLogic::Apply(true, dir_, "9999");
                    if (r.dll.error || r.toml.error) ++failures;
                    if (r.dll.written && r.toml.written) ++successes;
                });
            for (auto& t : writers) t.join();
            stop = true;
            observer.join();

            EXPECT_GE(successes.load(), 1) << "round " << round;
#ifndef _WIN32
            EXPECT_EQ(failures.load(), 0) << "round " << round;
#endif
            EXPECT_EQ(partialObservations.load(), 0) << "round " << round;
            EXPECT_EQ(Read(AliasDll()), bigDll) << "round " << round;
            EXPECT_EQ(Read(AliasToml()), CompatAliasLogic::BuildTomlAlias(kToml)) << "round " << round;
            EXPECT_TRUE(TempFiles().empty()) << "round " << round;
        }
    }

    // ── Orphaned temp files (a process killed mid-write) ────────────────────

    TEST_F(CompatAliasFileTest, StaleTempsFromAKilledWriterAreSweptAndFreshOnesKept) {
        WriteSources();
        const fs::path staleDll  = dir_ / "OpenSteamTool.dll.1234-0-99.amethyst-tmp";
        const fs::path staleToml = dir_ / "opensteamtool.toml.1234-1-99.amethyst-tmp";
        const fs::path legacy    = dir_ / "OpenSteamTool.dll.amethyst-tmp";   // fixed name (script / early builds)
        const fs::path fresh     = dir_ / "OpenSteamTool.dll.5678-0-99.amethyst-tmp";  // a live writer's
        Write(staleDll, "partial");  Write(staleToml, "partial");  Write(legacy, "partial");
        Write(fresh, "in progress");
        Age(staleDll, std::chrono::hours(2));
        Age(staleToml, std::chrono::hours(2));
        Age(legacy, std::chrono::hours(2));

        const auto r = CompatAliasLogic::Apply(true, dir_, "1");

        EXPECT_EQ(r.staleTempsRemoved, 3u);
        EXPECT_FALSE(fs::exists(staleDll));
        EXPECT_FALSE(fs::exists(staleToml));
        EXPECT_FALSE(fs::exists(legacy));
        EXPECT_TRUE(fs::exists(fresh));                       // might be another process's live temp
        EXPECT_EQ(Read(fresh), "in progress");
        // And the aliases themselves came out right regardless.
        EXPECT_EQ(Read(AliasDll()), kOurDll);
        EXPECT_EQ(Read(AliasToml()), CompatAliasLogic::BuildTomlAlias(kToml));
    }

    TEST_F(CompatAliasFileTest, SweepOnlyTouchesExactTempShapes) {
        const fs::path unrelated1 = dir_ / "OpenSteamTool.dll.bak";
        const fs::path unrelated2 = dir_ / "something.amethyst-tmp";            // not an alias temp
        const fs::path unrelated3 = dir_ / "OpenSteamTool.dllX.amethyst-tmp";   // alias name + junk, no '.'
        const fs::path unrelated4 = dir_ / "amethysttool.toml.1-0-0.amethyst-tmp";  // the SOURCE's name, not an alias
        const fs::path realAlias  = dir_ / "OpenSteamTool.dll";
        const fs::path dirLookalike = dir_ / "OpenSteamTool.dll.7-0-0.amethyst-tmp";
        Write(unrelated1, "x"); Write(unrelated2, "x"); Write(unrelated3, "x");
        Write(unrelated4, "x"); Write(realAlias, kOurDll);
        fs::create_directories(dirLookalike);
        for (const auto& p : { unrelated1, unrelated2, unrelated3, unrelated4, realAlias })
            Age(p, std::chrono::hours(48));
        Age(dirLookalike, std::chrono::hours(48));

        EXPECT_EQ(CompatAliasLogic::RemoveStaleTemps(dir_), 0u);

        for (const auto& p : { unrelated1, unrelated2, unrelated3, unrelated4, realAlias })
            EXPECT_TRUE(fs::exists(p)) << p;
        EXPECT_TRUE(fs::is_directory(dirLookalike));          // a directory is never removed
    }

    TEST_F(CompatAliasFileTest, SweepAgeBoundaryAndMissingDirectory) {
        const fs::path t = dir_ / "opensteamtool.toml.1-0-0.amethyst-tmp";
        Write(t, "x");
        Age(t, std::chrono::hours(1));
        // Older than a 2h threshold? No -> kept. Older than 30min? Yes -> removed.
        EXPECT_EQ(CompatAliasLogic::RemoveStaleTemps(dir_, std::chrono::hours(2)), 0u);
        EXPECT_TRUE(fs::exists(t));
        EXPECT_EQ(CompatAliasLogic::RemoveStaleTemps(dir_, std::chrono::minutes(30)), 1u);
        EXPECT_FALSE(fs::exists(t));
        // A directory that is not there is not an error.
        EXPECT_EQ(CompatAliasLogic::RemoveStaleTemps(dir_ / "nope"), 0u);
    }

    TEST_F(CompatAliasFileTest, DisabledNeverSweeps) {
        const fs::path stale = dir_ / "OpenSteamTool.dll.1-0-0.amethyst-tmp";
        Write(stale, "x");
        Age(stale, std::chrono::hours(48));
        const auto r = CompatAliasLogic::Apply(false, dir_, "1");
        EXPECT_EQ(r.staleTempsRemoved, 0u);
        EXPECT_TRUE(fs::exists(stale));                       // enabled = false means zero I/O
    }

    TEST_F(CompatAliasFileTest, DecideHelpersAreReadOnly) {
        WriteSources();
        EXPECT_EQ(CompatAliasLogic::DecideDllAlias(true, SourceDll(), AliasDll()), Decision::Create);
        EXPECT_EQ(CompatAliasLogic::DecideTomlAlias(true, SourceToml(), AliasToml()), Decision::Create);
        EXPECT_FALSE(fs::exists(AliasDll()));
        EXPECT_FALSE(fs::exists(AliasToml()));
        EXPECT_EQ(CompatAliasLogic::DecideDllAlias(false, SourceDll(), AliasDll()), Decision::Disabled);
    }

    // Non-ASCII Steam install path, decoded the same way dllmain.cpp does it
    // (SteamInstallPath is UTF-8 -> Utf8ToPath). On MSVC the ANSI-codepage
    // path(std::string) constructor would mangle this.
    TEST_F(CompatAliasFileTest, NonAsciiSteamPath) {
        const std::string utf8Dir =
            OSTPlatform::Encoding::PathToUtf8(dir_) + "/" + u8_to_string(u8"Usuários São Paulo 日本語/Steam");
        const fs::path steamDir = OSTPlatform::Encoding::Utf8ToPath(utf8Dir);
        fs::create_directories(steamDir);
        Write(steamDir / "AmethystTool.dll", kOurDll);
        Write(steamDir / "amethysttool.toml", kToml);

        const auto r = CompatAliasLogic::Apply(true, steamDir);

        EXPECT_TRUE(r.dll.written);
        EXPECT_TRUE(r.toml.written);
        EXPECT_EQ(Read(steamDir / "OpenSteamTool.dll"), kOurDll);
        EXPECT_EQ(Read(steamDir / "opensteamtool.toml"), CompatAliasLogic::BuildTomlAlias(kToml));
        // And it landed in the decoded directory, not a mangled sibling.
        EXPECT_TRUE(fs::exists(OSTPlatform::Encoding::Utf8ToPath(utf8Dir + "/OpenSteamTool.dll")));
    }

} // namespace
