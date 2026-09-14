#include "PCH.h"

#include "Platform/GtaoPass.h"

#include "Platform/Fallout4Renderer.h"
#include "Platform/OutputMergerScope.h"
#include "Platform/RendererConstants.h"

#include <DirectXMath.h>
#include <d3d11.h>

#include <algorithm>
#include <atomic>
#include <cstring>

#include <cassert>
#pragma warning(push, 3)
#include "XeGTAO.h"
#pragma warning(pop)

#include "CSDenoiseLastPass.h"
#include "CSDenoisePass.h"
#include "CSGTAOExtreme.h"
#include "CSGTAOHigh.h"
#include "CSGTAOLow.h"
#include "CSGTAOMedium.h"
#include "CSGTAOUltra.h"
#include "CSDecodeGBuffer.h"
#include "CSGenerateNormals.h"
#include "CSAoIntegrate1.h"
#include "CSAoIntegrate2.h"
#include "CSAoIntegrate3.h"
#include "CSAoIntegrate4.h"
#include "CSPrefilterDepths16x16.h"

namespace
{
    constexpr std::uint32_t kMainDepthIndex = 2;

    std::atomic<std::uint32_t> g_quality{ 2 };
    std::atomic<std::uint32_t> g_denoisePasses{ 1 };
    std::atomic<float> g_radius{ Platform::GtaoPass::kGameUnitsPerMetre };
    std::atomic<float> g_minScreenRadius{ 3.0F };
    std::atomic<float> g_radiusMultiplier{ 1.703F };
    std::atomic<float> g_falloffRange{ 0.601F };
    std::atomic<float> g_sampleDistributionPower{ 2.50F };
    std::atomic<float> g_occluderThickness{ 8.0F };
    std::atomic<float> g_finalValuePower{ 2.70F };
    std::atomic<bool> g_depthFadeEnabled{ true };
    std::atomic<float> g_depthFadeStart{ 40000.0F };
    std::atomic<float> g_depthFadeEnd{ 50000.0F };

    std::atomic<bool> g_shadersReady{ false };
    std::atomic<bool> g_failed{ false };
    std::atomic<std::uint32_t> g_width{ 0 };
    std::atomic<std::uint32_t> g_height{ 0 };
    std::atomic<std::uint64_t> g_frames{ 0 };

    template <class T>
    void SafeRelease(T*& a_ptr) noexcept
    {
        if (a_ptr != nullptr) {
            a_ptr->Release();
            a_ptr = nullptr;
        }
    }

    ID3D11ComputeShader* g_csPrefilter{ nullptr };
    ID3D11ComputeShader* g_csNormals{ nullptr };
    ID3D11ComputeShader* g_csDecode{ nullptr };
    ID3D11Buffer* g_decodeConstants{ nullptr };
    constexpr std::uint32_t kGBufferNormalIndex = 20;
    std::atomic<bool> g_engineNormalsAvailable{ false };
    ID3D11ComputeShader* g_csMain[5]{};
    ID3D11ComputeShader* g_csDenoisePass{ nullptr };
    ID3D11ComputeShader* g_csDenoiseLastPass{ nullptr };
    ID3D11Buffer* g_constants{ nullptr };
    ID3D11Buffer* g_extensionConstants{ nullptr };
    ID3D11ComputeShader* g_csIntegrate[4]{};
    ID3D11Buffer* g_integrateConstants{ nullptr };
    std::atomic<std::uint32_t> g_blendMode{ 1 };
    std::atomic<bool> g_lastIntegrateOk{ false };
    ID3D11ShaderResourceView* g_finalAoSrv{ nullptr };
    ID3D11SamplerState* g_pointClamp{ nullptr };
    ID3D11SamplerState* g_pointBorderFar{ nullptr };

    ID3D11Texture2D* g_linearDepth{ nullptr };
    ID3D11ShaderResourceView* g_linearDepthSrv{ nullptr };
    ID3D11UnorderedAccessView* g_linearDepthUav{ nullptr };
    ID3D11Texture2D* g_workingDepth{ nullptr };
    ID3D11ShaderResourceView* g_workingDepthSrv{ nullptr };
    ID3D11UnorderedAccessView* g_workingDepthMipUav[5]{};
    ID3D11Texture2D* g_normals{ nullptr };
    ID3D11ShaderResourceView* g_normalsSrv{ nullptr };
    ID3D11UnorderedAccessView* g_normalsUav{ nullptr };
    ID3D11Texture2D* g_aoTermA{ nullptr };
    ID3D11ShaderResourceView* g_aoTermASrv{ nullptr };
    ID3D11UnorderedAccessView* g_aoTermAUav{ nullptr };
    ID3D11Texture2D* g_aoTermB{ nullptr };
    ID3D11ShaderResourceView* g_aoTermBSrv{ nullptr };
    ID3D11UnorderedAccessView* g_aoTermBUav{ nullptr };
    ID3D11Texture2D* g_edges{ nullptr };
    ID3D11ShaderResourceView* g_edgesSrv{ nullptr };
    ID3D11UnorderedAccessView* g_edgesUav{ nullptr };

    void ReleaseSizedResources() noexcept
    {
        for (auto& uav : g_workingDepthMipUav) {
            SafeRelease(uav);
        }
        SafeRelease(g_linearDepthUav);
        SafeRelease(g_linearDepthSrv);
        SafeRelease(g_linearDepth);
        SafeRelease(g_workingDepthSrv);
        SafeRelease(g_workingDepth);
        SafeRelease(g_normalsUav);
        SafeRelease(g_normalsSrv);
        SafeRelease(g_normals);
        SafeRelease(g_aoTermAUav);
        SafeRelease(g_aoTermASrv);
        SafeRelease(g_aoTermA);
        SafeRelease(g_aoTermBUav);
        SafeRelease(g_aoTermBSrv);
        SafeRelease(g_aoTermB);
        SafeRelease(g_edgesUav);
        SafeRelease(g_edgesSrv);
        SafeRelease(g_edges);
    }

    [[nodiscard]] bool CreateShadersOnce(ID3D11Device* a_device) noexcept
    {
        if (g_shadersReady.load(std::memory_order_acquire)) {
            return true;
        }
        const auto cs = [&](const void* a_code, std::size_t a_size, ID3D11ComputeShader*& a_out) {
            return SUCCEEDED(a_device->CreateComputeShader(a_code, a_size, nullptr, &a_out));
        };
        bool ok = cs(g_csPrefilterDepths, sizeof(g_csPrefilterDepths), g_csPrefilter) &&
                  cs(g_csGenerateNormals, sizeof(g_csGenerateNormals), g_csNormals) &&
                  cs(g_csGtaoLow, sizeof(g_csGtaoLow), g_csMain[0]) &&
                  cs(g_csGtaoMedium, sizeof(g_csGtaoMedium), g_csMain[1]) &&
                  cs(g_csGtaoHigh, sizeof(g_csGtaoHigh), g_csMain[2]) &&
                  cs(g_csGtaoUltra, sizeof(g_csGtaoUltra), g_csMain[3]) &&
                  cs(g_csGtaoExtreme, sizeof(g_csGtaoExtreme), g_csMain[4]) &&
                  cs(g_csDenoise, sizeof(g_csDenoise), g_csDenoisePass) &&
                  cs(g_csDenoiseLast, sizeof(g_csDenoiseLast), g_csDenoiseLastPass);
        if (ok) {
            D3D11_BUFFER_DESC cb{};
            cb.ByteWidth = (sizeof(XeGTAO::GTAOConstants) + 15U) & ~15U;
            cb.Usage = D3D11_USAGE_DYNAMIC;
            cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            ok = SUCCEEDED(a_device->CreateBuffer(&cb, nullptr, &g_constants));
        }
        if (ok) {
            D3D11_BUFFER_DESC cb{};
            cb.ByteWidth = 16;
            cb.Usage = D3D11_USAGE_DYNAMIC;
            cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            ok = SUCCEEDED(a_device->CreateBuffer(&cb, nullptr, &g_extensionConstants));
        }
        if (ok) {
            ok = cs(g_csDecodeGBuffer, sizeof(g_csDecodeGBuffer), g_csDecode);
        }
        if (ok) {
            D3D11_BUFFER_DESC cb{};
            cb.ByteWidth = 96;
            cb.Usage = D3D11_USAGE_DYNAMIC;
            cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            ok = SUCCEEDED(a_device->CreateBuffer(&cb, nullptr, &g_decodeConstants));
        }
        if (ok) {
            D3D11_BUFFER_DESC cb{};
            cb.ByteWidth = 32;
            cb.Usage = D3D11_USAGE_DYNAMIC;
            cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            ok = SUCCEEDED(a_device->CreateBuffer(&cb, nullptr, &g_integrateConstants));
        }
        if (ok) {
            ok = cs(g_csAoIntegrate1, sizeof(g_csAoIntegrate1), g_csIntegrate[0]) &&
                 cs(g_csAoIntegrate2, sizeof(g_csAoIntegrate2), g_csIntegrate[1]) &&
                 cs(g_csAoIntegrate3, sizeof(g_csAoIntegrate3), g_csIntegrate[2]) &&
                 cs(g_csAoIntegrate4, sizeof(g_csAoIntegrate4), g_csIntegrate[3]);
        }
        if (ok) {
            D3D11_SAMPLER_DESC sampler{};
            sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
            sampler.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
            sampler.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
            sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
            sampler.MaxLOD = D3D11_FLOAT32_MAX;
            ok = SUCCEEDED(a_device->CreateSamplerState(&sampler, &g_pointClamp));
        }
        if (ok) {
            constexpr float kBorderFarDepth = 1.0e7F;
            D3D11_SAMPLER_DESC sampler{};
            sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
            sampler.AddressU = D3D11_TEXTURE_ADDRESS_BORDER;
            sampler.AddressV = D3D11_TEXTURE_ADDRESS_BORDER;
            sampler.AddressW = D3D11_TEXTURE_ADDRESS_BORDER;
            sampler.BorderColor[0] = kBorderFarDepth;
            sampler.BorderColor[1] = kBorderFarDepth;
            sampler.BorderColor[2] = kBorderFarDepth;
            sampler.BorderColor[3] = kBorderFarDepth;
            sampler.MaxLOD = D3D11_FLOAT32_MAX;
            ok = SUCCEEDED(a_device->CreateSamplerState(&sampler, &g_pointBorderFar));
        }

        if (!ok) {
            logger::warn("[GTAO] pipeline-object creation failed — GTAO unavailable this session.");
            g_failed.store(true, std::memory_order_relaxed);
            return false;
        }
        g_shadersReady.store(true, std::memory_order_release);
        logger::info("[GTAO] pipeline created: 13 compute shaders (decode, prefilter, normals "
                     "fallback, 4 quality tiers, 2 denoise, 4 integrate variants), constants, "
                     "point-clamp sampler.");
        return true;
    }

    [[nodiscard]] bool EnsureSizedResources(
        ID3D11Device* a_device, std::uint32_t a_width, std::uint32_t a_height) noexcept
    {
        if (a_width == 0 || a_height == 0) {
            return false;
        }
        if (g_width.load(std::memory_order_relaxed) == a_width &&
            g_height.load(std::memory_order_relaxed) == a_height && g_workingDepth != nullptr) {
            return true;
        }
        ReleaseSizedResources();

        const auto tex2d = [&](DXGI_FORMAT a_format, UINT a_mips, ID3D11Texture2D*& a_tex,
                               ID3D11ShaderResourceView*& a_srv,
                               ID3D11UnorderedAccessView** a_uavs, UINT a_uavCount) {
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = a_width;
            desc.Height = a_height;
            desc.MipLevels = a_mips;
            desc.ArraySize = 1;
            desc.Format = a_format;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
            if (FAILED(a_device->CreateTexture2D(&desc, nullptr, &a_tex))) {
                return false;
            }
            if (FAILED(a_device->CreateShaderResourceView(a_tex, nullptr, &a_srv))) {
                return false;
            }
            for (UINT mip = 0; mip < a_uavCount; ++mip) {
                D3D11_UNORDERED_ACCESS_VIEW_DESC uav{};
                uav.Format = a_format;
                uav.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
                uav.Texture2D.MipSlice = mip;
                if (FAILED(a_device->CreateUnorderedAccessView(a_tex, &uav, &a_uavs[mip]))) {
                    return false;
                }
            }
            return true;
        };

        ID3D11UnorderedAccessView* singleUav[1]{};
        bool ok = tex2d(DXGI_FORMAT_R32_FLOAT, 5, g_workingDepth, g_workingDepthSrv,
            g_workingDepthMipUav, 5);
        ok = ok && tex2d(DXGI_FORMAT_R32_FLOAT, 1, g_linearDepth, g_linearDepthSrv, singleUav, 1);
        g_linearDepthUav = singleUav[0];
        singleUav[0] = nullptr;
        ok = ok && tex2d(DXGI_FORMAT_R32_UINT, 1, g_normals, g_normalsSrv, singleUav, 1);
        g_normalsUav = singleUav[0];
        singleUav[0] = nullptr;
        ok = ok && tex2d(DXGI_FORMAT_R8_UINT, 1, g_aoTermA, g_aoTermASrv, singleUav, 1);
        g_aoTermAUav = singleUav[0];
        singleUav[0] = nullptr;
        ok = ok && tex2d(DXGI_FORMAT_R8_UINT, 1, g_aoTermB, g_aoTermBSrv, singleUav, 1);
        g_aoTermBUav = singleUav[0];
        singleUav[0] = nullptr;
        ok = ok && tex2d(DXGI_FORMAT_R8_UNORM, 1, g_edges, g_edgesSrv, singleUav, 1);
        g_edgesUav = singleUav[0];
        singleUav[0] = nullptr;

        if (!ok) {
            ReleaseSizedResources();
            logger::warn("[GTAO] working-texture creation failed at {}x{} — GTAO unavailable this "
                         "session.",
                a_width, a_height);
            g_failed.store(true, std::memory_order_relaxed);
            return false;
        }
        g_width.store(a_width, std::memory_order_relaxed);
        g_height.store(a_height, std::memory_order_relaxed);
        logger::info("[GTAO] working textures (re)created at {}x{} (depth 5-mip R32F, normals "
                     "R32U, AO terms 2x R8U, edges R8).",
            a_width, a_height);
        return true;
    }
}

namespace
{
    [[nodiscard]] std::uint32_t ComponentsForFormat(DXGI_FORMAT a_format) noexcept
    {
        switch (a_format) {
        case DXGI_FORMAT_R32_FLOAT:
        case DXGI_FORMAT_R16_FLOAT:
        case DXGI_FORMAT_R8_UNORM:
        case DXGI_FORMAT_R16_UNORM:
            return 1;
        case DXGI_FORMAT_R32G32_FLOAT:
        case DXGI_FORMAT_R16G16_FLOAT:
        case DXGI_FORMAT_R8G8_UNORM:
        case DXGI_FORMAT_R16G16_UNORM:
            return 2;
        case DXGI_FORMAT_R11G11B10_FLOAT:
            return 3;
        case DXGI_FORMAT_R32G32B32A32_FLOAT:
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R10G10B10A2_UNORM:
            return 4;
        default:
            return 0;
        }
    }
}

namespace Platform::GtaoPass
{
    void SetBlendMode(std::uint32_t a_mode) noexcept
    {
        g_blendMode.store(std::min(a_mode, 1U), std::memory_order_relaxed);
    }

    bool IntegrateInto(void* a_targetUav, void* a_targetSrv, std::uint32_t a_targetWidth,
        std::uint32_t a_targetHeight) noexcept
    {
        g_lastIntegrateOk.store(false, std::memory_order_relaxed);
        if (a_targetUav == nullptr || g_failed.load(std::memory_order_relaxed) ||
            !g_shadersReady.load(std::memory_order_acquire)) {
            return false;
        }
        try {
            auto* const data = RE::BSGraphics::RendererData::GetSingleton();
            if (data == nullptr || data->context == nullptr) {
                return false;
            }
            auto* const context = data->context;
            auto* const uav = static_cast<ID3D11UnorderedAccessView*>(a_targetUav);

            D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
            uav->GetDesc(&uavDesc);
            const std::uint32_t components = ComponentsForFormat(uavDesc.Format);
            if (components == 0 || uavDesc.ViewDimension != D3D11_UAV_DIMENSION_TEXTURE2D) {
                static std::atomic<bool> logged{ false };
                if (!logged.exchange(true, std::memory_order_relaxed)) {
                    logger::warn("[GTAO] integration declined: UAV format {} / dimension {} is not "
                                 "a supported 2D AO target.",
                        static_cast<int>(uavDesc.Format), static_cast<int>(uavDesc.ViewDimension));
                }
                return false;
            }

            const std::uint32_t blend = g_blendMode.load(std::memory_order_relaxed);
            const std::uint32_t sourceW = g_width.load(std::memory_order_relaxed);
            const std::uint32_t sourceH = g_height.load(std::memory_order_relaxed);
            if (sourceW == 0 || sourceH == 0 || a_targetWidth == 0 || a_targetHeight == 0) {
                return false;
            }

            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (FAILED(context->Map(g_integrateConstants, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
                return false;
            }
            const std::uint32_t constants[8]{ a_targetWidth, a_targetHeight, sourceW, sourceH,
                (blend == 1U && a_targetSrv != nullptr) ? 1U : 0U, 255U, 0U, 0U };
            std::memcpy(mapped.pData, constants, sizeof(constants));
            context->Unmap(g_integrateConstants, 0);

            ID3D11ShaderResourceView* srvs[2]{ g_finalAoSrv,
                static_cast<ID3D11ShaderResourceView*>(a_targetSrv) };
            context->CSSetShader(g_csIntegrate[components - 1], nullptr, 0);
            context->CSSetConstantBuffers(0, 1, &g_integrateConstants);
            context->CSSetShaderResources(0, 2, srvs);
            context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
            context->Dispatch((a_targetWidth + 7U) / 8U, (a_targetHeight + 7U) / 8U, 1U);

            ID3D11UnorderedAccessView* nullUav[1]{};
            ID3D11ShaderResourceView* nullSrv[2]{};
            context->CSSetUnorderedAccessViews(0, 1, nullUav, nullptr);
            context->CSSetShaderResources(0, 2, nullSrv);

            g_lastIntegrateOk.store(true, std::memory_order_relaxed);
            static std::atomic<bool> loggedFirst{ false };
            if (!loggedFirst.exchange(true, std::memory_order_relaxed)) {
                logger::info("[GTAO] ★ first INTEGRATION: {}x{} <- our {}x{}, {} components, "
                             "blend={} — written through the engine target's own UAV.",
                    a_targetWidth, a_targetHeight, sourceW, sourceH, components,
                    blend == 1U ? "min(existing,ours)" : "replace");
            }
            return true;
        } catch (...) {
            return false;
        }
    }

    void SetMinScreenRadius(float a_pixels) noexcept
    {
        g_minScreenRadius.store(std::clamp(a_pixels, 0.0F, 64.0F), std::memory_order_relaxed);
    }

    void SetQuality(std::uint32_t a_quality) noexcept
    {
        g_quality.store(std::min(a_quality, 4U), std::memory_order_relaxed);
    }

    void SetDenoisePasses(std::uint32_t a_passes) noexcept
    {
        g_denoisePasses.store(std::min(a_passes, 3U), std::memory_order_relaxed);
    }

    void SetRadius(float a_radius) noexcept
    {
        g_radius.store(std::clamp(a_radius, 0.1F, 50000.0F), std::memory_order_relaxed);
    }

    void SetRadiusMultiplier(float a_value) noexcept
    {
        g_radiusMultiplier.store(std::clamp(a_value, 0.1F, 5.0F), std::memory_order_relaxed);
    }

    void SetFalloffRange(float a_value) noexcept
    {
        g_falloffRange.store(std::clamp(a_value, 0.01F, 1.0F), std::memory_order_relaxed);
    }

    void SetSampleDistributionPower(float a_value) noexcept
    {
        g_sampleDistributionPower.store(std::clamp(a_value, 1.0F, 3.0F), std::memory_order_relaxed);
    }

    void SetOccluderThickness(float a_value) noexcept
    {
        g_occluderThickness.store(std::clamp(a_value, 0.5F, 100.0F), std::memory_order_relaxed);
    }

    void SetFinalValuePower(float a_value) noexcept
    {
        g_finalValuePower.store(std::clamp(a_value, 0.5F, 5.0F), std::memory_order_relaxed);
    }

    void RestoreDefaults() noexcept
    {
        g_quality.store(2U, std::memory_order_relaxed);
        g_denoisePasses.store(1U, std::memory_order_relaxed);
        g_radius.store(kGameUnitsPerMetre, std::memory_order_relaxed);
        g_minScreenRadius.store(3.0F, std::memory_order_relaxed);
        g_radiusMultiplier.store(1.703F, std::memory_order_relaxed);
        g_falloffRange.store(0.601F, std::memory_order_relaxed);
        g_sampleDistributionPower.store(2.50F, std::memory_order_relaxed);
        g_occluderThickness.store(8.0F, std::memory_order_relaxed);
        g_finalValuePower.store(2.70F, std::memory_order_relaxed);
        g_depthFadeEnabled.store(true, std::memory_order_relaxed);
        g_depthFadeStart.store(40000.0F, std::memory_order_relaxed);
        g_depthFadeEnd.store(50000.0F, std::memory_order_relaxed);
        logger::info("[GTAO] settings restored to defaults (radius {:.0f} units = 1.0 m, the "
                     "engine's own HBAO scale)",
            static_cast<double>(kGameUnitsPerMetre));
    }

    void SetDepthFadeEnabled(bool a_enabled) noexcept
    {
        if (g_depthFadeEnabled.exchange(a_enabled, std::memory_order_relaxed) != a_enabled) {
            logger::info("[GTAO] distance fade {}", a_enabled
                    ? "ON — AO fades out over the far band"
                    : "OFF — AO runs at every distance");
        }
    }

    void SetDepthFadeRange(float a_start, float a_end) noexcept
    {
        const float start = std::clamp(a_start, 0.0F, 200000.0F);
        g_depthFadeStart.store(start, std::memory_order_relaxed);
        g_depthFadeEnd.store(std::max(a_end, start + 1.0F), std::memory_order_relaxed);
    }

    bool Execute() noexcept
    {
        if (g_failed.load(std::memory_order_relaxed)) {
            return false;
        }
        try {
            auto* const data = RE::BSGraphics::RendererData::GetSingleton();
            if (data == nullptr || data->device == nullptr || data->context == nullptr) {
                return false;
            }
            auto* const context = data->context;

            const Platform::OutputMergerUnbindScope omScope(context);

            auto& depthTarget = data->depthStencilTargets[kMainDepthIndex];
            if (depthTarget.texture == nullptr || depthTarget.srViewDepth == nullptr) {
                return false;
            }
            D3D11_TEXTURE2D_DESC depthDesc{};
            depthTarget.texture->GetDesc(&depthDesc);

            if (!CreateShadersOnce(data->device) ||
                !EnsureSizedResources(data->device, depthDesc.Width, depthDesc.Height)) {
                return false;
            }

            XeGTAO::GTAOSettings settings{};
            settings.QualityLevel = static_cast<int>(g_quality.load(std::memory_order_relaxed));
            settings.DenoisePasses =
                static_cast<int>(g_denoisePasses.load(std::memory_order_relaxed));
            settings.Radius = g_radius.load(std::memory_order_relaxed);
            settings.RadiusMultiplier = g_radiusMultiplier.load(std::memory_order_relaxed);
            settings.FalloffRange = g_falloffRange.load(std::memory_order_relaxed);
            settings.SampleDistributionPower =
                g_sampleDistributionPower.load(std::memory_order_relaxed);
            settings.ThinOccluderCompensation =
                g_occluderThickness.load(std::memory_order_relaxed);
            settings.FinalValuePower = g_finalValuePower.load(std::memory_order_relaxed);

            const Platform::CameraConstants camera = Platform::Fallout4Renderer::CameraSnapshot();
            static std::atomic<bool> s_everValid{ false };
            if (!Platform::AreCameraConstantsValid(camera)) {
                static std::atomic<bool> loggedInvalid{ false };
                if (!loggedInvalid.exchange(true, std::memory_order_relaxed)) {
                    if (s_everValid.load(std::memory_order_relaxed)) {
                        logger::warn(
                            "[GTAO] camera constants invalid (zero/non-finite projection) — skipping "
                            "GTAO; the slot falls back to the neutral fill. Repeats are silent.");
                    } else {
                        logger::info(
                            "[GTAO] no camera yet (startup) — GTAO waits; the slot holds the neutral fill until the "
                            "first valid camera. Repeats are silent.");
                    }
                }
                return false;
            }
            s_everValid.store(true, std::memory_order_relaxed);
            float proj[16]{};
            std::memcpy(proj, camera.viewToClip, sizeof(proj));

            XeGTAO::GTAOConstants consts{};
            XeGTAO::GTAOUpdateConstants(consts, static_cast<int>(depthDesc.Width),
                static_cast<int>(depthDesc.Height), settings, proj,  true,
                 0U);

            static std::atomic<bool> constsLogged{ false };
            if (!constsLogged.exchange(true, std::memory_order_relaxed)) {
                logger::info(
                    "[GTAO] derived constants: tanHalfFOV=({:.4f},{:.4f}) NDCToViewMul=({:.4f},"
                    "{:.4f}) NDCToViewMul_x_PixelSize=({:.8f},{:.8f}) depthUnpack=({:.4f},{:.4f}) "
                    "| near={:.2f} far={:.1f} fovV={:.4f}rad | at viewspaceZ=1000 a radius of {:.1f} "
                    "covers {:.1f} px",
                    static_cast<double>(consts.CameraTanHalfFOV.x),
                    static_cast<double>(consts.CameraTanHalfFOV.y),
                    static_cast<double>(consts.NDCToViewMul.x),
                    static_cast<double>(consts.NDCToViewMul.y),
                    static_cast<double>(consts.NDCToViewMul_x_PixelSize.x),
                    static_cast<double>(consts.NDCToViewMul_x_PixelSize.y),
                    static_cast<double>(consts.DepthUnpackConsts.x),
                    static_cast<double>(consts.DepthUnpackConsts.y),
                    static_cast<double>(camera.nearPlane), static_cast<double>(camera.farPlane),
                    static_cast<double>(camera.fovVertical),
                    static_cast<double>(consts.EffectRadius),
                    static_cast<double>(consts.EffectRadius /
                        (1000.0F * std::max(consts.NDCToViewMul_x_PixelSize.x, 1e-9F))));
            }

            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (FAILED(context->Map(g_constants, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
                return false;
            }
            std::memcpy(mapped.pData, &consts, sizeof(consts));
            context->Unmap(g_constants, 0);

            ID3D11ShaderResourceView* nullSrv[2]{};
            ID3D11UnorderedAccessView* nullUav[5]{};

            const std::uint32_t w = depthDesc.Width;
            const std::uint32_t h = depthDesc.Height;

            float depthFadeMul = 0.0F;
            float depthFadeAdd = 1.0F;
            if (g_depthFadeEnabled.load(std::memory_order_relaxed)) {
                const float fadeStart = g_depthFadeStart.load(std::memory_order_relaxed);
                const float fadeEnd = g_depthFadeEnd.load(std::memory_order_relaxed);
                if (fadeEnd > fadeStart) {
                    const float span = fadeEnd - fadeStart;
                    depthFadeMul = -1.0F / span;
                    depthFadeAdd = fadeEnd / span;
                }
            }

            D3D11_MAPPED_SUBRESOURCE extensionMap{};
            if (SUCCEEDED(context->Map(g_extensionConstants, 0, D3D11_MAP_WRITE_DISCARD, 0,
                    &extensionMap))) {
                const float extension[4]{ g_minScreenRadius.load(std::memory_order_relaxed),
                    depthFadeMul, depthFadeAdd, 0.0F };
                std::memcpy(extensionMap.pData, extension, sizeof(extension));
                context->Unmap(g_extensionConstants, 0);
            }

            context->CSSetSamplers(0, 1, &g_pointClamp);

            auto& normalTarget = data->renderTargets[kGBufferNormalIndex];
            ID3D11ShaderResourceView* const normalSrv = normalTarget.srView;
            {
                const DirectX::XMMATRIX projMatrix = DirectX::XMLoadFloat4x4(
                    reinterpret_cast<const DirectX::XMFLOAT4X4*>(camera.viewToClip));
                DirectX::XMVECTOR determinant{};
                const DirectX::XMMATRIX invProj = DirectX::XMMatrixInverse(&determinant, projMatrix);
                struct DecodeConstants
                {
                    float invProj[16];
                    float rcpFrameDim[2];
                    float frameDim[2];
                    float viewZSign;
                    float padding[3];
                } decodeConstants{};
                DirectX::XMStoreFloat4x4(
                    reinterpret_cast<DirectX::XMFLOAT4X4*>(decodeConstants.invProj), invProj);
                decodeConstants.rcpFrameDim[0] = 1.0F / static_cast<float>(w);
                decodeConstants.rcpFrameDim[1] = 1.0F / static_cast<float>(h);
                decodeConstants.frameDim[0] = static_cast<float>(w);
                decodeConstants.frameDim[1] = static_cast<float>(h);
                decodeConstants.viewZSign = camera.viewToClip[11] < 0.0F ? -1.0F : 1.0F;

                D3D11_MAPPED_SUBRESOURCE decodeMap{};
                if (FAILED(context->Map(g_decodeConstants, 0, D3D11_MAP_WRITE_DISCARD, 0,
                        &decodeMap))) {
                    return false;
                }
                std::memcpy(decodeMap.pData, &decodeConstants, sizeof(decodeConstants));
                context->Unmap(g_decodeConstants, 0);

                ID3D11ShaderResourceView* decodeSrvs[2]{ depthTarget.srViewDepth, normalSrv };
                ID3D11UnorderedAccessView* decodeUavs[2]{ g_linearDepthUav, g_normalsUav };
                context->CSSetShader(g_csDecode, nullptr, 0);
                context->CSSetConstantBuffers(0, 1, &g_decodeConstants);
                context->CSSetShaderResources(0, 2, decodeSrvs);
                context->CSSetUnorderedAccessViews(0, 2, decodeUavs, nullptr);
                context->Dispatch((w + 7U) / 8U, (h + 7U) / 8U, 1U);
                context->CSSetUnorderedAccessViews(0, 2, nullUav, nullptr);
                context->CSSetShaderResources(0, 2, nullSrv);
            }

            context->CSSetConstantBuffers(0, 1, &g_constants);
            context->CSSetConstantBuffers(2, 1, &g_extensionConstants);

            if (normalSrv == nullptr) {
                context->CSSetShader(g_csNormals, nullptr, 0);
                context->CSSetShaderResources(0, 1, &depthTarget.srViewDepth);
                context->CSSetUnorderedAccessViews(0, 1, &g_normalsUav, nullptr);
                context->Dispatch((w + 7U) / 8U, (h + 7U) / 8U, 1U);
                context->CSSetUnorderedAccessViews(0, 1, nullUav, nullptr);
                context->CSSetShaderResources(0, 1, nullSrv);
            }
            if (g_engineNormalsAvailable.exchange(normalSrv != nullptr,
                    std::memory_order_relaxed) != (normalSrv != nullptr)) {
                logger::info("[GTAO] normals source: {}", normalSrv != nullptr
                        ? "the ENGINE's G-buffer (correct at silhouettes)"
                        : "DERIVED FROM DEPTH — G-buffer target absent; expect edge halos");
            }

            context->CSSetShader(g_csPrefilter, nullptr, 0);
            context->CSSetShaderResources(0, 1, &g_linearDepthSrv);
            context->CSSetUnorderedAccessViews(0, 5, g_workingDepthMipUav, nullptr);
            context->Dispatch((w + 15U) / 16U, (h + 15U) / 16U, 1U);
            context->CSSetUnorderedAccessViews(0, 5, nullUav, nullptr);
            context->CSSetShaderResources(0, 1, nullSrv);

            context->CSSetSamplers(0, 1, &g_pointBorderFar);
            context->CSSetShader(g_csMain[g_quality.load(std::memory_order_relaxed)], nullptr, 0);
            ID3D11ShaderResourceView* mainSrvs[2]{ g_workingDepthSrv, g_normalsSrv };
            context->CSSetShaderResources(0, 2, mainSrvs);
            ID3D11UnorderedAccessView* mainUavs[2]{ g_aoTermAUav, g_edgesUav };
            context->CSSetUnorderedAccessViews(0, 2, mainUavs, nullptr);
            context->Dispatch((w + 7U) / 8U, (h + 7U) / 8U, 1U);
            context->CSSetUnorderedAccessViews(0, 2, nullUav, nullptr);
            context->CSSetShaderResources(0, 2, nullSrv);
            context->CSSetSamplers(0, 1, &g_pointClamp);

            const std::uint32_t passes =
                std::max(1U, g_denoisePasses.load(std::memory_order_relaxed));
            struct AoBuffer
            {
                ID3D11ShaderResourceView* srv;
                ID3D11UnorderedAccessView* uav;
                ID3D11Texture2D* texture;
            };
            AoBuffer bufferA{ g_aoTermASrv, g_aoTermAUav, g_aoTermA };
            AoBuffer bufferB{ g_aoTermBSrv, g_aoTermBUav, g_aoTermB };
            AoBuffer* src = &bufferA;
            AoBuffer* dst = &bufferB;
            for (std::uint32_t pass = 0; pass < passes; ++pass) {
                const bool last = pass + 1U == passes;
                context->CSSetShader(last ? g_csDenoiseLastPass : g_csDenoisePass, nullptr, 0);
                ID3D11ShaderResourceView* denoiseSrvs[2]{ src->srv, g_edgesSrv };
                context->CSSetShaderResources(0, 2, denoiseSrvs);
                context->CSSetUnorderedAccessViews(0, 1, &dst->uav, nullptr);
                context->Dispatch((w + 15U) / 16U, (h + 7U) / 8U, 1U);
                context->CSSetUnorderedAccessViews(0, 1, nullUav, nullptr);
                context->CSSetShaderResources(0, 2, nullSrv);
                std::swap(src, dst);
            }
            ID3D11ShaderResourceView* const finalSrv = src->srv;
            g_finalAoSrv = finalSrv;

            g_frames.fetch_add(1, std::memory_order_relaxed);
            static std::atomic<bool> loggedFirst{ false };
            if (!loggedFirst.exchange(true, std::memory_order_relaxed)) {
                logger::info("[GTAO] ★ first frame produced: {}x{}, quality {}, {} denoise pass(es) "
                             "— computed AFTER the geometry pass, output-merger unbound and "
                             "restored. XeGTAO is live in the slot.",
                    w, h, g_quality.load(std::memory_order_relaxed), passes);
            }
            return true;
        } catch (...) {
            if (!g_failed.exchange(true, std::memory_order_relaxed)) {
                logger::warn("[GTAO] Execute threw — GTAO disabled for the session; the router "
                             "falls back to the constant fill.");
            }
            return false;
        }
    }

    State Snapshot() noexcept
    {
        State state{};
        state.quality = g_quality.load(std::memory_order_relaxed);
        state.denoisePasses = g_denoisePasses.load(std::memory_order_relaxed);
        state.radius = g_radius.load(std::memory_order_relaxed);
        state.radiusMultiplier = g_radiusMultiplier.load(std::memory_order_relaxed);
        state.falloffRange = g_falloffRange.load(std::memory_order_relaxed);
        state.sampleDistributionPower = g_sampleDistributionPower.load(std::memory_order_relaxed);
        state.occluderThickness =
            g_occluderThickness.load(std::memory_order_relaxed);
        state.finalValuePower = g_finalValuePower.load(std::memory_order_relaxed);
        state.shadersReady = g_shadersReady.load(std::memory_order_relaxed);
        state.width = g_width.load(std::memory_order_relaxed);
        state.height = g_height.load(std::memory_order_relaxed);
        state.frames = g_frames.load(std::memory_order_relaxed);
        state.minScreenRadius = g_minScreenRadius.load(std::memory_order_relaxed);
        state.usingEngineNormals = g_engineNormalsAvailable.load(std::memory_order_relaxed);
        state.depthFadeEnabled = g_depthFadeEnabled.load(std::memory_order_relaxed);
        state.depthFadeStart = g_depthFadeStart.load(std::memory_order_relaxed);
        state.depthFadeEnd = g_depthFadeEnd.load(std::memory_order_relaxed);
        return state;
    }
}
