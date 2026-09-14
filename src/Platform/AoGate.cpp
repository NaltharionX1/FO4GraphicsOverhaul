#include "PCH.h"

#include "Platform/AoGate.h"

#include "Platform/EngineMemory.h"

#include <atomic>

namespace
{
    constexpr std::size_t kCmpOffset = 0x193;
    constexpr std::size_t kCmpLength = 7;
    constexpr std::uint8_t kCmpOpcode0 = 0x80;
    constexpr std::uint8_t kCmpOpcode1 = 0x3D;
    constexpr std::uint8_t kJeOpcode = 0x74;
    constexpr std::uint8_t kJeDisplacement = 0x23;

    std::uintptr_t g_address{ 0 };

    std::atomic<bool> g_sessionHold{ false };

    std::atomic<bool> g_bound{ false };
    std::atomic<bool> g_everEnabled{ false };
    std::atomic<bool> g_refusedRaise{ false };
    std::atomic<std::uint64_t> g_corrections{ 0 };

    std::atomic<bool> g_failed{ false };

    [[nodiscard]] bool ReadByte(bool& a_out) noexcept
    {
        if (g_address == 0) {
            return false;
        }
        std::uint8_t raw = 0;
        if (!Platform::EngineMemory::SafeRead(&raw, reinterpret_cast<const void*>(g_address),
                sizeof(raw))) {
            return false;
        }
        a_out = raw != 0;
        return true;
    }

    std::atomic<bool> g_producersRetired{ false };
    std::atomic<bool> g_retireAttempted{ false };

    using ImageSpaceIsActiveFn = bool (*)(void*);
    [[maybe_unused]] ImageSpaceIsActiveFn g_saoIsActiveOriginal{ nullptr };
    [[maybe_unused]] ImageSpaceIsActiveFn g_saoCsIsActiveOriginal{ nullptr };

    bool SaoIsActiveThunk(void* a_self)
    {
        (void)a_self;
        static std::atomic<bool> s_logged{ false };
        if (!s_logged.exchange(true)) {
            logger::info("[AoGate] vanilla SSAO vetoed at the root (ScalableAmbientObscurance "
                         "IsActive->false, unconditional) — GTAO or nothing");
        }
        return false;
    }
}

namespace Platform::AoGate
{
    void Bind() noexcept
    {
        if (g_bound.load(std::memory_order_acquire) || g_failed.load(std::memory_order_relaxed)) {
            return;
        }

        std::uintptr_t base = 0;
        try {
            base = REL::Relocation<std::uintptr_t>{ REL::ID(984743) }.address();
        } catch (...) {
            base = 0;
        }
        if (base == 0) {
            g_failed.store(true, std::memory_order_relaxed);
            logger::warn("[AoGate] the AO dispatcher did not resolve — HBAO gate unavailable.");
            return;
        }

        std::uint8_t code[kCmpLength + 2]{};
        if (!Platform::EngineMemory::SafeRead(code, reinterpret_cast<const void*>(base + kCmpOffset),
                sizeof(code))) {
            g_failed.store(true, std::memory_order_relaxed);
            logger::warn("[AoGate] could not read the AO branch — HBAO gate unavailable.");
            return;
        }
        if (code[0] != kCmpOpcode0 || code[1] != kCmpOpcode1 || code[6] != 0x00 ||
            code[7] != kJeOpcode || code[8] != kJeDisplacement) {
            g_failed.store(true, std::memory_order_relaxed);
            logger::warn(
                "[AoGate] the instructions at REL(984743)+{:#x} are not the expected "
                "`cmp byte ptr [rip+d],0 / je +0x23` — got {:02x} {:02x} ... {:02x} {:02x}. This is "
                "not the build this was derived against, so the gate is disabled rather than "
                "guessed. AO falls back to the console-command route.",
                kCmpOffset, code[0], code[1], code[7], code[8]);
            return;
        }

        std::int32_t displacement = 0;
        std::memcpy(&displacement, code + 2, sizeof(displacement));
        const std::uintptr_t address =
            base + kCmpOffset + kCmpLength + static_cast<std::intptr_t>(displacement);

        if (!Platform::EngineMemory::WithinGameImage(address, sizeof(std::uint8_t))) {
            g_failed.store(true, std::memory_order_relaxed);
            logger::warn(
                "[AoGate] the derived gate address {:#x} is outside the game image — disabled.",
                address);
            return;
        }

        g_address = address;
        bool live = false;
        if (!ReadByte(live)) {
            g_address = 0;
            g_failed.store(true, std::memory_order_relaxed);
            logger::warn("[AoGate] the gate address resolved but could not be read — disabled.");
            return;
        }

        if (live) {
            g_everEnabled.store(true, std::memory_order_relaxed);
        }
        g_bound.store(true, std::memory_order_release);

        logger::info(
            "[AoGate] bound @ {:#x} (derived from the `cmp` operand at REL(984743)+{:#x}, opcode "
            "verified) — this is the byte the engine tests to decide whether the whole HBAO block "
            "runs. Engine currently holds it {}. This is the first VERIFIED read of AO "
            "implementation state this project has had; everything before it was bookkeeping.",
            g_address, kCmpOffset, live ? "UP (HBAO drawing)" : "down (HBAO not drawing)");
    }

    bool EverObservedEnabled() noexcept
    {
        return g_everEnabled.load(std::memory_order_relaxed);
    }

    void SetSessionHold(bool a_hold) noexcept
    {
        if (g_sessionHold.exchange(a_hold, std::memory_order_relaxed) != a_hold) {
            logger::info(
                "[AoGate] session hold {} — switch #1 is {} a constant: held UP so the block "
                "exists and the AO term stays bound. The on/off that users see is switch #2 "
                "(AoRenderSwitch), a different address entirely.",
                a_hold ? "ENABLED" : "disabled", a_hold ? "now" : "no longer");
        }
    }

    void EnforceFrame() noexcept
    {
        if (!g_bound.load(std::memory_order_acquire) || g_failed.load(std::memory_order_relaxed) ||
            !g_sessionHold.load(std::memory_order_relaxed)) {
            return;
        }

        bool live = false;
        if (!ReadByte(live)) {
            g_failed.store(true, std::memory_order_relaxed);
            logger::warn("[AoGate] read failed at the seam — enforcement disabled for the session.");
            return;
        }
        if (live) {
            g_everEnabled.store(true, std::memory_order_relaxed);
            return;
        }

        if (!g_everEnabled.load(std::memory_order_relaxed)) {
            if (!g_refusedRaise.exchange(true, std::memory_order_relaxed)) {
                logger::warn(
                    "[AoGate] the session hold found the gate DOWN and the engine has never held "
                    "it up this session — the capability probe said no, or the NVHBAO init never "
                    "ran. Refusing to assert an unprimed state; ambient occlusion will not draw "
                    "on this hardware.");
            }
            return;
        }

        const std::uint8_t raw = 1U;
        if (!Platform::EngineMemory::SafeWrite(reinterpret_cast<void*>(g_address), &raw,
                sizeof(raw))) {
            g_failed.store(true, std::memory_order_relaxed);
            logger::warn("[AoGate] write failed at the seam — enforcement disabled for the session.");
            return;
        }
        g_refusedRaise.store(false, std::memory_order_relaxed);

        const auto n = g_corrections.fetch_add(1, std::memory_order_relaxed) + 1U;
        if (n <= 3U || (n % 1000U) == 0U) {
            logger::warn("[AoGate] ★ gate RESTORED UP (correction #{}) — something lowered switch "
                         "#1 mid-session. The hold put back the engine-primed state; if this line "
                         "recurs, something is fighting us and that is the finding.",
                n);
        }
    }

    bool EngineProducersRetired() noexcept
    {
        return g_producersRetired.load(std::memory_order_relaxed);
    }

    void RetireEngineProducers() noexcept
    {
        if (g_retireAttempted.exchange(true, std::memory_order_relaxed)) {
            return;
        }

        g_producersRetired.store(true, std::memory_order_relaxed);
        logger::info(
            "[AoGate] ★ ENGINE AO DRAW RETIRED — HbaoHook (the dyn-res hook that owns the call "
            "at REL(984743)+0x1BA) now refuses to forward to the vanilla HBAO/SSAO compute "
            "dispatch, for the rest of the session. The block's setup keeps binding the AO "
            "term; GTAO is the only producer, and no INI/console/menu value can change what "
            "our own thunk does.");

        try {
            g_saoIsActiveOriginal = reinterpret_cast<ImageSpaceIsActiveFn>(
                REL::Relocation<std::uintptr_t>{
                    RE::VTABLE::ImageSpaceEffectScalableAmbientObscurance[0] }
                    .write_vfunc(0x8, SaoIsActiveThunk));
            g_saoCsIsActiveOriginal = reinterpret_cast<ImageSpaceIsActiveFn>(
                REL::Relocation<std::uintptr_t>{
                    RE::VTABLE::ImageSpaceEffectScalableAmbientObscuranceCS[0] }
                    .write_vfunc(0x8, SaoIsActiveThunk));
            logger::info("[AoGate] SSAO IsActive veto installed on both vtables "
                         "(ScalableAmbientObscurance + CS) — unconditional false, the TAA shape.");
        } catch (const std::exception& e) {
            logger::warn("[AoGate] SSAO IsActive veto failed: {} — the byte patch above (if "
                         "applied) still covers the deferred dispatch.",
                e.what());
        } catch (...) {
            logger::warn("[AoGate] SSAO IsActive veto failed (unknown exception).");
        }
    }

}
