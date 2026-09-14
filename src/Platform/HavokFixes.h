// SPDX-License-Identifier: GPL-3.0-or-later
// Portions adapted from High FPS Physics Fix, Copyright (c) 2025 AntoniX35, MIT License.

#pragma once

#include <cstdint>

namespace Platform
{
    class HavokFixes
    {
    public:
        struct Settings
        {
            bool disableActorFade{ false };
            bool disablePlayerFade{ false };
        };

        static void ApplyLoadTime(const Settings& a_settings) noexcept;

        static void ApplyStutterFixes() noexcept;
        static void ApplyWindSpeedFixes() noexcept;
        static void ApplyRotationFixes() noexcept;
        static void ApplySittingRotationFixes() noexcept;
        static void ApplyMotionFixes() noexcept;

        static void ApplyAfterGameSettings() noexcept;

        [[nodiscard]] static bool ApplyLoadingModelFixes(std::uintptr_t a_zoomSpeedValue,
            std::uintptr_t a_rotateSpeedValue) noexcept;

        struct Status
        {
            bool loadTimeRan{ false };
            bool afterSettingsRan{ false };
            bool untieApplied{ false };
            bool whiteScreenApplied{ false };
            bool stutterFixesRan{ false };
            bool windFixesRan{ false };
            bool rotationFixesRan{ false };
            bool sittingRotationRan{ false };
            bool loadingModelApplied{ false };
            bool actorFadeApplied{ false };
            bool playerFadeApplied{ false };
            bool fpsClampCleared{ false };
            std::int32_t fpsClampBefore{ -1 };
            std::int32_t fpsClampAfter{ -1 };
            bool ocbpDetected{ false };
        };

        [[nodiscard]] static Status CurrentStatus() noexcept;
    };
}
