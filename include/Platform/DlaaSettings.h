#pragma once

#include "Platform/AaEngineResolution.h"
#include <cmath>
#include <cstdint>

namespace Platform
{
    struct DlaaSettings
    {
        bool enable{ true };
        std::uint32_t preset{ 11 };
        float mipBias{ -1.0F };
        bool autoExposure{ true };
        float sharpness{ 0.3F };
        float exposureScale{ 1.0F };
        float resolutionScale{ 1.0F };
        std::uint32_t reflexMode{ 1 };
        std::uint32_t qualityMode{ 0 };
        float jitterScale{ 1.0F };

        bool engineFsr{ false };
        float fsrVelocity{ 1.0F };
        float fsrReactiveness{ 1.0F };
        float fsrShadingChange{ 1.0F };
        float fsrAccumulation{ 0.333F };
        float fsrMinDisocclusion{ -0.333F };
        bool fsrFrameGeneration{ false };
        bool fsrMasks{ true };
        float fsrTransparencyScale{ 1.0F };
        bool frameGeneration{ true };
        std::uint32_t frameGenerationFrames{ 1 };
        bool frameGenerationDynamic{ false };
        std::uint32_t frameGenerationDynamicTargetHz{ 0 };
        float frameGenerationDepthSeparation{ 40.0F };

        [[nodiscard]] bool operator==(const DlaaSettings&) const noexcept = default;
    };

    [[nodiscard]] inline bool IsKnownDlaaPreset(std::uint32_t preset) noexcept
    {
        switch (preset) {
        case 0:
        case 6: case 7: case 8: case 9:
        case 10: case 11: case 12: case 13:
        case 14: case 15:
            return true;
        default:
            return false;
        }
    }

    [[nodiscard]] inline bool IsSelectableDlaaPreset(std::uint32_t preset) noexcept
    {
        return preset >= 10U && preset <= 13U;
    }

    [[nodiscard]] inline std::uint32_t ValidateDlaaPreset(std::uint32_t preset) noexcept
    {
        return IsSelectableDlaaPreset(preset) ? preset : 11U;
    }

    [[nodiscard]] inline float ClampMipBias(float bias) noexcept
    {
        if (!std::isfinite(bias)) {
            return 0.0F;
        }
        if (bias < -3.0F) {
            return -3.0F;
        }
        if (bias > 0.0F) {
            return 0.0F;
        }
        return bias;
    }

    [[nodiscard]] inline float ClampSharpness(float sharpness) noexcept
    {
        if (!std::isfinite(sharpness) || sharpness < 0.0F) {
            return 0.0F;
        }
        return sharpness > 1.0F ? 1.0F : sharpness;
    }

    [[nodiscard]] inline float ClampExposureScale(float scale) noexcept
    {
        if (!std::isfinite(scale)) {
            return 1.0F;
        }
        if (scale < 0.1F) {
            return 0.1F;
        }
        return scale > 16.0F ? 16.0F : scale;
    }

    [[nodiscard]] inline float ClampResolutionScale(float scale) noexcept
    {
        if (!std::isfinite(scale) || scale < 1.0F) {
            return 1.0F;
        }
        return scale > 2.0F ? 2.0F : scale;
    }

    [[nodiscard]] inline std::uint32_t ClampReflexMode(std::uint32_t mode) noexcept
    {
        return mode <= 2U ? mode : 1U;
    }

    [[nodiscard]] inline std::uint32_t ClampFrameGenerationFrames(std::uint32_t frames) noexcept
    {
        return frames >= 1U && frames <= 5U ? frames : 1U;
    }

    [[nodiscard]] inline std::uint32_t ClampDynamicTargetHz(std::uint32_t hz) noexcept
    {
        return hz == 0U || (hz >= 30U && hz <= 1000U) ? hz : 0U;
    }

    inline constexpr float kDepthSeparationDefault = 40.0F;
    [[nodiscard]] inline float ClampDepthSeparation(float separation) noexcept
    {
        return (separation >= 1.0F && separation <= 1000.0F) ? separation : kDepthSeparationDefault;
    }

    inline constexpr std::uint32_t kDlaaQualityModeCount = 6U;
    [[nodiscard]] inline std::uint32_t ClampQualityMode(std::uint32_t mode) noexcept
    {
        return mode < kDlaaQualityModeCount ? mode : 0U;
    }

    [[nodiscard]] constexpr bool IsHdrColorFormat(std::uint32_t dxgiFormat) noexcept
    {
        switch (dxgiFormat) {
        case 2U:
        case 10U:
        case 26U:
            return true;
        default:
            return false;
        }
    }

    [[nodiscard]] inline const char* DlaaQualityModeName(std::uint32_t mode) noexcept
    {
        switch (ClampQualityMode(mode)) {
        case 1: return "Ultra Quality";
        case 2: return "Quality";
        case 3: return "Balanced";
        case 4: return "Performance";
        case 5: return "Ultra Performance";
        default: return "DLAA";
        }
    }

    [[nodiscard]] constexpr std::uint32_t ScaledSubRectDim(std::uint32_t full, float ratio) noexcept
    {
        if (!(ratio > 0.0F) || ratio >= 1.0F) {
            return full;
        }
        const auto scaled = static_cast<std::uint32_t>(static_cast<float>(full) * ratio);
        return scaled == 0U ? 1U : scaled;
    }

    [[nodiscard]] inline float DlaaQualityModeFallbackRatio(std::uint32_t mode) noexcept
    {
        switch (ClampQualityMode(mode)) {
        case 1: return 1.0F / 1.3F;
        case 2: return 1.0F / 1.5F;
        case 3: return 1.0F / 1.72F;
        case 4: return 0.5F;
        case 5: return 1.0F / 3.0F;
        default: return 1.0F;
        }
    }

    [[nodiscard]] inline float ClampJitterScale(float scale) noexcept
    {
        if (!std::isfinite(scale)) {
            return 1.0F;
        }
        if (scale < 0.0F) {
            return 0.0F;
        }
        return scale > 3.0F ? 3.0F : scale;
    }

    [[nodiscard]] inline float ClampFsrVelocity(float value) noexcept
    {
        if (!std::isfinite(value) || value > 1.0F) {
            return 1.0F;
        }
        return value < 0.0F ? 0.0F : value;
    }

    [[nodiscard]] inline float ClampInRange(float value, float low, float high, float fallback) noexcept
    {
        if (!std::isfinite(value)) {
            return fallback;
        }
        if (value < low) {
            return low;
        }
        return value > high ? high : value;
    }

    [[nodiscard]] inline float ClampFsrReactiveness(float value) noexcept
    {
        return ClampInRange(value, 0.0F, 2.0F, 1.0F);
    }

    [[nodiscard]] inline float ClampFsrShadingChange(float value) noexcept
    {
        return ClampInRange(value, 0.0F, 2.0F, 1.0F);
    }

    [[nodiscard]] inline float ClampFsrAccumulation(float value) noexcept
    {
        return ClampInRange(value, 0.0F, 1.0F, 0.333F);
    }

    [[nodiscard]] inline float ClampFsrMinDisocclusion(float value) noexcept
    {
        return ClampInRange(value, -1.0F, 1.0F, -0.333F);
    }

    [[nodiscard]] inline float ClampFsrTransparencyScale(float value) noexcept
    {
        return ClampInRange(value, 0.0F, 2.0F, 1.0F);
    }

    [[nodiscard]] inline AaEngineRequest RequestedEngineOf(const DlaaSettings& settings) noexcept
    {
        return settings.engineFsr ? AaEngineRequest::kFsr : AaEngineRequest::kDlss;
    }

    [[nodiscard]] inline DlaaSettings ClampDlaaSettings(DlaaSettings raw) noexcept
    {
        raw.preset = ValidateDlaaPreset(raw.preset);
        raw.mipBias = ClampMipBias(raw.mipBias);
        raw.sharpness = ClampSharpness(raw.sharpness);
        raw.exposureScale = ClampExposureScale(raw.exposureScale);
        raw.resolutionScale = ClampResolutionScale(raw.resolutionScale);
        raw.reflexMode = ClampReflexMode(raw.reflexMode);
        raw.frameGenerationFrames = ClampFrameGenerationFrames(raw.frameGenerationFrames);
        raw.frameGenerationDynamicTargetHz = ClampDynamicTargetHz(raw.frameGenerationDynamicTargetHz);
        raw.frameGenerationDepthSeparation = ClampDepthSeparation(raw.frameGenerationDepthSeparation);
        raw.jitterScale = ClampJitterScale(raw.jitterScale);
        raw.qualityMode = ClampQualityMode(raw.qualityMode);
        raw.fsrVelocity = ClampFsrVelocity(raw.fsrVelocity);
        raw.fsrReactiveness = ClampFsrReactiveness(raw.fsrReactiveness);
        raw.fsrShadingChange = ClampFsrShadingChange(raw.fsrShadingChange);
        raw.fsrAccumulation = ClampFsrAccumulation(raw.fsrAccumulation);
        raw.fsrMinDisocclusion = ClampFsrMinDisocclusion(raw.fsrMinDisocclusion);
        raw.fsrTransparencyScale = ClampFsrTransparencyScale(raw.fsrTransparencyScale);
        if (raw.engineFsr && raw.qualityMode == 1U) {
            raw.qualityMode = 2U;
        }
        return raw;
    }

    [[nodiscard]] DlaaSettings LoadDlaaSettings() noexcept;

    [[nodiscard]] const DlaaSettings& CurrentDlaaSettings() noexcept;

    void SaveDlaaSettings(const DlaaSettings& settings) noexcept;
}
