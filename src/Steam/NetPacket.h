#pragma once

#include "Types.h"

#include <atomic>
#include <cstdint>

// CNetPacket's layout differs between Steam client builds, so it is opaque
// (Structs.h): never declare its fields and never take its sizeof. The two
// fields AmethystTool uses are reached through Data()/Size(), which apply an
// offset detected at runtime from a live packet (NetPacketLogic::
// LayoutResolver, driven from hkRecvPkt).
//
//   field                stable      beta (steamclient64 d2d085e7+)
//   m_hConnection        +0x00       +0x00
//   version stamps       --          +0x04, +0x08  (+ 4 bytes padding @ +0x0C)
//   m_pubData            +0x08       +0x10
//   m_cubData            +0x10       +0x18
//   m_cRef               +0x14       +0x1C
//   m_pubNetworkBuffer   +0x18       +0x20
//   m_pNext              +0x20       +0x28
//
// Ported from upstream BetterSteamTools 0b776c5/1d15f39. The beta column is
// upstream's reverse engineering; it has not been verified against a beta
// client here (development machine runs the stable client).
//
// m_cubData always sits right after m_pubData, so one offset describes the
// whole layout -- which is what lets the detected state live in one word.

static_assert(sizeof(void*) == sizeof(uint64_t), "CNetPacket layout table is x64-only");

struct CNetPacket;

namespace NetPkt {

    struct Layout {
        const char* name;
        uint32_t    dataOff;   // offset of m_pubData
    };

    // The candidates the probe considers. A table rather than two branches:
    // Valve will shift this again, and a new row should be the whole change.
    inline constexpr Layout kLayouts[] = {
        { "stable", 0x08 },
        { "beta",   0x10 },
    };

    inline constexpr uint32_t kSizeAfterData = sizeof(uint8*);   // m_cubData - m_pubData

    // For logs only.
    inline const char* LayoutName(uint32_t dataOff) noexcept {
        for (const auto& layout : kLayouts)
            if (layout.dataOff == dataOff) return layout.name;
        return "?";
    }

    inline constexpr uint32_t kUnresolved = 0u;
    inline constexpr uint32_t kDisabled   = 0xFFFFFFFFu;

    // The entire published layout state, in one word so it can never be seen
    // half-written: a separate "resolved" flag beside the offset could be
    // observed set while the offset was still 0, and a field write would land
    // on m_hConnection. Relaxed ordering is enough -- the value is
    // self-contained and publishes nothing else. constinit so a hook firing
    // before dynamic initialization cannot read an uninitialized word.
    inline constinit std::atomic<uint32_t> g_dataOff{ kUnresolved };

    inline uint32_t State()      noexcept { return g_dataOff.load(std::memory_order_relaxed); }
    inline bool     IsResolved() noexcept { const uint32_t v = State(); return v != kUnresolved && v != kDisabled; }
    inline bool     IsDisabled() noexcept { return State() == kDisabled; }

    inline void Latch(uint32_t dataOff) noexcept { g_dataOff.store(dataOff, std::memory_order_relaxed); }

    // Terminal: the layout could not be identified, so no field is touched
    // again this session. Deliberately NOT "fall back to the compiled
    // default" -- on the other client that default is exactly the wild write
    // this mechanism exists to prevent.
    inline void Disable() noexcept { g_dataOff.store(kDisabled, std::memory_order_relaxed); }

    namespace detail {
        // Returned while the layout is unknown. Every real call site sits
        // behind the gate in hkRecvPkt, so this is unreachable today; it makes
        // a call accidentally added above the gate corrupt a dead global
        // instead of a live Steam object.
        inline uint8* g_trashData = nullptr;
        inline uint32 g_trashSize = 0;
    }

    // Field references at an explicit m_pubData offset. The fields are real
    // objects Steam constructed at that offset, so accessing them through
    // their own pointer types is not type punning -- the offset is the only
    // unknown, and it comes from the resolver. Split from Data()/Size() so the
    // offset math is testable without latching the process-wide state.
    inline uint8*& DataAt(CNetPacket* p, uint32_t dataOff) noexcept {
        if (!p || dataOff == kUnresolved || dataOff == kDisabled) return detail::g_trashData;
        return *reinterpret_cast<uint8**>(reinterpret_cast<uint8*>(p) + dataOff);
    }

    inline uint32& SizeAt(CNetPacket* p, uint32_t dataOff) noexcept {
        if (!p || dataOff == kUnresolved || dataOff == kDisabled) return detail::g_trashSize;
        return *reinterpret_cast<uint32*>(reinterpret_cast<uint8*>(p) + dataOff + kSizeAfterData);
    }

    // References, so reads, writes, save/restore pairs and in-place repoints
    // are all a plain substitution at the call sites.
    inline uint8*& Data(CNetPacket* p) noexcept { return DataAt(p, State()); }
    inline uint32& Size(CNetPacket* p) noexcept { return SizeAt(p, State()); }

} // namespace NetPkt
