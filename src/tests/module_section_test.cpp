// Unit tests for OSTPlatform::Memory::GetModuleSection -- the PE section lookup
// IPCLoader uses to find steamclient's .text. Runs against modules that are
// guaranteed to be loaded in the test process (this executable and kernel32).

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "OSTPlatform/include/Memory.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace {

    namespace Memory = OSTPlatform::Memory;

    // noinline + address taken: the function must really exist inside .text.
    __declspec(noinline) int AnchorInText(int x) { return x * 31 + 7; }

} // namespace

TEST(ModuleSection, TextOfTheRunningExecutableContainsItsOwnCode) {
    HMODULE self = GetModuleHandleW(nullptr);
    const auto text = Memory::GetModuleSection(self, ".text");
    ASSERT_TRUE(text.has_value());
    EXPECT_GT(text->size, 0u);

    const auto fn = reinterpret_cast<std::uintptr_t>(&AnchorInText);
    const auto lo = reinterpret_cast<std::uintptr_t>(text->base);
    EXPECT_GE(fn, lo);
    EXPECT_LT(fn, lo + text->size);
    EXPECT_TRUE(Memory::IsReadable(text->base, text->size));
}

TEST(ModuleSection, SectionStartsInsideTheModuleImage) {
    HMODULE self = GetModuleHandleW(nullptr);
    const auto text = Memory::GetModuleSection(self, ".text");
    ASSERT_TRUE(text.has_value());
    EXPECT_GT(reinterpret_cast<std::uintptr_t>(text->base), reinterpret_cast<std::uintptr_t>(self));
}

TEST(ModuleSection, FindsASystemModuleToo) {
    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    ASSERT_NE(kernel32, nullptr);
    const auto text = Memory::GetModuleSection(kernel32, ".text");
    ASSERT_TRUE(text.has_value());
    EXPECT_GT(text->size, 0u);
}

TEST(ModuleSection, DifferentSectionsAreDifferentRanges) {
    HMODULE self = GetModuleHandleW(nullptr);
    const auto text = Memory::GetModuleSection(self, ".text");
    const auto rdata = Memory::GetModuleSection(self, ".rdata");
    ASSERT_TRUE(text.has_value());
    ASSERT_TRUE(rdata.has_value());
    EXPECT_NE(text->base, rdata->base);
}

TEST(ModuleSection, UnknownOrMalformedNamesFindNothing) {
    HMODULE self = GetModuleHandleW(nullptr);
    EXPECT_FALSE(Memory::GetModuleSection(self, ".nosuch").has_value());
    EXPECT_FALSE(Memory::GetModuleSection(self, "").has_value());
    EXPECT_FALSE(Memory::GetModuleSection(self, ".text123456").has_value());   // > 8 characters
    EXPECT_FALSE(Memory::GetModuleSection(self, ".tex").has_value());          // prefix is not a match
}

TEST(ModuleSection, NullModuleFindsNothing) {
    EXPECT_FALSE(Memory::GetModuleSection(nullptr, ".text").has_value());
}

TEST(ModuleSection, ReadableMemoryThatIsNotAPeImageFindsNothing) {
    static std::array<std::uint8_t, 4096> zeros{};   // readable, but no MZ signature
    EXPECT_FALSE(Memory::GetModuleSection(zeros.data(), ".text").has_value());
}

// ── Forged headers ──────────────────────────────────────────────
// GetModuleSection reads headers it does not control, so every bounds check gets a
// case. The image is one committed page followed by a PAGE_NOACCESS page: anything
// that runs past the first page is unreadable, deterministically.

namespace {

    constexpr std::size_t kPage = 4096;
    constexpr std::size_t kNtOffset = 0x80;

    class FakePe {
    public:
        FakePe() {
            page_ = static_cast<std::uint8_t*>(
                VirtualAlloc(nullptr, 2 * kPage, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
            if (!page_) return;
            DWORD old = 0;
            VirtualProtect(page_ + kPage, kPage, PAGE_NOACCESS, &old);
            Reset();
        }
        ~FakePe() { if (page_) VirtualFree(page_, 0, MEM_RELEASE); }
        FakePe(const FakePe&) = delete;
        FakePe& operator=(const FakePe&) = delete;

        bool ok() const { return page_ != nullptr; }
        std::uint8_t* base() const { return page_; }
        IMAGE_DOS_HEADER* dos() const { return reinterpret_cast<IMAGE_DOS_HEADER*>(page_); }
        IMAGE_NT_HEADERS* nt() const { return reinterpret_cast<IMAGE_NT_HEADERS*>(page_ + kNtOffset); }
        IMAGE_SECTION_HEADER* section() const { return IMAGE_FIRST_SECTION(nt()); }

        // A valid one-section image whose ".text" is [0x400, 0x500).
        void Reset() {
            std::memset(page_, 0, kPage);
            dos()->e_magic = IMAGE_DOS_SIGNATURE;
            dos()->e_lfanew = static_cast<LONG>(kNtOffset);
            nt()->Signature = IMAGE_NT_SIGNATURE;
            nt()->FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64;
            nt()->FileHeader.NumberOfSections = 1;
            nt()->FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER);
            nt()->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR_MAGIC;
            nt()->OptionalHeader.SizeOfImage = static_cast<DWORD>(kPage);
            std::memcpy(section()->Name, ".text", 5);
            section()->VirtualAddress = 0x400;
            section()->Misc.VirtualSize = 0x100;
            section()->SizeOfRawData = 0x100;
        }

    private:
        std::uint8_t* page_ = nullptr;
    };

    bool Finds(const FakePe& pe, std::string_view name = ".text") {
        return Memory::GetModuleSection(pe.base(), name).has_value();
    }

} // namespace

TEST(ModuleSectionForged, TheBaselineImageIsAccepted) {
    FakePe pe;
    ASSERT_TRUE(pe.ok());
    const auto text = Memory::GetModuleSection(pe.base(), ".text");
    ASSERT_TRUE(text.has_value());
    EXPECT_EQ(text->base, pe.base() + 0x400);
    EXPECT_EQ(text->size, 0x100u);
}

TEST(ModuleSectionForged, VirtualSizeZeroFallsBackToTheRawSize) {
    FakePe pe;
    ASSERT_TRUE(pe.ok());
    pe.section()->Misc.VirtualSize = 0;
    pe.section()->SizeOfRawData = 0x80;
    const auto text = Memory::GetModuleSection(pe.base(), ".text");
    ASSERT_TRUE(text.has_value());
    EXPECT_EQ(text->size, 0x80u);
}

TEST(ModuleSectionForged, AnEmptySectionFindsNothing) {
    FakePe pe;
    ASSERT_TRUE(pe.ok());
    pe.section()->Misc.VirtualSize = 0;
    pe.section()->SizeOfRawData = 0;
    EXPECT_FALSE(Finds(pe));
}

TEST(ModuleSectionForged, ASectionPastTheEndOfTheImageFindsNothing) {
    FakePe pe;
    ASSERT_TRUE(pe.ok());

    pe.section()->VirtualAddress = 0xF80;                  // 0xF80 + 0x100 > SizeOfImage (0x1000)
    EXPECT_FALSE(Finds(pe));

    pe.Reset();
    pe.section()->VirtualAddress = 0x2000;                 // starts beyond the image
    EXPECT_FALSE(Finds(pe));

    pe.Reset();
    pe.section()->Misc.VirtualSize = 0xFFFFFFF0u;          // would wrap a naive offset + size
    EXPECT_FALSE(Finds(pe));

    pe.Reset();
    pe.section()->VirtualAddress = 0xFFFFFFFFu;
    pe.section()->Misc.VirtualSize = 0xFFFFFFFFu;
    EXPECT_FALSE(Finds(pe));
}

TEST(ModuleSectionForged, ABadNtOffsetFindsNothing) {
    FakePe pe;
    ASSERT_TRUE(pe.ok());

    pe.dos()->e_lfanew = -1;
    EXPECT_FALSE(Finds(pe));

    pe.Reset();
    pe.dos()->e_lfanew = 0;
    EXPECT_FALSE(Finds(pe));

    pe.Reset();
    pe.dos()->e_lfanew = static_cast<LONG>(kPage);         // the NT headers would sit in the NOACCESS page
    EXPECT_FALSE(Finds(pe));

    pe.Reset();
    pe.dos()->e_lfanew = static_cast<LONG>(kPage - 8);     // they would straddle into it
    EXPECT_FALSE(Finds(pe));
}

TEST(ModuleSectionForged, BadSignaturesAndMagicFindNothing) {
    FakePe pe;
    ASSERT_TRUE(pe.ok());

    pe.dos()->e_magic = 0;
    EXPECT_FALSE(Finds(pe));

    pe.Reset();
    pe.nt()->Signature = 0;
    EXPECT_FALSE(Finds(pe));

    pe.Reset();
    pe.nt()->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR32_MAGIC;   // a PE32 layout is not this build's
    EXPECT_FALSE(Finds(pe));
}

TEST(ModuleSectionForged, ASectionTableRunningIntoUnreadableMemoryFindsNothing) {
    FakePe pe;
    ASSERT_TRUE(pe.ok());
    pe.nt()->FileHeader.NumberOfSections = 0xFFFF;         // 2.6 MB of "table"
    EXPECT_FALSE(Finds(pe));
}

TEST(ModuleSectionForged, ASectionBackedByUnreadableMemoryFindsNothing) {
    FakePe pe;
    ASSERT_TRUE(pe.ok());
    pe.nt()->OptionalHeader.SizeOfImage = static_cast<DWORD>(2 * kPage);   // the image "includes" the NOACCESS page
    pe.section()->VirtualAddress = static_cast<DWORD>(kPage);
    pe.section()->Misc.VirtualSize = 0x100;
    EXPECT_FALSE(Finds(pe));
}

TEST(ModuleSectionForged, EightCharacterNamesHaveNoTerminatorAndMatchExactly) {
    FakePe pe;
    ASSERT_TRUE(pe.ok());
    std::memcpy(pe.section()->Name, ".textabc", IMAGE_SIZEOF_SHORT_NAME);   // exactly 8, no NUL
    EXPECT_TRUE(Finds(pe, ".textabc"));
    EXPECT_FALSE(Finds(pe, ".text"));      // a prefix is not a match
    EXPECT_FALSE(Finds(pe, ".textabcd"));  // longer than a section name can be
}
