#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

// Pure failover policy behind ManifestClient::FetchManifestRequestCode (v1.5.0),
// header-only so it is unit-testable without WinHTTP, Config or LuaConfig: the
// clock and the per-provider fetch are injected (see RunChain).
//
// Problem: one provider was active at a time, so when it stopped answering every
// download of a Lua-added depot failed until the user edited the TOML. The
// request code cannot be minted locally for depots the account does not own, so
// the realistic fix is to take over with the next provider.
//
// Policy:
//   * chain    -- the configured provider first, then the defaults (wudrm,
//                 steamrun). A [manifest] url_template override is the user's own
//                 endpoint and is never swapped for a built-in one.
//   * budget   -- ~10 s for the whole chain. Steam's receive path stops waiting at
//                 12 s (Hooks_NetPacket.cpp, kMaxWaitSeconds), so a slower answer
//                 is thrown away anyway. Each attempt gets an even share of what is
//                 left, so a provider that fails fast hands its time to the next.
//                 Best effort, not a hard limit: WinHTTP applies its timeouts per
//                 phase and per read, so a provider that trickles bytes can overrun
//                 its share. The 12 s wait is the real backstop.
//   * breaker  -- a provider that is down (no response, or 5xx) is skipped for
//                 5 minutes, so later requests do not each pay its timeout. A 4xx or
//                 an unparsable 200 only means "this provider lacks that code": try
//                 the next one, but do not quarantine. A 429 is in that group on
//                 purpose: with several depots asking at once the burst can be ours,
//                 and quarantining a provider for our own traffic would be wrong.
//   * no blackout -- if every provider is quarantined, all of them are tried
//                 anyway. Five minutes of no manifests after a network blip is
//                 worse than one slow attempt.
namespace ManifestFailoverLogic {

    using Clock  = std::chrono::steady_clock;
    using Millis = std::chrono::milliseconds;

    inline constexpr Millis kTotalBudget{ 10'000 };
    // Shorter than this cannot finish a TLS handshake plus a request, so such an
    // attempt is not started at all.
    inline constexpr Millis kMinAttemptBudget{ 1'500 };
    inline constexpr std::chrono::minutes kQuarantine{ 5 };

    // Built-in providers in failover order. The configured provider goes first and
    // is skipped here. "opensteamtool" is deliberately absent: it stopped serving
    // manifests for games the account does not own (2026-09), see ManifestClient.cpp.
    inline constexpr std::array<std::string_view, 2> kDefaultChain = { "wudrm", "steamrun" };

    // `primary` is the configured provider. With failover off, or with a url_template
    // override (the user's own endpoint), the chain is just that one entry.
    [[nodiscard]] inline std::vector<std::string> BuildChain(std::string_view primary,
                                                             bool failoverEnabled,
                                                             bool hasOverride)
    {
        std::vector<std::string> chain{ std::string(primary) };
        if (!failoverEnabled || hasOverride) return chain;
        for (std::string_view name : kDefaultChain)
            if (name != primary) chain.emplace_back(name);
        return chain;
    }

    // Did this answer say the provider is down, as opposed to "I do not have that
    // code"? Only "down" is worth a quarantine.
    [[nodiscard]] constexpr bool IsProviderDown(bool transportOk, std::uint32_t status) noexcept
    {
        return !transportOk || status >= 500;
    }

    // After a depot-aware request failed: ask the same provider again, gid-only?
    //   shapeAllowsRetry -- ManifestUrlLogic::ShouldFallbackToGidOnly said yes
    //   fit              -- other providers are queued behind this one
    // Alone (fit = false) the rule is exactly the pre-v1.5.0 one. With others
    // waiting, a provider that is down gets one request, not two, and the retry
    // must still fit what is left of this attempt's time.
    [[nodiscard]] constexpr bool ShouldRetryGidOnly(bool shapeAllowsRetry, bool fit, bool transportOk,
                                                    std::uint32_t status, Millis timeLeft) noexcept
    {
        if (!shapeAllowsRetry) return false;
        if (!fit) return true;
        return !IsProviderDown(transportOk, status) && timeLeft >= kMinAttemptBudget;
    }

    // Thread-safe: requests for different depots run concurrently. The mutex is a
    // leaf lock, never held while a request is in flight.
    class CircuitBreaker {
    public:
        explicit CircuitBreaker(Millis quarantine = std::chrono::duration_cast<Millis>(kQuarantine))
            : quarantine_(quarantine) {}

        [[nodiscard]] bool IsQuarantined(std::string_view provider, Clock::time_point now) const
        {
            const std::lock_guard lock(mutex_);
            const auto it = until_.find(provider);
            return it != until_.end() && now < it->second;
        }

        void RecordDown(std::string_view provider, Clock::time_point now)
        {
            const std::lock_guard lock(mutex_);
            until_[std::string(provider)] = now + quarantine_;
        }

        // The provider answered (even with a 404): lift any quarantine.
        void RecordUp(std::string_view provider)
        {
            const std::lock_guard lock(mutex_);
            if (const auto it = until_.find(provider); it != until_.end()) until_.erase(it);
        }

    private:
        Millis quarantine_;
        mutable std::mutex mutex_;
        std::map<std::string, Clock::time_point, std::less<>> until_;
    };

    // The chain to attempt now: quarantined providers are dropped -- unless that
    // would leave nothing, in which case the whole chain is tried.
    [[nodiscard]] inline std::vector<std::string> EffectiveChain(const std::vector<std::string>& chain,
                                                                 const CircuitBreaker& breaker,
                                                                 Clock::time_point now)
    {
        std::vector<std::string> live;
        for (const std::string& provider : chain)
            if (!breaker.IsQuarantined(provider, now)) live.push_back(provider);
        return live.empty() ? chain : live;
    }

    // Time one attempt may take: an even share of what is left among the attempts
    // still to make, never below kMinAttemptBudget. Zero means "do not start it".
    [[nodiscard]] constexpr Millis AttemptBudget(Millis remaining, std::size_t attemptsLeft) noexcept
    {
        if (attemptsLeft == 0 || remaining < kMinAttemptBudget) return Millis::zero();
        const Millis share = remaining / static_cast<Millis::rep>(attemptsLeft);
        return share < kMinAttemptBudget ? kMinAttemptBudget : share;
    }

    struct PhaseTimeouts {
        std::uint32_t resolve = 5000;
        std::uint32_t connect = 5000;
        std::uint32_t send    = 10000;
        std::uint32_t recv    = 10000;
    };

    // Caps every WinHTTP phase at `budget`, so no single wait can outlast the
    // attempt's share. A failing request ends in the phase that hangs, so this
    // bounds it; shrinking all four in proportion instead would cut a 5 s connect to
    // under a second, and a lost SYN alone costs about 3 s on Windows -- a healthy
    // provider on a lossy line would time out and be quarantined. A phase configured
    // as 0 means "no limit" to WinHTTP, which would defeat the budget, so it becomes
    // the budget. No phase comes out as 0 for the same reason.
    [[nodiscard]] inline PhaseTimeouts CapTimeouts(PhaseTimeouts cfg, Millis budget) noexcept
    {
        const std::uint64_t cap = budget.count() > 0 ? static_cast<std::uint64_t>(budget.count()) : 1;
        const auto capped = [cap](std::uint32_t v) -> std::uint32_t {
            const std::uint64_t limited = v == 0 ? cap : std::min<std::uint64_t>(v, cap);
            return static_cast<std::uint32_t>(std::clamp<std::uint64_t>(limited, 1, UINT32_MAX));
        };
        return { capped(cfg.resolve), capped(cfg.connect), capped(cfg.send), capped(cfg.recv) };
    }

    struct AttemptOutcome {
        bool          transportOk = false;   // WinHTTP produced a response
        std::uint32_t status      = 0;       // HTTP status of the last request made
        bool          gotCode     = false;   // 200 and the body parsed to a request code
        std::uint64_t code        = 0;
    };

    struct ChainResult {
        bool          ok = false;
        std::uint64_t code = 0;
        std::string   provider;              // who answered
        std::size_t   attempts = 0;          // providers actually asked
    };

    // Walks the chain until one provider yields a code or the budget runs out.
    //   now()                  -> Clock::time_point (injected so tests control time)
    //   attempt(provider, end) -> AttemptOutcome: ONE provider's whole fetch (including
    //                             its own depot-aware -> gid-only retry), done by `end`.
    // Quarantine bookkeeping happens here, so a caller cannot forget it.
    template <class NowFn, class AttemptFn>
    ChainResult RunChain(const std::vector<std::string>& chain, CircuitBreaker& breaker,
                         Millis totalBudget, NowFn&& now, AttemptFn&& attempt)
    {
        ChainResult result;
        const Clock::time_point start = now();
        const Clock::time_point end = start + totalBudget;
        const std::vector<std::string> live = EffectiveChain(chain, breaker, start);

        for (std::size_t i = 0; i < live.size(); ++i) {
            const Clock::time_point t = now();
            const Millis budget = AttemptBudget(std::chrono::duration_cast<Millis>(end - t), live.size() - i);
            if (budget == Millis::zero()) break;

            ++result.attempts;
            const AttemptOutcome outcome = attempt(live[i], t + budget);

            if (IsProviderDown(outcome.transportOk, outcome.status)) breaker.RecordDown(live[i], now());
            else                                                     breaker.RecordUp(live[i]);

            // 0 is not a request code: Steam's receive path treats it as a failure
            // (Hooks_NetPacket.cpp), so it must not end the chain here.
            if (outcome.gotCode && outcome.code != 0) {
                result.ok = true;
                result.code = outcome.code;
                result.provider = live[i];
                return result;
            }
        }
        return result;
    }

} // namespace ManifestFailoverLogic
