// SPDX-License-Identifier: GPL-3.0-or-later
// Portions adapted from High FPS Physics Fix, Copyright (c) 2025 AntoniX35, MIT License.

#include "PCH.h"

#include "Platform/HavokFixes.h"

#include "Platform/EnginePatch.h"

#include "RE/Bethesda/Settings.h"

#include <mutex>

namespace
{
    std::mutex g_mutex;
    Platform::HavokFixes::Status g_status{};

    constexpr std::uint64_t kUntieId = 462873;
    constexpr std::ptrdiff_t kUntieOffset = 0x6B;
    constexpr std::uint8_t kUntiePatch[]{ 0x00 };

    constexpr const char* kFpsClampSetting = "iFPSClamp:General";

    constexpr std::uint64_t kWhiteScreenId = 703643;
    constexpr std::ptrdiff_t kWhiteScreenOffset = 0x13;
    constexpr std::size_t kWhiteScreenFill = 0x3C;
    constexpr std::uint8_t kNop = 0x90;

    constexpr std::uint64_t kActorFadeId = 295466;
    constexpr std::ptrdiff_t kActorFadeOffset = 0x663;
    constexpr std::uint8_t kJmpShort = 0xEB;

    constexpr std::uint64_t kPlayerFadeId = 202079;
    constexpr std::ptrdiff_t kPlayerFadeOffset = 0x162;
    constexpr std::uint8_t kPlayerFadeJmp[]{ 0xEB, 0x6F, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };

    constexpr const char* kLoadingModelZoomSetting = "fLoadingModel_TriggerZoomSpeed:Interface";
    constexpr const char* kLoadingModelRotateSetting = "fLoadingModel_MouseToRotateSpeed:Interface";

    constexpr std::ptrdiff_t kSettingValueOffset = 0x08;
    static_assert(sizeof(RE::Setting) == 0x18, "RE::Setting layout changed — recheck the value offset");

    [[nodiscard]] std::uintptr_t ResolveIniFloatValueAddress(const char* a_name) noexcept
    {
        try {
            RE::Setting* setting = nullptr;
            if (auto* const ini = RE::INISettingCollection::GetSingleton()) {
                setting = ini->GetSetting(a_name);
            }
            if (!setting) {
                if (auto* const prefs = RE::INIPrefSettingCollection::GetSingleton()) {
                    setting = prefs->GetSetting(a_name);
                }
            }
            if (!setting) {
                logger::warn("[Havok] setting '{}' not found in either INI collection", a_name);
                return 0U;
            }
            if (setting->GetType() != RE::Setting::SETTING_TYPE::kFloat) {
                logger::warn("[Havok] setting '{}' is not a float — refusing to hand its address to generated code",
                    a_name);
                return 0U;
            }
            const auto address =
                reinterpret_cast<std::uintptr_t>(setting) + static_cast<std::uintptr_t>(kSettingValueOffset);
            logger::info("[Havok] setting '{}' resolved @{:#x} (current value {:.4f})", a_name, address,
                static_cast<double>(setting->GetFloat()));
            return address;
        } catch (...) {
            logger::warn("[Havok] resolving setting '{}' threw — skipped (fail-open)", a_name);
            return 0U;
        }
    }

    constexpr const char* kOcbpModules[]{ "ocbp.dll", "ocbpc.dll", "cbp.dll" };

    [[nodiscard]] bool DetectOcbp() noexcept
    {
        for (const char* const name : kOcbpModules) {
            if (const HMODULE module = ::GetModuleHandleA(name)) {
                logger::info("[Havok] OCBP detected ({} @{}) — NO speed patch applied: the reference mod advertises FixOCBPSpeed but never implemented it, and we do not ship a guessed patch into third-party physics",
                    name, static_cast<void*>(module));
                return true;
            }
        }
        logger::info("[Havok] OCBP not present — FixOCBPSpeed is inert (nothing to correct)");
        return false;
    }
}

namespace Platform
{
    void HavokFixes::ApplyLoadTime(const Settings& a_settings) noexcept
    {
        bool untied = false;

        {
            untied = EnginePatch::WriteBytes(kUntieId, kUntieOffset, kUntiePatch, {},
                "Havok/UntieSpeedFromFPS");
            if (untied) {
                logger::info("[Havok] UntieSpeedFromFPS applied — simulation speed no longer scales with framerate");
            } else {
                logger::warn("[Havok] UntieSpeedFromFPS did NOT apply — physics will still speed up at high FPS (see the [Patch] line above)");
            }
        }

        bool whiteScreen = false;
        {
            whiteScreen = EnginePatch::FillBytes(kWhiteScreenId, kWhiteScreenOffset, kNop,
                kWhiteScreenFill, "Havok/FixWhiteScreen");
            logger::info("[Havok] FixWhiteScreen {} — white flash on transitions at high FPS",
                whiteScreen ? "applied" : "NOT applied");
        }

        ApplyStutterFixes();
        ApplyWindSpeedFixes();
        ApplyRotationFixes();
        ApplySittingRotationFixes();
        ApplyMotionFixes();

        bool actorFade = false;
        if (a_settings.disableActorFade) {
            actorFade = EnginePatch::WriteByte(kActorFadeId, kActorFadeOffset, kJmpShort,
                "Misc/DisableActorFade");
            logger::info("[Havok] DisableActorFade {}", actorFade ? "applied" : "NOT applied");
        }

        bool playerFade = false;
        if (a_settings.disablePlayerFade) {
            playerFade = EnginePatch::WriteBytes(kPlayerFadeId, kPlayerFadeOffset, kPlayerFadeJmp, {},
                "Misc/DisablePlayerFade");
            logger::info("[Havok] DisablePlayerFade {} (jump displacement is position-dependent — verify the original bytes above if the player model misbehaves)",
                playerFade ? "applied" : "NOT applied");
        }

        {
            const std::scoped_lock lock{ g_mutex };
            g_status.loadTimeRan = true;
            g_status.untieApplied = untied;
            g_status.whiteScreenApplied = whiteScreen;
            g_status.stutterFixesRan = true;
            g_status.windFixesRan = true;
            g_status.rotationFixesRan = true;
            g_status.sittingRotationRan = true;
            g_status.actorFadeApplied = actorFade;
            g_status.playerFadeApplied = playerFade;
        }
        EnginePatch::LogSummary("Havok load-time fixes");
    }

    void HavokFixes::ApplyAfterGameSettings() noexcept
    {
        std::int32_t before = -1;
        std::int32_t after = -1;
        bool cleared = false;

        {
            try {
                auto* const collection = RE::INISettingCollection::GetSingleton();
                RE::Setting* const setting =
                    collection ? collection->GetSetting(kFpsClampSetting) : nullptr;
                if (!setting) {
                    logger::warn("[Havok] DisableiFPSClamp: '{}' not found in the game's settings — untie may be partially negated",
                        kFpsClampSetting);
                } else if (setting->GetType() != RE::Setting::SETTING_TYPE::kInt) {
                    logger::warn("[Havok] DisableiFPSClamp: '{}' is not an integer setting — refusing to write",
                        kFpsClampSetting);
                } else {
                    before = setting->GetInt();
                    if (before != 0) {
                        setting->SetInt(0);
                        after = setting->GetInt();
                        cleared = (after == 0);
                        if (cleared) {
                            logger::info("[Havok] DisableiFPSClamp: '{}' was {} -> now {} (verified by read-back) — untie can no longer be re-capped",
                                kFpsClampSetting, before, after);
                        } else {
                            logger::warn("[Havok] DisableiFPSClamp: wrote 0 to '{}' but it read back as {} — the engine is overriding us",
                                kFpsClampSetting, after);
                        }
                    } else {
                        after = 0;
                        cleared = true;
                        logger::info("[Havok] DisableiFPSClamp: '{}' was already 0 — nothing to do", kFpsClampSetting);
                    }
                }
            } catch (...) {
                logger::warn("[Havok] DisableiFPSClamp threw while resolving '{}' — skipped (fail-open)",
                    kFpsClampSetting);
            }
        }

        const std::uintptr_t zoomValue = ResolveIniFloatValueAddress(kLoadingModelZoomSetting);
        const std::uintptr_t rotateValue = ResolveIniFloatValueAddress(kLoadingModelRotateSetting);
        const bool loadingModel = ApplyLoadingModelFixes(zoomValue, rotateValue);

        const bool ocbp = DetectOcbp();

        {
            const std::scoped_lock lock{ g_mutex };
            g_status.afterSettingsRan = true;
            g_status.fpsClampCleared = cleared;
            g_status.fpsClampBefore = before;
            g_status.fpsClampAfter = after;
            g_status.loadingModelApplied = loadingModel;
            g_status.ocbpDetected = ocbp;
        }
        EnginePatch::LogSummary("Havok after-settings fixes");

        const bool untied = CurrentStatus().untieApplied;
        if (untied && !cleared) {
            logger::warn("[Havok] PARTIAL: untie is applied but the engine FPS clamp is not cleared — expect speed to still scale with framerate in places");
        }
    }

    HavokFixes::Status HavokFixes::CurrentStatus() noexcept
    {
        const std::scoped_lock lock{ g_mutex };
        return g_status;
    }
}
