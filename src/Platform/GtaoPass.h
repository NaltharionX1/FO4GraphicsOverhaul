#pragma once

#include <cstdint>

namespace Platform::GtaoPass
{

    void SetMinScreenRadius(float a_pixels) noexcept;

    void SetQuality(std::uint32_t a_quality) noexcept;
    void SetDenoisePasses(std::uint32_t a_passes) noexcept;
    void SetRadius(float a_radius) noexcept;
    void SetRadiusMultiplier(float a_value) noexcept;
    void SetFalloffRange(float a_value) noexcept;
    void SetSampleDistributionPower(float a_value) noexcept;
    void SetOccluderThickness(float a_value) noexcept;
    void SetFinalValuePower(float a_value) noexcept;

    inline constexpr float kGameUnitsPerMetre = 70.0F;

    void RestoreDefaults() noexcept;

    void SetDepthFadeEnabled(bool a_enabled) noexcept;
    void SetDepthFadeRange(float a_start, float a_end) noexcept;

    [[nodiscard]] bool Execute() noexcept;

    [[nodiscard]] bool IntegrateInto(void* a_targetUav, void* a_targetSrv,
        std::uint32_t a_targetWidth, std::uint32_t a_targetHeight) noexcept;

    void SetBlendMode(std::uint32_t a_mode) noexcept;

    struct State
    {
        std::uint32_t quality;
        std::uint32_t denoisePasses;
        float radius;
        float radiusMultiplier;
        float falloffRange;
        float sampleDistributionPower;
        float occluderThickness;
        float finalValuePower;
        bool shadersReady;
        std::uint32_t width;
        std::uint32_t height;
        std::uint64_t frames;
        float minScreenRadius;
        bool usingEngineNormals;
        bool depthFadeEnabled;
        float depthFadeStart;
        float depthFadeEnd;
    };

    [[nodiscard]] State Snapshot() noexcept;
}
