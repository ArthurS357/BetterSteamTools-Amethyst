#include "Utils/Tickets/AppTicket.h"

#include <cstring>

// Pure parsing logic split out of AppTicket.cpp so it can be compiled and unit
// tested without pulling in Hooks_Decryption/LuaConfig/SteamCredentialStore
// (see src/tests/CMakeLists.txt, which compiles this TU directly).
namespace AppTicket {

    namespace {
        constexpr std::size_t kSteamIdTicketMinimumSize = kAppTicketSteamIdOffset + sizeof(uint64_t);
    }

    uint64_t ExtractSteamIdFromTicketBytes(const std::vector<uint8_t>& ticket) noexcept {
        // Layout: ticket bytes start with [uint32 Size][uint32 Version][uint64 SteamID][...].
        if (ticket.size() < kSteamIdTicketMinimumSize) return 0;

        // memcpy into a local object instead of reinterpret_cast + dereference:
        // the buffer holds no uint64_t object, so aliasing it directly is UB
        // even though it "works" on MSVC today (see cpp-typing, RAII/aliasing).
        uint64_t steamId = 0;
        std::memcpy(&steamId, ticket.data() + kAppTicketSteamIdOffset, sizeof(steamId));
        return steamId;
    }

} // namespace AppTicket
