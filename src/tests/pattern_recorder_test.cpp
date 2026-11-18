// Tests for PatternRecorder::BuildFingerprintFile (v1.5.0, pillar 4): the disk side of
// the shadow-mode recorder, run on a synthetic PE32+ DLL written to a temp folder. The
// text it returns is parsed with the same TOML library the pattern loader uses, so the
// keys and shapes a future tools/pattern_migrate reads are checked for real. Nothing here
// touches Steam, a hook, or the atomic write (that is RemoteToml::ReplaceCacheFile).

#include "OSTPlatform/include/Hash.h"
#include "Utils/SteamMetadata/PatternRecorder.h"
#include "synthetic_image.h"

#include <gtest/gtest.h>

#include <toml++/toml.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace SyntheticImage;
using PatternMigrateLogic::NamedRva;

namespace {

    namespace fs = std::filesystem;

    constexpr std::uint32_t kSeedF = 0xF0F0F0F0;
    constexpr std::uint32_t kSeedA = 0xA0A0A0A0;

    std::string ShaOf(const Bytes& b) {
        return OSTPlatform::Hash::Sha256OfBuffer(b.data(), b.size());
    }

    // A fresh folder under the system temp dir, removed on destruction.
    class TempDir {
    public:
        TempDir() {
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            path_ = fs::temp_directory_path() / ("amethyst_recorder_test_" + std::to_string(stamp));
            fs::create_directories(path_);
        }
        ~TempDir() { std::error_code ec; fs::remove_all(path_, ec); }
        TempDir(const TempDir&) = delete;
        TempDir& operator=(const TempDir&) = delete;
        const fs::path& Path() const { return path_; }
    private:
        fs::path path_;
    };

    fs::path WriteFile(const TempDir& dir, const std::string& name, const Bytes& data) {
        const fs::path p = dir.Path() / name;
        std::ofstream out(p, std::ios::binary);
        out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        return p;
    }

    struct World {
        Img img;
        std::uint32_t a = 0;
        std::uint32_t f = 0;
        std::vector<NamedRva> resolved;
    };

    World MakeWorld() {
        World w;
        w.a = w.img.Add(Func(kSeedA, 100));
        w.f = w.img.Add(Func(kSeedF, 120));
        w.img.PatchCall(w.f, w.a);
        w.resolved = { { "BuildDepotDependency", w.f }, { "GetAppByID", w.a } };
        return w;
    }

    // The DLL on disk and the SHA-256 of exactly those bytes, as the pattern loader names it.
    struct Dll {
        fs::path path;
        std::string sha;
    };

    Dll WriteDll(const TempDir& dir, const Bytes& bytes) {
        return { WriteFile(dir, "steamclient64.dll", bytes), ShaOf(bytes) };
    }

    const toml::table* FunctionNamed(const toml::table& root, const std::string& name) {
        const toml::array* arr = root["function"].as_array();
        if (!arr) return nullptr;
        for (const toml::node& n : *arr)
            if (const toml::table* t = n.as_table())
                if ((*t)["name"].value<std::string>() == name) return t;
        return nullptr;
    }

    // ── the happy path ───────────────────────────────────────────────

    TEST(PatternRecorderFile, WritesAParsableTomlWithTheKeysTheToolReads) {
        TempDir dir;
        const World w = MakeWorld();
        const Dll dll = WriteDll(dir, BuildPeFile(w.img));

        std::string why;
        const auto body = PatternRecorder::BuildFingerprintFile(dll.path, "steamclient", dll.sha, w.resolved, &why);
        ASSERT_TRUE(body.has_value()) << why;

        toml::table root;
        ASSERT_NO_THROW(root = toml::parse(*body));
        EXPECT_EQ(root["version"].value<std::int64_t>(), 1);
        EXPECT_EQ(root["component"].value<std::string>(), "steamclient");
        EXPECT_EQ(root["sha256"].value<std::string>(), dll.sha);
        EXPECT_EQ(root["prefix_len"].value<std::int64_t>(), 64);
        EXPECT_FALSE(root.contains("skipped"));

        const toml::array* fns = root["function"].as_array();
        ASSERT_NE(fns, nullptr);
        EXPECT_EQ(fns->size(), 2u);

        const toml::table* f = FunctionNamed(root, "BuildDepotDependency");
        ASSERT_NE(f, nullptr);
        EXPECT_EQ((*f)["rva"].value<std::string>(), "0x1070");        // second function, 16-aligned after the first
        EXPECT_EQ((*f)["pdata"].value<bool>(), true);
        EXPECT_EQ((*f)["size"].value<std::int64_t>(), 120);
        EXPECT_EQ((*f)["complete"].value<bool>(), true);
        EXPECT_EQ((*f)["matches"].value<std::int64_t>(), 1);
        const std::string sig = (*f)["sig"].value_or(std::string());
        EXPECT_EQ(sig.rfind("48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 B8 F0 F0 F0 F0 E8 ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ??", 0), 0u);
        const toml::array* calls = (*f)["calls"].as_array();
        ASSERT_NE(calls, nullptr);
        ASSERT_EQ(calls->size(), 1u);
        EXPECT_EQ(calls->front().value<std::string>(), "GetAppByID");

        const toml::table* a = FunctionNamed(root, "GetAppByID");
        ASSERT_NE(a, nullptr);
        EXPECT_EQ((*a)["rva"].value<std::string>(), "0x1000");
        EXPECT_EQ((*a)["size"].value<std::int64_t>(), 100);
        EXPECT_EQ((*a)["calls"].as_array()->size(), 0u);
    }

    TEST(PatternRecorderFile, TheSameDllAndInputGiveTheSameBytesWhateverTheOrder) {
        TempDir dir;
        const World w = MakeWorld();
        const Dll dll = WriteDll(dir, BuildPeFile(w.img));

        std::vector<NamedRva> reversed(w.resolved.rbegin(), w.resolved.rend());
        const auto one = PatternRecorder::BuildFingerprintFile(dll.path, "steamclient", dll.sha, w.resolved, nullptr);
        const auto two = PatternRecorder::BuildFingerprintFile(dll.path, "steamclient", dll.sha, reversed, nullptr);
        ASSERT_TRUE(one.has_value());
        ASSERT_TRUE(two.has_value());
        EXPECT_EQ(*one, *two);
    }

    TEST(PatternRecorderFile, TheFileNamesNothingAboutTheMachine) {
        TempDir dir;
        const World w = MakeWorld();
        const Dll dll = WriteDll(dir, BuildPeFile(w.img));
        const auto body = PatternRecorder::BuildFingerprintFile(dll.path, "steamclient", dll.sha, w.resolved, nullptr);
        ASSERT_TRUE(body.has_value());
        EXPECT_EQ(body->find(dir.Path().filename().string()), std::string::npos);
        EXPECT_EQ(body->find("steamclient64"), std::string::npos);
        EXPECT_EQ(body->find(":\\"), std::string::npos);
    }

    TEST(PatternRecorderFile, AFunctionOutsideTextIsListedAsSkippedAndTheRestIsKept) {
        TempDir dir;
        World w = MakeWorld();
        w.resolved.push_back({ "FromAnotherModule", 0x10 });
        const Dll dll = WriteDll(dir, BuildPeFile(w.img));

        const auto body = PatternRecorder::BuildFingerprintFile(dll.path, "steamclient", dll.sha, w.resolved, nullptr);
        ASSERT_TRUE(body.has_value());
        const toml::table root = toml::parse(*body);
        const toml::array* skipped = root["skipped"].as_array();
        ASSERT_NE(skipped, nullptr);
        ASSERT_EQ(skipped->size(), 1u);
        EXPECT_EQ(skipped->front().value<std::string>(), "FromAnotherModule");
        EXPECT_EQ(root["function"].as_array()->size(), 2u);
    }

    TEST(PatternRecorderFile, ANameThatNeedsEscapingStillProducesValidToml) {
        TempDir dir;
        World w = MakeWorld();
        w.resolved = { { "Odd\"Name\\x", w.f } };
        const Dll dll = WriteDll(dir, BuildPeFile(w.img));

        const auto body = PatternRecorder::BuildFingerprintFile(dll.path, "steamclient", dll.sha, w.resolved, nullptr);
        ASSERT_TRUE(body.has_value());
        toml::table root;
        ASSERT_NO_THROW(root = toml::parse(*body));
        EXPECT_NE(FunctionNamed(root, "Odd\"Name\\x"), nullptr);
    }

    // ── the bytes must be the ones the SHA-256 names ─────────────────

    TEST(PatternRecorderFile, ADllThatChangedSinceItWasHashedIsRefused) {
        TempDir dir;
        const World w = MakeWorld();
        const Bytes original = BuildPeFile(w.img);
        const std::string shaAtLoad = ShaOf(original);

        Bytes replaced = original;                                   // "Steam updated the file meanwhile"
        replaced[kRawStart + 40] = static_cast<std::uint8_t>(replaced[kRawStart + 40] ^ 0xFF);   // one byte inside .text
        const fs::path dll = WriteFile(dir, "steamclient64.dll", replaced);

        std::string why;
        EXPECT_FALSE(PatternRecorder::BuildFingerprintFile(dll, "steamclient", shaAtLoad, w.resolved, &why).has_value());
        EXPECT_NE(why.find("SHA-256"), std::string::npos) << why;

        // the same file under ITS hash is fine
        EXPECT_TRUE(PatternRecorder::BuildFingerprintFile(dll, "steamclient", ShaOf(replaced), w.resolved, nullptr).has_value());
    }

    TEST(PatternRecorderFile, AWellFormedButUnrelatedShaIsRefused) {
        TempDir dir;
        const World w = MakeWorld();
        const Dll dll = WriteDll(dir, BuildPeFile(w.img));
        std::string why;
        EXPECT_FALSE(PatternRecorder::BuildFingerprintFile(dll.path, "steamclient", std::string(64, 'b'), w.resolved, &why).has_value());
        EXPECT_NE(why.find("SHA-256"), std::string::npos) << why;
    }

    // ── refusals ─────────────────────────────────────────────────────

    TEST(PatternRecorderFile, UnsafePathSegmentsAreRefusedBeforeAnyRead) {
        TempDir dir;
        const World w = MakeWorld();
        const Dll dll = WriteDll(dir, BuildPeFile(w.img));
        std::string why;
        EXPECT_FALSE(PatternRecorder::BuildFingerprintFile(dll.path, "../x", dll.sha, w.resolved, &why).has_value());
        EXPECT_FALSE(why.empty());
        EXPECT_FALSE(PatternRecorder::BuildFingerprintFile(dll.path, "steamclient", "not-a-hash", w.resolved, &why).has_value());
        EXPECT_FALSE(PatternRecorder::BuildFingerprintFile(dll.path, "steamclient", std::string(64, 'B'), w.resolved, &why).has_value());
    }

    TEST(PatternRecorderFile, AMissingEmptyOrNonPeFileIsRefused) {
        TempDir dir;
        const World w = MakeWorld();
        std::string why;

        const std::string anySha(64, 'c');
        EXPECT_FALSE(PatternRecorder::BuildFingerprintFile(dir.Path() / "nope.dll", "steamclient", anySha, w.resolved, &why).has_value());
        EXPECT_FALSE(why.empty());

        const fs::path empty = WriteFile(dir, "empty.dll", Bytes{});
        why.clear();
        EXPECT_FALSE(PatternRecorder::BuildFingerprintFile(empty, "steamclient", anySha, w.resolved, &why).has_value());
        EXPECT_FALSE(why.empty());

        // With the right hash the failure is the PE check, not the hash check.
        const Bytes tinyBytes{ 'M', 'Z', 0, 0 };
        const fs::path tiny = WriteFile(dir, "tiny.dll", tinyBytes);
        why.clear();
        EXPECT_FALSE(PatternRecorder::BuildFingerprintFile(tiny, "steamclient", ShaOf(tinyBytes), w.resolved, &why).has_value());
        EXPECT_NE(why.find("PE"), std::string::npos) << why;

        const Bytes textBytes(5000, 'x');
        const fs::path text = WriteFile(dir, "text.dll", textBytes);
        why.clear();
        EXPECT_FALSE(PatternRecorder::BuildFingerprintFile(text, "steamclient", ShaOf(textBytes), w.resolved, &why).has_value());
        EXPECT_NE(why.find("PE"), std::string::npos) << why;
    }

    TEST(PatternRecorderFile, ATruncatedDllIsRefusedNotMisread) {
        TempDir dir;
        const World w = MakeWorld();
        Bytes file = BuildPeFile(w.img);
        file.resize(kRawStart + AlignUp(w.img.text.size(), 0x200) + 8);   // only 8 of the 24 .pdata bytes left
        const Dll dll = WriteDll(dir, file);                              // hashed as it is: the PE check must catch it
        std::string why;
        EXPECT_FALSE(PatternRecorder::BuildFingerprintFile(dll.path, "steamclient", dll.sha, w.resolved, &why).has_value());
        EXPECT_NE(why.find("PE"), std::string::npos) << why;
    }

    TEST(PatternRecorderFile, NothingToFingerprintIsAFailureNotAnEmptyFile) {
        TempDir dir;
        const World w = MakeWorld();
        const Dll dll = WriteDll(dir, BuildPeFile(w.img));
        std::string why;

        EXPECT_FALSE(PatternRecorder::BuildFingerprintFile(dll.path, "steamclient", dll.sha, {}, &why).has_value());
        EXPECT_FALSE(PatternRecorder::BuildFingerprintFile(dll.path, "steamclient", dll.sha, { { "Gone", 0x10 } }, &why).has_value());
        EXPECT_NE(why.find("fingerprinted"), std::string::npos) << why;
    }

} // namespace
