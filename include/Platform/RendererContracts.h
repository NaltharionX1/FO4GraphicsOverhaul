#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace Platform
{
    struct Fallout4RenderTargetIndex
    {
        static constexpr std::size_t kFrameBuffer = 0;
        static constexpr std::size_t kMain = 3;
        static constexpr std::size_t kMainTemp = 4;
        static constexpr std::size_t kMotionVectors = 29;
    };

    enum class FrameValidation : std::uint8_t
    {
        kValid,
        kMissingColor,
        kMissingDepth,
        kMissingMotionVectors,
        kInvalidDisplaySize,
        kInvalidRenderSize,
        kRenderSizeOutOfRange,
        kInvalidJitter
    };

    struct FrameInputs
    {
        void* color{ nullptr };
        void* depth{ nullptr };
        void* motionVectors{ nullptr };
        std::uint32_t displayWidth{ 0 };
        std::uint32_t displayHeight{ 0 };
        std::uint32_t renderWidth{ 0 };
        std::uint32_t renderHeight{ 0 };
        float jitterX{ 0.0F };
        float jitterY{ 0.0F };
    };

    [[nodiscard]] inline FrameValidation ValidateFrameInputs(const FrameInputs& inputs) noexcept
    {
        if (!inputs.color) {
            return FrameValidation::kMissingColor;
        }
        if (!inputs.depth) {
            return FrameValidation::kMissingDepth;
        }
        if (!inputs.motionVectors) {
            return FrameValidation::kMissingMotionVectors;
        }
        if (inputs.displayWidth == 0 || inputs.displayHeight == 0) {
            return FrameValidation::kInvalidDisplaySize;
        }
        if (inputs.renderWidth == 0 || inputs.renderHeight == 0) {
            return FrameValidation::kInvalidRenderSize;
        }

        const auto maxWidth = static_cast<std::uint64_t>(inputs.displayWidth) * 2;
        const auto maxHeight = static_cast<std::uint64_t>(inputs.displayHeight) * 2;
        if (inputs.renderWidth > maxWidth || inputs.renderHeight > maxHeight) {
            return FrameValidation::kRenderSizeOutOfRange;
        }
        if (!std::isfinite(inputs.jitterX) || !std::isfinite(inputs.jitterY)) {
            return FrameValidation::kInvalidJitter;
        }
        return FrameValidation::kValid;
    }

    struct ProbeCounters
    {
        std::uint64_t beginCount{ 0 };
        std::uint64_t endCount{ 0 };

        void OnBegin() noexcept { ++beginCount; }
        void OnEnd() noexcept { ++endCount; }

        [[nodiscard]] bool Balanced() const noexcept { return beginCount == endCount; }
    };
}
