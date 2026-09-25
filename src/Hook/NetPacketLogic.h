#pragma once

#include "Steam/NetPacket.h"
#include "Steam/Structs.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

// Pure wire-layout logic behind Hooks_NetPacket.cpp, split out so it compiles
// in the unit-test binary without HookMacros/Detour/the hook engine -- same
// split as LicenseListLogic / OnlineFixLogic.
namespace NetPacketLogic {

    // ── CNetPacket layout detection ─────────────────────────────────────
    // Identifies which NetPkt::kLayouts row the running client uses from live
    // packets instead of compiling one in (see Steam/NetPacket.h). Ported from
    // upstream BetterSteamTools 1d15f39, split into a probe (reads memory) and
    // a resolver (pure state machine) so every transition is unit-testable.

    // Readability check for memory of unknown provenance. Production passes
    // OSTPlatform::Memory::IsReadable; it is a parameter so the probe never
    // dereferences a byte it has not first asked about.
    using ReadableFn = bool (*)(const void* addr, size_t bytes);

    // Does the kLayouts row with m_pubData at `dataOff` describe `packet`?
    // True only if the fields at that offset form a plausible (pointer, size,
    // refcount) triple AND the pointed-to bytes parse as a Steam protobuf frame
    // header. Reads nothing `readable` has not approved.
    [[nodiscard]] bool ProbeLayout(const void* packet, uint32_t dataOff, ReadableFn readable);

    // Bit i set <=> NetPkt::kLayouts[i] passes ProbeLayout on this packet.
    [[nodiscard]] uint32_t MatchingLayouts(const void* packet, ReadableFn readable);

    // Two-packet confirmation over MatchingLayouts() results. Latches only
    // when exactly one candidate matches and the same one also won the
    // previous informative packet: ambiguity is the one thing it must never
    // latch on, and requiring agreement costs at most one early proto message.
    // A failed probe costs one packet (passed through untouched); a wrong latch
    // would cost a wild pointer write into a live Steam object -- hence the bar.
    //
    // Pure (no memory access). One instance lives in Hooks_NetPacket.cpp and
    // its Latched/Disabled results are published through NetPkt::Latch/
    // Disable. Members are atomic: hkRecvPkt runs on whichever Steam thread
    // delivers the packet.
    class LayoutResolver {
    public:
        static constexpr int kMaxAttempts = 512;

        enum class Outcome : uint8_t {
            NoMatch,    // nothing matched (e.g. a non-proto frame): no evidence, agreement kept
            Ambiguous,  // more than one candidate matched: agreement dropped
            Awaiting,   // one candidate matched, not yet confirmed by a second packet
            Latched,    // the same single candidate won twice in a row: dataOff is final
            Disabled,   // attempt budget exhausted: give up for the session
        };

        struct Step {
            Outcome  outcome  = Outcome::NoMatch;
            uint32_t dataOff  = NetPkt::kUnresolved;   // set for Awaiting/Latched
            int      attempts = 0;
        };

        [[nodiscard]] Step Observe(uint32_t matchMask) noexcept;

    private:
        std::atomic<int>      attempts_{ 0 };
        std::atomic<uint32_t> agreed_{ NetPkt::kUnresolved };
    };

    // Splits a raw CM frame into (eMsg, proto header, body). Only protobuf
    // frames (kMsgHdrProtoFlag set) are accepted; every output is zeroed on
    // failure. `size` is the real buffer size, `headerLength` is what the frame
    // claims -- never trusted until checked against `size`.
    inline bool UnpackRaw(const uint8* data, uint32 size,
                          EMsg& eMsg, const uint8*& pHdr, uint32& cbHdr,
                          const uint8*& pBody, uint32& cbBody)
    {
        if (!data || size < sizeof(MsgHdr)) {
        fail:
            eMsg = static_cast<EMsg>(0);
            cbHdr = 0;
            pHdr = nullptr;
            pBody = nullptr;
            cbBody = 0;
            return false;
        }
        const MsgHdr* hdr = reinterpret_cast<const MsgHdr*>(data);
        if (!(hdr->eMsg & kMsgHdrProtoFlag)) goto fail;

        eMsg  = static_cast<EMsg>(hdr->eMsg & ~kMsgHdrProtoFlag);
        cbHdr = hdr->headerLength;
        // Bound before adding: `sizeof(MsgHdr) + cbHdr` truncated to uint32
        // wraps for cbHdr near UINT32_MAX and slipped past `off > size`.
        // size >= sizeof(MsgHdr) holds from the check above.
        if (cbHdr > size - sizeof(MsgHdr)) goto fail;
        const uint32 off = static_cast<uint32>(sizeof(MsgHdr)) + cbHdr;
        pHdr   = data + sizeof(MsgHdr);
        pBody  = data + off;
        cbBody = size - off;
        return true;
    }

} // namespace NetPacketLogic
