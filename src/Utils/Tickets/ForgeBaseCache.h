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
#include <utility>

namespace ForgeBaseCache {

    template <typename T>
    class AccountKeyedCache {
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
        bool Store(uint32_t accountId, T value) {
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

        bool     HasValue() const  { return m_valid; }
        uint32_t AccountId() const { return m_accountId; }

    private:
        bool     m_valid = false;
        uint32_t m_accountId = 0;
        T        m_value{};
    };

} // namespace ForgeBaseCache
