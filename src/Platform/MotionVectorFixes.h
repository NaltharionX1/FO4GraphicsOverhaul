// SPDX-License-Identifier: GPL-3.0-or-later
// Portions ported from Motion Vector Fixes (fo4test) by doodlum, GPL-3.0-or-later with its modding exception.

#pragma once

#include <cstdint>

namespace Platform::MotionVectorFixes
{
    void Install() noexcept;

    void OnDataLoaded() noexcept;

    struct State
    {
        bool standingDown{ false };
        bool weapon{ false };
        bool animated{ false };
        bool frozenLod{ false };
        bool sinkLive{ false };
        char animatedReason[96]{};
    };
    [[nodiscard]] State Snapshot() noexcept;
}
