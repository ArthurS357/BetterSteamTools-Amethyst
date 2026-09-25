#pragma once

#include "Steam/Structs.h"

// Pure wire-layout logic behind Hooks_NetPacket.cpp, split out so it compiles
// in the unit-test binary without HookMacros/Detour/the hook engine -- same
// split as LicenseListLogic / OnlineFixLogic.
namespace NetPacketLogic {

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
