#include "PCH.h"

#include "Platform/ImagespaceOverride.h"

#include "Platform/EngineMemory.h"

#include <atomic>
#include <cstring>

namespace
{
    constexpr std::size_t kMaxArgs = 9;

    struct Handler
    {
        const char* command;
        std::size_t slotOffset;
        std::size_t callOffset;
        std::size_t storeOffset;
        std::uint8_t storeBytes[8];
        std::size_t storeLength;
        std::size_t argCount;
        std::ptrdiff_t fields[kMaxArgs];
    };

    constexpr Handler kHandlers[]{
        { "scp", 0x6C, 0x73, 0x78, { 0xF3, 0x0F, 0x10, 0x44, 0x24, 0x50 }, 6, 3,
            { 0x24, 0x28, 0x2C } },
        { "stp", 0x91, 0x98, 0x9D, { 0xF3, 0x0F, 0x10, 0x44, 0x24, 0x60 }, 6, 4,
            { 0x34, 0x38, 0x3C, 0x30 } },
        { "shp", 0xC6, 0xCD, 0xD2, { 0xF3, 0x0F, 0x10, 0x45, 0x0B }, 5, 9,
            { 0x00, 0x20, 0x04, 0x08, 0x0C, 0x10, 0x14, 0x18, 0x1C } },
    };
    constexpr std::size_t kHandlerCount = std::size(kHandlers);

    using Accessor_t = void* (*)(void*);

    std::uintptr_t g_slot{ 0 };
    Accessor_t g_accessor{ nullptr };
    bool g_bound[kHandlerCount]{};
    std::atomic<bool> g_anyBound{ false };

    [[nodiscard]] std::ptrdiff_t IndexOf(const char* a_command) noexcept
    {
        if (a_command == nullptr) {
            return -1;
        }
        for (std::size_t i = 0; i < kHandlerCount; ++i) {
            if (::_stricmp(a_command, kHandlers[i].command) == 0) {
                return static_cast<std::ptrdiff_t>(i);
            }
        }
        return -1;
    }

    [[nodiscard]] std::uintptr_t RipTarget(const std::uint8_t* a_handler, std::size_t a_offset,
        std::size_t a_length) noexcept
    {
        std::int32_t disp = 0;
        std::memcpy(&disp, a_handler + a_offset + a_length - 4, sizeof(disp));
        return reinterpret_cast<std::uintptr_t>(a_handler) + a_offset + a_length +
               static_cast<std::intptr_t>(disp);
    }
}

namespace Platform::ImagespaceOverride
{
    void Bind(std::span<RE::SCRIPT_FUNCTION> a_functions) noexcept
    {
        constexpr std::uint8_t kSlotOpcode[]{ 0x48, 0x8B, 0x0D };

        for (std::size_t i = 0; i < kHandlerCount; ++i) {
            if (g_bound[i]) {
                continue;
            }
            const Handler& want = kHandlers[i];

            const std::uint8_t* handler = nullptr;
            for (const RE::SCRIPT_FUNCTION& fn : a_functions) {
                if (fn.shortName != nullptr && ::_stricmp(fn.shortName, want.command) == 0) {
                    handler = reinterpret_cast<const std::uint8_t*>(fn.executeFunction);
                    break;
                }
            }
            if (handler == nullptr) {
                logger::warn("[Imagespace] '{}' is not in the console function table — not owned",
                    want.command);
                continue;
            }

            if (std::memcmp(handler + want.slotOffset, kSlotOpcode, sizeof(kSlotOpcode)) != 0 ||
                handler[want.callOffset] != 0xE8 ||
                std::memcmp(handler + want.storeOffset, want.storeBytes, want.storeLength) != 0) {
                logger::warn("[Imagespace] '{}' handler bytes mismatch — REFUSED; its console "
                             "command stays reachable and the mod keeps using it",
                    want.command);
                continue;
            }

            const std::uintptr_t slot = RipTarget(handler, want.slotOffset, 7);
            const auto accessor =
                reinterpret_cast<Accessor_t>(RipTarget(handler, want.callOffset, 5));

            if (g_anyBound.load(std::memory_order_acquire) &&
                (slot != g_slot || accessor != g_accessor)) {
                logger::warn("[Imagespace] '{}' derives a different slot/accessor than its "
                             "siblings (slot 0x{:X} vs 0x{:X}) — REFUSED",
                    want.command, slot, g_slot);
                continue;
            }

            if (!EngineMemory::WithinGameImage(slot, sizeof(std::uintptr_t)) ||
                !EngineMemory::WithinGameImage(
                    reinterpret_cast<std::uintptr_t>(accessor), sizeof(std::uintptr_t))) {
                logger::warn("[Imagespace] '{}' derives an address outside the game image — "
                             "REFUSED; its console command stays reachable",
                    want.command);
                continue;
            }

            g_slot = slot;
            g_accessor = accessor;
            g_bound[i] = true;
            g_anyBound.store(true, std::memory_order_release);
            logger::info("[Imagespace] {:<4} OWNED — slot/accessor derived from its own bytes; "
                         "{} field(s) write directly, no console",
                want.command, want.argCount);
        }
    }

    bool Owns(const char* a_command) noexcept
    {
        const std::ptrdiff_t index = IndexOf(a_command);
        return index >= 0 && g_bound[static_cast<std::size_t>(index)];
    }

}
