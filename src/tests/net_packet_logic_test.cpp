// Unit tests for NetPacketLogic -- the pure wire-layout logic behind
// Hooks_NetPacket.cpp (see Hook/NetPacketLogic.h), extracted so it is
// testable without the hook engine or a live Steam connection.

#include "Hook/NetPacketLogic.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstring>
#include <vector>

using NetPacketLogic::UnpackRaw;

namespace {

    constexpr uint32 kProtoLicenseList = kMsgHdrProtoFlag | 780u;

    // A frame whose MsgHdr claims `claimedHdrLen`, backed by `realHdr` header
    // bytes and `body` body bytes. Offsets come from MsgHdr itself, never
    // literals, so the fixture tracks the real wire struct.
    std::vector<uint8> Frame(uint32 rawEMsg, uint32 claimedHdrLen,
                             size_t realHdr, size_t body) {
        std::vector<uint8> f(sizeof(MsgHdr) + realHdr + body, 0xAB);
        std::memcpy(f.data() + offsetof(MsgHdr, eMsg), &rawEMsg, sizeof(rawEMsg));
        std::memcpy(f.data() + offsetof(MsgHdr, headerLength), &claimedHdrLen, sizeof(claimedHdrLen));
        return f;
    }

    struct Unpacked {
        bool         ok     = false;
        EMsg         eMsg   = static_cast<EMsg>(-1);
        const uint8* pHdr   = reinterpret_cast<const uint8*>(1);
        uint32       cbHdr  = 0xDEAD;
        const uint8* pBody  = reinterpret_cast<const uint8*>(1);
        uint32       cbBody = 0xDEAD;
    };

    Unpacked Unpack(const std::vector<uint8>& f) {
        Unpacked u;
        u.ok = UnpackRaw(f.data(), static_cast<uint32>(f.size()),
                         u.eMsg, u.pHdr, u.cbHdr, u.pBody, u.cbBody);
        return u;
    }

    void ExpectZeroed(const Unpacked& u) {
        EXPECT_FALSE(u.ok);
        EXPECT_EQ(u.pHdr, nullptr);
        EXPECT_EQ(u.cbHdr, 0u);
        EXPECT_EQ(u.pBody, nullptr);
        EXPECT_EQ(u.cbBody, 0u);
    }

    // ── UnpackRaw ────────────────────────────────────────────────────────

    TEST(NetPacketLogicTest, UnpackRawSplitsWellFormedProtoFrame) {
        const auto f = Frame(kProtoLicenseList, 3, 3, 5);
        const Unpacked u = Unpack(f);

        ASSERT_TRUE(u.ok);
        EXPECT_EQ(static_cast<uint32>(u.eMsg), 780u);
        EXPECT_EQ(u.pHdr, f.data() + sizeof(MsgHdr));
        EXPECT_EQ(u.cbHdr, 3u);
        EXPECT_EQ(u.pBody, f.data() + sizeof(MsgHdr) + 3);
        EXPECT_EQ(u.cbBody, 5u);
    }

    // Boundary, not an attack: a header that exactly fills the frame is
    // legitimate and must not be rejected.
    TEST(NetPacketLogicTest, UnpackRawAcceptsHeaderFillingWholeFrame) {
        const auto f = Frame(kProtoLicenseList, 16, 16, 0);
        const Unpacked u = Unpack(f);

        ASSERT_TRUE(u.ok);
        EXPECT_EQ(u.cbHdr, 16u);
        EXPECT_EQ(u.cbBody, 0u);
        EXPECT_EQ(u.pBody, f.data() + f.size());
    }

    TEST(NetPacketLogicTest, UnpackRawRejectsHeaderOneByteLongerThanFrame) {
        ExpectZeroed(Unpack(Frame(kProtoLicenseList, 17, 16, 0)));
    }

    // Regression: sizeof(MsgHdr) + headerLength used to be computed in size_t
    // and truncated to uint32, so a headerLength near UINT32_MAX wrapped the
    // offset to a small value, passed the bounds test, and handed callers a
    // ~4 GB cbHdr over a tiny buffer.
    TEST(NetPacketLogicTest, UnpackRawRejectsHeaderLengthThatWrapsTheOffset) {
        for (const uint32 claimed : {0xFFFFFFFFu, 0xFFFFFFFCu, 0xFFFFFFF9u,
                                     static_cast<uint32>(0u - sizeof(MsgHdr))}) {
            SCOPED_TRACE(claimed);
            ExpectZeroed(Unpack(Frame(kProtoLicenseList, claimed, 8, 8)));
        }
    }

    TEST(NetPacketLogicTest, UnpackRawRejectsTruncatedNullAndNonProtoFrames) {
        std::vector<uint8> truncated(sizeof(MsgHdr) - 1, 0);
        ExpectZeroed(Unpack(truncated));

        Unpacked u;
        u.ok = UnpackRaw(nullptr, 64, u.eMsg, u.pHdr, u.cbHdr, u.pBody, u.cbBody);
        ExpectZeroed(u);

        ExpectZeroed(Unpack(Frame(780u /* no proto flag */, 3, 3, 5)));
    }

} // namespace
