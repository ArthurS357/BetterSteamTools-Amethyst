// Unit tests for RemoteTomlLogic::DescribeLoadSource -- the log label derived
// from RemoteToml::Result::{fromCache, cacheFallback} (v1.3.4). Cache-first is
// the normal path and must read "cache hit"; "cache fallback" is reserved for
// the case where the network failed and the cache was plan B.

#include "Utils/SteamMetadata/RemoteTomlLogic.h"

#include <gtest/gtest.h>

#include <string>

using RemoteTomlLogic::DescribeLoadSource;

TEST(DescribeLoadSource, RemoteWhenNotFromCache) {
    EXPECT_STREQ(DescribeLoadSource(false, false), "remote");
}

TEST(DescribeLoadSource, CacheHitIsTheCacheFirstPath) {
    EXPECT_STREQ(DescribeLoadSource(true, false), "cache hit");
}

TEST(DescribeLoadSource, CacheFallbackOnlyAfterNetworkFailure) {
    EXPECT_STREQ(DescribeLoadSource(true, true), "cache fallback");
}

TEST(DescribeLoadSource, InconsistentFlagsStayRemote) {
    // cacheFallback without fromCache can't happen; never claim a cache source.
    EXPECT_STREQ(DescribeLoadSource(false, true), "remote");
}

TEST(DescribeLoadSource, IsUsableInConstantExpressions) {
    constexpr const char* s = DescribeLoadSource(true, false);
    EXPECT_EQ(std::string(s), "cache hit");
}

// ── IsHttpsUrl (M-4: background revalidation is https-only) ──────

TEST(RemoteTomlIsHttpsUrl, AcceptsHttpsCaseInsensitiveRejectsEverythingElse) {
    using RemoteTomlLogic::IsHttpsUrl;
    static_assert(IsHttpsUrl("https://raw.githubusercontent.com/x"));
    EXPECT_TRUE(IsHttpsUrl("HTTPS://cdn.jsdelivr.net/gh/x"));
    EXPECT_FALSE(IsHttpsUrl("http://intranet.local/pattern/steamclient/abc.toml"));
    EXPECT_FALSE(IsHttpsUrl("https:/missing-slash"));
    EXPECT_FALSE(IsHttpsUrl("ftp://x"));
    EXPECT_FALSE(IsHttpsUrl(""));
    EXPECT_FALSE(IsHttpsUrl(" https://leading-space"));
}
