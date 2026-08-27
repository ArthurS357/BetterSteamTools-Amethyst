// Unit tests for AppTicket::ExtractSteamIdFromTicketBytes — the parser that
// pulls the SteamID out of a cached app-ownership-ticket byte blob.
//
// Contract per AppTicket.h: layout is [uint32 Size][uint32 Version][uint64
// SteamID][...]; SteamID lives at byte offset kAppTicketSteamIdOffset (8).
// Returns 0 (not an error type) when the ticket is too short to contain one —
// callers treat 0 as "no SteamID available", matching k_steamIDNil semantics.
//
// Three categories: happy path, malicious/malformed input, and false-positive
// guards (legitimate-but-unusual input that must NOT be rejected).

#include "Utils/Tickets/AppTicket.h"

#include <gtest/gtest.h>

#include <cstring>

using namespace AppTicket;

namespace {

    // Builds a minimal ticket byte blob: [Size][Version][SteamID][trailer...].
    std::vector<uint8_t> MakeTicket(uint32_t size, uint32_t version, uint64_t steamId,
                                     const std::vector<uint8_t>& trailer = {}) {
        std::vector<uint8_t> ticket;
        ticket.resize(kAppTicketSteamIdOffset + sizeof(uint64_t));
        std::memcpy(ticket.data() + 0, &size, sizeof(size));
        std::memcpy(ticket.data() + 4, &version, sizeof(version));
        std::memcpy(ticket.data() + kAppTicketSteamIdOffset, &steamId, sizeof(steamId));
        ticket.insert(ticket.end(), trailer.begin(), trailer.end());
        return ticket;
    }

} // namespace

TEST(ExtractSteamIdFromTicketBytes, HappyPath) {
    const uint64_t steamId = 76561197960287930ull; // real-shaped 64-bit SteamID
    const auto ticket = MakeTicket(/*size=*/1000, /*version=*/1, steamId);
    EXPECT_EQ(ExtractSteamIdFromTicketBytes(ticket), steamId);
}

TEST(ExtractSteamIdFromTicketBytes, IgnoresTrailingBytes) {
    // A real ticket has a lot more after the SteamID (app ownership flags,
    // signature, ...) — only the fixed-offset SteamID field should be read.
    const uint64_t steamId = 42;
    std::vector<uint8_t> trailer(200, 0xAB);
    const auto ticket = MakeTicket(1000, 1, steamId, trailer);
    EXPECT_EQ(ExtractSteamIdFromTicketBytes(ticket), steamId);
}

TEST(ExtractSteamIdFromTicketBytes, MaliciousAndMalformed) {
    EXPECT_EQ(ExtractSteamIdFromTicketBytes({}), 0u); // empty ticket
    EXPECT_EQ(ExtractSteamIdFromTicketBytes(std::vector<uint8_t>(4, 0)), 0u); // header only
    // One byte short of containing a full SteamID: must not read past the end.
    std::vector<uint8_t> oneShort(kAppTicketSteamIdOffset + sizeof(uint64_t) - 1, 0xFF);
    EXPECT_EQ(ExtractSteamIdFromTicketBytes(oneShort), 0u);
}

TEST(ExtractSteamIdFromTicketBytes, FalsePositiveGuards) {
    // Exactly the minimum size (no trailer at all) must still parse — this is
    // the boundary a naive "> minimum" check would wrongly reject.
    const uint64_t steamId = 0x0102030405060708ull;
    const auto ticket = MakeTicket(0, 0, steamId);
    ASSERT_EQ(ticket.size(), kAppTicketSteamIdOffset + sizeof(uint64_t));
    EXPECT_EQ(ExtractSteamIdFromTicketBytes(ticket), steamId);

    // A SteamID whose value happens to be 0 is legitimate input (not an error
    // sentinel here) and must round-trip as 0, not be conflated with "too short".
    const auto zeroIdTicket = MakeTicket(0, 0, 0);
    EXPECT_EQ(ExtractSteamIdFromTicketBytes(zeroIdTicket), 0u);
}
