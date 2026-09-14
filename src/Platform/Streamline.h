#pragma once

#include "Platform/AaEngineSelection.h"
#include "Platform/AaMailbox.h"
#include "Platform/DeviceGenerationTracker.h"
#include "Platform/DlaaSettings.h"
#include "Platform/RendererConstants.h"

#include <cstdint>

#include <d3d11.h>

namespace Platform
{
    class Streamline final
    {
    public:
        static void InitMailbox(const DlaaSettings& settings);

        static PumpResult PumpMailbox(std::uint64_t frame) noexcept;

        [[nodiscard]] static bool DrainAffectsActiveEngine() noexcept;

        [[nodiscard]] static AaRequest AppliedRequest() noexcept;
        [[nodiscard]] static std::uint32_t AppliedDamagedScope() noexcept;
        [[nodiscard]] static AaEffectiveEngine AppliedEffectiveEngine() noexcept;

        static void ObserveAaDevice(ID3D11Device* device) noexcept;

        [[nodiscard]] static AaEngineSelectionSnapshot EngineSelectionSnapshot() noexcept;

        static void NoteDeviceCreated(ID3D11Device* device) noexcept;
        static void NoteDeviceObserved(ID3D11Device* device) noexcept;

        [[nodiscard]] static Platform::DeviceGeneration RenderOwnedDeviceGeneration() noexcept;

        [[nodiscard]] static AaDiagnosticSnapshot MailboxSnapshot() noexcept;

        static void ApplyDlaaSettings(bool enable, std::uint32_t preset, bool autoExposure) noexcept;
        static void ApplyDlaaSettingsLive(bool enable, std::uint32_t preset, bool autoExposure) noexcept;
        [[nodiscard]] static bool DlaaEnabled() noexcept;

        static void LogDebugSnapshot() noexcept;

        [[nodiscard]] static bool EvaluateDLAA(ID3D11DeviceContext* context, ID3D11Resource* color,
            ID3D11Resource* depth, ID3D11Resource* motionVectors, const D3D11_TEXTURE2D_DESC& colorDesc,
            std::uint32_t renderWidth, std::uint32_t renderHeight,
            float jitterPixelX, float jitterPixelY, bool reset) noexcept;

        static void StampDlaaResult(ID3D11DeviceContext* context, ID3D11Resource* color) noexcept;

        static void SetSharpness(float sharpness) noexcept;

        [[nodiscard]] static bool SharpenOnly(ID3D11DeviceContext* context, ID3D11Resource* color,
            const D3D11_TEXTURE2D_DESC& colorDesc) noexcept;

        static void SetExposureScale(float scale) noexcept;

        static void SetMipBias(float bias) noexcept;

        static void SetResolutionScale(float scale, bool stageRecreate = true) noexcept;
        [[nodiscard]] static float CurrentResolutionScale() noexcept;

        static void SetQualityMode(std::uint32_t mode, bool stageRecreate = true) noexcept;
        [[nodiscard]] static std::uint32_t CurrentQualityMode() noexcept;
        [[nodiscard]] static float QueryOptimalRatio(
            std::uint32_t qualityMode, std::uint32_t outputW, std::uint32_t outputH) noexcept;

        [[nodiscard]] static bool ColorPathIsHdr(std::uint32_t& outDxgiFormat) noexcept;

        [[nodiscard]] static float EffectiveOutOverRender() noexcept;

        static void SetRequestedEngine(AaEngineRequest engine) noexcept;
        static void SetFsrVelocity(float value) noexcept;
        static void SetFsrReactiveness(float value) noexcept;
        static void SetFsrShadingChange(float value) noexcept;
        static void SetFsrAccumulation(float value) noexcept;
        static void SetFsrMinDisocclusion(float value) noexcept;
        static void SetFsrMasks(bool enabled) noexcept;
        static void SetFsrTransparencyScale(float value) noexcept;

    };
}
