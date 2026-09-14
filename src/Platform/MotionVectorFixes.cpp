// SPDX-License-Identifier: GPL-3.0-or-later
// Portions ported from Motion Vector Fixes (fo4test) by doodlum, GPL-3.0-or-later with its modding exception.

#include "PCH.h"

#include "Platform/MotionVectorFixes.h"

#include "Platform/Reflex.h"

#include <Detours.h>

#include <psapi.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <unordered_map>

namespace
{
    std::atomic<bool> g_installAttempted{ false };
    std::atomic<bool> g_standingDown{ false };
    std::atomic<bool> g_loadingMenuOpen{ false };
    std::atomic<bool> g_sinkActive{ false };
    std::atomic<bool> g_fixWeaponActive{ false };
    std::atomic<bool> g_fixAnimatedActive{ false };
    std::atomic<bool> g_fixCollapseActive{ false };
    char g_animatedReason[96]{};

    [[nodiscard]] bool IsExecutableAddress(std::uintptr_t a_addr) noexcept
    {
        if (a_addr == 0) {
            return false;
        }
        MEMORY_BASIC_INFORMATION mbi{};
        if (::VirtualQuery(reinterpret_cast<const void*>(a_addr), &mbi, sizeof(mbi)) != sizeof(mbi)) {
            return false;
        }
        constexpr DWORD kExec = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        return mbi.State == MEM_COMMIT && (mbi.Protect & kExec) != 0;
    }

    [[nodiscard]] bool InsideGameModule(std::uintptr_t a_addr) noexcept
    {
        HMODULE game = ::GetModuleHandleW(nullptr);
        if (game == nullptr) {
            return false;
        }
        MODULEINFO info{};
        if (!::GetModuleInformation(::GetCurrentProcess(), game, &info, sizeof(info))) {
            return false;
        }
        const auto base = reinterpret_cast<std::uintptr_t>(info.lpBaseOfDll);
        return a_addr >= base && a_addr < base + info.SizeOfImage;
    }

    constexpr std::uint64_t kFlagMultiTextureLandscape = 1ULL << 14;
    constexpr std::uint64_t kFlagLODLandscape = 1ULL << 33;
    constexpr std::uint64_t kFlagLODObjects = 1ULL << 34;
    constexpr std::uint64_t kFlagLODLandBlend = 1ULL << 46;
    constexpr std::uint64_t kLodFlagsMask =
        kFlagMultiTextureLandscape | kFlagLODLandscape | kFlagLODObjects | kFlagLODLandBlend;

    constexpr std::uint32_t kMainGameActive = 1U << 0;
    constexpr std::uint32_t kMainInMenuMode = 1U << 1;
    constexpr std::uint32_t kMainFreezeTime = 1U << 2;
    constexpr std::uint32_t kMainPublished = 1U << 31;
    std::atomic<std::uint32_t> g_mainState{ 0 };

    [[nodiscard]] std::uint32_t MainStateNow() noexcept
    {
        const std::uint32_t published = g_mainState.load(std::memory_order_relaxed);
        if ((published & kMainPublished) != 0) {
            return published;
        }
        const auto* main = RE::Main::GetSingleton();
        if (main == nullptr) {
            return 0;
        }
        return (main->gameActive ? kMainGameActive : 0U) | (main->inMenuMode ? kMainInMenuMode : 0U) |
               (main->freezeTime ? kMainFreezeTime : 0U);
    }

    class LoadingMenuSink final : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
    {
    public:
        [[nodiscard]] static LoadingMenuSink& GetSingleton() noexcept
        {
            static LoadingMenuSink singleton;
            return singleton;
        }

        RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent& a_event,
            RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
        {
            if (a_event.menuName == "LoadingMenu") {
                g_loadingMenuOpen.store(a_event.opening, std::memory_order_relaxed);
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    void ResetPreviousWorldDownwards(RE::NiAVObject* a_self)
    {
        if (a_self == nullptr) {
            return;
        }
        if (RE::NiNode* node = fallout_cast<RE::NiNode*>(a_self)) {
            for (auto& child : node->children) {
                ResetPreviousWorldDownwards(child.get());
            }
        }
        a_self->previousWorld = a_self->world;
    }

    struct CachedNode
    {
        RE::NiPointer<RE::NiAVObject> keepAlive;
        RE::NiTransform world;
    };
    using TransformCache = std::unordered_map<RE::NiAVObject*, CachedNode>;

    void CacheWorldDownwards(RE::NiAVObject* a_self, TransformCache& a_cache)
    {
        if (a_self == nullptr) {
            return;
        }
        if (RE::NiNode* node = fallout_cast<RE::NiNode*>(a_self)) {
            for (auto& child : node->children) {
                CacheWorldDownwards(child.get(), a_cache);
            }
        }
        a_cache.try_emplace(a_self, CachedNode{ RE::NiPointer<RE::NiAVObject>(a_self), a_self->world });
    }

    void PublishPreviousWorldDownwards(RE::NiAVObject* a_self, const TransformCache& a_cache)
    {
        if (a_self == nullptr) {
            return;
        }
        if (RE::NiNode* node = fallout_cast<RE::NiNode*>(a_self)) {
            for (auto& child : node->children) {
                PublishPreviousWorldDownwards(child.get(), a_cache);
            }
        }
        if (const auto it = a_cache.find(a_self); it != a_cache.end()) {
            a_self->previousWorld = it->second.world;
        }
    }

    struct SetSequencePosition
    {
        static void thunk(RE::NiAVObject* a_this, RE::NiUpdateData* a_updateData)
        {
            func(a_this, a_updateData);
            GUARD_BEGIN
            ResetPreviousWorldDownwards(a_this);
            GUARD_END("MVFix.SetSequencePosition")
        }
        static inline REL::Relocation<decltype(thunk)> func;
    };

    struct OnIdleUpdatePlayer
    {
        static void thunk(RE::Main* a_this)
        {
            Platform::Reflex::NoteGameUpdate();
            if (a_this != nullptr) {
                const std::uint32_t state = kMainPublished | (a_this->gameActive ? kMainGameActive : 0U) |
                                            (a_this->inMenuMode ? kMainInMenuMode : 0U) |
                                            (a_this->freezeTime ? kMainFreezeTime : 0U);
                g_mainState.store(state, std::memory_order_relaxed);
            }
            static thread_local TransformCache cache;
            cache.clear();
            GUARD_BEGIN
            if (auto* player = RE::PlayerCharacter::GetSingleton()) {
                CacheWorldDownwards(player->Get3D(false), cache);
            }
            GUARD_END("MVFix.OnIdle.cache")
            func(a_this);
            Platform::Reflex::NoteGameUpdateEnd();
            GUARD_BEGIN
            if (auto* player = RE::PlayerCharacter::GetSingleton()) {
                PublishPreviousWorldDownwards(player->Get3D(false), cache);
            }
            GUARD_END("MVFix.OnIdle.publish")
            cache.clear();
        }
        static inline REL::Relocation<decltype(thunk)> func;
    };

    using GetRenderPassesFn = void* (*)(RE::BSShaderProperty*, RE::NiAVObject*, std::uint32_t, void*);
    GetRenderPassesFn g_getRenderPassesOriginal{ nullptr };

    void* GetRenderPassesThunk(RE::BSShaderProperty* a_this, RE::NiAVObject* a_geometry,
        std::uint32_t a_renderMode, void* a_accumulator)
    {
        if (a_this != nullptr && a_geometry != nullptr && g_sinkActive.load(std::memory_order_relaxed) &&
            !g_loadingMenuOpen.load(std::memory_order_relaxed)) {
            const std::uint32_t mainState = MainStateNow();
            const bool frozenTime = (mainState & kMainGameActive) != 0 &&
                                    (mainState & (kMainInMenuMode | kMainFreezeTime)) != 0;
            const bool lodObject = (a_this->flags & kLodFlagsMask) != 0;
            if (frozenTime || lodObject) {
                a_geometry->previousWorld = a_geometry->world;
            }
        }
        return g_getRenderPassesOriginal(a_this, a_geometry, a_renderMode, a_accumulator);
    }
}

namespace Platform::MotionVectorFixes
{
    void Install() noexcept
    {
        if (g_installAttempted.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        if (::GetModuleHandleW(L"MotionVectorFixes.dll") != nullptr) {
            g_standingDown.store(true, std::memory_order_release);
            logger::info("[MVFix] the standalone Motion Vector Fixes plugin is loaded — the built-in corrections stand "
                         "down (no double install; that plugin provides them)");
            return;
        }

        try {
            const std::uintptr_t target = REL::ID(1318162).address();
            const std::uintptr_t original = Detours::X64::DetourFunction(
                target, reinterpret_cast<std::uintptr_t>(&OnIdleUpdatePlayer::thunk));
            if (original != 0) {
                OnIdleUpdatePlayer::func = original;
                g_fixWeaponActive.store(true, std::memory_order_release);
            } else {
                logger::warn("[MVFix] the player-update detour was refused at {:#x} (the prologue is unsuitable or "
                             "another hook owns it) — the weapon transform correction is inactive", target);
            }
        } catch (...) {
            logger::warn("[MVFix] the player-update detour threw — the weapon transform correction is inactive");
        }

        try {
            const std::uintptr_t callSite = REL::ID(854236).address() + 0x1D7;
            const auto* bytes = reinterpret_cast<const std::uint8_t*>(callSite);
            if (bytes[0] != 0xE8) {
                std::snprintf(g_animatedReason, sizeof(g_animatedReason),
                    "call site %#llx does not hold a CALL (byte %02X)", static_cast<unsigned long long>(callSite), bytes[0]);
                logger::warn("[MVFix] {} — the animated-object correction is NOT installed", g_animatedReason);
            } else {
                std::int32_t rel = 0;
                std::memcpy(&rel, bytes + 1, sizeof(rel));
                const std::uintptr_t callTarget = callSite + 5 + static_cast<std::uintptr_t>(static_cast<std::intptr_t>(rel));
                if (!InsideGameModule(callTarget) || !IsExecutableAddress(callTarget)) {
                    std::snprintf(g_animatedReason, sizeof(g_animatedReason),
                        "the CALL at %#llx targets %#llx, outside the game's code", static_cast<unsigned long long>(callSite),
                        static_cast<unsigned long long>(callTarget));
                    logger::warn("[MVFix] {} — the animated-object correction is NOT installed", g_animatedReason);
                } else {
                    SetSequencePosition::func = F4SE::GetTrampoline().write_call<5>(callSite, SetSequencePosition::thunk);
                    g_fixAnimatedActive.store(true, std::memory_order_release);
                }
            }
        } catch (...) {
            std::snprintf(g_animatedReason, sizeof(g_animatedReason), "the call-site thunk threw");
            logger::warn("[MVFix] the sequence-position thunk threw — the animated-object correction is inactive");
        }

        try {
            REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE::BSLightingShaderProperty[0] };
            const auto existing = *reinterpret_cast<const std::uintptr_t*>(vtbl.address() + 43U * sizeof(std::uintptr_t));
            if (IsExecutableAddress(existing)) {
                g_getRenderPassesOriginal = reinterpret_cast<GetRenderPassesFn>(existing);
                vtbl.write_vfunc(43, GetRenderPassesThunk);
                g_fixCollapseActive.store(true, std::memory_order_release);
            } else {
                logger::warn("[MVFix] GetRenderPasses slot 43 is not a valid executable address — the frozen-time/LOD "
                             "correction is NOT installed");
            }
        } catch (...) {
            logger::warn("[MVFix] the GetRenderPasses vtable hook threw — the frozen-time/LOD correction is inactive");
        }

        logger::info("[MVFix] install matrix: weapon transform={} animated objects={} frozen-time/LOD={} (doodlum's "
                     "Motion Vector Fixes, pre-NG; feeds the DLSS upscaler, FSR, frame generation and the neural pass; "
                     "the frozen-time collapse also waits for the LoadingMenu sink{}){}",
            g_fixWeaponActive.load(std::memory_order_relaxed) ? "on" : "OFF",
            g_fixAnimatedActive.load(std::memory_order_relaxed) ? "on" : "OFF",
            g_fixCollapseActive.load(std::memory_order_relaxed) ? "on" : "OFF",
            g_fixWeaponActive.load(std::memory_order_relaxed) ? "" : "; with the weapon fix off, frozen time is read from Main directly, as the reference does",
            g_fixWeaponActive.load(std::memory_order_relaxed)
                ? ". Reflex's game-thread sleep has its frame signal from the player-update detour"
                : ". REFLEX'S GAME-THREAD SLEEP IS INACTIVE: it needs the player-update detour to name the game thread and its frames; Reflex falls back to sleeping at the present's tail");
    }

    void OnDataLoaded() noexcept
    {
        if (!g_fixCollapseActive.load(std::memory_order_acquire) || g_sinkActive.load(std::memory_order_acquire)) {
            return;
        }
        try {
            if (auto* ui = RE::UI::GetSingleton()) {
                ui->RegisterSink<RE::MenuOpenCloseEvent>(&LoadingMenuSink::GetSingleton());
                g_sinkActive.store(true, std::memory_order_release);
                logger::info("[MVFix] LoadingMenu sink registered — the frozen-time/LOD correction is live");
            }
        } catch (...) {
            logger::warn("[MVFix] the menu sink registration threw — the frozen-time/LOD correction stays off; retried at the next load");
        }
    }

    State Snapshot() noexcept
    {
        State state{};
        state.standingDown = g_standingDown.load(std::memory_order_relaxed);
        state.weapon = g_fixWeaponActive.load(std::memory_order_relaxed);
        state.animated = g_fixAnimatedActive.load(std::memory_order_relaxed);
        state.frozenLod = g_fixCollapseActive.load(std::memory_order_relaxed);
        state.sinkLive = g_sinkActive.load(std::memory_order_relaxed);
        std::snprintf(state.animatedReason, sizeof(state.animatedReason), "%s", g_animatedReason);
        return state;
    }
}
