#pragma once

#include "Platform/RendererConstants.h"

#include <d3d12.h>
#include <dxgiformat.h>

#include <cstdint>

namespace Platform::FrameGenEngine
{
    void SetEnabled(bool a_enabled) noexcept;
    [[nodiscard]] bool Enabled() noexcept;
    void RequestRetry() noexcept;

    void NoteCamera(const CameraConstants& a_camera) noexcept;
    void CollectGuides() noexcept;
    void CaptureHudless() noexcept;
    [[nodiscard]] void* TakeHudlessForPresent(std::uint32_t a_width, std::uint32_t a_height, DXGI_FORMAT a_format) noexcept;
    void NoteHudlessPresented(std::uint64_t a_fence) noexcept;

    [[nodiscard]] bool WantsInterpolation() noexcept;
    [[nodiscard]] bool RealFramesOnly() noexcept;
    [[nodiscard]] bool Prepare(std::uint32_t a_width, std::uint32_t a_height, DXGI_FORMAT a_format, std::uint32_t a_frames) noexcept;
    [[nodiscard]] bool OutputsNeedRebuild(std::uint32_t a_width, std::uint32_t a_height, DXGI_FORMAT a_format, std::uint32_t a_frames) noexcept;
    [[nodiscard]] std::uint32_t FramesThisFrame(float a_realIntervalMs, std::uint32_t a_syncInterval) noexcept;
    constexpr std::uint32_t kMaxGeneratedFrames = 5;
    void SetFrames(std::uint32_t a_frames) noexcept;
    void SetDynamic(bool a_dynamic) noexcept;
    void SetDynamicTargetHz(std::uint32_t a_hz) noexcept;
    void SetDisplayRefresh(float a_hz) noexcept;
    void SetDepthSeparation(float a_separation) noexcept;
    void ProduceUiAlpha(ID3D11DeviceContext* a_context, ID3D11Texture2D* a_backbuffer11, std::uint32_t a_width,
        std::uint32_t a_height) noexcept;
    [[nodiscard]] std::uint32_t Evaluate(ID3D12GraphicsCommandList* a_list, ID3D12Resource* a_backbuffer,
        std::uint32_t a_width, std::uint32_t a_height, DXGI_FORMAT a_format, std::uint64_t a_frameId,
        ID3D12Resource** a_outputs) noexcept;
    void DropFrameInputs() noexcept;
    void NoteSubmitted(std::uint64_t a_fence) noexcept;
    void RestoreOutputState(ID3D12GraphicsCommandList* a_list, ID3D12Resource* a_output) noexcept;
    [[nodiscard]] bool Interpolating() noexcept;
    void NoteInterpolating(bool a_on) noexcept;
    void LatchPresentWorkerFailure(const char* a_reason) noexcept;

    void Release() noexcept;
    void AbandonAfterUnretiredSubmission() noexcept;
    [[nodiscard]] bool HasResources() noexcept;

    struct State
    {
        bool enabled{ false };
        bool latched{ false };
        char reason[160]{};
        bool interpolating{ false };
        std::uint64_t evaluations{ 0 };
        std::uint32_t creates{ 0 };
        std::uint32_t lastResult{ 0 };
        int capability{ -1 };
        std::uint32_t frames{ 1 };
        std::uint32_t frameCap{ kMaxGeneratedFrames };
        int hudlessSource{ -1 };
        bool uiAlphaActive{ false };
        bool uiAlphaRefused{ false };
        bool dynamic{ false };
        std::uint32_t pairFrames{ 1 };
        float pairIntervalMs{ 0.0F };
        std::uint32_t dynamicTargetHz{ 0 };
        float displayHz{ 0.0F };
        bool dynamicRefused{ false };
        float depthSeparation{ 40.0F };
    };
    [[nodiscard]] State Snapshot() noexcept;
}
