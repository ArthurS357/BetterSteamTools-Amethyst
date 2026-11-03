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

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

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
        EXPECT_FALSE(fs::exists(dir_ / "OpenSteamTool.dll.amethyst-tmp"));
        EXPECT_FALSE(fs::exists(dir_ / "opensteamtool.toml.amethyst-tmp"));
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
    TEST_F(CompatAliasFileTest, WriteFailureIsReportedNotThrown) {
        WriteSources();
        fs::create_directories(dir_ / "OpenSteamTool.dll.amethyst-tmp");

        CompatAliasLogic::ApplyResult r;
        EXPECT_NO_THROW(r = CompatAliasLogic::Apply(true, dir_));

        EXPECT_EQ(r.dll.decision, Decision::Create);
        EXPECT_FALSE(r.dll.written);
        EXPECT_TRUE(static_cast<bool>(r.dll.error));
        EXPECT_FALSE(fs::exists(AliasDll()));
        EXPECT_TRUE(r.toml.written);
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
