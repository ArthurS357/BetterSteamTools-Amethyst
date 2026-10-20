// Unit tests for PatternLoaderLogic -- the required/optional "missing
// signature" policy behind PatternLoader::FindPattern / FindPatternOptional
// (see Utils/SteamMetadata/PatternLoaderLogic.h), extracted so it is testable
// without the TOML parser, the module scanner or the popup.

#include "Utils/SteamMetadata/PatternLoaderLogic.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using PatternLoaderLogic::MissingFunctionLog;
using PatternLoaderLogic::Requirement;
using PatternLoaderLogic::ResolveOrRecord;

namespace {

    int g_target = 0;
    void* const kFound = &g_target;

    void* Present() { return kFound; }
    void* Absent()  { return nullptr; }

    // ── optional ─────────────────────────────────────────────────────

    TEST(PatternLoaderLogicTest, OptionalAbsentReturnsNullAndIsNotReported) {
        MissingFunctionLog log;
        EXPECT_EQ(ResolveOrRecord("YldLoadDepotManifest", Requirement::Optional, log, Absent), nullptr);
        EXPECT_TRUE(log.Empty());
        EXPECT_TRUE(log.TakeAll().empty());   // nothing for the popup
    }

    TEST(PatternLoaderLogicTest, OptionalPresentResolvesNormally) {
        MissingFunctionLog log;
        EXPECT_EQ(ResolveOrRecord("YldLoadDepotManifest", Requirement::Optional, log, Present), kFound);
        EXPECT_TRUE(log.Empty());
    }

    // ── required (pre-v1.3.2 behavior, unchanged) ────────────────────

    TEST(PatternLoaderLogicTest, RequiredAbsentIsReported) {
        MissingFunctionLog log;
        EXPECT_EQ(ResolveOrRecord("BuildDepotDependency", Requirement::Required, log, Absent), nullptr);
        EXPECT_EQ(log.TakeAll(), (std::vector<std::string>{ "BuildDepotDependency" }));
    }

    TEST(PatternLoaderLogicTest, RequiredPresentIsNotReported) {
        MissingFunctionLog log;
        EXPECT_EQ(ResolveOrRecord("BuildDepotDependency", Requirement::Required, log, Present), kFound);
        EXPECT_TRUE(log.Empty());
    }

    // A mixed install pass reports only the required misses, in order.
    TEST(PatternLoaderLogicTest, MixedPassReportsOnlyRequiredMisses) {
        MissingFunctionLog log;
        (void)ResolveOrRecord("A", Requirement::Required, log, Absent);
        (void)ResolveOrRecord("Opt", Requirement::Optional, log, Absent);
        (void)ResolveOrRecord("B", Requirement::Required, log, Present);
        (void)ResolveOrRecord("C", Requirement::Required, log, Absent);
        EXPECT_EQ(log.TakeAll(), (std::vector<std::string>{ "A", "C" }));
    }

    // ── reset invariant ──────────────────────────────────────────────

    TEST(PatternLoaderLogicTest, TakeAllDrainsSoASecondReportIsEmpty) {
        MissingFunctionLog log;
        log.Record("A", Requirement::Required);
        EXPECT_EQ(log.TakeAll().size(), 1u);
        EXPECT_TRUE(log.Empty());
        EXPECT_TRUE(log.TakeAll().empty());

        log.Record("B", Requirement::Required);   // a later pass starts clean
        EXPECT_EQ(log.TakeAll(), (std::vector<std::string>{ "B" }));
    }

    TEST(PatternLoaderLogicTest, LookupRunsExactlyOnce) {
        MissingFunctionLog log;
        int calls = 0;
        (void)ResolveOrRecord("A", Requirement::Required, log, [&] { ++calls; return Absent(); });
        EXPECT_EQ(calls, 1);
    }

} // namespace
