// Tests for RuntimeMigrateLogic (v1.6.0): which fingerprint files the runtime migration
// tries, when a result is accepted, the file it becomes, and that writing it can never
// damage a pattern file already on disk. Synthetic images only; the file writes go to a
// temp folder. Runs on Linux too (no Windows header, no toml++).

#include "Utils/Config/CompatAliasLogic.h"
#include "Utils/SteamMetadata/RuntimeMigrateLogic.h"
#include "synthetic_image.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace RuntimeMigrateLogic;
using namespace SyntheticImage;
using PatternMigrateLogic::NamedRva;

namespace {

    const std::string kOldSha(64, 'a');
    const std::string kNewSha(64, 'b');

    class TempDir {
    public:
        TempDir() {
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            path_ = fs::temp_directory_path() / ("amethyst_runtime_migrate_test_" + std::to_string(stamp));
            fs::create_directories(path_);
        }
        ~TempDir() { std::error_code ec; fs::remove_all(path_, ec); }
        TempDir(const TempDir&) = delete;
        TempDir& operator=(const TempDir&) = delete;
        const fs::path& Path() const { return path_; }
    private:
        fs::path path_;
    };

    std::string ReadAll(const fs::path& p) {
        std::ifstream in(p, std::ios::binary);
        return { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
    }

    void WriteAll(const fs::path& p, const std::string& body) {
        fs::create_directories(p.parent_path());
        std::ofstream(p, std::ios::binary) << body;
    }

    // The old build (what the recorder saw): A, then F which calls A.
    std::string OldFingerprintText(const std::string& sha = kOldSha, const std::string& component = "steamclient") {
        Img old;
        const std::uint32_t a = old.Add(Func(0xA0A0A0A0, 100));
        const std::uint32_t f = old.Add(Func(0xF0F0F0F0, 120));
        old.PatchCall(f, a);
        const std::vector<NamedRva> resolved = { { "BuildDepotDependency", f }, { "GetAppByID", a } };
        return PatternRecorderLogic::SerializeFingerprints(component, sha,
                                                           PatternRecorderLogic::RecordFunctions(old.View(), resolved));
    }

    // The new build: everything moved, F 10 bytes longer.
    struct NewBuild {
        Img img;
        std::uint32_t a = 0, f = 0;
    };
    NewBuild MakeNewBuild() {
        NewBuild n;
        n.img.Add(Func(0x11111111, 80));
        n.a = n.img.Add(Func(0xA0A0A0A0, 100));
        n.f = n.img.Add(Func(0xF0F0F0F0, 130));
        n.img.PatchCall(n.f, n.a);
        return n;
    }

    // A build that has nothing in common with the old one.
    Img UnrelatedBuild() {
        Img img;
        img.Add(Func(0x33333333, 90));
        img.Add(Func(0x44444444, 110));
        return img;
    }

    bool WriteViaTemp(const fs::path& target, const std::string& body) {
        return !CompatAliasLogic::detail::WriteBytesViaTemp(body, target, CompatAliasLogic::detail::MakeTempTag("t"));
    }

} // namespace

// ── marker ───────────────────────────────────────────────────────────

TEST(RuntimeMigrateMarker, OnlyTheFirstLineCounts) {
    EXPECT_TRUE(IsAutoMigrated(std::string(kMarker) + " (v1.6.0)\n[0x1]\n"));
    EXPECT_TRUE(IsAutoMigrated("\xEF\xBB\xBF" + std::string(kMarker)));
    EXPECT_FALSE(IsAutoMigrated(""));
    EXPECT_FALSE(IsAutoMigrated("[0x82428E37]\nname = \"X\"\n" + std::string(kMarker)));
    EXPECT_FALSE(IsAutoMigrated("# AmethystTool candidate pattern file, made by tools/pattern_migrate"))
        << "a file the offline tool made and a human placed is authoritative, not a stopgap";
}

// ── candidates ───────────────────────────────────────────────────────

TEST(RuntimeMigrateCandidates, CurrentBuildFirstThenNewestCappedAndClean) {
    const std::string c(64, 'c'), d(64, 'd'), e(64, 'e'), f(64, 'f');
    const auto order = OrderCandidates({ { c, 10 }, { kNewSha, 1 }, { d, 30 }, { "notasha", 99 },
                                         { e, 20 }, { std::string(64, 'A'), 50 }, { f, 5 }, { d, 30 } },
                                       kNewSha);
    EXPECT_EQ(order, (std::vector<std::string>{ kNewSha, d, e })) << "upper-case and non-hex names are dropped";
    EXPECT_TRUE(OrderCandidates({}, kNewSha).empty());
    EXPECT_EQ(OrderCandidates({ { c, 1 }, { d, 1 } }, kNewSha), (std::vector<std::string>{ c, d })) << "ties by name";
}

TEST(RuntimeMigrateCandidates, EnoughMigratedThreshold) {
    EXPECT_FALSE(EnoughMigrated(0, 0));
    EXPECT_FALSE(EnoughMigrated(0, 10));
    EXPECT_FALSE(EnoughMigrated(4, 10));
    EXPECT_TRUE(EnoughMigrated(5, 10));
    EXPECT_TRUE(EnoughMigrated(1, 2));
    EXPECT_TRUE(EnoughMigrated(20, 20));
    EXPECT_FALSE(EnoughMigrated(3, 2)) << "more migrated than recorded is nonsense";
}

// ── one attempt ──────────────────────────────────────────────────────

TEST(RuntimeMigrateAttempt, AcceptsAndRendersALoadablePatternFile) {
    const NewBuild n = MakeNewBuild();
    const Attempt a = TryOne(OldFingerprintText(), "steamclient", kOldSha, n.img.View());
    ASSERT_TRUE(a.accepted) << a.why;
    EXPECT_EQ(a.migrated, 2u);
    EXPECT_EQ(a.total, 2u);
    EXPECT_TRUE(IsAutoMigrated(a.body));
    EXPECT_NE(a.body.find("[0xC37F2D8E]\nname = \"BuildDepotDependency\"\nrva = " + PatternRecorderLogic::HexRva(n.f)),
              std::string::npos) << a.body;
    EXPECT_NE(a.body.find("name = \"GetAppByID\"\nrva = " + PatternRecorderLogic::HexRva(n.a)), std::string::npos);
    // The tool's reader sees the same two functions: runtime and tool write one format.
    const auto names = PatternMigrateCliLogic::ParseRequiredFromPatternToml(a.body);
    ASSERT_TRUE(names.has_value());
    EXPECT_EQ(*names, (std::vector<std::string>{ "BuildDepotDependency", "GetAppByID" }));
}

TEST(RuntimeMigrateAttempt, RefusesWhatItCannotTrust) {
    const NewBuild n = MakeNewBuild();
    EXPECT_FALSE(TryOne("not a fingerprint", "steamclient", kOldSha, n.img.View()).accepted);
    EXPECT_FALSE(TryOne(OldFingerprintText(), "steamui", kOldSha, n.img.View()).accepted) << "other component";
    EXPECT_FALSE(TryOne(OldFingerprintText(), "steamclient", kNewSha, n.img.View()).accepted)
        << "a file named after another build than it describes";
    const Attempt none = TryOne(OldFingerprintText(), "steamclient", kOldSha, UnrelatedBuild().View());
    EXPECT_FALSE(none.accepted);
    EXPECT_EQ(none.migrated, 0u);
    EXPECT_TRUE(none.body.empty()) << "a refusal produces nothing that could be written";
    EXPECT_FALSE(TryOne(OldFingerprintText(), "steamclient", kOldSha, PatternMigrateLogic::ImageView{}).accepted);
}

TEST(RuntimeMigrateAttempt, FirstAcceptedSkipsBadCandidatesAndHonoursTheDeadline) {
    const NewBuild n = MakeNewBuild();
    const std::string bad(64, 'c');
    const std::string unreadable(64, 'd');
    std::vector<std::string> refused;
    const auto read = [&](const std::string& stem) -> std::optional<std::string> {
        if (stem == unreadable) return std::nullopt;
        if (stem == bad) return std::string("garbage");
        return OldFingerprintText();
    };
    const auto now = [] { return std::chrono::steady_clock::now(); };
    const auto log = [&](const std::string& stem, const Attempt&) { refused.push_back(stem); };

    const Attempt a = FirstAccepted({ unreadable, bad, kOldSha }, "steamclient", n.img.View(), read, now,
                                    std::chrono::steady_clock::now() + std::chrono::hours(1), log);
    EXPECT_TRUE(a.accepted) << a.why;
    EXPECT_EQ(refused, (std::vector<std::string>{ unreadable, bad }));

    int reads = 0;
    const Attempt late = FirstAccepted({ kOldSha }, "steamclient", n.img.View(),
                                       [&](const std::string&) { ++reads; return std::optional<std::string>(OldFingerprintText()); },
                                       now, std::chrono::steady_clock::now() - std::chrono::seconds(1), log);
    EXPECT_FALSE(late.accepted);
    EXPECT_EQ(reads, 0) << "no candidate is started after the deadline";
    EXPECT_FALSE(FirstAccepted({}, "steamclient", n.img.View(), read, now,
                               std::chrono::steady_clock::now() + std::chrono::hours(1), log).accepted);
}

// ── persisting ───────────────────────────────────────────────────────

TEST(RuntimeMigratePersist, NeverOverAnExistingFile) {
    TempDir dir;
    const fs::path target = dir.Path() / (kNewSha + ".toml");
    WriteAll(target, "[0x1]\nname = \"Published\"\n");
    int writes = 0;
    EXPECT_EQ(Persist(target, "migrated", [&](const fs::path&, const std::string&) { ++writes; return true; }),
              PersistResult::TargetExists);
    EXPECT_EQ(writes, 0);
    EXPECT_EQ(ReadAll(target), "[0x1]\nname = \"Published\"\n");
}

TEST(RuntimeMigratePersist, WritesAFreshTargetAndReportsAFailedWrite) {
    TempDir dir;
    const fs::path target = dir.Path() / (kNewSha + ".toml");
    EXPECT_EQ(Persist(target, "x", [](const fs::path&, const std::string&) { return false; }), PersistResult::WriteFailed);
    EXPECT_FALSE(fs::exists(target));
    EXPECT_EQ(Persist(target, "body", WriteViaTemp), PersistResult::Written);
    EXPECT_EQ(ReadAll(target), "body");
}

// ── stress ───────────────────────────────────────────────────────────

// Thousands of damaged fingerprint files (byte flips, truncations, dropped and duplicated
// lines): nothing throws, and whatever is accepted names only recorded functions at real
// function starts of the new image. A damaged input can lose a migration, never invent one.
TEST(RuntimeMigrateStress, DamagedFingerprintsNeverYieldAWrongAddress) {
    const NewBuild n = MakeNewBuild();
    const PatternMigrateLogic::ImageView view = n.img.View();
    const std::string good = OldFingerprintText();
    std::set<std::uint32_t> starts;
    for (const auto& f : view.pdata) starts.insert(f.begin);

    std::mt19937 rng(20261128);
    std::size_t accepted = 0;
    for (int i = 0; i < 3000; ++i) {
        std::string text = good;
        switch (i % 4) {
            case 0:   // flip a few bytes
                for (int k = 0; k < 1 + static_cast<int>(rng() % 4); ++k)
                    text[rng() % text.size()] = static_cast<char>(rng() & 0xFF);
                break;
            case 1:   // truncate
                text.resize(rng() % text.size());
                break;
            case 2: { // drop a line
                const std::size_t at = text.find('\n', rng() % text.size());
                if (at != std::string::npos) {
                    const std::size_t end = text.find('\n', at + 1);
                    text.erase(at, end == std::string::npos ? std::string::npos : end - at);
                }
                break;
            }
            default: { // duplicate a slice
                const std::size_t from = rng() % text.size();
                text.insert(rng() % text.size(), text.substr(from, rng() % 64));
                break;
            }
        }
        Attempt a;
        ASSERT_NO_THROW(a = TryOne(text, "steamclient", kOldSha, view)) << i;
        if (!a.accepted) {
            EXPECT_TRUE(a.body.empty()) << i;
            continue;
        }
        ++accepted;
        ASSERT_TRUE(IsAutoMigrated(a.body)) << i;
        ASSERT_TRUE(PatternMigrateCliLogic::ParseRequiredFromPatternToml(a.body).has_value()) << i << "\n" << a.body;
        // A recorded name keeps its own function. (A flipped name byte yields a name no hook
        // looks up, which is harmless; the RVA under it is still a real recorded function.)
        const auto rvaOf = [&](const std::string& name) -> std::optional<std::uint32_t> {
            const std::string key = "name = \"" + name + "\"\nrva = \"0x";
            const std::size_t at = a.body.find(key);
            if (at == std::string::npos) return std::nullopt;
            return static_cast<std::uint32_t>(std::stoul(a.body.substr(at + key.size(), 8), nullptr, 16));
        };
        if (const auto r = rvaOf("BuildDepotDependency")) EXPECT_EQ(*r, n.f) << i;
        if (const auto r = rvaOf("GetAppByID")) EXPECT_EQ(*r, n.a) << i;
        for (std::size_t at = a.body.find("rva = \"0x"); at != std::string::npos; at = a.body.find("rva = \"0x", at + 1)) {
            const std::uint32_t rva = static_cast<std::uint32_t>(std::stoul(a.body.substr(at + 9, 8), nullptr, 16));
            EXPECT_TRUE(starts.count(rva)) << i << " rva " << rva;
            EXPECT_TRUE(rva == n.a || rva == n.f) << i << " rva " << rva << " is not one of the recorded functions";
        }
    }
    EXPECT_GT(accepted, 0u) << "the undamaged cases still migrate";
}

// Eight threads migrate and persist to the same cache folder at once, half of them with
// damaged fingerprints and some with a writer that fails. The pattern file of the old build
// and the fingerprint file are byte-identical afterwards; the new build's entry is either
// absent or one complete migrated file; no temp file is left behind.
TEST(RuntimeMigrateStress, ConcurrentFailuresNeverCorruptTheFilesOnDisk) {
    TempDir dir;
    const fs::path patternDir = dir.Path() / "pattern" / "steamclient";
    const fs::path oldToml = patternDir / (kOldSha + ".toml");
    const fs::path fingerprint = dir.Path() / "fingerprint" / "steamclient" / (kOldSha + ".toml");
    const fs::path target = patternDir / (kNewSha + ".toml");
    const std::string oldBody = "[0xC37F2D8E]\nname = \"BuildDepotDependency\"\nrva = \"0x1234\"\n";
    WriteAll(oldToml, oldBody);
    WriteAll(fingerprint, OldFingerprintText());
    const std::string fpBody = ReadAll(fingerprint);

    const NewBuild n = MakeNewBuild();
    std::atomic<int> written{ 0 };
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&, t] {
            std::mt19937 rng(static_cast<std::uint32_t>(t));
            for (int i = 0; i < 100; ++i) {
                std::string text = ReadAll(fingerprint);
                if ((t + i) % 2) text.resize(rng() % (text.size() + 1));   // a damaged read
                const Attempt a = TryOne(text, "steamclient", kOldSha, n.img.View());
                if (!a.accepted) continue;
                const bool failing = (i % 5) == 0;
                const PersistResult r = Persist(target, a.body, [&](const fs::path& p, const std::string& b) {
                    return failing ? false : WriteViaTemp(p, b);
                });
                if (r == PersistResult::Written) ++written;
            }
        });
    }
    for (auto& th : threads) th.join();

    EXPECT_EQ(ReadAll(oldToml), oldBody);
    EXPECT_EQ(ReadAll(fingerprint), fpBody);
    ASSERT_TRUE(fs::exists(target));
    EXPECT_GE(written.load(), 1);
    const std::string got = ReadAll(target);
    EXPECT_TRUE(IsAutoMigrated(got));
    EXPECT_EQ(got, TryOne(fpBody, "steamclient", kOldSha, n.img.View()).body) << "one complete file, not a mix";

    std::size_t files = 0;
    for (const auto& e : fs::recursive_directory_iterator(dir.Path())) {
        if (!e.is_regular_file()) continue;
        ++files;
        EXPECT_EQ(e.path().extension(), ".toml") << e.path();
    }
    EXPECT_EQ(files, 3u) << "old pattern, fingerprint, new pattern: no temp file left";
}
