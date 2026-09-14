#include "PCH.h"

#include "Platform/AoRenderSwitch.h"

#include "Platform/EngineMemory.h"

#include <atomic>
#include <cstring>

namespace
{
    constexpr std::size_t kMovRax = 0xA5;
    constexpr std::uint8_t kMovRaxOp[]{ 0x48, 0x8B, 0x05 };
    constexpr std::size_t kDeref1 = 0xB4;
    constexpr std::uint8_t kDeref1Op[]{ 0x48, 0x8B, 0x48, 0x18 };
    constexpr std::size_t kDeref2 = 0xB8;
    constexpr std::uint8_t kDeref2Op[]{ 0x48, 0x8B, 0x99, 0x28, 0x02, 0x00, 0x00 };
    constexpr std::size_t kStoreOn = 0xFC;
    constexpr std::uint8_t kStoreOnOp[]{ 0xC6, 0x43, 0x08, 0x01 };

    constexpr std::ptrdiff_t kFirst = 0x18;
    constexpr std::ptrdiff_t kSecond = 0x228;
    constexpr std::ptrdiff_t kField = 0x8;

    constexpr std::size_t kWindow = 0x100;

    std::atomic<std::uintptr_t> g_slot{ 0 };

    std::atomic<std::uint64_t> g_writeFailures{ 0 };

    [[nodiscard]] bool ShouldLogFailure() noexcept
    {
        const auto n = g_writeFailures.fetch_add(1, std::memory_order_relaxed) + 1U;
        return n <= 3U || (n % 1000U) == 0U;
    }

    [[nodiscard]] bool Walk(
        std::uintptr_t a_slot, std::uintptr_t& a_byteAddress, std::uint8_t& a_value) noexcept
    {
        std::uintptr_t singleton = 0;
        std::uintptr_t first = 0;
        std::uintptr_t second = 0;
        std::uint8_t value = 0;

        const bool ok =
            Platform::EngineMemory::SafeRead(
                &singleton, reinterpret_cast<const void*>(a_slot), sizeof(singleton)) &&
            singleton != 0 &&
            Platform::EngineMemory::SafeRead(
                &first, reinterpret_cast<const void*>(singleton + kFirst), sizeof(first)) &&
            first != 0 &&
            Platform::EngineMemory::SafeRead(
                &second, reinterpret_cast<const void*>(first + kSecond), sizeof(second)) &&
            second != 0 &&
            Platform::EngineMemory::SafeRead(
                &value, reinterpret_cast<const void*>(second + kField), sizeof(value));

        if (!ok) {
            return false;
        }
        a_byteAddress = second + kField;
        a_value = value;
        return true;
    }

    [[nodiscard]] bool SiteMatches(const std::uint8_t* a_code, std::size_t a_offset,
        const std::uint8_t* a_expected, std::size_t a_length) noexcept
    {
        return std::memcmp(a_code + a_offset, a_expected, a_length) == 0;
    }
}

namespace Platform::AoRenderSwitch
{
    void BindFromHandler(const void* a_handler) noexcept
    {
        if (g_slot.load(std::memory_order_relaxed) != 0) {
            return;
        }
        if (a_handler == nullptr) {
            logger::warn("[AoSwitch] bind asked with a null handler — switch #2 stays unbound");
            return;
        }

        std::uint8_t code[kWindow]{};
        if (!EngineMemory::SafeRead(code, a_handler, sizeof(code))) {
            logger::warn("[AoSwitch] could not read the ToggleAmbientOcclusion handler — switch #2 "
                         "stays unbound");
            return;
        }

        struct Site
        {
            std::size_t offset;
            const std::uint8_t* expected;
            std::size_t length;
            const char* what;
        };
        const Site sites[]{
            { kMovRax, kMovRaxOp, sizeof(kMovRaxOp), "mov rax,[rip+disp32] (singleton slot)" },
            { kDeref1, kDeref1Op, sizeof(kDeref1Op), "mov rcx,[rax+0x18]" },
            { kDeref2, kDeref2Op, sizeof(kDeref2Op), "mov rbx,[rcx+0x228]" },
            { kStoreOn, kStoreOnOp, sizeof(kStoreOnOp), "mov byte ptr [rbx+8],1 (the store)" },
        };
        for (const Site& site : sites) {
            if (!SiteMatches(code, site.offset, site.expected, site.length)) {
                logger::warn("[AoSwitch] handler byte mismatch at +{:#x} ({}) — the handler is not "
                             "the one the chain was derived from. REFUSING to bind switch #2.",
                    site.offset, site.what);
                return;
            }
        }

        std::int32_t disp = 0;
        std::memcpy(&disp, &code[kMovRax + 3], sizeof(disp));
        const auto handler = reinterpret_cast<std::uintptr_t>(a_handler);
        const auto slot = static_cast<std::uintptr_t>(
            static_cast<std::intptr_t>(handler + kMovRax + 7) + disp);

        if (!EngineMemory::WithinGameImage(slot, sizeof(std::uintptr_t))) {
            logger::warn("[AoSwitch] derived slot {:#x} is outside the game image — REFUSING to "
                         "bind switch #2",
                slot);
            return;
        }

        g_slot.store(slot, std::memory_order_release);
        logger::info("[AoSwitch] ★ SWITCH #2 (AO RENDER) bound — singleton slot @{:#x}, derived "
                     "from ToggleAmbientOcclusion's own bytes, all four sites opcode-verified. The "
                     "mod's AO toggle drives this byte directly, and intent is re-asserted after every "
                     "scene load.",
            slot);
    }

    bool Bound() noexcept
    {
        return g_slot.load(std::memory_order_acquire) != 0;
    }

    bool Write(bool a_render) noexcept
    {
        const std::uintptr_t slot = g_slot.load(std::memory_order_acquire);
        if (slot == 0) {
            if (ShouldLogFailure()) {
                logger::warn("[AoSwitch] write requested but switch #2 never bound — see the bind "
                             "refusal earlier in this log. The console `ao` command was left "
                             "reachable as the manual fallback.");
            }
            return false;
        }

        std::uintptr_t address = 0;
        std::uint8_t value = 0;
        if (!Walk(slot, address, value)) {
            if (ShouldLogFailure()) {
                logger::warn("[AoSwitch] the chain did not resolve — the mailbox retries next "
                             "frame (failure #{})",
                    g_writeFailures.load(std::memory_order_relaxed));
            }
            return false;
        }

        const std::uint8_t want = a_render ? 1U : 0U;
        if (value != want) {
            if (!Platform::EngineMemory::SafeWrite(
                    reinterpret_cast<void*>(address), &want, sizeof(want))) {
                if (ShouldLogFailure()) {
                    logger::warn("[AoSwitch] write FAILED @{:#x} — the mailbox retries next frame",
                        address);
                }
                return false;
            }
            logger::info("[AoSwitch] wrote {} @{:#x} (was {}) — direct drive, no console", want,
                address, value);
        }
        return true;
    }

}
