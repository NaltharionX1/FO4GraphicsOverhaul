#include "PCH.h"

#include "Platform/GradingPass.h"

#include "Platform/FrameBufferResolve.h"
#include "Platform/OutputMergerScope.h"
#include "Platform/RendererContracts.h"

#include "RE/Bethesda/BSGraphics.h"

#include <d3d11.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>

#include "CSGrading.h"

namespace
{
    constexpr std::uint32_t kFrameBufferIndex = static_cast<std::uint32_t>(Platform::Fallout4RenderTargetIndex::kFrameBuffer);
    constexpr std::uint32_t kRenderTargetCount = 101;

    std::atomic<bool> g_enabled{ false };
    std::atomic<bool> g_failed{ false };
    std::atomic<std::uint64_t> g_framesGraded{ 0 };

    std::atomic<float> g_saturation{ 1.0F };
    std::atomic<float> g_brightness{ 1.0F };
    std::atomic<float> g_contrast{ 1.0F };
    std::atomic<float> g_tintR{ 0.0F };
    std::atomic<float> g_tintG{ 0.0F };
    std::atomic<float> g_tintB{ 0.0F };
    std::atomic<float> g_tintStrength{ 0.0F };

    ID3D11ComputeShader* g_shader = nullptr;
    ID3D11Buffer* g_constants = nullptr;
    ID3D11Texture2D* g_scratch = nullptr;
    ID3D11UnorderedAccessView* g_scratchUav = nullptr;
    ID3D11ShaderResourceView* g_sourceSrv = nullptr;
    ID3D11Texture2D* g_sourceSrvTexture = nullptr;
    std::uint32_t g_width = 0;
    std::uint32_t g_height = 0;
    DXGI_FORMAT g_format = DXGI_FORMAT_UNKNOWN;

    void Fail(const char* a_what) noexcept
    {
        if (!g_failed.exchange(true, std::memory_order_relaxed)) {
            logger::warn("[Grading] {} — the pass is OFF for the session; the game's own frame is "
                         "untouched (fail-open).",
                a_what);
        }
    }

    [[nodiscard]] bool CreateShaderOnce(ID3D11Device* a_device) noexcept
    {
        if (g_shader != nullptr && g_constants != nullptr) {
            return true;
        }
        if (g_shader == nullptr &&
            FAILED(a_device->CreateComputeShader(
                g_csGrading, sizeof(g_csGrading), nullptr, &g_shader))) {
            Fail("CreateComputeShader failed for the grading pass");
            return false;
        }
        if (g_constants == nullptr) {
            D3D11_BUFFER_DESC desc{};
            desc.ByteWidth = 48;
            desc.Usage = D3D11_USAGE_DYNAMIC;
            desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            if (FAILED(a_device->CreateBuffer(&desc, nullptr, &g_constants))) {
                Fail("constant-buffer creation failed for the grading pass");
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool EnsureSizedResources(
        ID3D11Device* a_device, const D3D11_TEXTURE2D_DESC& a_target) noexcept
    {
        if (g_scratch != nullptr && g_width == a_target.Width && g_height == a_target.Height &&
            g_format == a_target.Format) {
            return true;
        }
        if (g_scratchUav) { g_scratchUav->Release(); g_scratchUav = nullptr; }
        if (g_scratch) { g_scratch->Release(); g_scratch = nullptr; }

        D3D11_TEXTURE2D_DESC desc = a_target;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        desc.CPUAccessFlags = 0;
        desc.MiscFlags = 0;
        if (FAILED(a_device->CreateTexture2D(&desc, nullptr, &g_scratch))) {
            Fail("scratch texture creation failed");
            return false;
        }
        if (FAILED(a_device->CreateUnorderedAccessView(g_scratch, nullptr, &g_scratchUav))) {
            Fail("UAV creation on the scratch failed (typed UAV store unsupported for this "
                 "format?) — field evidence for the draw-path fallback");
            return false;
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
            Fail("SRV creation on the engine's frame buffer failed");
            return false;
        }
        g_sourceSrvTexture = a_texture;
        return true;
    }

}

namespace Platform::GradingPass
{
    void ExecuteAfterEffectRange() noexcept
    {
        if (!g_enabled.load(std::memory_order_relaxed) ||
            g_failed.load(std::memory_order_relaxed)) {
            return;
        }
        try {
            auto* const data = RE::BSGraphics::RendererData::GetSingleton();
            if (data == nullptr || data->context == nullptr || data->device == nullptr) {
                static std::atomic<bool> s_loggedNoRenderer{ false };
                if (!s_loggedNoRenderer.exchange(true, std::memory_order_relaxed)) {
                    logger::warn(
                        "[Grading] armed, but the renderer singleton is not usable (data={} "
                        "context={} device={}) — the pass idles; one-shot line",
                        fmt::ptr(data), fmt::ptr(data != nullptr ? data->context : nullptr),
                        fmt::ptr(data != nullptr ? static_cast<void*>(data->device) : nullptr));
                }
                return;
            }
            auto* const device = reinterpret_cast<ID3D11Device*>(data->device);
            auto* const context = data->context;
            auto& target = data->renderTargets[kFrameBufferIndex];
            static_assert(kFrameBufferIndex < kRenderTargetCount);
            Platform::ResolvedFrameBuffer frame{};
            Platform::ResolveFrameBufferTexture(target, frame);
            if (frame.texture == nullptr) {
                static std::atomic<bool> s_loggedNoTarget{ false };
                if (!s_loggedNoTarget.exchange(true, std::memory_order_relaxed)) {
                    const auto& swapTarget = data->renderWindow[0].swapChainRenderTarget;
                    logger::warn(
                        "[Grading] armed, but renderTargets[{}] has no texture and no view that "
                        "resolves to one (rtView={} srView={} uaView={}); "
                        "renderWindow[0].swapChainRenderTarget: texture={} rtView={} srView={} "
                        "swapChain={} — frame-buffer identity needs re-derivation on this "
                        "install; the pass idles (one-shot line)",
                        kFrameBufferIndex, fmt::ptr(target.rtView), fmt::ptr(target.srView),
                        fmt::ptr(target.uaView), fmt::ptr(swapTarget.texture),
                        fmt::ptr(swapTarget.rtView), fmt::ptr(swapTarget.srView),
                        fmt::ptr(data->renderWindow[0].swapChain));
                }
                return;
            }
            D3D11_TEXTURE2D_DESC desc{};
            frame.texture->GetDesc(&desc);
            if (!CreateShaderOnce(device) || !EnsureSizedResources(device, desc) ||
                !EnsureSourceSrv(device, frame.texture, target.srView)) {
                return;
            }

            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (FAILED(context->Map(g_constants, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
                return;
            }
            struct Constants
            {
                float saturation, brightness, contrast, tintStrength;
                float tintR, tintG, tintB, pad0;
                std::uint32_t width, height, pad1, pad2;
            } constants{
                g_saturation.load(std::memory_order_relaxed),
                g_brightness.load(std::memory_order_relaxed),
                g_contrast.load(std::memory_order_relaxed),
                g_tintStrength.load(std::memory_order_relaxed),
                g_tintR.load(std::memory_order_relaxed),
                g_tintG.load(std::memory_order_relaxed),
                g_tintB.load(std::memory_order_relaxed),
                0.0F,
                desc.Width, desc.Height, 0U, 0U
            };
            static_assert(sizeof(Constants) == 48);
            std::memcpy(mapped.pData, &constants, sizeof(constants));
            context->Unmap(g_constants, 0);

            const Platform::OutputMergerUnbindScope omScope(context);

            ID3D11ShaderResourceView* srv =
                target.srView != nullptr ? target.srView : g_sourceSrv;
            context->CSSetShader(g_shader, nullptr, 0);
            context->CSSetConstantBuffers(0, 1, &g_constants);
            context->CSSetShaderResources(0, 1, &srv);
            context->CSSetUnorderedAccessViews(0, 1, &g_scratchUav, nullptr);
            context->Dispatch((desc.Width + 7U) / 8U, (desc.Height + 7U) / 8U, 1U);

            ID3D11UnorderedAccessView* nullUav[1]{};
            ID3D11ShaderResourceView* nullSrv[1]{};
            context->CSSetUnorderedAccessViews(0, 1, nullUav, nullptr);
            context->CSSetShaderResources(0, 1, nullSrv);
            context->CSSetShader(nullptr, nullptr, 0);

            context->CopyResource(frame.texture, g_scratch);

            g_framesGraded.fetch_add(1, std::memory_order_relaxed);
            static std::atomic<bool> s_loggedFirst{ false };
            if (!s_loggedFirst.exchange(true, std::memory_order_relaxed)) {
                logger::info("[Grading] ★ first graded frame: {}x{} format={} — display-referred "
                             "grading over the engine's own per-weather output; the engine's "
                             "values were not written and never will be by this pass.",
                    desc.Width, desc.Height, static_cast<int>(desc.Format));
            }
        } catch (...) {
            Fail("C++ exception in the grading pass");
        }
    }

    bool Enabled() noexcept
    {
        return g_enabled.load(std::memory_order_relaxed);
    }

    void SetEnabled(bool a_enabled) noexcept
    {
        if (g_enabled.exchange(a_enabled, std::memory_order_relaxed) != a_enabled) {
            if (a_enabled) {
                logger::info("[Grading] ON — our pass grades the displayed image after the "
                             "engine's own per-weather grading; the engine's values are never "
                             "written");
            } else {
                logger::info("[Grading] off after {} graded frame(s) this session — the pass is "
                             "no longer dispatched; the frame is vanilla again immediately",
                    g_framesGraded.load(std::memory_order_relaxed));
            }
        }
    }

    void SetParam(const char* a_command, std::uint32_t a_argIndex, float a_value) noexcept
    {
        if (a_command == nullptr) {
            return;
        }
        const float value = std::clamp(a_value, 0.0F, 10.0F);
        if (std::strcmp(a_command, "scp") == 0) {
            switch (a_argIndex) {
            case 0: g_saturation.store(value, std::memory_order_relaxed); return;
            case 1: g_brightness.store(value, std::memory_order_relaxed); return;
            case 2: g_contrast.store(value, std::memory_order_relaxed); return;
            default: return;
            }
        }
        if (std::strcmp(a_command, "stp") == 0) {
            switch (a_argIndex) {
            case 0: g_tintR.store(value, std::memory_order_relaxed); return;
            case 1: g_tintG.store(value, std::memory_order_relaxed); return;
            case 2: g_tintB.store(value, std::memory_order_relaxed); return;
            case 3: g_tintStrength.store(value, std::memory_order_relaxed); return;
            default: return;
            }
        }
    }
}
