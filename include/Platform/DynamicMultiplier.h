#pragma once

#include <cmath>
#include <cstdint>

namespace Platform::DynamicMultiplier
{
    [[nodiscard]] constexpr long long OwnBackPressure(long long a_blocked, long long a_paceSleep) noexcept
    {
        if (a_blocked <= 0 || a_paceSleep <= 0) {
            return 0;
        }
        return a_blocked < a_paceSleep ? a_blocked : a_paceSleep;
    }

    [[nodiscard]] constexpr float PresentedCapHz(float a_displayHz, std::uint32_t a_syncInterval) noexcept
    {
        return a_displayHz > 0.0F && a_syncInterval > 0U ? a_displayHz / static_cast<float>(a_syncInterval) : 0.0F;
    }

    [[nodiscard]] constexpr float EffectiveTargetHz(float a_targetHz, float a_capHz) noexcept
    {
        return a_capHz > 0.0F && a_capHz < a_targetHz ? a_capHz : a_targetHz;
    }

    constexpr double kTolerance = 0.05;

    [[nodiscard]] inline std::uint32_t IdealGeneratedFrames(float a_targetHz, double a_periodMs, std::uint32_t a_cap) noexcept
    {
        if (!(a_targetHz > 0.0F) || !(a_periodMs > 0.0) || a_cap == 0U) {
            return 0U;
        }
        const double wanted = static_cast<double>(a_targetHz) * a_periodMs / 1000.0 * (1.0 - kTolerance);
        const double generated = std::ceil(wanted) - 1.0;
        if (generated < 1.0) {
            return 1U;
        }
        if (generated > static_cast<double>(a_cap)) {
            return a_cap;
        }
        return static_cast<std::uint32_t>(generated);
    }

    [[nodiscard]] constexpr double PresentedHz(std::uint32_t a_generated, double a_periodMs) noexcept
    {
        return a_periodMs > 0.0 ? (static_cast<double>(a_generated) + 1.0) * 1000.0 / a_periodMs : 0.0;
    }
}
