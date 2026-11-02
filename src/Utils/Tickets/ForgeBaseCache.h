#pragma once

// Account-keyed single-slot cache for the forge base ticket (AppTicket.cpp).
//
// A forged ownership ticket inherits the base ticket's SteamID, and Denuvo
// cross-checks it against GetSteamID. A base resolved for account A is therefore
// wrong the moment the active account becomes B (switching accounts or Family
// Sharing without restarting Steam) and would guarantee error 54. The slot
// remembers WHICH account it was resolved for and only hits for that account.
//
// Header-only and dependency-free so it is unit-testable. NOT internally
// synchronized: AppTicket.cpp guards it with its own mutex.

#include <cstdint>
#include <type_traits>
#include <utility>

namespace ForgeBaseCache {

    // True if a ticket carrying ticketSteamId may serve as a forge base for the
    // active account. The account id is the low 32 bits of a SteamID64. When the
    // active account is unknown (activeAccountId == 0) the check cannot be made
    // and the ticket is accepted (pre-v1.3.4 behaviour for the fallback probes).
    // A ticket whose SteamID could not be read (0) is rejected when the account
    // IS known.
    [[nodiscard]] constexpr bool TicketBelongsToAccount(uint64_t ticketSteamId,
                                                        uint32_t activeAccountId) noexcept
    {
        if (activeAccountId == 0) return true;
        return static_cast<uint32_t>(ticketSteamId & 0xFFFFFFFFull) == activeAccountId;
    }

    template <typename T>
    class AccountKeyedCache {
        // L-5: requirements the members below actually use. Copy-assignable is
        // needed by Lookup (copies the value out), move-assignable by Store and
        // Invalidate. T need NOT be trivially copyable (SourceTicket holds a vector).
        static_assert(std::is_default_constructible_v<T>, "AccountKeyedCache<T>: T must be default-constructible");
        static_assert(std::is_copy_assignable_v<T>,       "AccountKeyedCache<T>: Lookup copies T out");
        static_assert(std::is_move_assignable_v<T>,       "AccountKeyedCache<T>: Store/Invalidate move-assign T");

    public:
        // True and *out filled only when a value is stored for exactly accountId.
        // accountId == 0 (no active user) never hits.
        bool Lookup(uint32_t accountId, T& out) const {
            if (!m_valid || accountId == 0 || accountId != m_accountId) return false;
            out = m_value;
            return true;
        }

        // Store a freshly resolved value for accountId (replaces any other
        // account's entry). accountId == 0 is refused.
        [[nodiscard]] bool Store(uint32_t accountId, T value) {
            if (accountId == 0) return false;
            m_accountId = accountId;
            m_value = std::move(value);
            m_valid = true;
            return true;
        }

        // Drop the entry. Returns true if there was one.
        bool Invalidate() {
            const bool had = m_valid;
            m_valid = false;
            m_accountId = 0;
            m_value = T{};
            return had;
        }

        // Invalidate if the entry belongs to a different account than accountId
        // (including accountId == 0). Returns true if an entry was dropped.
        bool InvalidateIfAccountChanged(uint32_t accountId) {
            if (m_valid && m_accountId != accountId) return Invalidate();
            return false;
        }

        [[nodiscard]] bool     HasValue() const noexcept  { return m_valid; }
        [[nodiscard]] uint32_t AccountId() const noexcept { return m_accountId; }

    private:
        bool     m_valid = false;
        uint32_t m_accountId = 0;
        T        m_value{};
    };

} // namespace ForgeBaseCache
