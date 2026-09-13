// Unit tests for ManifestCacheLogic::EnsureCached -- the pure logic behind
// ManifestCache (see Utils/SteamMetadata/ManifestCache.h/.cpp), extracted
// specifically so it is testable without WinHTTP or a real Steam install.
// The archive fetch is injected (ManifestCacheLogic::Fetcher), so every case
// here runs entirely offline.
//
// The unit touches the real filesystem (a scratch temp directory per test,
// same technique as config_migration_test.cpp), but never the network.
// Depot/gid values are unique per test: the negative-cache and
// in-flight-dedup state inside ManifestCacheLogic is process-global, so
// reusing a pair across tests would leak state between them.

#include "Utils/SteamMetadata/ManifestCacheLogic.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using ManifestCacheLogic::EnsureCached;
using ManifestCacheLogic::FetchResult;
using ManifestCacheLogic::Fetcher;

namespace {

    // A well-formed manifest body: payload magic + padding + EOF magic.
    // EnsureCached only checks the first/last 4 bytes and a 16-byte minimum
    // size, so this minimal fixture is faithful to the real validation.
    std::string ValidManifestBody() {
        std::string body;
        body += '\xD0'; body += '\x17'; body += '\xF6'; body += '\x71'; // payload magic
        body += "........";                                             // padding
        body += '\xAB'; body += '\x15'; body += '\xC4'; body += '\x32'; // EOF magic
        return body; // 16 bytes -- exactly the minimum
    }

    Fetcher CountingFetch(int& calls, FetchResult result) {
        return [&calls, result](const std::string&, uint32_t) {
            ++calls;
            return result;
        };
    }

    class ManifestCacheLogicTest : public ::testing::Test {
    protected:
        void SetUp() override {
            dir_ = fs::temp_directory_path() /
                   ("amethyst_manifestcache_" +
                    std::to_string(reinterpret_cast<uintptr_t>(this)));
            fs::remove_all(dir_);
            fs::create_directories(dir_);
        }
        void TearDown() override {
            std::error_code ec;
            fs::remove_all(dir_, ec);
        }

        fs::path DestFor(uint32_t depot, uint64_t gid) const {
            return dir_ / (std::to_string(depot) + "_" + std::to_string(gid) + ".manifest");
        }

        fs::path dir_;
    };

    // ── Disabled / degenerate inputs -- must never call fetch ───────────────
    TEST_F(ManifestCacheLogicTest, EmptyArchiveUrlNeverFetches) {
        int calls = 0;
        Fetcher fetch = CountingFetch(calls, FetchResult{ValidManifestBody(), 200, true});

        EXPECT_FALSE(EnsureCached(480, 1001, 111, dir_, /*archiveBaseUrl=*/"", fetch,
                                  0, nullptr, false));
        EXPECT_EQ(calls, 0);
        EXPECT_FALSE(fs::exists(DestFor(1001, 111)));
    }

    TEST_F(ManifestCacheLogicTest, EmptyCacheDirNeverFetches) {
        int calls = 0;
        Fetcher fetch = CountingFetch(calls, FetchResult{ValidManifestBody(), 200, true});

        EXPECT_FALSE(EnsureCached(480, 1002, 112, fs::path{}, "https://example.test", fetch,
                                  0, nullptr, false));
        EXPECT_EQ(calls, 0);
    }

    TEST_F(ManifestCacheLogicTest, ZeroDepotOrGidNeverFetches) {
        int calls = 0;
        Fetcher fetch = CountingFetch(calls, FetchResult{ValidManifestBody(), 200, true});

        EXPECT_FALSE(EnsureCached(480, 0, 113, dir_, "https://example.test", fetch, 0, nullptr, false));
        EXPECT_FALSE(EnsureCached(480, 1003, 0, dir_, "https://example.test", fetch, 0, nullptr, false));
        EXPECT_EQ(calls, 0);
    }

    TEST_F(ManifestCacheLogicTest, AlreadyCachedFileSkipsFetchEntirely) {
        const uint32_t depot = 1004; const uint64_t gid = 114;
        { std::ofstream(DestFor(depot, gid), std::ios::binary) << "already here"; }

        int calls = 0;
        Fetcher fetch = CountingFetch(calls, FetchResult{ValidManifestBody(), 200, true});

        EXPECT_TRUE(EnsureCached(480, depot, gid, dir_, "https://example.test", fetch, 0, nullptr, false));
        EXPECT_EQ(calls, 0); // pre-existing file wins; no network needed
    }

    // ── Happy path ───────────────────────────────────────────────────────────
    TEST_F(ManifestCacheLogicTest, SuccessfulFetchWritesFile) {
        const uint32_t depot = 2001; const uint64_t gid = 211;
        int calls = 0;
        Fetcher fetch = CountingFetch(calls, FetchResult{ValidManifestBody(), 200, true});

        bool notArchived = true; // must flip to false on success
        EXPECT_TRUE(EnsureCached(480, depot, gid, dir_, "https://example.test", fetch,
                                 0, &notArchived, false));
        EXPECT_EQ(calls, 1);
        EXPECT_FALSE(notArchived);

        const fs::path dest = DestFor(depot, gid);
        ASSERT_TRUE(fs::exists(dest));
        std::ifstream in(dest, std::ios::binary);
        const std::string written((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        EXPECT_EQ(written, ValidManifestBody());
    }

    TEST_F(ManifestCacheLogicTest, UrlIsBuiltFromArchiveBaseUrlDepotAndGid) {
        const uint32_t depot = 2002; const uint64_t gid = 212;
        std::string capturedUrl;
        Fetcher fetch = [&](const std::string& url, uint32_t) {
            capturedUrl = url;
            return FetchResult{ValidManifestBody(), 200, true};
        };

        EXPECT_TRUE(EnsureCached(480, depot, gid, dir_, "https://example.test", fetch, 0, nullptr, false));
        EXPECT_EQ(capturedUrl, "https://example.test/m/2002/212");
    }

    // ── Malicious / malformed responses ──────────────────────────────────────
    TEST_F(ManifestCacheLogicTest, MalformedBodyRejectedNotWritten) {
        const uint32_t depot = 3001; const uint64_t gid = 311;
        Fetcher fetch = [](const std::string&, uint32_t) {
            return FetchResult{"not a real manifest, no magic bytes here at all", 200, true};
        };

        EXPECT_FALSE(EnsureCached(480, depot, gid, dir_, "https://example.test", fetch, 0, nullptr, false));
        EXPECT_FALSE(fs::exists(DestFor(depot, gid)));
    }

    TEST_F(ManifestCacheLogicTest, TooShortBodyRejected) {
        const uint32_t depot = 3002; const uint64_t gid = 312;
        Fetcher fetch = [](const std::string&, uint32_t) {
            return FetchResult{"short", 200, true}; // below the 16-byte minimum
        };

        EXPECT_FALSE(EnsureCached(480, depot, gid, dir_, "https://example.test", fetch, 0, nullptr, false));
        EXPECT_FALSE(fs::exists(DestFor(depot, gid)));
    }

    // ── 404 vs. transient failure: negative-cache distinction ───────────────
    TEST_F(ManifestCacheLogicTest, NotFoundSetsOutNotArchivedAndPopulatesNegativeCache) {
        const uint32_t depot = 4001; const uint64_t gid = 411;
        int calls = 0;
        Fetcher fetch = CountingFetch(calls, FetchResult{"", 404, true});

        bool notArchived = false;
        EXPECT_FALSE(EnsureCached(480, depot, gid, dir_, "https://example.test", fetch,
                                  0, &notArchived, false));
        EXPECT_TRUE(notArchived);
        EXPECT_EQ(calls, 1);

        // Immediate retry: negative cache must skip the fetch entirely.
        bool notArchived2 = false;
        EXPECT_FALSE(EnsureCached(480, depot, gid, dir_, "https://example.test", fetch,
                                  0, &notArchived2, false));
        EXPECT_EQ(calls, 1);          // unchanged -- no second fetch happened
        EXPECT_FALSE(notArchived2);   // a cached miss is not a fresh 404 signal
    }

    TEST_F(ManifestCacheLogicTest, ServerErrorFailsClosedWithoutNegativeCaching) {
        const uint32_t depot = 4002; const uint64_t gid = 412;
        int calls = 0;
        Fetcher fetch = CountingFetch(calls, FetchResult{"", 500, true});

        bool notArchived = true;
        EXPECT_FALSE(EnsureCached(480, depot, gid, dir_, "https://example.test", fetch,
                                  0, &notArchived, false));
        EXPECT_FALSE(notArchived); // 5xx is transient, not "not archived"
        EXPECT_EQ(calls, 1);

        // NOT negative-cached: an immediate retry must fetch again.
        EXPECT_FALSE(EnsureCached(480, depot, gid, dir_, "https://example.test", fetch,
                                  0, nullptr, false));
        EXPECT_EQ(calls, 2);
    }

    TEST_F(ManifestCacheLogicTest, TransportFailureFailsClosedWithoutNegativeCaching) {
        const uint32_t depot = 4003; const uint64_t gid = 413;
        int calls = 0;
        Fetcher fetch = CountingFetch(calls, FetchResult{"", 0, false}); // ok=false: no response at all

        EXPECT_FALSE(EnsureCached(480, depot, gid, dir_, "https://example.test", fetch, 0, nullptr, false));
        EXPECT_FALSE(EnsureCached(480, depot, gid, dir_, "https://example.test", fetch, 0, nullptr, false));
        EXPECT_EQ(calls, 2); // retried both times -- never negative-cached
    }

    TEST_F(ManifestCacheLogicTest, BypassNegativeCacheForcesRefetchAfter404) {
        const uint32_t depot = 4004; const uint64_t gid = 414;
        int calls = 0;
        Fetcher fetch = CountingFetch(calls, FetchResult{"", 404, true});

        EXPECT_FALSE(EnsureCached(480, depot, gid, dir_, "https://example.test", fetch, 0, nullptr, false));
        EXPECT_EQ(calls, 1);

        // A real download starting: bypass the negative cache even though the
        // TTL has not elapsed.
        EXPECT_FALSE(EnsureCached(480, depot, gid, dir_, "https://example.test", fetch,
                                  0, nullptr, /*bypassNegativeCache=*/true));
        EXPECT_EQ(calls, 2);
    }

    // ── Non-ASCII path handling ──────────────────────────────────────────────
    TEST_F(ManifestCacheLogicTest, NonAsciiCacheDirWritesCorrectly) {
        // Stresses the MoveFileExW fix: a cache directory containing
        // accented characters ("depósito_não_ascii") must round-trip
        // correctly, unlike upstream's MoveFileExA (ANSI-codepage
        // narrowing of the native wide path). \uXXXX escapes are used
        // instead of literal accented characters so the test does not
        // depend on this file being compiled with /utf-8.
        const fs::path accentedDir = dir_ / fs::path(L"depósito_não_ascii");
        std::error_code ec;
        fs::create_directories(accentedDir, ec);
        ASSERT_FALSE(ec);

        const uint32_t depot = 5001; const uint64_t gid = 511;
        Fetcher fetch = [](const std::string&, uint32_t) {
            return FetchResult{ValidManifestBody(), 200, true};
        };

        EXPECT_TRUE(EnsureCached(480, depot, gid, accentedDir, "https://example.test", fetch,
                                 0, nullptr, false));

        const fs::path dest = accentedDir / (std::to_string(depot) + "_" + std::to_string(gid) + ".manifest");
        ASSERT_TRUE(fs::exists(dest));
        std::ifstream in(dest, std::ios::binary);
        const std::string written((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        EXPECT_EQ(written, ValidManifestBody());
    }

} // namespace
