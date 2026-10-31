// Unit tests for LocalAchievements::Join -- the pure join of Steam's local
// UserGameStatsSchema_<app>.bin and UserGameStats_<acct>_<app>.bin (both binary
// KeyValues) into api_name -> unlock state.
//
// Contract per LocalAchievementsLogic.h: fails closed (empty map) when the
// schema is missing/malformed/empty; a missing or malformed stats blob yields
// every schema achievement locked.
//
// Three categories: happy path, malicious/malformed input, and false-positive
// guards (legitimate-but-unusual input that must NOT be rejected). The blobs
// are built byte-by-byte here, not read from disk: the real files are
// attacker-controllable input to the parser, so the tests feed it hostile shapes.

#include "Utils/Achievements/LocalAchievementsLogic.h"

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

using namespace LocalAchievements;

namespace {

    using Bytes = std::vector<uint8_t>;

    // Binary KeyValues tags (mirrors the parser's BkvType).
    constexpr uint8_t kSection = 0x00, kString = 0x01, kInt = 0x02, kFloat = 0x03, kEnd = 0x08;

    void Name(Bytes& b, const std::string& n) { b.insert(b.end(), n.begin(), n.end()); b.push_back(0); }
    void Section(Bytes& b, const std::string& n) { b.push_back(kSection); Name(b, n); }
    void End(Bytes& b) { b.push_back(kEnd); }
    void Str(Bytes& b, const std::string& n, const std::string& v) {
        b.push_back(kString); Name(b, n); b.insert(b.end(), v.begin(), v.end()); b.push_back(0);
    }
    void Int(Bytes& b, const std::string& n, uint32_t v) {
        b.push_back(kInt); Name(b, n);
        const auto* p = reinterpret_cast<const uint8_t*>(&v);
        b.insert(b.end(), p, p + sizeof(v));
    }
    void Float(Bytes& b, const std::string& n, float v) {
        b.push_back(kFloat); Name(b, n);
        const auto* p = reinterpret_cast<const uint8_t*>(&v);
        b.insert(b.end(), p, p + sizeof(v));
    }

    // "730" { "stats" {
    //     "1" { name=stat_1  bits { "0" { name=ACH_A }
    //                               "1" { name=ACH_B  progress { value { operation=statvalue operand1=kills } } } } }
    //     "2" { name=kills } } }
    Bytes MakeSchema() {
        Bytes b;
        Section(b, "730");
          Section(b, "stats");
            Section(b, "1");
              Str(b, "name", "stat_1");
              Section(b, "bits");
                Section(b, "0"); Str(b, "name", "ACH_A"); End(b);
                Section(b, "1"); Str(b, "name", "ACH_B");
                  Section(b, "progress"); Section(b, "value");
                    Str(b, "operation", "statvalue"); Str(b, "operand1", "kills");
                  End(b); End(b);
                End(b);
              End(b);   // bits
            End(b);     // stat 1
            Section(b, "2"); Str(b, "name", "kills"); End(b);
          End(b);       // stats
        End(b);         // 730
        End(b);         // root
        return b;
    }

    // "cache" { "1" { data=<bitfield> [AchievementTimes { "<bit>"=<t> }] }  "2" { data=<kills> }  crc  PendingChanges }
    Bytes MakeStats(uint32_t bitfield, uint32_t kills,
                    const std::vector<std::pair<uint32_t, uint32_t>>& times = {}) {
        Bytes b;
        Section(b, "cache");
          Section(b, "1");
            Int(b, "data", bitfield);
            if (!times.empty()) {
                Section(b, "AchievementTimes");
                for (const auto& [bit, t] : times) Int(b, std::to_string(bit), t);
                End(b);
            }
          End(b);
          Section(b, "2"); Int(b, "data", kills); End(b);
          Int(b, "crc", 0xDEADBEEF);
          Section(b, "PendingChanges"); Int(b, "data", 0xFFFFFFFF); End(b);
        End(b);         // cache
        End(b);         // root
        return b;
    }

} // namespace

// ── happy path ──────────────────────────────────────────────────────────────

TEST(LocalAchievementsJoin, UnlockedBitFlipsOnlyThatAchievement) {
    const auto r = Join(MakeSchema(), MakeStats(0b01, 0));
    ASSERT_EQ(r.size(), 2u);
    EXPECT_TRUE(r.at("ACH_A").unlocked);
    EXPECT_FALSE(r.at("ACH_B").unlocked);
}

TEST(LocalAchievementsJoin, RecordedTimeImpliesUnlockedAndCarriesTime) {
    // Bit 1 is clear in the bitfield but has a recorded unlock time.
    const auto r = Join(MakeSchema(), MakeStats(0b00, 0, {{1, 1700000000u}}));
    ASSERT_EQ(r.size(), 2u);
    EXPECT_TRUE(r.at("ACH_B").unlocked);
    EXPECT_EQ(r.at("ACH_B").unlockTime, 1700000000u);
    EXPECT_FALSE(r.at("ACH_A").unlocked);
}

TEST(LocalAchievementsJoin, ProgressComesFromTheLinkedStat) {
    const auto r = Join(MakeSchema(), MakeStats(0, 42));
    EXPECT_FLOAT_EQ(r.at("ACH_B").progress, 42.0f);
    EXPECT_FLOAT_EQ(r.at("ACH_A").progress, 0.0f);   // no progress link
}

TEST(LocalAchievementsJoin, FloatStatValueIsReadAsFloat) {
    Bytes stats;
    Section(stats, "cache");
      Section(stats, "1"); Int(stats, "data", 0); End(stats);
      Section(stats, "2"); Float(stats, "data", 3.5f); End(stats);
    End(stats); End(stats);
    EXPECT_FLOAT_EQ(Join(MakeSchema(), stats).at("ACH_B").progress, 3.5f);
}

// ── fail closed / malicious ─────────────────────────────────────────────────

TEST(LocalAchievementsJoin, MissingOrGarbageStatsMeansEveryAchievementLocked) {
    for (const Bytes& stats : {Bytes{}, Bytes{0x01, 0x02, 0x03}, Bytes(64, 0xFF)}) {
        const auto r = Join(MakeSchema(), stats);
        ASSERT_EQ(r.size(), 2u);
        EXPECT_FALSE(r.at("ACH_A").unlocked);
        EXPECT_FALSE(r.at("ACH_B").unlocked);
    }
}

TEST(LocalAchievementsJoin, MissingOrGarbageSchemaYieldsNothing) {
    const Bytes stats = MakeStats(0xFFFFFFFF, 1);
    EXPECT_TRUE(Join({}, stats).empty());
    EXPECT_TRUE(Join(Bytes{0x01}, stats).empty());
    EXPECT_TRUE(Join(Bytes(64, 0xFF), stats).empty());
}

TEST(LocalAchievementsJoin, EveryTruncationOfTheSchemaIsRejectedWithoutCrashing) {
    const Bytes full = MakeSchema();
    const Bytes stats = MakeStats(0b11, 7);
    // The last byte is the root terminator, optional at EOF; every shorter prefix
    // cuts inside the "730" section and must fail closed.
    for (size_t n = 0; n + 1 < full.size(); ++n) {
        const Bytes cut(full.begin(), full.begin() + static_cast<std::ptrdiff_t>(n));
        EXPECT_TRUE(Join(cut, stats).empty()) << "prefix length " << n;
    }
    EXPECT_EQ(Join(full, stats).size(), 2u);
}

TEST(LocalAchievementsJoin, EveryTruncationOfTheStatsIsSafe) {
    const Bytes schema = MakeSchema();
    const Bytes full = MakeStats(0b11, 7);
    for (size_t n = 0; n < full.size(); ++n) {
        const Bytes cut(full.begin(), full.begin() + static_cast<std::ptrdiff_t>(n));
        EXPECT_EQ(Join(schema, cut).size(), 2u) << "prefix length " << n;   // schema list stays complete
    }
}

TEST(LocalAchievementsJoin, NameWithoutTerminatorIsRejected) {
    EXPECT_TRUE(Join(Bytes{kSection, 'a', 'b'}, MakeStats(1, 0)).empty());
}

TEST(LocalAchievementsJoin, NestingBeyondTheDepthLimitIsRejected) {
    Bytes bomb;
    for (int i = 0; i < 300; ++i) Section(bomb, "x");
    for (int i = 0; i < 300; ++i) End(bomb);
    EXPECT_TRUE(Join(bomb, MakeStats(1, 0)).empty());
    EXPECT_EQ(Join(MakeSchema(), bomb).size(), 2u);   // hostile stats: schema still complete, all locked
}

TEST(LocalAchievementsJoin, UnknownKeyValuesTagIsRejected) {
    EXPECT_TRUE(Join(Bytes{0x05, 'a', 0, 0, 0, 0, 0}, MakeStats(1, 0)).empty());   // 0x05 = wide string, unsupported
}

// ── false-positive guards ───────────────────────────────────────────────────

TEST(LocalAchievementsJoin, SchemaBitsOutsideTheBitfieldOrWithBadNamesAreIgnored) {
    Bytes schema;
    Section(schema, "730"); Section(schema, "stats");
      Section(schema, "1x"); Str(schema, "name", "s"); Section(schema, "bits");
        Section(schema, "0"); Str(schema, "name", "BAD_STAT_ID"); End(schema);
      End(schema); End(schema);
      Section(schema, "-1"); Section(schema, "bits");
        Section(schema, "0"); Str(schema, "name", "BAD_NEGATIVE"); End(schema);
      End(schema); End(schema);
      Section(schema, "99999999999"); Section(schema, "bits");
        Section(schema, "0"); Str(schema, "name", "BAD_OVERFLOW"); End(schema);
      End(schema); End(schema);
      Section(schema, "3"); Section(schema, "bits");
        Section(schema, "32"); Str(schema, "name", "BAD_BIT_32"); End(schema);
      End(schema); End(schema);
    End(schema); End(schema); End(schema);
    EXPECT_TRUE(Join(schema, MakeStats(0xFFFFFFFF, 0)).empty());
}

TEST(LocalAchievementsJoin, UnlockedBitsThatTheSchemaDoesNotListAreIgnored) {
    const auto r = Join(MakeSchema(), MakeStats(0xFFFFFFFC, 0));   // only bits 2..31 set; schema has 0 and 1
    ASSERT_EQ(r.size(), 2u);
    EXPECT_FALSE(r.at("ACH_A").unlocked);
    EXPECT_FALSE(r.at("ACH_B").unlocked);
}

TEST(LocalAchievementsJoin, NonNumericCacheSiblingsAreNotTreatedAsStats) {
    // MakeStats puts crc / PendingChanges (data=0xFFFFFFFF) beside the real stats.
    const auto r = Join(MakeSchema(), MakeStats(0b01, 0));
    EXPECT_TRUE(r.at("ACH_A").unlocked);
    EXPECT_FALSE(r.at("ACH_B").unlocked);
}

TEST(LocalAchievementsJoin, SchemaWithStatsDirectlyAtRootIsAccepted) {
    // Some dumps have "stats" at the top level instead of under the <appid> section.
    Bytes schema;
    Section(schema, "stats");
      Section(schema, "1"); Str(schema, "name", "s");
        Section(schema, "bits"); Section(schema, "5"); Str(schema, "name", "ACH_ROOT"); End(schema); End(schema);
      End(schema);
    End(schema); End(schema);
    // Top-level section "stats" has no "stats" child, so the parser's second branch must match it.
    const auto r = Join(schema, MakeStats(0, 0));
    ASSERT_EQ(r.size(), 1u);
    EXPECT_FALSE(r.at("ACH_ROOT").unlocked);
}
