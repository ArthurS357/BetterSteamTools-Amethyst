// Unit tests for ForgeBaseCache::AccountKeyedCache -- the account-keyed slot
// behind AppTicket's forge base (v1.3.4). Switching Steam accounts (or Family
// Sharing) without restarting Steam must not reuse a base ticket resolved for
// the previous account (it would carry the old SteamID -> Denuvo error 54).
// GetActiveUser is modelled by the accountId argument the production code reads
// before every cache access.

#include "Utils/Tickets/ForgeBaseCache.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace {
    using Cache = ForgeBaseCache::AccountKeyedCache<std::vector<uint8_t>>;
    constexpr uint32_t kAcctA = 1111;
    constexpr uint32_t kAcctB = 2222;
    const std::vector<uint8_t> kTicketA{1, 2, 3};
    const std::vector<uint8_t> kTicketB{9, 8, 7, 6};

    // Mirrors PickForgeSourceTicket's cached branch for a given active account.
    // `resolves` counts how many times the (expensive) localconfig read ran.
    std::vector<uint8_t> Pick(Cache& c, uint32_t active, const std::vector<uint8_t>& resolved, int& resolves) {
        c.InvalidateIfAccountChanged(active);
        std::vector<uint8_t> out;
        if (c.Lookup(active, out)) return out;
        if (active == 0) return {};
        ++resolves;
        c.Store(active, resolved);
        return resolved;
    }
}

TEST(ForgeBaseCache, EmptyCacheMisses) {
    Cache c;
    std::vector<uint8_t> out;
    EXPECT_FALSE(c.Lookup(kAcctA, out));
    EXPECT_FALSE(c.HasValue());
}

TEST(ForgeBaseCache, HitsForTheSameAccount) {
    Cache c;
    ASSERT_TRUE(c.Store(kAcctA, kTicketA));
    std::vector<uint8_t> out;
    ASSERT_TRUE(c.Lookup(kAcctA, out));
    EXPECT_EQ(out, kTicketA);
}

TEST(ForgeBaseCache, MissesForADifferentAccountWithoutExplicitInvalidate) {
    Cache c;
    c.Store(kAcctA, kTicketA);
    std::vector<uint8_t> out;
    EXPECT_FALSE(c.Lookup(kAcctB, out));
}

TEST(ForgeBaseCache, ZeroAccountNeverStoresOrHits) {
    Cache c;
    EXPECT_FALSE(c.Store(0, kTicketA));
    EXPECT_FALSE(c.HasValue());
    c.Store(kAcctA, kTicketA);
    std::vector<uint8_t> out;
    EXPECT_FALSE(c.Lookup(0, out));
}

TEST(ForgeBaseCache, InvalidateIfAccountChangedDropsOnlyOnChange) {
    Cache c;
    c.Store(kAcctA, kTicketA);
    EXPECT_FALSE(c.InvalidateIfAccountChanged(kAcctA));
    EXPECT_TRUE(c.HasValue());
    EXPECT_TRUE(c.InvalidateIfAccountChanged(kAcctB));
    EXPECT_FALSE(c.HasValue());
    EXPECT_EQ(c.AccountId(), 0u);
}

TEST(ForgeBaseCache, LoggingOutInvalidates) {
    Cache c;
    c.Store(kAcctA, kTicketA);
    EXPECT_TRUE(c.InvalidateIfAccountChanged(0));
    EXPECT_FALSE(c.HasValue());
}

TEST(ForgeBaseCache, InvalidateOnEmptyReportsNothingDropped) {
    Cache c;
    EXPECT_FALSE(c.Invalidate());
    EXPECT_FALSE(c.InvalidateIfAccountChanged(kAcctA));
}

TEST(ForgeBaseCache, StoreReplacesOtherAccountsEntry) {
    Cache c;
    c.Store(kAcctA, kTicketA);
    c.Store(kAcctB, kTicketB);
    std::vector<uint8_t> out;
    EXPECT_FALSE(c.Lookup(kAcctA, out));
    ASSERT_TRUE(c.Lookup(kAcctB, out));
    EXPECT_EQ(out, kTicketB);
}

TEST(ForgeBaseCache, AccountSwitchReResolvesBaseTicket) {
    Cache c;
    int resolves = 0;
    EXPECT_EQ(Pick(c, kAcctA, kTicketA, resolves), kTicketA);
    EXPECT_EQ(Pick(c, kAcctA, kTicketA, resolves), kTicketA);   // cached
    EXPECT_EQ(resolves, 1);

    // Switch to B without a Steam restart: must resolve B's base, not reuse A's.
    EXPECT_EQ(Pick(c, kAcctB, kTicketB, resolves), kTicketB);
    EXPECT_EQ(resolves, 2);
    EXPECT_EQ(Pick(c, kAcctB, kTicketB, resolves), kTicketB);
    EXPECT_EQ(resolves, 2);

    // And back to A: B's entry is gone, A is resolved again.
    EXPECT_EQ(Pick(c, kAcctA, kTicketA, resolves), kTicketA);
    EXPECT_EQ(resolves, 3);
}

TEST(ForgeBaseCache, NoActiveUserYieldsNothingAndClearsStaleEntry) {
    Cache c;
    int resolves = 0;
    Pick(c, kAcctA, kTicketA, resolves);
    EXPECT_TRUE(Pick(c, 0, kTicketA, resolves).empty());
    EXPECT_FALSE(c.HasValue());
}
