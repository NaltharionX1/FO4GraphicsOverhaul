#pragma once

#include "Platform/AaEngineResolution.h"

#include <cstdint>
#include <d3d11.h>

namespace Platform::FsrEngine
{
    [[nodiscard]] AaAvailability ProbeFirstDevice(ID3D11Device* a_device) noexcept;

    [[nodiscard]] bool Evaluate(ID3D11DeviceContext* a_ctx, ID3D11Texture2D* a_colorInOut,
        ID3D11Texture2D* a_superResOut, ID3D11Resource* a_depth, ID3D11Resource* a_mvec,
        std::uint32_t a_renderW, std::uint32_t a_renderH, std::uint32_t a_outW,
        std::uint32_t a_outH, bool a_colorIsHDR, float a_sharpness, float a_jitterX,
        float a_jitterY, bool a_reset) noexcept;

    bool DestroyContext() noexcept;

    void RearmFailureLatch() noexcept;

    void SetVelocityFactor(float a_value) noexcept;
    void SetReactiveness(float a_value) noexcept;
    void SetShadingChange(float a_value) noexcept;
    void SetAccumulation(float a_value) noexcept;
    void SetMinDisocclusion(float a_value) noexcept;
    void SetMasksEnabled(bool a_enabled) noexcept;
    void SetTransparencyScale(float a_value) noexcept;
    [[nodiscard]] bool MasksEnabled() noexcept;
    [[nodiscard]] float VelocityFactor() noexcept;
}
