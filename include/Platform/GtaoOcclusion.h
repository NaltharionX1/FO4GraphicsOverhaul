#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace Platform::GtaoOcclusion
{
    inline constexpr float kPi = 3.14159265358979323846F;
    inline constexpr float kPiHalf = kPi * 0.5F;

    [[nodiscard]] inline float Iarc(float a_h, float a_n, float a_cosNorm) noexcept
    {
        return (a_cosNorm + 2.0F * a_h * std::sin(a_n) - std::cos(2.0F * a_h - a_n)) / 4.0F;
    }

    [[nodiscard]] inline float ArcMeasure(float a_from, float a_to, float a_n, float a_cosNorm) noexcept
    {
        const float lo = std::min(a_from, a_to);
        const float hi = std::max(a_from, a_to);
        if (lo >= 0.0F) {
            return Iarc(hi, a_n, a_cosNorm) - Iarc(lo, a_n, a_cosNorm);
        }
        if (hi <= 0.0F) {
            return Iarc(lo, a_n, a_cosNorm) - Iarc(hi, a_n, a_cosNorm);
        }
        return Iarc(lo, a_n, a_cosNorm) + Iarc(hi, a_n, a_cosNorm);
    }

    [[nodiscard]] inline float DomainMin(float a_n) noexcept { return a_n - kPiHalf; }
    [[nodiscard]] inline float DomainMax(float a_n) noexcept { return a_n + kPiHalf; }

    struct Occluder
    {
        float front;
        float back;
    };

    [[nodiscard]] inline float VisibilityStock(const Occluder* a_occluders, std::size_t a_count,
        float a_n, float a_cosNorm, float a_projNormLen, float a_thinOccluderCompensation) noexcept
    {
        float horizon0 = DomainMax(a_n);
        float horizon1 = DomainMin(a_n);

        for (std::size_t i = 0; i < a_count; ++i) {
            const float h = a_occluders[i].front;
            if (h >= 0.0F) {
                const float raised = std::min(horizon0, h);
                horizon0 = (horizon0 < h) ? std::lerp(raised, h, a_thinOccluderCompensation) : raised;
            } else {
                const float raised = std::max(horizon1, h);
                horizon1 = (horizon1 > h) ? std::lerp(raised, h, a_thinOccluderCompensation) : raised;
            }
        }

        const float visibility = Iarc(horizon1, a_n, a_cosNorm) + Iarc(horizon0, a_n, a_cosNorm);
        return a_projNormLen * visibility;
    }

    inline constexpr std::uint32_t kSectorCount = 32;

    [[nodiscard]] inline float SectorAt(float a_h, float a_n) noexcept
    {
        const float lo = DomainMin(a_n);
        const float span = kPi;
        return (a_h - lo) / span * static_cast<float>(kSectorCount);
    }

    [[nodiscard]] inline float AngleAtSector(float a_sector, float a_n) noexcept
    {
        return DomainMin(a_n) + (a_sector / static_cast<float>(kSectorCount)) * kPi;
    }

    [[nodiscard]] inline std::uint32_t AccumulateMask(const Occluder* a_occluders, std::size_t a_count,
        float a_n) noexcept
    {
        std::uint32_t mask = 0;
        for (std::size_t i = 0; i < a_count; ++i) {
            const float a = SectorAt(std::min(a_occluders[i].front, a_occluders[i].back), a_n);
            const float b = SectorAt(std::max(a_occluders[i].front, a_occluders[i].back), a_n);

            int first = static_cast<int>(std::floor(a));
            int last = static_cast<int>(std::ceil(b)) - 1;
            first = std::max(first, 0);
            last = std::min(last, static_cast<int>(kSectorCount) - 1);
            for (int s = first; s <= last; ++s) {
                mask |= (1U << s);
            }
        }
        return mask;
    }

    [[nodiscard]] inline float VisibilityBitmask(const Occluder* a_occluders, std::size_t a_count,
        float a_n, float a_cosNorm, float a_projNormLen) noexcept
    {
        const std::uint32_t mask = AccumulateMask(a_occluders, a_count, a_n);

        float visibility = 0.0F;
        std::uint32_t sector = 0;
        while (sector < kSectorCount) {
            if ((mask & (1U << sector)) != 0U) {
                ++sector;
                continue;
            }
            const std::uint32_t runStart = sector;
            while (sector < kSectorCount && (mask & (1U << sector)) == 0U) {
                ++sector;
            }
            visibility += ArcMeasure(AngleAtSector(static_cast<float>(runStart), a_n),
                AngleAtSector(static_cast<float>(sector), a_n), a_n, a_cosNorm);
        }
        return a_projNormLen * visibility;
    }

    [[nodiscard]] inline float FinishSlices(float a_summedVisibility, float a_sliceCount,
        float a_finalValuePower) noexcept
    {
        float visibility = a_summedVisibility / a_sliceCount;
        visibility = std::pow(std::abs(visibility), a_finalValuePower);
        return std::max(0.03F, visibility);
    }
}
