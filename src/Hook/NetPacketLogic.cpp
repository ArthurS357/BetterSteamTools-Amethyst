#include "Hook/NetPacketLogic.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <iterator>
#include <limits>

#include "steam_messages.pb.h"

namespace NetPacketLogic {

namespace {

    // Plausibility limits for a live CNetPacket. Not kMaxPacketSize from
    // Hooks_NetPacket.cpp: a large Multi must not fail the true candidate.
    constexpr uint32    kProbeMaxPacket   = 1u << 20;             // 1 MiB
    constexpr uint32    kProbeMaxHdrLen   = 8192;
    constexpr uint32    kProbeMinHdrLen   = 2;
    constexpr uint32    kProbeMaxEMsg     = 0x10000;
    constexpr int32     kProbeMaxRefCount = 4096;
    constexpr uintptr_t kMinUserPtr       = 0x10000;              // below: null-page region
    constexpr uintptr_t kMaxUserPtr       = 0x7FFF'FFFF'0000ull;  // x64 user-mode ceiling

    // m_pubData + m_cubData + m_cRef, contiguous in every known layout.
    constexpr size_t kFieldSpan = sizeof(uint8*) + sizeof(uint32) + sizeof(int32);

    constexpr size_t kLayoutCount = std::size(NetPkt::kLayouts);
    static_assert(kLayoutCount <= std::numeric_limits<uint32_t>::digits,
                  "MatchingLayouts packs one bit per layout");
    constexpr uint32_t kLayoutMask = (1u << kLayoutCount) - 1;

} // namespace

bool ProbeLayout(const void* packet, uint32_t dataOff, ReadableFn readable) {
    if (!packet || !readable) return false;

    const auto* fields = static_cast<const uint8*>(packet) + dataOff;
    if (!readable(fields, kFieldSpan)) return false;

    // memcpy, not reinterpret_cast + dereference: at this point the bytes are
    // only a *candidate* layout, not a known object.
    const uint8* data = nullptr;
    uint32 size = 0;
    int32  cRef = 0;
    std::memcpy(static_cast<void*>(&data), fields, sizeof(data));
    std::memcpy(&size, fields + sizeof(data), sizeof(size));
    std::memcpy(&cRef, fields + sizeof(data) + sizeof(size), sizeof(cRef));

    const auto addr = reinterpret_cast<uintptr_t>(data);
    if (addr < kMinUserPtr || addr >= kMaxUserPtr)       return false;
    if (size < sizeof(MsgHdr) || size > kProbeMaxPacket) return false;
    if (cRef < 1 || cRef > kProbeMaxRefCount)            return false;

    if (!readable(data, sizeof(MsgHdr))) return false;

    // Raw dwords rather than MsgHdr::eMsg: EMsg is an unscoped enum with a
    // signed underlying type, so testing 0x80000000 through it only works by
    // accident.
    uint32 rawEMsg = 0;
    uint32 hdrLen  = 0;
    std::memcpy(&rawEMsg, data + offsetof(MsgHdr, eMsg), sizeof(rawEMsg));
    std::memcpy(&hdrLen,  data + offsetof(MsgHdr, headerLength), sizeof(hdrLen));
    if (!(rawEMsg & kMsgHdrProtoFlag)) return false;
    const uint32 eMsg = rawEMsg & ~kMsgHdrProtoFlag;
    if (eMsg == 0 || eMsg >= kProbeMaxEMsg) return false;

    const uint32 maxHdrLen = (std::min)(size - static_cast<uint32>(sizeof(MsgHdr)), kProbeMaxHdrLen);
    if (hdrLen < kProbeMinHdrLen || hdrLen > maxHdrLen) return false;

    if (!readable(data, sizeof(MsgHdr) + hdrLen)) return false;

    // Strongest signal available: the bytes really are a Steam protobuf
    // header. Only ever runs while probing.
    CMsgProtoBufHeader hdr;
    return hdr.ParseFromArray(data + sizeof(MsgHdr), static_cast<int>(hdrLen));
}

uint32_t MatchingLayouts(const void* packet, ReadableFn readable) {
    uint32_t mask = 0;
    for (size_t i = 0; i < kLayoutCount; ++i)
        if (ProbeLayout(packet, NetPkt::kLayouts[i].dataOff, readable))
            mask |= 1u << i;
    return mask;
}

LayoutResolver::Step LayoutResolver::Observe(uint32_t matchMask) noexcept {
    const int attempts = attempts_.fetch_add(1, std::memory_order_relaxed) + 1;
    if (attempts > kMaxAttempts) return { Outcome::Disabled, NetPkt::kUnresolved, attempts };

    matchMask &= kLayoutMask;
    if (matchMask == 0) {
        // What a non-protobuf frame looks like -- no evidence either way.
        // Keep any standing agreement: dropping it here would let one
        // interleaved non-proto packet restart confirmation, which is exactly
        // what early connection traffic does.
        return { Outcome::NoMatch, NetPkt::kUnresolved, attempts };
    }
    if (!std::has_single_bit(matchMask)) {
        // Genuine ambiguity: several layouts read as valid on the same packet.
        // That IS evidence, and it says the standing agreement is not trusted.
        agreed_.store(NetPkt::kUnresolved, std::memory_order_relaxed);
        return { Outcome::Ambiguous, NetPkt::kUnresolved, attempts };
    }

    const uint32_t winner = NetPkt::kLayouts[std::countr_zero(matchMask)].dataOff;
    const uint32_t previous = agreed_.exchange(winner, std::memory_order_relaxed);
    return { previous == winner ? Outcome::Latched : Outcome::Awaiting, winner, attempts };
}

} // namespace NetPacketLogic
