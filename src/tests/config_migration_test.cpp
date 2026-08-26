// Unit tests for Config::MigrateLegacyConfig — the one-time copy that carries a
// user's settings over from the pre-rename config name (opensteamtool.toml) to
// the current one (amethysttool.toml) when they update from upstream.
//
// Contract per ConfigMigration.h: copy (never move) legacy -> new ONLY when the
// new file is absent and the legacy file exists; never overwrite an existing new
// file; leave the legacy file in place; return true iff a copy happened; failures
// are non-fatal (return false). Contents/keys are never rewritten.
//
// The unit touches the real filesystem, so each test works inside its own unique
// temp directory and cleans up after itself. Categories mirror the other suites:
// happy path, guard cases (must NOT act), and false-positive guards.

#include "Utils/Config/ConfigMigration.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace {

    // Unique scratch directory per test; removed on teardown.
    class ConfigMigrationTest : public ::testing::Test {
    protected:
        void SetUp() override {
            dir_ = fs::temp_directory_path() /
                   ("amethyst_migration_" +
                    std::to_string(reinterpret_cast<uintptr_t>(this)));
            fs::remove_all(dir_);
            fs::create_directories(dir_);
            legacy_ = dir_ / "opensteamtool.toml";
            current_ = dir_ / "amethysttool.toml";
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

        fs::path dir_;
        fs::path legacy_;
        fs::path current_;
    };

    // Happy path: legacy present, new absent -> copy performed, contents identical.
    TEST_F(ConfigMigrationTest, MigratesWhenLegacyPresentAndTargetAbsent) {
        const std::string body = "[log]\nlevel = \"info\"\n";
        Write(legacy_, body);

        EXPECT_TRUE(Config::MigrateLegacyConfig(current_, legacy_));

        ASSERT_TRUE(fs::exists(current_));
        EXPECT_EQ(Read(current_), body);   // contents copied verbatim, keys untouched
    }

    // Copy, not move: the legacy file must remain for backward compatibility.
    TEST_F(ConfigMigrationTest, PreservesLegacyFileAfterMigration) {
        Write(legacy_, "x = 1\n");

        EXPECT_TRUE(Config::MigrateLegacyConfig(current_, legacy_));

        EXPECT_TRUE(fs::exists(legacy_));  // not moved/deleted
    }

    // Guard: an existing new config must never be overwritten (one-time, new wins).
    TEST_F(ConfigMigrationTest, DoesNotOverwriteExistingTarget) {
        Write(legacy_, "from = \"legacy\"\n");
        const std::string kept = "from = \"current\"\n";
        Write(current_, kept);

        EXPECT_FALSE(Config::MigrateLegacyConfig(current_, legacy_));

        EXPECT_EQ(Read(current_), kept);   // untouched
    }

    // Nothing to migrate: neither file exists.
    TEST_F(ConfigMigrationTest, NoOpWhenNeitherExists) {
        EXPECT_FALSE(Config::MigrateLegacyConfig(current_, legacy_));
        EXPECT_FALSE(fs::exists(current_));
    }

    // Fresh install: only the new file exists, no legacy to pull from.
    TEST_F(ConfigMigrationTest, NoOpWhenOnlyTargetExists) {
        const std::string kept = "fresh = true\n";
        Write(current_, kept);

        EXPECT_FALSE(Config::MigrateLegacyConfig(current_, legacy_));
        EXPECT_EQ(Read(current_), kept);
    }

    // Filesystem error is non-fatal: a legacy exists but the copy cannot succeed
    // (target's parent directory does not exist) -> returns false, never throws,
    // and produces no partial file. The caller then proceeds with defaults.
    TEST_F(ConfigMigrationTest, ReturnsFalseWhenCopyFails) {
        Write(legacy_, "x = 1\n");
        const fs::path unreachable = dir_ / "missing_subdir" / "amethysttool.toml";

        EXPECT_FALSE(Config::MigrateLegacyConfig(unreachable, legacy_));
        EXPECT_FALSE(fs::exists(unreachable));
        EXPECT_TRUE(fs::exists(legacy_));  // legacy still intact
    }

} // namespace
