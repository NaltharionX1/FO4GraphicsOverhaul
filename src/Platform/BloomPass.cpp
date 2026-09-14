#include "PCH.h"

#include "Platform/BloomPass.h"

#include "Platform/FrameBufferResolve.h"
#include "Platform/OutputMergerScope.h"
#include "Platform/RendererContracts.h"

#include "RE/Bethesda/BSGraphics.h"

#include <d3d11.h>

#include <algorithm>
#include <atomic>
#include <cstring>

#include "CSBloomPrefilter.h"
#include "CSBloomDown.h"
#include "CSBloomUp.h"
#include "CSBloomComposite.h"

namespace
{
    constexpr std::uint32_t kFrameBufferIndex = static_cast<std::uint32_t>(Platform::Fallout4RenderTargetIndex::kFrameBuffer);
    constexpr std::uint32_t kRenderTargetCount = 101;
    constexpr std::uint32_t kChainLevels = 5;

    std::atomic<bool> g_failed{ false };

    std::atomic<float> g_threshold{ 0.75F };
    std::atomic<float> g_strength{ 0.0F };
    std::atomic<float> g_radius{ 1.0F };

    ID3D11ComputeShader* g_prefilterShader = nullptr;
    ID3D11ComputeShader* g_downShader = nullptr;
    ID3D11ComputeShader* g_upShader = nullptr;
    ID3D11ComputeShader* g_compositeShader = nullptr;
    ID3D11Buffer* g_constants = nullptr;
    ID3D11SamplerState* g_linearClamp = nullptr;

    ID3D11Texture2D* g_chain[kChainLevels]{};
    ID3D11ShaderResourceView* g_chainSrv[kChainLevels]{};
    ID3D11UnorderedAccessView* g_chainUav[kChainLevels]{};
    ID3D11Texture2D* g_accum[kChainLevels]{};
    ID3D11ShaderResourceView* g_accumSrv[kChainLevels]{};
    ID3D11UnorderedAccessView* g_accumUav[kChainLevels]{};
    std::uint32_t g_levelW[kChainLevels]{};
    std::uint32_t g_levelH[kChainLevels]{};

    ID3D11Texture2D* g_scratch = nullptr;
    ID3D11UnorderedAccessView* g_scratchUav = nullptr;
    ID3D11ShaderResourceView* g_sourceSrv = nullptr;
    ID3D11Texture2D* g_sourceSrvTexture = nullptr;
    std::uint32_t g_width = 0;
    std::uint32_t g_height = 0;
    DXGI_FORMAT g_format = DXGI_FORMAT_UNKNOWN;

    struct BloomConstants
    {
        std::uint32_t srcW, srcH, dstW, dstH;
        float p0, p1, p2, p3;
    };
    static_assert(sizeof(BloomConstants) == 32);

    void Fail(const char* a_what) noexcept
    {
        if (!g_failed.exchange(true, std::memory_order_relaxed)) {
            logger::warn("[Bloom] {} — the pass is OFF for the session; the game's own frame is "
                         "untouched (fail-open).",
                a_what);
        }
    }

    [[nodiscard]] bool CreateShadersOnce(ID3D11Device* a_device) noexcept
    {
        if (g_prefilterShader != nullptr && g_downShader != nullptr && g_upShader != nullptr &&
            g_compositeShader != nullptr && g_constants != nullptr && g_linearClamp != nullptr) {
            return true;
        }
        struct Entry
        {
            ID3D11ComputeShader** slot;
            const BYTE* bytecode;
            std::size_t size;
            const char* what;
        };
        const Entry entries[]{
            { &g_prefilterShader, g_csBloomPrefilter, sizeof(g_csBloomPrefilter),
                "CreateComputeShader failed for the bloom prefilter" },
            { &g_downShader, g_csBloomDown, sizeof(g_csBloomDown),
                "CreateComputeShader failed for the bloom downsample" },
            { &g_upShader, g_csBloomUp, sizeof(g_csBloomUp),
                "CreateComputeShader failed for the bloom upsample" },
            { &g_compositeShader, g_csBloomComposite, sizeof(g_csBloomComposite),
                "CreateComputeShader failed for the bloom composite" },
        };
        for (const auto& entry : entries) {
            if (*entry.slot == nullptr &&
                FAILED(a_device->CreateComputeShader(
                    entry.bytecode, entry.size, nullptr, entry.slot))) {
                Fail(entry.what);
                return false;
            }
        }
        if (g_constants == nullptr) {
            D3D11_BUFFER_DESC desc{};
            desc.ByteWidth = sizeof(BloomConstants);
            desc.Usage = D3D11_USAGE_DYNAMIC;
            desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            if (FAILED(a_device->CreateBuffer(&desc, nullptr, &g_constants))) {
                Fail("constant-buffer creation failed for the bloom pass");
                return false;
            }
        }
        if (g_linearClamp == nullptr) {
            D3D11_SAMPLER_DESC desc{};
            desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
            desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
            desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
            desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
            desc.MaxLOD = D3D11_FLOAT32_MAX;
            if (FAILED(a_device->CreateSamplerState(&desc, &g_linearClamp))) {
                Fail("linear-clamp sampler creation failed for the bloom pass");
                return false;
            }
        }
        return true;
    }

    void ReleaseSizedResources() noexcept
    {
        for (std::uint32_t i = 0; i < kChainLevels; ++i) {
            if (g_chainUav[i]) { g_chainUav[i]->Release(); g_chainUav[i] = nullptr; }
            if (g_chainSrv[i]) { g_chainSrv[i]->Release(); g_chainSrv[i] = nullptr; }
            if (g_chain[i]) { g_chain[i]->Release(); g_chain[i] = nullptr; }
            if (g_accumUav[i]) { g_accumUav[i]->Release(); g_accumUav[i] = nullptr; }
            if (g_accumSrv[i]) { g_accumSrv[i]->Release(); g_accumSrv[i] = nullptr; }
            if (g_accum[i]) { g_accum[i]->Release(); g_accum[i] = nullptr; }
        }
        if (g_scratchUav) { g_scratchUav->Release(); g_scratchUav = nullptr; }
        if (g_scratch) { g_scratch->Release(); g_scratch = nullptr; }
    }

    [[nodiscard]] bool EnsureSizedResources(
        ID3D11Device* a_device, const D3D11_TEXTURE2D_DESC& a_target) noexcept
    {
        if (g_scratch != nullptr && g_width == a_target.Width && g_height == a_target.Height &&
            g_format == a_target.Format) {
            return true;
        }
        ReleaseSizedResources();

        D3D11_TEXTURE2D_DESC scratchDesc = a_target;
        scratchDesc.Usage = D3D11_USAGE_DEFAULT;
        scratchDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        scratchDesc.CPUAccessFlags = 0;
        scratchDesc.MiscFlags = 0;
        if (FAILED(a_device->CreateTexture2D(&scratchDesc, nullptr, &g_scratch))) {
            Fail("bloom scratch texture creation failed");
            return false;
        }
        if (FAILED(a_device->CreateUnorderedAccessView(g_scratch, nullptr, &g_scratchUav))) {
            Fail("UAV creation on the bloom scratch failed");
            return false;
        }

        std::uint32_t w = a_target.Width;
        std::uint32_t h = a_target.Height;
        for (std::uint32_t i = 0; i < kChainLevels; ++i) {
            w = std::max(1U, w / 2U);
            h = std::max(1U, h / 2U);
            g_levelW[i] = w;
            g_levelH[i] = h;
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = w;
            desc.Height = h;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
            if (FAILED(a_device->CreateTexture2D(&desc, nullptr, &g_chain[i])) ||
                FAILED(a_device->CreateShaderResourceView(g_chain[i], nullptr, &g_chainSrv[i])) ||
                FAILED(a_device->CreateUnorderedAccessView(g_chain[i], nullptr, &g_chainUav[i])) ||
                FAILED(a_device->CreateTexture2D(&desc, nullptr, &g_accum[i])) ||
                FAILED(a_device->CreateShaderResourceView(g_accum[i], nullptr, &g_accumSrv[i])) ||
                FAILED(a_device->CreateUnorderedAccessView(g_accum[i], nullptr, &g_accumUav[i]))) {
                Fail("bloom chain texture/view creation failed");
                return false;
            }
        }

        g_width = a_target.Width;
        g_height = a_target.Height;
        g_format = a_target.Format;
        return true;
    }

    [[nodiscard]] bool EnsureSourceSrv(
        ID3D11Device* a_device, ID3D11Texture2D* a_texture,
        ID3D11ShaderResourceView* a_engineSrv) noexcept
    {
        if (a_engineSrv != nullptr) {
            return true;
        }
        if (g_sourceSrv != nullptr && g_sourceSrvTexture == a_texture) {
            return true;
        }
        if (g_sourceSrv) { g_sourceSrv->Release(); g_sourceSrv = nullptr; }
        if (FAILED(a_device->CreateShaderResourceView(a_texture, nullptr, &g_sourceSrv))) {
            Fail("SRV creation on the engine's frame buffer failed for the bloom pass");
            return false;
        }
        g_sourceSrvTexture = a_texture;
        return true;
    }

    [[nodiscard]] bool UploadConstants(
        ID3D11DeviceContext* a_context, const BloomConstants& a_values) noexcept
    {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(a_context->Map(g_constants, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            return false;
        }
        std::memcpy(mapped.pData, &a_values, sizeof(a_values));
        a_context->Unmap(g_constants, 0);
        return true;
    }

    void Dispatch(ID3D11DeviceContext* a_context, ID3D11ComputeShader* a_shader,
        ID3D11ShaderResourceView* a_srv0, ID3D11ShaderResourceView* a_srv1,
        ID3D11UnorderedAccessView* a_uav, std::uint32_t a_width, std::uint32_t a_height) noexcept
    {
        ID3D11ShaderResourceView* srvs[2]{ a_srv0, a_srv1 };
        a_context->CSSetShader(a_shader, nullptr, 0);
        a_context->CSSetShaderResources(0, 2, srvs);
        a_context->CSSetUnorderedAccessViews(0, 1, &a_uav, nullptr);
        a_context->Dispatch((a_width + 7U) / 8U, (a_height + 7U) / 8U, 1U);

        ID3D11UnorderedAccessView* nullUav[1]{};
        ID3D11ShaderResourceView* nullSrv[2]{};
        a_context->CSSetUnorderedAccessViews(0, 1, nullUav, nullptr);
        a_context->CSSetShaderResources(0, 2, nullSrv);
    }
}

namespace Platform::BloomPass
{
    void ExecuteAfterEffectRange() noexcept
    {
        const float strength = g_strength.load(std::memory_order_relaxed);
        if (strength <= 0.0F || g_failed.load(std::memory_order_relaxed)) {
            return;
        }
        try {
            auto* const data = RE::BSGraphics::RendererData::GetSingleton();
            if (data == nullptr || data->context == nullptr || data->device == nullptr) {
                return;
            }
            auto* const device = reinterpret_cast<ID3D11Device*>(data->device);
            auto* const context = data->context;
            auto& target = data->renderTargets[kFrameBufferIndex];
            static_assert(kFrameBufferIndex < kRenderTargetCount);
            Platform::ResolvedFrameBuffer frame{};
            Platform::ResolveFrameBufferTexture(target, frame);
            if (frame.texture == nullptr) {
                return;
            }
            D3D11_TEXTURE2D_DESC desc{};
            frame.texture->GetDesc(&desc);
            if (!CreateShadersOnce(device) || !EnsureSizedResources(device, desc) ||
                !EnsureSourceSrv(device, frame.texture, target.srView)) {
                return;
            }

            const float threshold = g_threshold.load(std::memory_order_relaxed);
            const float radius = g_radius.load(std::memory_order_relaxed);
            ID3D11ShaderResourceView* frameSrv =
                target.srView != nullptr ? target.srView : g_sourceSrv;

            const Platform::OutputMergerUnbindScope omScope(context);
            context->CSSetConstantBuffers(0, 1, &g_constants);
            context->CSSetSamplers(0, 1, &g_linearClamp);

            if (!UploadConstants(context,
                    { desc.Width, desc.Height, g_levelW[0], g_levelH[0],
                        threshold, 0.5F, 0.0F, 0.0F })) {
                return;
            }
            Dispatch(context, g_prefilterShader, frameSrv, nullptr, g_chainUav[0],
                g_levelW[0], g_levelH[0]);

            for (std::uint32_t i = 0; i + 1 < kChainLevels; ++i) {
                if (!UploadConstants(context,
                        { g_levelW[i], g_levelH[i], g_levelW[i + 1], g_levelH[i + 1],
                            0.0F, 0.0F, 0.0F, 0.0F })) {
                    return;
                }
                Dispatch(context, g_downShader, g_chainSrv[i], nullptr, g_chainUav[i + 1],
                    g_levelW[i + 1], g_levelH[i + 1]);
            }

            context->CopyResource(g_accum[kChainLevels - 1], g_chain[kChainLevels - 1]);

            for (std::uint32_t i = kChainLevels - 1; i > 0; --i) {
                const std::uint32_t level = i - 1;
                if (!UploadConstants(context,
                        { g_levelW[i], g_levelH[i], g_levelW[level], g_levelH[level],
                            radius, 0.0F, 0.0F, 0.0F })) {
                    return;
                }
                Dispatch(context, g_upShader, g_accumSrv[i], g_chainSrv[level],
                    g_accumUav[level], g_levelW[level], g_levelH[level]);
            }

            if (!UploadConstants(context,
                    { g_levelW[0], g_levelH[0], desc.Width, desc.Height,
                        strength, 0.0F, 0.0F, 0.0F })) {
                return;
            }
            Dispatch(context, g_compositeShader, frameSrv, g_accumSrv[0], g_scratchUav,
                desc.Width, desc.Height);
            context->CSSetShader(nullptr, nullptr, 0);
            context->CopyResource(frame.texture, g_scratch);

            static std::atomic<bool> s_loggedFirst{ false };
            if (!s_loggedFirst.exchange(true, std::memory_order_relaxed)) {
                logger::info("[Bloom] ★ first bloomed frame: {}x{} format={} — our display-"
                             "referred glow over the engine's own output ({} chain levels from "
                             "half-res); the engine's bloom and values are untouched.",
                    desc.Width, desc.Height, static_cast<int>(desc.Format), kChainLevels);
            }
        } catch (...) {
            Fail("C++ exception in the bloom pass");
        }
    }

    void SetParam(std::uint32_t a_argIndex, float a_value) noexcept
    {
        switch (a_argIndex) {
        case 0:
            g_threshold.store(std::clamp(a_value, 0.0F, 1.0F), std::memory_order_relaxed);
            return;
        case 1:
            g_strength.store(std::clamp(a_value, 0.0F, 2.0F), std::memory_order_relaxed);
            return;
        case 2:
            g_radius.store(std::clamp(a_value, 0.5F, 2.0F), std::memory_order_relaxed);
            return;
        default:
            return;
        }
    }
}
