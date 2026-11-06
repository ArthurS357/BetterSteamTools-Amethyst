#include "Utils/SteamMetadata/UgcLogic.h"

#include <gtest/gtest.h>

#include <array>
#include <climits>
#include <cstdint>
#include <cstring>
#include <vector>

// UgcLogic: when a Workshop API-call result may be reported as k_EResultOK, and
// the in-place patch of the callback bytes. Pure logic, no Steam/IPC headers.

namespace {

    using namespace UgcLogic;

    constexpr std::uint32_t kLuaApp = 255710;   // any non-zero AppID
    constexpr std::uint32_t kSubscribeResult = 1313;   // RemoteStorageSubscribePublishedFileResult_t
    constexpr std::uint32_t kUnsubscribeResult = 1315; // RemoteStorageUnsubscribePublishedFileResult_t
    constexpr std::uint32_t kQueryCompleted = 3401;    // SteamUGCQueryCompleted_t: uint64 handle FIRST

    std::int32_t FirstInt32(const std::vector<std::uint8_t>& bytes) {
        std::int32_t v = 0;
        std::memcpy(&v, bytes.data(), sizeof(v));
        return v;
    }

    // Callback buffer: first int32 = `eresult`, then a recognisable tail.
    std::vector<std::uint8_t> MakeCallback(std::int32_t eresult, std::size_t size = 12) {
        std::vector<std::uint8_t> bytes(size);
        for (std::size_t i = 0; i < size; ++i) bytes[i] = static_cast<std::uint8_t>(0xA0 + i);
        if (size >= sizeof(eresult)) std::memcpy(bytes.data(), &eresult, sizeof(eresult));
        return bytes;
    }

} // namespace

// --- ShouldOverrideUgcResult -----------------------------------------------

TEST(UgcLogicDecision, UnmanagedAppIsNeverOverridden) {
    for (std::int32_t r : {kEResultFail, kEResultInvalidParam, kEResultInvalidState,
                           kEResultAccessDenied, kEResultInsufficientPrivilege, kEResultOK}) {
        EXPECT_FALSE(ShouldOverrideUgcResult(kLuaApp, r, /*isLuaManaged=*/false)) << "result " << r;
    }
}

TEST(UgcLogicDecision, ManagedAppAccessAndLicenseErrorsAreOverridden) {
    EXPECT_TRUE(ShouldOverrideUgcResult(kLuaApp, kEResultAccessDenied, true));
    EXPECT_TRUE(ShouldOverrideUgcResult(kLuaApp, kEResultInsufficientPrivilege, true));
    EXPECT_TRUE(ShouldOverrideUgcResult(kLuaApp, kEResultFail, true));
    EXPECT_TRUE(ShouldOverrideUgcResult(kLuaApp, kEResultInvalidParam, true));
    EXPECT_TRUE(ShouldOverrideUgcResult(kLuaApp, kEResultInvalidState, true));
}

TEST(UgcLogicDecision, ManagedAppOkIsLeftAlone) {
    EXPECT_FALSE(ShouldOverrideUgcResult(kLuaApp, kEResultOK, true));
}

TEST(UgcLogicDecision, ManagedAppNetworkAndTimeoutErrorsAreLeftAlone) {
    EXPECT_FALSE(ShouldOverrideUgcResult(kLuaApp, 3, true));    // k_EResultNoConnection
    EXPECT_FALSE(ShouldOverrideUgcResult(kLuaApp, 16, true));   // k_EResultTimeout
    EXPECT_FALSE(ShouldOverrideUgcResult(kLuaApp, 20, true));   // k_EResultServiceUnavailable
    EXPECT_FALSE(ShouldOverrideUgcResult(kLuaApp, 21, true));   // k_EResultNotLoggedOn
}

TEST(UgcLogicDecision, ManagedAppNotFoundAndBusyAreLeftAlone) {
    EXPECT_FALSE(ShouldOverrideUgcResult(kLuaApp, 9, true));    // k_EResultFileNotFound
    EXPECT_FALSE(ShouldOverrideUgcResult(kLuaApp, 10, true));   // k_EResultBusy
}

TEST(UgcLogicDecision, ManagedAppGenericZeroResultIsLeftAlone) {
    EXPECT_FALSE(ShouldOverrideUgcResult(kLuaApp, 0, true));    // k_EResultNone
}

TEST(UgcLogicDecision, UnknownOrOutOfRangeResultsAreLeftAlone) {
    for (std::int32_t r : {-1, 4, 9999, INT32_MAX, INT32_MIN}) {
        EXPECT_FALSE(ShouldOverrideUgcResult(kLuaApp, r, true)) << "result " << r;
    }
}

TEST(UgcLogicDecision, UnresolvedAppIdIsLeftAlone) {
    EXPECT_FALSE(ShouldOverrideUgcResult(0, kEResultAccessDenied, true));
}

// --- IsEResultLeadingCallback ----------------------------------------------

TEST(UgcLogicCallbacks, SubscribeAndUnsubscribeResultsAreEligible) {
    EXPECT_TRUE(IsEResultLeadingCallback(kSubscribeResult));
    EXPECT_TRUE(IsEResultLeadingCallback(kUnsubscribeResult));
    EXPECT_TRUE(IsEResultLeadingCallback(3403));   // CreateItemResult_t
    EXPECT_TRUE(IsEResultLeadingCallback(3417));   // DeleteItemResult_t
}

TEST(UgcLogicCallbacks, CallbacksWhoseFirstFieldIsNotEResultAreNotEligible) {
    EXPECT_FALSE(IsEResultLeadingCallback(kQueryCompleted));   // SteamUGCQueryCompleted_t
    EXPECT_FALSE(IsEResultLeadingCallback(3406));              // DownloadItemResult_t
    EXPECT_FALSE(IsEResultLeadingCallback(3407));              // UserFavoriteItemsListChanged_t
    EXPECT_FALSE(IsEResultLeadingCallback(3408));              // SetUserItemVoteResult_t
    EXPECT_FALSE(IsEResultLeadingCallback(3409));              // GetUserItemVoteResult_t
    EXPECT_FALSE(IsEResultLeadingCallback(1332));              // RemoteStorageFileReadAsyncComplete_t
}

TEST(UgcLogicCallbacks, UnrelatedCallbackIdsAreNotEligible) {
    EXPECT_FALSE(IsEResultLeadingCallback(0));
    EXPECT_FALSE(IsEResultLeadingCallback(154));               // EncryptedAppTicketResponse_t
    EXPECT_FALSE(IsEResultLeadingCallback(1020094));           // AppLicensesChanged_t
    EXPECT_FALSE(IsEResultLeadingCallback(UINT32_MAX));
}

// --- TryOverrideCallback ---------------------------------------------------

TEST(UgcLogicOverride, RewritesFirstInt32ToOkAndReportsOriginal) {
    auto cb = MakeCallback(kEResultAccessDenied);
    const auto tailBefore = std::vector<std::uint8_t>(cb.begin() + 4, cb.end());

    const auto original = TryOverrideCallback(kLuaApp, true, kSubscribeResult, cb);

    ASSERT_TRUE(original.has_value());
    EXPECT_EQ(*original, kEResultAccessDenied);
    EXPECT_EQ(FirstInt32(cb), kEResultOK);
    EXPECT_EQ(std::vector<std::uint8_t>(cb.begin() + 4, cb.end()), tailBefore);   // nothing past byte 3 touched
}

TEST(UgcLogicOverride, ExactlyFourBytesIsEnough) {
    auto cb = MakeCallback(kEResultFail, 4);
    const auto original = TryOverrideCallback(kLuaApp, true, kUnsubscribeResult, cb);
    ASSERT_TRUE(original.has_value());
    EXPECT_EQ(*original, kEResultFail);
    EXPECT_EQ(FirstInt32(cb), kEResultOK);
}

TEST(UgcLogicOverride, ShortBufferIsNeverModified) {
    for (std::size_t size = 0; size < sizeof(std::int32_t); ++size) {
        auto cb = MakeCallback(kEResultAccessDenied, size);
        const auto before = cb;
        EXPECT_FALSE(TryOverrideCallback(kLuaApp, true, kSubscribeResult, cb).has_value()) << "size " << size;
        EXPECT_EQ(cb, before) << "size " << size;
    }
}

TEST(UgcLogicOverride, EmptySpanIsSafe) {
    EXPECT_FALSE(TryOverrideCallback(kLuaApp, true, kSubscribeResult, {}).has_value());
}

TEST(UgcLogicOverride, NonLeadingCallbackIsNeverModified) {
    // First int32 happens to be a "failure" value, but in SteamUGCQueryCompleted_t it is
    // the low half of the query handle -- overwriting it would corrupt the handle.
    auto cb = MakeCallback(kEResultFail);
    const auto before = cb;
    EXPECT_FALSE(TryOverrideCallback(kLuaApp, true, kQueryCompleted, cb).has_value());
    EXPECT_EQ(cb, before);
}

TEST(UgcLogicOverride, UnmanagedAppIsNeverModified) {
    auto cb = MakeCallback(kEResultAccessDenied);
    const auto before = cb;
    EXPECT_FALSE(TryOverrideCallback(kLuaApp, false, kSubscribeResult, cb).has_value());
    EXPECT_EQ(cb, before);
}

TEST(UgcLogicOverride, OkResultIsNotModified) {
    auto cb = MakeCallback(kEResultOK);
    const auto before = cb;
    EXPECT_FALSE(TryOverrideCallback(kLuaApp, true, kSubscribeResult, cb).has_value());
    EXPECT_EQ(cb, before);
}

TEST(UgcLogicOverride, TimeoutResultIsNotModified) {
    auto cb = MakeCallback(16);   // k_EResultTimeout
    const auto before = cb;
    EXPECT_FALSE(TryOverrideCallback(kLuaApp, true, kSubscribeResult, cb).has_value());
    EXPECT_EQ(cb, before);
}

TEST(UgcLogicOverride, UnresolvedAppIdIsNotModified) {
    auto cb = MakeCallback(kEResultAccessDenied);
    const auto before = cb;
    EXPECT_FALSE(TryOverrideCallback(0, true, kSubscribeResult, cb).has_value());
    EXPECT_EQ(cb, before);
}
