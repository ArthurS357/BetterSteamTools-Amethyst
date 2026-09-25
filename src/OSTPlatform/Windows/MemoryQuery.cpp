#include "include/Memory.h"

#include "include/Log.h"

#include <windows.h>

#include <cstdint>

// Ported from upstream BetterSteamTools 1d15f39 (Memory.cpp there). Split into
// its own TU here -- see the note on IsReadable in include/Memory.h.

namespace OSTPlatform::Memory {

namespace {

    // PAGE_* protections that permit a read. PAGE_NOACCESS and bare
    // PAGE_EXECUTE (execute-only) are absent on purpose: neither can be read.
    bool IsReadableProtect(DWORD protect) {
        // Strip the modifier bits before comparing -- they combine with the
        // base protection rather than replacing it.
        const DWORD base = protect & ~static_cast<DWORD>(PAGE_GUARD | PAGE_NOCACHE | PAGE_WRITECOMBINE);
        switch (base) {
        case PAGE_READONLY:
        case PAGE_READWRITE:
        case PAGE_WRITECOPY:
        case PAGE_EXECUTE_READ:
        case PAGE_EXECUTE_READWRITE:
        case PAGE_EXECUTE_WRITECOPY:
            return true;
        default:
            return false;
        }
    }

} // namespace

bool IsReadable(const void* addr, size_t bytes) {
    if (!addr || bytes == 0) return false;

    const uintptr_t start = reinterpret_cast<uintptr_t>(addr);
    if (start > UINTPTR_MAX - bytes) return false;   // wraps
    const uintptr_t end = start + bytes;

    // The cursor stays a pointer (advanced by region sizes) so VirtualQuery
    // never receives an integer-to-pointer cast; comparisons use its address.
    const auto* cursor = static_cast<const uint8_t*>(addr);
    for (uintptr_t at = start; at < end; at = reinterpret_cast<uintptr_t>(cursor)) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(cursor, &mbi, sizeof(mbi)) != sizeof(mbi)) {
            OSTP_LOG_TRACE("IsReadable({}, {}): VirtualQuery failed at 0x{:X} (error={})",
                           addr, bytes, at, GetLastError());
            return false;
        }
        // A guard page faults once on first touch, so reading it is not safe
        // even though its base protection says otherwise.
        if (mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD) || !IsReadableProtect(mbi.Protect)) {
            OSTP_LOG_TRACE("IsReadable({}, {}): 0x{:X} not readable (state=0x{:X} protect=0x{:X})",
                           addr, bytes, at, mbi.State, mbi.Protect);
            return false;
        }

        const uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (regionEnd <= at) return false;   // no forward progress; refuse to spin
        cursor += regionEnd - at;
    }
    return true;
}

} // namespace OSTPlatform::Memory
