// Unit tests for NetPacketLogic -- the pure wire-layout logic behind
// Hooks_NetPacket.cpp (see Hook/NetPacketLogic.h), extracted so it is
// testable without the hook engine or a live Steam connection.

#include "Hook/NetPacketLogic.h"
#include "OSTPlatform/include/Memory.h"
#include "Steam/NetPacket.h"

#include "steam_messages.pb.h"

#include <gtest/gtest.h>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

using NetPacketLogic::LayoutResolver;
using NetPacketLogic::MatchingLayouts;
using NetPacketLogic::UnpackRaw;
using Outcome = LayoutResolver::Outcome;

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

    // ── CNetPacket layout detection ──────────────────────────────────────

    // The fixtures below place fields by these offsets; pin the table order.
    static_assert(NetPkt::kLayouts[0].dataOff == 0x08, "stable layout first");
    static_assert(NetPkt::kLayouts[1].dataOff == 0x10, "beta layout second");
    constexpr uint32_t kStableOff = NetPkt::kLayouts[0].dataOff;
    constexpr uint32_t kBetaOff   = NetPkt::kLayouts[1].dataOff;
    constexpr uint32_t kStableBit = 1u << 0;
    constexpr uint32_t kBetaBit   = 1u << 1;

    constexpr NetPacketLogic::ReadableFn kReadable = &OSTPlatform::Memory::IsReadable;

    // Bytes of a fake CNetPacket, sized for the largest known layout
    // (m_pNext ends at +0x30 in the beta layout) so probing every candidate
    // stays inside the buffer.
    struct alignas(8) FakePacket {
        std::array<uint8, 0x30> bytes{};
        CNetPacket* get() { return reinterpret_cast<CNetPacket*>(bytes.data()); }
    };

    FakePacket PacketAt(uint32_t dataOff, const uint8* data, uint32 size, int32 cRef = 1) {
        FakePacket p;
        std::memcpy(p.bytes.data() + dataOff, &data, sizeof(data));
        std::memcpy(p.bytes.data() + dataOff + sizeof(data), &size, sizeof(size));
        std::memcpy(p.bytes.data() + dataOff + sizeof(data) + sizeof(size), &cRef, sizeof(cRef));
        return p;
    }

    FakePacket PacketAt(uint32_t dataOff, const std::vector<uint8>& frame, int32 cRef = 1) {
        return PacketAt(dataOff, frame.data(), static_cast<uint32>(frame.size()), cRef);
    }

    // A frame the probe must accept: MsgHdr + a real serialized
    // CMsgProtoBufHeader + a few body bytes.
    std::vector<uint8> ProtoFrame(uint32 rawEMsg = kProtoLicenseList) {
        CMsgProtoBufHeader hdr;
        hdr.set_steamid(76561197960287930ull);
        hdr.set_client_sessionid(42);
        const std::string wire = hdr.SerializeAsString();
        auto f = Frame(rawEMsg, static_cast<uint32>(wire.size()), wire.size(), 4);
        std::memcpy(f.data() + sizeof(MsgHdr), wire.data(), wire.size());
        return f;
    }

    // Owns a VirtualAlloc reservation for the duration of a test.
    struct Pages {
        explicit Pages(size_t bytes) : size(bytes),
            base(static_cast<uint8*>(VirtualAlloc(nullptr, bytes, MEM_RESERVE, PAGE_NOACCESS))) {}
        ~Pages() { if (base) VirtualFree(base, 0, MEM_RELEASE); }
        Pages(const Pages&) = delete;
        Pages& operator=(const Pages&) = delete;
        bool Commit(size_t offset, size_t bytes, DWORD protect) {
            return VirtualAlloc(base + offset, bytes, MEM_COMMIT, protect) != nullptr;
        }
        bool Protect(size_t offset, size_t bytes, DWORD protect) {
            DWORD old = 0;
            return VirtualProtect(base + offset, bytes, protect, &old) != FALSE;
        }
        size_t size;
        uint8* base;
    };

    size_t PageSize() {
        SYSTEM_INFO si{};
        GetSystemInfo(&si);
        return si.dwPageSize;
    }

    TEST(NetPacketLayoutTest, ProbeMatchesOnlyTheLayoutThePacketUses) {
        const auto frame = ProtoFrame();
        auto stable = PacketAt(kStableOff, frame);
        auto beta   = PacketAt(kBetaOff, frame);

        EXPECT_EQ(MatchingLayouts(stable.get(), kReadable), kStableBit);
        EXPECT_EQ(MatchingLayouts(beta.get(), kReadable), kBetaBit);
    }

    // A non-proto frame carries no evidence: no layout may match.
    TEST(NetPacketLayoutTest, ProbeMatchesNothingOnNonProtoFrame) {
        const auto frame = ProtoFrame(780u /* proto flag clear */);
        auto p = PacketAt(kStableOff, frame);

        EXPECT_EQ(MatchingLayouts(p.get(), kReadable), 0u);
    }

    TEST(NetPacketLayoutTest, ProbeRejectsImplausibleFieldsAndNonProtobufHeader) {
        const auto frame = ProtoFrame();
        auto deadRef = PacketAt(kStableOff, frame, /*cRef=*/0);
        EXPECT_EQ(MatchingLayouts(deadRef.get(), kReadable), 0u);

        auto hugeRef = PacketAt(kStableOff, frame, /*cRef=*/4097);
        EXPECT_EQ(MatchingLayouts(hugeRef.get(), kReadable), 0u);

        // Header bytes that are not a protobuf message (an unterminated varint).
        auto junk = ProtoFrame();
        std::fill(junk.begin() + sizeof(MsgHdr), junk.end() - 4, uint8{0xFF});
        auto p = PacketAt(kStableOff, junk);
        EXPECT_EQ(MatchingLayouts(p.get(), kReadable), 0u);
    }

    // The probe must ask before it reads: a would-be valid frame behind a
    // PAGE_NOACCESS page is rejected, not faulted on.
    TEST(NetPacketLayoutTest, ProbeNeverReadsThroughUnreadablePointer) {
        const size_t page = PageSize();
        Pages mem(page);
        ASSERT_NE(mem.base, nullptr);
        ASSERT_TRUE(mem.Commit(0, page, PAGE_READWRITE));
        const auto frame = ProtoFrame();
        std::memcpy(mem.base, frame.data(), frame.size());
        ASSERT_TRUE(mem.Protect(0, page, PAGE_NOACCESS));

        auto p = PacketAt(kStableOff, mem.base, static_cast<uint32>(frame.size()));
        EXPECT_EQ(MatchingLayouts(p.get(), kReadable), 0u);
    }

    TEST(NetPacketLayoutTest, ResolverLatchesEachLayoutFromTwoLivePackets) {
        for (const auto& layout : NetPkt::kLayouts) {
            SCOPED_TRACE(layout.name);
            const auto frame = ProtoFrame();
            auto p = PacketAt(layout.dataOff, frame);
            LayoutResolver r;

            const auto first = r.Observe(MatchingLayouts(p.get(), kReadable));
            EXPECT_EQ(first.outcome, Outcome::Awaiting);

            const auto second = r.Observe(MatchingLayouts(p.get(), kReadable));
            EXPECT_EQ(second.outcome, Outcome::Latched);
            EXPECT_EQ(second.dataOff, layout.dataOff);
            EXPECT_EQ(second.attempts, 2);
        }
    }

    // Early connection traffic interleaves non-proto frames; one of them must
    // not restart the confirmation, but it still counts against the budget.
    TEST(NetPacketLayoutTest, ResolverKeepsAgreementAcrossNonProtoFrames) {
        LayoutResolver r;
        EXPECT_EQ(r.Observe(kStableBit).outcome, Outcome::Awaiting);

        const auto none = r.Observe(0);
        EXPECT_EQ(none.outcome, Outcome::NoMatch);
        EXPECT_EQ(none.attempts, 2);

        EXPECT_EQ(r.Observe(kStableBit).outcome, Outcome::Latched);
    }

    TEST(NetPacketLayoutTest, ResolverNeverLatchesOnAmbiguity) {
        LayoutResolver r;
        EXPECT_EQ(r.Observe(kStableBit).outcome, Outcome::Awaiting);
        EXPECT_EQ(r.Observe(kStableBit | kBetaBit).outcome, Outcome::Ambiguous);

        // The standing agreement was dropped: confirmation starts over.
        EXPECT_EQ(r.Observe(kStableBit).outcome, Outcome::Awaiting);
        EXPECT_EQ(r.Observe(kStableBit).outcome, Outcome::Latched);
    }

    TEST(NetPacketLayoutTest, ResolverRestartsConfirmationWhenWinnerChanges) {
        LayoutResolver r;
        EXPECT_EQ(r.Observe(kStableBit).outcome, Outcome::Awaiting);

        const auto flip = r.Observe(kBetaBit);
        EXPECT_EQ(flip.outcome, Outcome::Awaiting);
        EXPECT_EQ(flip.dataOff, kBetaOff);

        const auto confirm = r.Observe(kBetaBit);
        EXPECT_EQ(confirm.outcome, Outcome::Latched);
        EXPECT_EQ(confirm.dataOff, kBetaOff);
    }

    // Fail-closed: a client whose layout matches no candidate ends disabled
    // (no field ever touched), never latched on a guess.
    TEST(NetPacketLayoutTest, ResolverGivesUpAfterAttemptBudget) {
        LayoutResolver r;
        for (int i = 0; i < LayoutResolver::kMaxAttempts; ++i)
            ASSERT_EQ(r.Observe(0).outcome, Outcome::NoMatch);

        const auto over = r.Observe(kStableBit);   // would have matched
        EXPECT_EQ(over.outcome, Outcome::Disabled);
        EXPECT_EQ(over.attempts, LayoutResolver::kMaxAttempts + 1);
        EXPECT_EQ(r.Observe(kStableBit).outcome, Outcome::Disabled);
    }

    TEST(NetPacketLayoutTest, AccessorsReadAndWriteAtTheGivenOffset) {
        for (const auto& layout : NetPkt::kLayouts) {
            SCOPED_TRACE(layout.name);
            const auto frame = ProtoFrame();
            auto p = PacketAt(layout.dataOff, frame);

            EXPECT_EQ(NetPkt::DataAt(p.get(), layout.dataOff), frame.data());
            EXPECT_EQ(NetPkt::SizeAt(p.get(), layout.dataOff), frame.size());

            uint8 replacement[4]{};
            NetPkt::DataAt(p.get(), layout.dataOff) = replacement;
            NetPkt::SizeAt(p.get(), layout.dataOff) = 4;

            const uint8* data = nullptr;
            uint32 size = 0;
            std::memcpy(&data, p.bytes.data() + layout.dataOff, sizeof(data));
            std::memcpy(&size, p.bytes.data() + layout.dataOff + sizeof(data), sizeof(size));
            EXPECT_EQ(data, replacement);
            EXPECT_EQ(size, 4u);
        }
    }

    // Until a layout is latched (and after it is disabled) the accessors must
    // hit the discard sink, never the packet.
    TEST(NetPacketLayoutTest, AccessorsNeverTouchThePacketWhileUnresolved) {
        ASSERT_FALSE(NetPkt::IsResolved());   // nothing in this binary latches the global

        const auto frame = ProtoFrame();
        auto p = PacketAt(kStableOff, frame);
        const auto before = p.bytes;

        uint8 other[1]{};
        NetPkt::Data(p.get()) = other;
        NetPkt::Size(p.get()) = 1;
        EXPECT_EQ(p.bytes, before);
        EXPECT_EQ(&NetPkt::Data(p.get()), &NetPkt::detail::g_trashData);
        EXPECT_EQ(&NetPkt::DataAt(p.get(), NetPkt::kDisabled), &NetPkt::detail::g_trashData);
        EXPECT_EQ(&NetPkt::SizeAt(p.get(), NetPkt::kDisabled), &NetPkt::detail::g_trashSize);
    }

    // ── OSTPlatform::Memory::IsReadable ──────────────────────────────────

    TEST(MemoryIsReadableTest, AcceptsCommittedMemoryAndRejectsDegenerateInput) {
        const std::vector<uint8> heap(64, 0);
        const int stack = 0;
        EXPECT_TRUE(OSTPlatform::Memory::IsReadable(heap.data(), heap.size()));
        EXPECT_TRUE(OSTPlatform::Memory::IsReadable(&stack, sizeof(stack)));

        EXPECT_FALSE(OSTPlatform::Memory::IsReadable(nullptr, 8));
        EXPECT_FALSE(OSTPlatform::Memory::IsReadable(heap.data(), 0));
        EXPECT_FALSE(OSTPlatform::Memory::IsReadable(reinterpret_cast<const void*>(UINTPTR_MAX - 3), 8));
    }

    TEST(MemoryIsReadableTest, RejectsNoAccessGuardAndStraddlingRanges) {
        const size_t page = PageSize();
        Pages mem(2 * page);                  // [0] committed below, [1] reserved only
        ASSERT_NE(mem.base, nullptr);
        ASSERT_TRUE(mem.Commit(0, page, PAGE_READWRITE));

        EXPECT_TRUE(OSTPlatform::Memory::IsReadable(mem.base, page));
        // One byte into the reserved neighbour is enough to fail the range.
        EXPECT_FALSE(OSTPlatform::Memory::IsReadable(mem.base + page - 4, 8));
        EXPECT_FALSE(OSTPlatform::Memory::IsReadable(mem.base + page, 1));

        ASSERT_TRUE(mem.Protect(0, page, PAGE_READWRITE | PAGE_GUARD));
        EXPECT_FALSE(OSTPlatform::Memory::IsReadable(mem.base, 1));

        ASSERT_TRUE(mem.Protect(0, page, PAGE_NOACCESS));
        EXPECT_FALSE(OSTPlatform::Memory::IsReadable(mem.base, 1));
    }

} // namespace
