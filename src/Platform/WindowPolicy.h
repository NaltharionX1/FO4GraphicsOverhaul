// SPDX-License-Identifier: GPL-3.0-or-later
// Portions adapted from High FPS Physics Fix, Copyright (c) 2025 AntoniX35, MIT License.

#pragma once

#include <cstdint>

namespace Platform
{
    class WindowPolicy
    {
    public:
        static void ApplyLoadTime() noexcept;

        static void SetCursorLockEnabled(bool a_enabled) noexcept;
        [[nodiscard]] static bool CursorLockEnabled() noexcept;

        struct Status
        {
            bool ran{ false };
            bool fullscreenForcedOff{ false };
            bool borderlessForcedOn{ false };
            bool ghostingDisabled{ false };
            bool cursorLockEnabled{ false };
        };

        [[nodiscard]] static Status CurrentStatus() noexcept;
    };
}
