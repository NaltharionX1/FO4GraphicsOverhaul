#pragma once

#include "Platform/AaEngineResolution.h"
#include "Platform/AaMailbox.h"

#include <d3d11.h>

#include <cstdint>

namespace Platform::Dlss12Engine
{
    struct Inputs
    {
        ID3D11Texture2D* colorIn{ nullptr };
        ID3D11Texture2D* colorOut{ nullptr };
        ID3D11Resource* depth{ nullptr };
        ID3D11Resource* motionVectors{ nullptr };
        std::uint32_t renderW{ 0 }, renderH{ 0 };
        std::uint32_t outW{ 0 }, outH{ 0 };
        bool colorIsHDR{ true };
        bool autoExposure{ true };
        float exposureScale{ 1.0F };
        std::uint32_t preset{ 11 };
        int perfQuality{ 5 };
        float jitterX{ 0.0F }, jitterY{ 0.0F };
        bool reset{ false };
        float nearPlane{ 0.0F }, farPlane{ 0.0F };
    };

    [[nodiscard]] bool MotionDilationApplied() noexcept;
    [[nodiscard]] bool FirstPersonConditionedLastFrame() noexcept;

    [[nodiscard]] AaAvailability ProbeFirstDevice(ID3D11Device* a_device) noexcept;

    [[nodiscard]] bool Evaluate(ID3D11DeviceContext* a_context, const Inputs& a_inputs) noexcept;

    [[nodiscard]] AaTeardownResult DestroyFeature() noexcept;

    void RearmExposure() noexcept;

    struct State
    {
        bool latched{ false };
        char reason[160]{};
        bool featureReady{ false };
        std::uint32_t renderW{ 0 }, renderH{ 0 }, outW{ 0 }, outH{ 0 };
        std::uint64_t evaluations{ 0 };
        std::uint32_t creates{ 0 };
        float cpuMs{ 0.0F };
        bool fp16Carrier{ false };
        std::uint32_t lastResult{ 0 };
    };
    [[nodiscard]] State Snapshot() noexcept;
}
