#include "PCH.h"

#include "Platform/FovOwner.h"

#include "Platform/EngineMemory.h"
#include "Platform/SettingsRuler.h"

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
    };

    constexpr Site kSlot1{ 0xF3, { 0x48, 0x8B, 0x0D }, 3, 7 };
    constexpr Site kCall1{ 0x108, { 0xE8 }, 1, 5 };
    constexpr Site kCall2{ 0x110, { 0xE8 }, 1, 5 };
    constexpr Site kCamA{ 0x115, { 0x48, 0x8B, 0x05 }, 3, 7 };
    constexpr Site kStore168{ 0x123, { 0xF3, 0x0F, 0x11, 0xB0, 0x68, 0x01, 0x00, 0x00 }, 8, 8 };
    constexpr Site kCamB{ 0x12B, { 0x48, 0x8B, 0x05 }, 3, 7 };
    constexpr Site kStore16C{ 0x132, { 0xF3, 0x0F, 0x11, 0xB0, 0x6C, 0x01, 0x00, 0x00 }, 8, 8 };
    constexpr Site kSetA{ 0x13A, { 0xF3, 0x0F, 0x11, 0x35 }, 4, 8 };
    constexpr Site kSetB{ 0x14E, { 0xF3, 0x0F, 0x11, 0x3D }, 4, 8 };

    constexpr float kEngineFovCeiling = 160.0f;

    using Apply1_t = void (*)(void*, float, std::uint32_t, std::uint32_t, bool);
    using Apply2_t = void (*)(float);

    std::uintptr_t g_instanceSlot{ 0 };
    Apply1_t g_apply1{ nullptr };
    Apply2_t g_apply2{ nullptr };
    std::uintptr_t g_cameraSlot{ 0 };
    float* g_settingItems{ nullptr };
    float* g_settingWorld{ nullptr };
    std::atomic<bool> g_bound{ false };

    [[nodiscard]] bool Verify(const std::uint8_t* a_code, const Site& a_site) noexcept
    {
        return std::memcmp(a_code + a_site.offset, a_site.bytes, a_site.prefixLength) == 0;
    }

    [[nodiscard]] std::uintptr_t Target(const std::uint8_t* a_handler, const Site& a_site) noexcept
    {
        std::int32_t disp = 0;
        std::memcpy(&disp, a_handler + a_site.offset + a_site.length - 4, sizeof(disp));
        return reinterpret_cast<std::uintptr_t>(a_handler) + a_site.offset + a_site.length +
               static_cast<std::intptr_t>(disp);
    }

    [[nodiscard]] float* SettingValueAddress(const char* a_key) noexcept
    {
        RE::Setting* const setting = Platform::SettingsRuler::ResolveSetting(a_key);
        if (setting == nullptr) {
            return nullptr;
        }
        return reinterpret_cast<float*>(reinterpret_cast<std::uintptr_t>(setting) + 8U);
    }
}

namespace Platform::FovOwner
{
    void Bind(std::span<RE::SCRIPT_FUNCTION> a_functions) noexcept
    {
        if (g_bound.load(std::memory_order_acquire)) {
            return;
        }

        const std::uint8_t* handler = nullptr;
        for (const RE::SCRIPT_FUNCTION& fn : a_functions) {
            if (fn.shortName != nullptr && ::_stricmp(fn.shortName, "fov") == 0) {
                handler = reinterpret_cast<const std::uint8_t*>(fn.executeFunction);
                break;
            }
        }
        if (handler == nullptr) {
            logger::warn("[Fov] 'fov' is not in the console function table — not owned; the "
                         "console composite stays in use");
            return;
        }

        for (const Site* site : { &kSlot1, &kCall1, &kCall2, &kCamA, &kStore168, &kCamB,
                 &kStore16C, &kSetA, &kSetB }) {
            if (!Verify(handler, *site)) {
                logger::warn("[Fov] handler byte mismatch at +0x{:X} — REFUSED; the console "
                             "composite stays in use (a game update changed this handler)",
                    site->offset);
                return;
            }
        }

        const std::uintptr_t camA = Target(handler, kCamA);
        const std::uintptr_t camB = Target(handler, kCamB);
        if (camA != camB) {
            logger::warn("[Fov] the two camera-slot loads disagree (0x{:X} vs 0x{:X}) — REFUSED",
                camA, camB);
            return;
        }

        float* const itemsByName = SettingValueAddress("fDefaultWorldFOV:Display");
        float* const worldByName = SettingValueAddress("fDefault1stPersonFOV:Display");
        const auto itemsDerived = reinterpret_cast<float*>(Target(handler, kSetA));
        const auto worldDerived = reinterpret_cast<float*>(Target(handler, kSetB));
        if (itemsByName == nullptr || worldByName == nullptr || itemsByName != itemsDerived ||
            worldByName != worldDerived) {
            logger::warn("[Fov] the derived Setting addresses and the name-resolved ones disagree "
                         "(derived {}/{}, resolved {}/{}) — REFUSED",
                static_cast<void*>(itemsDerived), static_cast<void*>(worldDerived),
                static_cast<void*>(itemsByName), static_cast<void*>(worldByName));
            return;
        }

        const std::uintptr_t instanceSlot = Target(handler, kSlot1);
        const std::uintptr_t apply1 = Target(handler, kCall1);
        const std::uintptr_t apply2 = Target(handler, kCall2);
        for (const std::uintptr_t address : { instanceSlot, apply1, apply2, camA }) {
            if (!EngineMemory::WithinGameImage(address, sizeof(std::uintptr_t))) {
                logger::warn("[Fov] a derived address (0x{:X}) is outside the game image — "
                             "REFUSED; the console composite stays in use",
                    address);
                return;
            }
        }

        g_instanceSlot = instanceSlot;
        g_apply1 = reinterpret_cast<Apply1_t>(apply1);
        g_apply2 = reinterpret_cast<Apply2_t>(apply2);
        g_cameraSlot = camA;
        g_settingItems = itemsDerived;
        g_settingWorld = worldDerived;
        g_bound.store(true, std::memory_order_release);

        logger::info("[Fov] OWNED — derived from the handler's own bytes and name-cross-checked. "
                     "items view (named fDefaultWorldFOV) = {:.1f}, world view (named "
                     "fDefault1stPersonFOV) = {:.1f}; direct writes from here on, no console",
            *g_settingItems, *g_settingWorld);
    }

    bool Owns() noexcept
    {
        return g_bound.load(std::memory_order_acquire);
    }

    bool Current(float& a_firstPersonFov, float& a_thirdPersonFov) noexcept
    {
        if (!g_bound.load(std::memory_order_acquire)) {
            return false;
        }
        return EngineMemory::SafeRead(&a_thirdPersonFov, g_settingItems,
                   sizeof(a_thirdPersonFov)) &&
               EngineMemory::SafeRead(&a_firstPersonFov, g_settingWorld,
                   sizeof(a_firstPersonFov));
    }

    bool Write(float a_firstPersonFov, float a_thirdPersonFov, float a_viewModelFov) noexcept
    {
        if (!g_bound.load(std::memory_order_acquire)) {
            return false;
        }
        if (!(a_firstPersonFov > 0.0f) || !(a_thirdPersonFov > 0.0f) ||
            !(a_viewModelFov > 0.0f)) {
            return false;
        }
        const auto* const tasks = F4SE::GetTaskInterface();
        if (tasks == nullptr) {
            return false;
        }

        const auto hold = [](float a_value) noexcept {
            return a_value >= kEngineFovCeiling ? kEngineFovCeiling : a_value;
        };
        const float firstPerson = hold(a_firstPersonFov);
        const float thirdPerson = hold(a_thirdPersonFov);
        const float viewModel = hold(a_viewModelFov);

        tasks->AddTask([firstPerson, thirdPerson, viewModel]() noexcept {
            std::uint8_t* camera = nullptr;
            const bool haveCamera =
                EngineMemory::SafeRead(&camera, reinterpret_cast<const void*>(g_cameraSlot),
                    sizeof(camera)) &&
                camera != nullptr;
            if (haveCamera) {
                (void)EngineMemory::SafeWrite(camera + 0x16C, &firstPerson, sizeof(firstPerson));
            }
            (void)EngineMemory::SafeWrite(g_settingItems, &thirdPerson, sizeof(thirdPerson));
            (void)EngineMemory::SafeWrite(g_settingWorld, &firstPerson, sizeof(firstPerson));

            void* instance = nullptr;
            if (EngineMemory::SafeRead(&instance, reinterpret_cast<const void*>(g_instanceSlot),
                    sizeof(instance)) &&
                instance != nullptr) {
                g_apply1(instance, viewModel, 0U, 0U, false);
            }
            g_apply2(viewModel);
            if (haveCamera) {
                (void)EngineMemory::SafeWrite(camera + 0x168, &viewModel, sizeof(viewModel));
            }
            logger::info("[Fov] 3rd person {:.1f} / 1st person {:.1f} / viewmodel {:.1f} — applied "
                         "on the game thread, no console",
                thirdPerson, firstPerson, viewModel);
        });
        return true;
    }
}
