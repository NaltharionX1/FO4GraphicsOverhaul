#pragma once

#include <cstddef>
#include <cstdint>

namespace Platform
{
    [[nodiscard]] constexpr std::uint32_t MaxMipLevelsForDims(
        std::uint32_t width, std::uint32_t height) noexcept
    {
        std::uint32_t dim = width > height ? width : height;
        std::uint32_t mips = 1U;
        while (dim > 1U) {
            dim >>= 1U;
            ++mips;
        }
        return mips;
    }

    [[nodiscard]] constexpr bool ShouldCopyOnOverride(
        const int* a_list, std::size_t a_count, int a_index) noexcept
    {
        for (std::size_t i = 0; i < a_count; ++i) {
            if (a_list[i] == a_index) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] constexpr bool ShouldCopyOnReset(
        const int* a_list, std::size_t a_count, int a_index) noexcept
    {
        return a_count == 0 || ShouldCopyOnOverride(a_list, a_count, a_index);
    }

    [[nodiscard]] constexpr std::uint32_t ResolveTargetDim(
        std::uint32_t a_render, float a_scale, std::uint32_t a_monitor) noexcept
    {
        if (!(a_scale > 1.001F)) {
            return a_monitor;
        }
        const auto target =
            static_cast<std::uint32_t>(static_cast<float>(a_render) * a_scale + 0.5F);
        return target == 0U ? 1U : target;
    }

    [[nodiscard]] constexpr std::uint32_t EffectiveRatioBucket(float a_ratio) noexcept
    {
        if (a_ratio >= 0.999F) {
            return 0U;
        }
        if (a_ratio >= 0.62F) {
            return 1U;
        }
        if (a_ratio >= 0.54F) {
            return 2U;
        }
        if (a_ratio >= 0.40F) {
            return 3U;
        }
        return 4U;
    }
}
