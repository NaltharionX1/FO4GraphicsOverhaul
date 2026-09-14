#include "PCH.h"

#include "Platform/FogOwner.h"

#include "Platform/EngineMemory.h"

#include <atomic>
#include <cstring>

namespace
{
    struct Site
    {
        std::size_t offset;
        std::uint8_t bytes[8];
        std::size_t prefixLength;
        std::size_t length;
    };

    constexpr Site kSlotA{ 0x56, { 0x48, 0x8B, 0x05 }, 3, 7 };
    constexpr Site kChain98A{ 0x5D, { 0x48, 0x8B, 0x88 }, 3, 7 };
    constexpr Site kChain70A{ 0x64, { 0x48, 0x8B, 0x49 }, 3, 4 };
    constexpr Site kAccessor{ 0x68, { 0xE8 }, 1, 5 };
    constexpr Site kSlotB{ 0x89, { 0x48, 0x8B, 0x05 }, 3, 7 };
    constexpr Site kFlag{ 0x92, { 0x0F, 0x94, 0x05 }, 3, 7 };
    constexpr Site kChain98B{ 0x99, { 0x48, 0x8B, 0x88 }, 3, 7 };
    constexpr Site kChain70B{ 0xA0, { 0x48, 0x8B, 0x41 }, 3, 4 };
    constexpr Site kByte{ 0xA4, { 0x88, 0x50 }, 2, 3 };
    constexpr Site kStoreFirst{ 0xB9, { 0xF3, 0x41, 0x0F, 0x11, 0x40 }, 5, 6 };
    constexpr Site kStoreSecond{ 0xBF, { 0xF3, 0x41, 0x0F, 0x11, 0x48 }, 5, 6 };

    constexpr Site kArgPtrSecond{ 0x11, { 0x49, 0x8D, 0x43, 0xE8 }, 4, 4 };
    constexpr Site kArgSlotSecond{ 0x15, { 0x49, 0x89, 0x43, 0xD8 }, 4, 4 };
    constexpr Site kArgPtrFirst{ 0x19, { 0x49, 0x8D, 0x43, 0xEC }, 4, 4 };
    constexpr Site kArgSlotFirst{ 0x1D, { 0x49, 0x89, 0x43, 0xD0 }, 4, 4 };
    constexpr Site kLoadSecond{ 0xA7, { 0x66, 0x0F, 0x6E, 0x4C, 0x24, 0x50 }, 6, 6 };
    constexpr Site kLoadFirst{ 0xAD, { 0x66, 0x0F, 0x6E, 0x44, 0x24, 0x54 }, 6, 6 };

    using Accessor_t = void* (*)(void*);

    std::uintptr_t g_slot{ 0 };
    Accessor_t g_accessor{ nullptr };
    std::uintptr_t g_flagByte{ 0 };
    std::uint32_t g_chainFirst{ 0 };
    std::uint8_t g_chainSecond{ 0 };
    std::uint8_t g_byteOffset{ 0 };
    std::uint8_t g_firstOffset{ 0 };
    std::uint8_t g_secondOffset{ 0 };
    std::atomic<bool> g_bound{ false };

    [[nodiscard]] bool Verify(const std::uint8_t* a_code, const Site& a_site) noexcept
    {
        return std::memcmp(a_code + a_site.offset, a_site.bytes, a_site.prefixLength) == 0;
    }

    [[nodiscard]] std::uintptr_t RipTarget(const std::uint8_t* a_handler, const Site& a_site) noexcept
    {
        std::int32_t disp = 0;
        std::memcpy(&disp, a_handler + a_site.offset + a_site.length - 4, sizeof(disp));
        return reinterpret_cast<std::uintptr_t>(a_handler) + a_site.offset + a_site.length +
               static_cast<std::intptr_t>(disp);
    }

    [[nodiscard]] std::uint32_t Disp32(const std::uint8_t* a_handler, const Site& a_site) noexcept
    {
        std::uint32_t value = 0;
        std::memcpy(&value, a_handler + a_site.offset + a_site.prefixLength, sizeof(value));
        return value;
    }

    [[nodiscard]] std::uint8_t Disp8(const std::uint8_t* a_handler, const Site& a_site) noexcept
    {
        return a_handler[a_site.offset + a_site.prefixLength];
    }

    [[nodiscard]] void* WalkToFogObject() noexcept
    {
        void* first = nullptr;
        if (!Platform::EngineMemory::SafeRead(&first, reinterpret_cast<const void*>(g_slot),
                sizeof(first)) ||
            first == nullptr) {
            return nullptr;
        }
        void* second = nullptr;
        if (!Platform::EngineMemory::SafeRead(&second,
                reinterpret_cast<const void*>(reinterpret_cast<std::uintptr_t>(first) +
                    g_chainFirst),
                sizeof(second)) ||
            second == nullptr) {
            return nullptr;
        }
        void* object = nullptr;
        if (!Platform::EngineMemory::SafeRead(&object,
                reinterpret_cast<const void*>(reinterpret_cast<std::uintptr_t>(second) +
                    g_chainSecond),
                sizeof(object))) {
            return nullptr;
        }
        return object;
    }
}

namespace Platform::FogOwner
{
    void Bind(std::span<RE::SCRIPT_FUNCTION> a_functions) noexcept
    {
        if (g_bound.load(std::memory_order_acquire)) {
            return;
        }

        const std::uint8_t* handler = nullptr;
        for (const RE::SCRIPT_FUNCTION& fn : a_functions) {
            const bool match = (fn.shortName != nullptr && ::_stricmp(fn.shortName, "setfog") == 0) ||
                               (fn.functionName != nullptr &&
                                   ::_stricmp(fn.functionName, "setfog") == 0);
            if (match) {
                handler = reinterpret_cast<const std::uint8_t*>(fn.executeFunction);
                break;
            }
        }
        if (handler == nullptr) {
            logger::warn("[Fog] 'setfog' is not in the console function table — not owned; the "
                         "console composite stays in use");
            return;
        }

        for (const Site* site : { &kArgPtrSecond, &kArgSlotSecond, &kArgPtrFirst, &kArgSlotFirst,
                 &kSlotA, &kChain98A, &kChain70A, &kAccessor, &kSlotB, &kFlag, &kChain98B,
                 &kChain70B, &kByte, &kLoadSecond, &kLoadFirst, &kStoreFirst, &kStoreSecond }) {
            if (!Verify(handler, *site)) {
                logger::warn("[Fog] handler byte mismatch at +0x{:X} — REFUSED; the console "
                             "composite stays in use (a game update changed this handler)",
                    site->offset);
                return;
            }
        }

        const std::uintptr_t slotA = RipTarget(handler, kSlotA);
        const std::uintptr_t slotB = RipTarget(handler, kSlotB);
        if (slotA != slotB) {
            logger::warn("[Fog] the two slot loads disagree (0x{:X} vs 0x{:X}) — REFUSED",
                slotA, slotB);
            return;
        }

        const std::uint32_t chainFirstA = Disp32(handler, kChain98A);
        const std::uint32_t chainFirstB = Disp32(handler, kChain98B);
        const std::uint8_t chainSecondA = Disp8(handler, kChain70A);
        const std::uint8_t chainSecondB = Disp8(handler, kChain70B);
        if (chainFirstA != chainFirstB || chainSecondA != chainSecondB) {
            logger::warn("[Fog] the two chain walks disagree (+0x{:X}/+0x{:X} vs +0x{:X}/+0x{:X}) "
                         "— REFUSED",
                chainFirstA, chainSecondA, chainFirstB, chainSecondB);
            return;
        }

        const std::uintptr_t accessor = RipTarget(handler, kAccessor);
        const std::uintptr_t flagByte = RipTarget(handler, kFlag);
        for (const std::uintptr_t address : { slotA, accessor, flagByte }) {
            if (!EngineMemory::WithinGameImage(address, sizeof(std::uintptr_t))) {
                logger::warn("[Fog] a derived address (0x{:X}) is outside the game image — "
                             "REFUSED; the console composite stays in use",
                    address);
                return;
            }
        }

        g_slot = slotA;
        g_accessor = reinterpret_cast<Accessor_t>(accessor);
        g_flagByte = flagByte;
        g_chainFirst = chainFirstA;
        g_chainSecond = chainSecondA;
        g_byteOffset = Disp8(handler, kByte);
        g_firstOffset = Disp8(handler, kStoreFirst);
        g_secondOffset = Disp8(handler, kStoreSecond);
        g_bound.store(true, std::memory_order_release);

        logger::info("[Fog] OWNED — slot @0x{:X}, chain +0x{:X}/+0x{:X}, accessor @0x{:X}, "
                     "override flag @0x{:X}, engine-drives byte +0x{:02X}, arg1 +0x{:02X} / arg2 "
                     "+0x{:02X}; direct writes from here on, no console",
            g_slot, g_chainFirst, g_chainSecond, accessor, g_flagByte, g_byteOffset, g_firstOffset,
            g_secondOffset);
    }

    bool Owns() noexcept
    {
        return g_bound.load(std::memory_order_acquire);
    }

    bool Write(int a_first, int a_second) noexcept
    {
        if (!g_bound.load(std::memory_order_acquire)) {
            return false;
        }
        const auto* const tasks = F4SE::GetTaskInterface();
        if (tasks == nullptr) {
            return false;
        }

        tasks->AddTask([a_first, a_second]() noexcept {
            void* object = WalkToFogObject();
            if (object == nullptr) {
                logger::warn("[Fog] the fog object is not up — write dropped");
                return;
            }

            void* const settings = g_accessor(object);
            if (settings == nullptr) {
                logger::warn("[Fog] the engine did not hand back a fog settings block — write "
                             "dropped");
                return;
            }

            const bool engineDrives = (a_first == 0 && a_second == 0);
            const std::uint8_t objectByte = engineDrives ? 1U : 0U;
            const std::uint8_t overrideFlag = engineDrives ? 0U : 1U;

            const bool flagOk = Platform::EngineMemory::SafeWrite(
                reinterpret_cast<void*>(g_flagByte), &overrideFlag, sizeof(overrideFlag));

            void* const objectAgain = WalkToFogObject();
            bool byteOk = false;
            if (objectAgain != nullptr) {
                byteOk = Platform::EngineMemory::SafeWrite(
                    reinterpret_cast<void*>(
                        reinterpret_cast<std::uintptr_t>(objectAgain) + g_byteOffset),
                    &objectByte, sizeof(objectByte));
            } else {
                logger::warn("[Fog] the fog object vanished between the accessor call and the "
                             "mode write — the mode byte was NOT updated");
            }

            const auto firstValue = static_cast<float>(a_first);
            const auto secondValue = static_cast<float>(a_second);
            const auto base = reinterpret_cast<std::uintptr_t>(settings);
            const bool firstOk = Platform::EngineMemory::SafeWrite(
                reinterpret_cast<void*>(base + g_firstOffset), &firstValue, sizeof(firstValue));
            const bool secondOk = Platform::EngineMemory::SafeWrite(
                reinterpret_cast<void*>(base + g_secondOffset), &secondValue, sizeof(secondValue));
            const char* const meaning =
                engineDrives ? "engine drives fog per weather"
                             : (a_first == a_second ? "fog off (equal pair collapses the range)"
                                                    : "custom distance");
            if (!firstOk || !secondOk || !flagOk || !byteOk) {
                logger::warn("[Fog] setfog {} {} — PARTIALLY APPLIED (distances {}, override flag "
                             "{}, mode byte {}). The picture and the menu may now disagree.",
                    a_first, a_second, (firstOk && secondOk) ? "ok" : "FAILED",
                    flagOk ? "ok" : "FAILED", byteOk ? "ok" : "FAILED");
                return;
            }

            logger::info("[Fog] setfog {} {} — {} — direct write, no console", a_first, a_second,
                meaning);
        });
        return true;
    }
}
