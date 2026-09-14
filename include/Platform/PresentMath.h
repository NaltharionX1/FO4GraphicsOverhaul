// SPDX-License-Identifier: GPL-3.0-or-later
// Portions adapted from High FPS Physics Fix, Copyright (c) 2025 AntoniX35, MIT License.

#pragma once

#include <cstdint>

namespace Platform
{

    enum class SwapEffectChoice : std::uint32_t
    {
        kAuto = 0,
        kDiscard = 1,
        kSequential = 2,
        kFlipSequential = 3,
        kFlipDiscard = 4,
    };

    struct SwapChainCaps
    {
        bool flipSequential{ false };
        bool flipDiscard{ false };
        bool tearing{ false };
    };

    [[nodiscard]] constexpr bool IsFlipEffect(SwapEffectChoice a_effect) noexcept
    {
        return a_effect == SwapEffectChoice::kFlipSequential ||
               a_effect == SwapEffectChoice::kFlipDiscard;
    }

    [[nodiscard]] constexpr SwapEffectChoice ResolveSwapEffect(SwapEffectChoice a_requested,
        const SwapChainCaps& a_caps, bool a_windowed) noexcept
    {
        if (!a_windowed) {
            return SwapEffectChoice::kDiscard;
        }
        if (a_requested == SwapEffectChoice::kAuto) {
            if (a_caps.flipDiscard) {
                return SwapEffectChoice::kFlipDiscard;
            }
            if (a_caps.flipSequential) {
                return SwapEffectChoice::kFlipSequential;
            }
            return SwapEffectChoice::kDiscard;
        }
        if (a_requested == SwapEffectChoice::kFlipDiscard && !a_caps.flipDiscard) {
            return a_caps.flipSequential ? SwapEffectChoice::kFlipSequential
                                         : SwapEffectChoice::kDiscard;
        }
        if (a_requested == SwapEffectChoice::kFlipSequential && !a_caps.flipSequential) {
            return SwapEffectChoice::kDiscard;
        }
        return a_requested;
    }

    [[nodiscard]] constexpr std::uint32_t ResolveBufferCount(std::uint32_t a_requested,
        SwapEffectChoice a_effect, std::uint32_t a_engineValue) noexcept
    {
        const bool flip = IsFlipEffect(a_effect);
        if (a_requested == 0U) {
            return flip ? 3U : a_engineValue;
        }
        if (a_requested > 8U) {
            return flip ? 3U : a_engineValue;
        }
        return (flip && a_requested < 2U) ? 2U : a_requested;
    }

    struct PresentParams
    {
        std::uint32_t syncInterval{ 1 };
        std::uint32_t flags{ 0 };
    };

    inline constexpr std::uint32_t kPresentAllowTearing = 0x200U;

    [[nodiscard]] constexpr PresentParams ResolvePresentParams(bool a_vsyncEnabled,
        std::uint32_t a_vsyncInterval, bool a_chainAllowsTearing) noexcept
    {
        if (a_vsyncEnabled) {
            const std::uint32_t interval =
                a_vsyncInterval == 0U ? 1U : (a_vsyncInterval > 4U ? 4U : a_vsyncInterval);
            return PresentParams{ interval, 0U };
        }
        return PresentParams{ 0U, a_chainAllowsTearing ? kPresentAllowTearing : 0U };
    }

    [[nodiscard]] constexpr long long PaceSpinTicks(long long a_qpcFrequency) noexcept
    {
        return a_qpcFrequency / 4000;
    }

    [[nodiscard]] constexpr long long PaceTimerDue100ns(long long a_remainingTicks, long long a_spinTicks,
        long long a_qpcFrequency) noexcept
    {
        if (a_qpcFrequency <= 0 || a_remainingTicks <= a_spinTicks) {
            return 0;
        }
        return -((a_remainingTicks - a_spinTicks) * 10000000LL / a_qpcFrequency);
    }
}
