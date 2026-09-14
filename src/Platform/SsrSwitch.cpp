#include "PCH.h"

#include "Platform/SsrSwitch.h"

#include "Platform/EngineMemory.h"

#include "RE/Bethesda/Settings.h"

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

        std::size_t immediateBytes;
    };

    constexpr Site kIntentRead{ 0x04, { 0x80, 0x3D }, 2, 7, 1 };
    constexpr Site kIntentStore{ 0x15, { 0x88, 0x15 }, 2, 6, 0 };
    constexpr Site kLatchRead{ 0x0E, { 0x80, 0x3D }, 2, 7, 1 };
    constexpr Site kLatchStore{ 0x28, { 0xC6, 0x05 }, 2, 7, 1 };
    constexpr Site kSlotLoad{ 0x3B, { 0x48, 0x8B, 0x05 }, 3, 7, 0 };
    constexpr Site kDeref18{ 0x42, { 0x48, 0x8B, 0x48, 0x18 }, 4, 4, 0 };
    constexpr Site kDeref238{ 0x46, { 0x48, 0x8B, 0x81, 0x38, 0x02, 0x00, 0x00 }, 7, 7, 0 };
    constexpr Site kMirrorStore{ 0x52, { 0x88, 0x90, 0x20, 0x01, 0x00, 0x00 }, 6, 6, 0 };
    constexpr Site kDemandRead{ 0x61, { 0x80, 0xB8, 0x21, 0x01, 0x00, 0x00 }, 6, 7, 1 };
    constexpr Site kGateStore{ 0x73, { 0x88, 0x48, 0x08 }, 3, 3, 0 };
    constexpr Site kRefreshCall{ 0x76, { 0xE8 }, 1, 5, 0 };

    constexpr std::ptrdiff_t kEffectOffset = 0x238;
    constexpr std::ptrdiff_t kMirrorOffset = 0x120;
    constexpr std::ptrdiff_t kDemandOffset = 0x121;
    constexpr std::ptrdiff_t kGateOffset = 0x08;

    using Refresh_t = void (*)();

    std::uint8_t* g_intent{ nullptr };
    std::uint8_t* g_latch{ nullptr };
    std::uintptr_t g_slot{ 0 };
    Refresh_t g_refresh{ nullptr };
    std::atomic<bool> g_bound{ false };

    [[nodiscard]] bool Verify(const std::uint8_t* a_code, const Site& a_site) noexcept
    {
        return std::memcmp(a_code + a_site.offset, a_site.bytes, a_site.prefixLength) == 0;
    }

    [[nodiscard]] std::uintptr_t Target(const std::uint8_t* a_code, const Site& a_site) noexcept
    {
        std::int32_t disp = 0;
        std::memcpy(&disp, a_code + a_site.offset + a_site.length - 4 - a_site.immediateBytes,
            sizeof(disp));
        return reinterpret_cast<std::uintptr_t>(a_code) + a_site.offset + a_site.length +
               static_cast<std::intptr_t>(disp);
    }

    [[nodiscard]] std::uint8_t* ReadPointer(std::uintptr_t a_at) noexcept
    {
        std::uint8_t* value = nullptr;
        if (!Platform::EngineMemory::SafeRead(
                &value, reinterpret_cast<const void*>(a_at), sizeof(value))) {
            return nullptr;
        }
        return value;
    }

    [[nodiscard]] std::uint8_t* EffectObject() noexcept
    {
        if (g_slot == 0) {
            return nullptr;
        }
        std::uint8_t* const singleton = ReadPointer(g_slot);
        if (singleton == nullptr) {
            return nullptr;
        }
        std::uint8_t* const inner = ReadPointer(reinterpret_cast<std::uintptr_t>(singleton) + 0x18);
        if (inner == nullptr) {
            return nullptr;
        }
        return ReadPointer(reinterpret_cast<std::uintptr_t>(inner) + kEffectOffset);
    }

    [[nodiscard]] bool ReadByte(const void* a_at, std::uint8_t& a_out) noexcept
    {
        return Platform::EngineMemory::SafeRead(&a_out, a_at, sizeof(a_out));
    }

    [[nodiscard]] bool WriteByte(void* a_at, std::uint8_t a_value) noexcept
    {
        return Platform::EngineMemory::SafeWrite(a_at, &a_value, sizeof(a_value));
    }
}

namespace Platform::SsrSwitch
{
    void Bind(std::span<RE::SCRIPT_FUNCTION> a_functions) noexcept
    {
        if (g_bound.load(std::memory_order_acquire)) {
            return;
        }

        const std::uint8_t* handler = nullptr;
        for (const RE::SCRIPT_FUNCTION& fn : a_functions) {
            if (fn.shortName != nullptr && ::_stricmp(fn.shortName, "ssr") == 0) {
                handler = reinterpret_cast<const std::uint8_t*>(fn.executeFunction);
                break;
            }
        }
        if (handler == nullptr) {
            logger::warn("[Ssr] 'ssr' is not in the console function table — not owned");
            return;
        }

        for (const Site* site : { &kIntentRead, &kIntentStore, &kLatchRead, &kLatchStore,
                 &kSlotLoad, &kDeref18, &kDeref238, &kMirrorStore, &kDemandRead, &kGateStore,
                 &kRefreshCall }) {
            if (!Verify(handler, *site)) {
                logger::warn("[Ssr] handler byte mismatch at +0x{:X} — REFUSED; the console "
                             "command stays reachable and the mod keeps using it",
                    site->offset);
                return;
            }
        }

        const std::uintptr_t intentRead = Target(handler, kIntentRead);
        const std::uintptr_t intentWrite = Target(handler, kIntentStore);
        const std::uintptr_t latchRead = Target(handler, kLatchRead);
        const std::uintptr_t latchWrite = Target(handler, kLatchStore);
        if (intentRead != intentWrite || latchRead != latchWrite) {
            logger::warn("[Ssr] a read/write pair disagrees (intent 0x{:X} vs 0x{:X}, latch "
                         "0x{:X} vs 0x{:X}) — REFUSED",
                intentRead, intentWrite, latchRead, latchWrite);
            return;
        }

        const std::uintptr_t slot = Target(handler, kSlotLoad);
        const std::uintptr_t refresh = Target(handler, kRefreshCall);
        for (const std::uintptr_t address : { intentWrite, latchWrite, slot, refresh }) {
            if (!EngineMemory::WithinGameImage(address, sizeof(std::uintptr_t))) {
                logger::warn("[Ssr] a derived address (0x{:X}) is outside the game image — "
                             "REFUSED; the console command stays reachable",
                    address);
                return;
            }
        }

        g_intent = reinterpret_cast<std::uint8_t*>(intentWrite);
        g_latch = reinterpret_cast<std::uint8_t*>(latchWrite);
        g_slot = slot;
        g_refresh = reinterpret_cast<Refresh_t>(refresh);
        g_bound.store(true, std::memory_order_release);

        std::uint8_t intentNow = 0;
        (void)ReadByte(g_intent, intentNow);
        logger::info("[Ssr] OWNED — intent byte @0x{:X} (currently {}), Setting latch @0x{:X}, "
                     "effect chain +0x{:X}, refresh derived; direct writes from here on, no "
                     "console",
            reinterpret_cast<std::uintptr_t>(g_intent), intentNow != 0 ? "on" : "off",
            reinterpret_cast<std::uintptr_t>(g_latch),
            static_cast<unsigned>(kEffectOffset));
    }

    bool Owns() noexcept
    {
        return g_bound.load(std::memory_order_acquire);
    }

    bool IntentEnabled() noexcept
    {
        if (!g_bound.load(std::memory_order_acquire) || g_intent == nullptr) {
            return false;
        }
        std::uint8_t value = 0;
        return ReadByte(g_intent, value) && value != 0;
    }

    bool EngineDemand() noexcept
    {
        if (!g_bound.load(std::memory_order_acquire)) {
            return false;
        }
        std::uint8_t* const effect = EffectObject();
        std::uint8_t value = 0;
        return effect != nullptr && ReadByte(effect + kDemandOffset, value) && value != 0;
    }

    bool Write(bool a_on) noexcept
    {
        if (!g_bound.load(std::memory_order_acquire)) {
            return false;
        }
        const auto* const tasks = F4SE::GetTaskInterface();
        if (tasks == nullptr) {
            return false;
        }

        tasks->AddTask([a_on]() noexcept {
            const auto want = static_cast<std::uint8_t>(a_on ? 1 : 0);

            if (!WriteByte(g_intent, want)) {
                logger::error("[Ssr] the intent byte could not be written — SSR did not change");
                return;
            }

            std::uint8_t latch = 0;
            if (a_on && ReadByte(g_latch, latch) && latch == 0 && WriteByte(g_latch, 1)) {
                logger::info("[Ssr] bScreenSpaceReflections:LightingShader latched on (one-way, "
                             "as the engine's own handler does)");
            }

            std::uint8_t* const effect = EffectObject();
            if (effect == nullptr) {
                logger::warn("[Ssr] the effect chain is not up — intent stored, the render side "
                             "and the engine refresh are skipped this time");
                return;
            }

            std::uint8_t demand = 0;
            (void)ReadByte(effect + kDemandOffset, demand);
            const auto gate = static_cast<std::uint8_t>((want | demand) != 0 ? 1 : 0);
            const bool mirrored = WriteByte(effect + kMirrorOffset, want);
            const bool gated = WriteByte(effect + kGateOffset, gate);
            if (!mirrored || !gated) {
                logger::error("[Ssr] the render-side write failed — reflections may not match the "
                              "checkbox until the next toggle");
                return;
            }
            logger::info("[Ssr] intent {} | engine demand {} -> gate {} — direct writes, no "
                         "console",
                want != 0 ? "on" : "off", demand != 0 ? "on" : "off", gate != 0 ? "on" : "off");

            g_refresh();
        });
        return true;
    }
}
