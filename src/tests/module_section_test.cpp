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
