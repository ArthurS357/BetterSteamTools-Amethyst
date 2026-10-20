#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Pure "missing signature" bookkeeping behind PatternLoader, split out
// (header-only) so the required/optional policy is unit-testable without the
// TOML parser, the module scanner or the popup -- same pattern as
// Hook/ManifestHookLogic.h.
namespace PatternLoaderLogic {

    // Required: absence is reported in the "Missing Signatures" popup (the
    // behavior every hook had before v1.3.2). Optional: best-effort hook whose
    // absence only degrades a feature -- skipped silently, never reported.
    enum class Requirement : uint8_t { Required, Optional };

    // Names of required functions that could not be resolved, accumulated
    // across one hook-install pass and drained by ReportMissingFunctions().
    class MissingFunctionLog {
    public:
        void Record(std::string_view name, Requirement req)
        {
            if (req == Requirement::Required)
                names_.emplace_back(name);
        }

        // Returns everything recorded so far and leaves the log empty, so a
        // second report never repeats names from an earlier pass.
        [[nodiscard]] std::vector<std::string> TakeAll() noexcept
        {
            return std::exchange(names_, {});
        }

        [[nodiscard]] bool Empty() const noexcept { return names_.empty(); }

    private:
        std::vector<std::string> names_;
    };

    // Runs lookup(); a null result is recorded in `log` according to `req`.
    // The address (or nullptr) is returned unchanged either way.
    template <typename Lookup>
    [[nodiscard]] void* ResolveOrRecord(std::string_view name, Requirement req,
                                        MissingFunctionLog& log, Lookup lookup)
    {
        void* addr = lookup();
        if (!addr)
            log.Record(name, req);
        return addr;
    }

} // namespace PatternLoaderLogic
