// SPDX-License-Identifier: GPL-3.0-or-later
// Portions adapted from Community Shaders for Fallout 4 (northaxosky), GPL-3.0.

#include "PCH.h"

#include "Platform/FsrEngine.h"

#include "Platform/OpaqueCapture.h"
#include "Platform/SidecarGuides.h"

#include "CSFsrTransparency.h"

#include "Platform/DlaaSettings.h"
#include "Platform/Fallout4Renderer.h"

#include <atomic>
#include <new>

#pragma warning(push, 0)
#include <FidelityFX/host/backends/dx11/ffx_dx11.h>
#include <FidelityFX/host/ffx_fsr3.h>
#pragma warning(pop)

extern "C" {
    extern volatile long g_ffxDx11TexUavCreateCalls;
    extern volatile long g_ffxDx11TexUavCreateFails;
}

namespace
{
    struct OutputKey
    {
        std::uint32_t outW{ 0 };
        std::uint32_t outH{ 0 };
        DXGI_FORMAT format{ DXGI_FORMAT_UNKNOWN };
        bool hdr{ false };

        friend constexpr bool operator==(const OutputKey&, const OutputKey&) = default;
    };

    std::atomic<ID3D11Device*> g_device{ nullptr };
    std::atomic<bool> g_pathAvailable{ false };

    void* g_scratch{ nullptr };
    FfxInterface g_interface{};
    bool g_backendReady{ false };
    std::atomic<bool> g_backendPoisoned{ false };
    std::atomic<bool> g_poisonLogged{ false };

    FfxFsr3Context* g_context{ nullptr };
    OutputKey g_ctxKey{};

    ID3D11Texture2D* g_fsrOut{ nullptr };
    OutputKey g_fsrOutKey{};

    std::uint64_t g_frameId{ 0 };
    std::uint32_t g_lastDispatchOutW{ 0 };
    std::uint32_t g_lastDispatchOutH{ 0 };
    std::uint32_t g_lastDispatchRenderW{ 0 };
    std::uint32_t g_lastDispatchRenderH{ 0 };

    std::atomic<float> g_velocityFactor{ 1.0F };
    float g_appliedVelocityFactor{ -1.0F };
    ID3D11Texture2D* g_reactiveMask{ nullptr };
    std::uint32_t g_reactiveW{ 0 };
    std::uint32_t g_reactiveH{ 0 };
    ID3D11Texture2D* g_transparencyMask{ nullptr };
    ID3D11UnorderedAccessView* g_transparencyUav{ nullptr };
    ID3D11ShaderResourceView* g_opaqueSrv{ nullptr };
    ID3D11ShaderResourceView* g_finalSrv{ nullptr };
    ID3D11Texture2D* g_opaqueSrvOwner{ nullptr };
    ID3D11Texture2D* g_finalSrvOwner{ nullptr };
    ID3D11ComputeShader* g_transparencyCs{ nullptr };
    ID3D11Buffer* g_transparencyCb{ nullptr };
    std::uint32_t g_transparencyW{ 0 };
    std::uint32_t g_transparencyH{ 0 };

    std::atomic<bool> g_masksEnabled{ true };
    bool g_masksRefused{ false };
    std::atomic<float> g_transparencyScale{ 1.0F };

    void FreeTransparency() noexcept;
    void FreeMaskViews() noexcept;

    std::atomic<float> g_reactiveness{ 1.0F };
    std::atomic<float> g_shadingChange{ 1.0F };
    std::atomic<float> g_accumulation{ 0.333F };
    std::atomic<float> g_minDisocclusion{ -0.333F };
    float g_appliedReactiveness{ -999.0F };
    float g_appliedShadingChange{ -999.0F };
    float g_appliedAccumulation{ -999.0F };
    float g_appliedMinDisocclusion{ -999.0F };

    bool g_failed{ false };
    OutputKey g_failedKey{};
    std::atomic<bool> g_rearmRequested{ false };

    constexpr std::uint32_t kSettleFrames = 6;
    OutputKey g_pendingKey{};
    std::uint32_t g_pendingStable{ 0 };

    void MarkFailure(const OutputKey& a_key) noexcept
    {
        g_failed = true;
        g_failedKey = a_key;
    }

    inline constexpr std::uint32_t kAutoReactiveUseComponentsMax = 8U;

    [[nodiscard]] FfxResource Wrap(ID3D11Resource* a_res, FfxResourceStates a_state) noexcept
    {
        FfxResource res{};
        res.resource = a_res;
        res.description = GetFfxResourceDescriptionDX11(a_res);
        res.state = a_state;
        return res;
    }

    void BackendFailureLog(const char* a_message) noexcept
    {
        logger::error("[FSR backend] {}", a_message != nullptr ? a_message : "(null)");
    }

    [[nodiscard]] bool EnsureBackend(const OutputKey& a_key) noexcept
    {
        if (g_backendReady) {
            return true;
        }
        ID3D11Device* const device = g_device.load(std::memory_order_acquire);
        if (device == nullptr) {
            return false;
        }
        const size_t scratchSize = ffxGetScratchMemorySizeDX11(1);
        g_scratch = calloc(scratchSize, 1);
        if (g_scratch == nullptr) {
            MarkFailure(a_key);
            logger::error("[FSR] backend scratch allocation FAILED ({} bytes) — frame stays "
                          "native (no AA fallback, per the DLSS-or-FSR-or-nothing law)",
                scratchSize);
            return false;
        }
        ffxDx11SetFailureLog(&BackendFailureLog);
        const FfxErrorCode err = ffxGetInterfaceDX11(
            &g_interface, ffxGetDeviceDX11_Fsr31(device), g_scratch, scratchSize, 1);
        if (err != FFX_OK) {
            MarkFailure(a_key);
            free(g_scratch);
            g_scratch = nullptr;
            logger::error("[FSR] ffxGetInterfaceDX11 FAILED ({:#x}) — frame stays native",
                static_cast<std::uint32_t>(err));
            return false;
        }
        g_backendReady = true;
        return true;
    }

    void DestroyContextOnly() noexcept
    {
        if (g_reactiveMask != nullptr) {
            g_reactiveMask->Release();
            g_reactiveMask = nullptr;
        }
        g_reactiveW = 0;
        g_reactiveH = 0;
        g_masksRefused = false;
        FreeTransparency();
        FreeMaskViews();
        Platform::OpaqueCapture::Release();
        if (g_context != nullptr) {
            const FfxErrorCode err = ffxFsr3ContextDestroy(g_context);
            if (err != FFX_OK) {
                static std::atomic<std::uint32_t> s_logs{ 0 };
                if (s_logs.fetch_add(1, std::memory_order_relaxed) < 3) {
                    logger::error("[FSR] context destroy reported {:#x} (continuing teardown)",
                        static_cast<std::uint32_t>(err));
                }
            }
            delete g_context;
            g_context = nullptr;
            g_ctxKey = {};
            g_appliedVelocityFactor = -1.0F;
            g_appliedReactiveness = -999.0F;
            g_appliedShadingChange = -999.0F;
            g_appliedAccumulation = -999.0F;
            g_appliedMinDisocclusion = -999.0F;
            g_lastDispatchOutW = g_lastDispatchOutH = 0;
            g_lastDispatchRenderW = g_lastDispatchRenderH = 0;
            logger::info("[FSR] context destroyed (recreates at the next evaluate)");
        }
    }

    [[nodiscard]] bool EnsureContext(const OutputKey& a_key) noexcept
    {
        if (g_context != nullptr && g_ctxKey == a_key) {
            return true;
        }
        if (g_backendPoisoned.load(std::memory_order_acquire)) {
            MarkFailure(a_key);
            if (!g_poisonLogged.exchange(true, std::memory_order_relaxed)) {
                logger::error("[FSR] off for the rest of this session: the backend threw inside a context creation earlier "
                              "and its effect slot cannot be reused safely — restart the game to try FSR again");
            }
            return false;
        }
        DestroyContextOnly();
        if (!EnsureBackend(a_key)) {
            return false;
        }

        g_context = new (std::nothrow) FfxFsr3Context();
        if (g_context == nullptr) {
            MarkFailure(a_key);
            logger::error("[FSR] context allocation FAILED — frame stays native");
            return false;
        }
        FfxFsr3ContextDescription desc{};
        desc.flags = FFX_FSR3_ENABLE_UPSCALING_ONLY;
        desc.maxRenderSize = { a_key.outW, a_key.outH };
        desc.maxUpscaleSize = { a_key.outW, a_key.outH };
        desc.displaySize = { a_key.outW, a_key.outH };
        desc.backendInterfaceUpscaling = g_interface;
        desc.backBufferFormat = ffxGetSurfaceFormatDX11(a_key.format);

        FfxErrorCode err = FFX_ERROR_BACKEND_API_ERROR;
        bool threw = false;
        try {
            err = ffxFsr3ContextCreate(g_context, &desc);
        } catch (...) {
            threw = true;
        }
        if (threw) {
            MarkFailure(a_key);
            g_backendPoisoned.store(true, std::memory_order_release);
            g_context = nullptr;
            logger::error("[FSR] ffxFsr3ContextCreate THREW (out {}x{}): a D3D11 call inside the backend failed (the "
                          "[FSR backend] line above names it); the partial context is retained, not destroyed — frame "
                          "stays native", a_key.outW, a_key.outH);
            return false;
        }
        if (err != FFX_OK) {
            MarkFailure(a_key);
            (void)ffxFsr3ContextDestroy(g_context);
            delete g_context;
            g_context = nullptr;
            logger::error(
                "[FSR] ffxFsr3ContextCreate FAILED ({:#x}, out {}x{}) — frame stays native",
                static_cast<std::uint32_t>(err), a_key.outW, a_key.outH);
            return false;
        }
        g_ctxKey = a_key;
        logger::info("[FSR] context created: output {}x{} (colour path HDR={}, context LDR — the "
                     "Community Shaders-proven flags; FSR {}.{}.{}, native DX11, upscaling-only, "
                     "fixed exposure; maxRender=output, the Skyrim-proven shape)",
            a_key.outW, a_key.outH, a_key.hdr, FFX_FSR3UPSCALER_VERSION_MAJOR,
            FFX_FSR3UPSCALER_VERSION_MINOR, FFX_FSR3UPSCALER_VERSION_PATCH);
        return true;
    }

    [[nodiscard]] bool EnsureFsrOut(
        ID3D11DeviceContext* a_ctx, const OutputKey& a_key) noexcept
    {
        if (g_fsrOut != nullptr && g_fsrOutKey == a_key) {
            return true;
        }
        if (g_fsrOut != nullptr) {
            g_fsrOut->Release();
            g_fsrOut = nullptr;
        }
        ID3D11Device* device = nullptr;
        a_ctx->GetDevice(&device);
        if (device == nullptr) {
            MarkFailure(a_key);
            return false;
        }
        D3D11_TEXTURE2D_DESC td{};
        td.Width = a_key.outW;
        td.Height = a_key.outH;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = a_key.format;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        const HRESULT hr = device->CreateTexture2D(&td, nullptr, &g_fsrOut);
        device->Release();
        g_fsrOutKey = a_key;
        if (FAILED(hr) || g_fsrOut == nullptr) {
            MarkFailure(a_key);
            g_fsrOut = nullptr;
            logger::error("[FSR] output scratch CreateTexture2D FAILED ({:#x}) — frame stays "
                          "native",
                static_cast<std::uint32_t>(hr));
            return false;
        }
        return true;
    }

    void FreeTransparency() noexcept
    {
        if (g_transparencyUav != nullptr) {
            g_transparencyUav->Release();
            g_transparencyUav = nullptr;
        }
        if (g_transparencyMask != nullptr) {
            g_transparencyMask->Release();
            g_transparencyMask = nullptr;
        }
        g_transparencyW = 0;
        g_transparencyH = 0;
    }

    void FreeMaskViews() noexcept
    {
        if (g_opaqueSrv != nullptr) {
            g_opaqueSrv->Release();
            g_opaqueSrv = nullptr;
        }
        if (g_finalSrv != nullptr) {
            g_finalSrv->Release();
            g_finalSrv = nullptr;
        }
        g_opaqueSrvOwner = nullptr;
        g_finalSrvOwner = nullptr;
    }

    [[nodiscard]] bool EnsureSrv(ID3D11Device* a_device, ID3D11Texture2D* a_tex, ID3D11ShaderResourceView*& a_srv,
        ID3D11Texture2D*& a_owner) noexcept
    {
        if (a_srv != nullptr && a_owner == a_tex) {
            return true;
        }
        if (a_srv != nullptr) {
            a_srv->Release();
            a_srv = nullptr;
        }
        a_owner = nullptr;
        if (a_tex == nullptr) {
            return false;
        }
        if (FAILED(a_device->CreateShaderResourceView(a_tex, nullptr, &a_srv))) {
            a_srv = nullptr;
            return false;
        }
        a_owner = a_tex;
        return true;
    }

    [[nodiscard]] bool EnsureTransparency(ID3D11Device* a_device, std::uint32_t a_w, std::uint32_t a_h) noexcept
    {
        if (g_transparencyCs == nullptr) {
            if (FAILED(a_device->CreateComputeShader(g_csFsrTransparency, sizeof(g_csFsrTransparency), nullptr,
                    &g_transparencyCs))) {
                g_transparencyCs = nullptr;
                return false;
            }
        }
        if (g_transparencyCb == nullptr) {
            D3D11_BUFFER_DESC cb{};
            cb.ByteWidth = 16;
            cb.Usage = D3D11_USAGE_DYNAMIC;
            cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            if (FAILED(a_device->CreateBuffer(&cb, nullptr, &g_transparencyCb))) {
                g_transparencyCb = nullptr;
                return false;
            }
        }
        if (g_transparencyMask != nullptr && g_transparencyW == a_w && g_transparencyH == a_h) {
            return true;
        }
        FreeTransparency();
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = a_w;
        desc.Height = a_h;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        if (FAILED(a_device->CreateTexture2D(&desc, nullptr, &g_transparencyMask))) {
            FreeTransparency();
            return false;
        }
        if (FAILED(a_device->CreateUnorderedAccessView(g_transparencyMask, nullptr, &g_transparencyUav))) {
            FreeTransparency();
            return false;
        }
        g_transparencyW = a_w;
        g_transparencyH = a_h;
        return true;
    }

    [[nodiscard]] bool EncodeTransparency(ID3D11DeviceContext* a_ctx, ID3D11Device* a_device,
        ID3D11Texture2D* a_opaque, ID3D11Texture2D* a_final, std::uint32_t a_w, std::uint32_t a_h) noexcept
    {
        if (!EnsureTransparency(a_device, a_w, a_h) || !EnsureSrv(a_device, a_opaque, g_opaqueSrv, g_opaqueSrvOwner) ||
            !EnsureSrv(a_device, a_final, g_finalSrv, g_finalSrvOwner)) {
            return false;
        }
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(a_ctx->Map(g_transparencyCb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            return false;
        }
        struct Constants
        {
            std::uint32_t renderW;
            std::uint32_t renderH;
            float transparencyScale;
            float padding;
        };
        Constants c{};
        c.renderW = a_w;
        c.renderH = a_h;
        c.transparencyScale = g_transparencyScale.load(std::memory_order_relaxed);
        std::memcpy(mapped.pData, &c, sizeof(c));
        a_ctx->Unmap(g_transparencyCb, 0);

        const Platform::SidecarGuides::ComputeBindingsScope csScope(a_ctx);
        ID3D11ShaderResourceView* const srvs[]{ g_opaqueSrv, g_finalSrv };
        a_ctx->CSSetShader(g_transparencyCs, nullptr, 0);
        a_ctx->CSSetShaderResources(0, 2, srvs);
        a_ctx->CSSetUnorderedAccessViews(0, 1, &g_transparencyUav, nullptr);
        a_ctx->CSSetConstantBuffers(0, 1, &g_transparencyCb);
        a_ctx->Dispatch((a_w + 7) / 8, (a_h + 7) / 8, 1);
        return true;
    }

    [[nodiscard]] bool EnsureReactiveMask(ID3D11Device* a_device, std::uint32_t a_w, std::uint32_t a_h) noexcept
    {
        if (g_reactiveMask != nullptr && g_reactiveW == a_w && g_reactiveH == a_h) {
            return true;
        }
        if (g_reactiveMask != nullptr) {
            g_reactiveMask->Release();
            g_reactiveMask = nullptr;
        }
        g_reactiveW = 0;
        g_reactiveH = 0;
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = a_w;
        desc.Height = a_h;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        if (FAILED(a_device->CreateTexture2D(&desc, nullptr, &g_reactiveMask))) {
            g_reactiveMask = nullptr;
            if (!g_masksRefused) {
                g_masksRefused = true;
                logger::warn("[FSR] the reactive mask could not be allocated ({}x{} R8) — the masks are off until "
                             "the context is rebuilt", a_w, a_h);
            }
            return false;
        }
        g_reactiveW = a_w;
        g_reactiveH = a_h;
        return true;
    }

    void ApplyConstant(FfxFsr3UpscalerConfigureKey a_key, const std::atomic<float>& a_wanted, float& a_applied,
        const char* a_name, const char* a_meaning) noexcept
    {
        const float wanted = a_wanted.load(std::memory_order_relaxed);
        if (wanted == a_applied) {
            return;
        }
        float v = wanted;
        if (ffxFsr3SetUpscalerConstant(g_context, a_key, &v) == FFX_OK) {
            a_applied = wanted;
            logger::info("[FSR] {} -> {:.3f} ({})", a_name, static_cast<double>(v), a_meaning);
        }
    }

    void ClearBindingHazards(ID3D11DeviceContext* a_ctx, ID3D11Resource* a_color,
        ID3D11Resource* a_depth, ID3D11Resource* a_mvec, ID3D11Resource* a_out) noexcept
    {
        static std::atomic<std::uint32_t> s_hazardLogs{ 0 };
        ID3D11Resource* const inputs[]{ a_color, a_depth, a_mvec };

        ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
        ID3D11DepthStencilView* dsv = nullptr;
        a_ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, &dsv);
        bool omHazard = false;
        for (auto& rtv : rtvs) {
            if (rtv == nullptr) {
                continue;
            }
            ID3D11Resource* res = nullptr;
            rtv->GetResource(&res);
            for (ID3D11Resource* const in : inputs) {
                if (res == in) {
                    omHazard = true;
                }
            }
            if (res != nullptr) {
                res->Release();
            }
            rtv->Release();
            rtv = nullptr;
        }
        if (dsv != nullptr) {
            ID3D11Resource* res = nullptr;
            dsv->GetResource(&res);
            if (res == a_depth) {
                omHazard = true;
            }
            if (res != nullptr) {
                res->Release();
            }
            dsv->Release();
        }
        if (omHazard) {
            a_ctx->OMSetRenderTargets(0, nullptr, nullptr);
            if (s_hazardLogs.fetch_add(1, std::memory_order_relaxed) < 3) {
                logger::warn("[FSR] pre-dispatch HAZARD cleared: an FSR input was still bound "
                             "on OM (it would have NULLed its SRV inside the FSR passes)");
            }
        }

        ID3D11UnorderedAccessView* uavs[D3D11_PS_CS_UAV_REGISTER_COUNT]{};
        a_ctx->CSGetUnorderedAccessViews(0, D3D11_PS_CS_UAV_REGISTER_COUNT, uavs);
        for (UINT slot = 0; slot < D3D11_PS_CS_UAV_REGISTER_COUNT; ++slot) {
            if (uavs[slot] == nullptr) {
                continue;
            }
            ID3D11Resource* res = nullptr;
            uavs[slot]->GetResource(&res);
            bool hazard = res == a_out;
            for (ID3D11Resource* const in : inputs) {
                if (res == in) {
                    hazard = true;
                }
            }
            if (hazard) {
                ID3D11UnorderedAccessView* nullUav = nullptr;
                a_ctx->CSSetUnorderedAccessViews(slot, 1, &nullUav, nullptr);
                if (s_hazardLogs.fetch_add(1, std::memory_order_relaxed) < 3) {
                    logger::warn("[FSR] pre-dispatch HAZARD cleared: an FSR resource was still "
                                 "bound as CS UAV slot {}",
                        slot);
                }
            }
            if (res != nullptr) {
                res->Release();
            }
            uavs[slot]->Release();
        }
    }
}

namespace Platform::FsrEngine
{
    AaAvailability ProbeFirstDevice(ID3D11Device* a_device) noexcept
    {
        try {
            if (a_device == nullptr) {
                return AaAvailability::kUnavailable;
            }
            if (g_device.load(std::memory_order_acquire) != nullptr) {
                return g_pathAvailable.load(std::memory_order_relaxed)
                           ? AaAvailability::kAvailable
                           : AaAvailability::kUnavailable;
            }

            UINT sup = 0;
            D3D11_FEATURE_DATA_FORMAT_SUPPORT2 sup2{};
            sup2.InFormat = DXGI_FORMAT_R11G11B10_FLOAT;
            const bool okA =
                SUCCEEDED(a_device->CheckFormatSupport(DXGI_FORMAT_R11G11B10_FLOAT, &sup));
            const bool okB = SUCCEEDED(a_device->CheckFeatureSupport(
                D3D11_FEATURE_FORMAT_SUPPORT2, &sup2, sizeof(sup2)));
            const bool srvSample = okA && (sup & D3D11_FORMAT_SUPPORT_SHADER_SAMPLE) != 0;
            const bool typedUavView =
                okA && (sup & D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW) != 0;
            const bool typedUavStore =
                okB && (sup2.OutFormatSupport2 & D3D11_FORMAT_SUPPORT2_UAV_TYPED_STORE) != 0;
            const bool available = srvSample && typedUavView && typedUavStore;
            logger::info("[FSR] first-device probe: fmt26 SRV_SAMPLE={} TYPED_UAV_VIEW={} "
                         "UAV_TYPED_STORE={} -> FSR {}",
                srvSample, typedUavView, typedUavStore,
                available ? "AVAILABLE" : "unavailable on this adapter");

            a_device->AddRef();
            g_pathAvailable.store(available, std::memory_order_relaxed);
            g_device.store(a_device, std::memory_order_release);
            return available ? AaAvailability::kAvailable : AaAvailability::kUnavailable;
        } catch (...) {
            return AaAvailability::kUnavailable;
        }
    }

    bool Evaluate(ID3D11DeviceContext* a_ctx, ID3D11Texture2D* a_colorInOut,
        ID3D11Texture2D* a_superResOut, ID3D11Resource* a_depth, ID3D11Resource* a_mvec,
        std::uint32_t a_renderW, std::uint32_t a_renderH, std::uint32_t a_outW,
        std::uint32_t a_outH, bool a_colorIsHDR, float a_sharpness, float a_jitterX,
        float a_jitterY, bool a_reset) noexcept
    {
        try {
            if (g_device.load(std::memory_order_acquire) == nullptr || a_ctx == nullptr ||
                a_colorInOut == nullptr || a_depth == nullptr || a_mvec == nullptr ||
                a_renderW == 0 || a_renderH == 0 || a_outW == 0 || a_outH == 0) {
                return false;
            }

            D3D11_TEXTURE2D_DESC cd{};
            a_colorInOut->GetDesc(&cd);

            const OutputKey key{ a_outW, a_outH, cd.Format, a_colorIsHDR };

            if (g_failed) {
                if (g_rearmRequested.exchange(false, std::memory_order_acq_rel)) {
                    g_failed = false;
                    logger::info("[FSR] failure latch cleared (engine re-selected) — one "
                                 "retry at {}x{}",
                        a_outW, a_outH);
                } else if (!(key == g_failedKey)) {
                    g_failed = false;
                    logger::info("[FSR] output key changed after a failure — one recovery "
                                 "attempt at {}x{}",
                        a_outW, a_outH);
                } else {
                    return false;
                }
            } else {
                (void)g_rearmRequested.exchange(false, std::memory_order_acq_rel);
            }

            if (g_context != nullptr && !(g_ctxKey == key)) {
                if (g_pendingKey == key) {
                    ++g_pendingStable;
                } else {
                    g_pendingKey = key;
                    g_pendingStable = 1;
                }
                if (g_pendingStable < kSettleFrames) {
                    return false;
                }
            }
            g_pendingStable = 0;

            if (!EnsureContext(key)) {
                return false;
            }

            ID3D11Texture2D* outTex = a_superResOut;
            if (outTex == nullptr) {
                if (!EnsureFsrOut(a_ctx, key)) {
                    return false;
                }
                outTex = g_fsrOut;
            }

            ClearBindingHazards(a_ctx, reinterpret_cast<ID3D11Resource*>(a_colorInOut),
                a_depth, a_mvec, reinterpret_cast<ID3D11Resource*>(outTex));

            const float velocity = g_velocityFactor.load(std::memory_order_relaxed);
            if (velocity != g_appliedVelocityFactor) {
                float v = velocity;
                if (ffxFsr3SetUpscalerConstant(g_context,
                        FFX_FSR3UPSCALER_CONFIGURE_UPSCALE_KEY_FVELOCITYFACTOR, &v) == FFX_OK) {
                    g_appliedVelocityFactor = velocity;
                    logger::info("[FSR] velocity factor -> {:.2f} (1.0 = AMD default; 0.0 = "
                                 "max bright-pixel stability)",
                        static_cast<double>(v));
                }
            }
            ApplyConstant(FFX_FSR3UPSCALER_CONFIGURE_UPSCALE_KEY_FREACTIVENESSSCALE, g_reactiveness,
                g_appliedReactiveness, "reactiveness scale", "1.0 = AMD default; higher = less ghosting");
            ApplyConstant(FFX_FSR3UPSCALER_CONFIGURE_UPSCALE_KEY_FSHADINGCHANGESCALE, g_shadingChange,
                g_appliedShadingChange, "shading-change scale", "1.0 = AMD default; higher = more reactive");
            ApplyConstant(FFX_FSR3UPSCALER_CONFIGURE_UPSCALE_KEY_FACCUMULATIONADDEDPERFRAME, g_accumulation,
                g_appliedAccumulation, "accumulation added per frame",
                "0.333 = AMD default; lower = more thin-feature flicker");
            ApplyConstant(FFX_FSR3UPSCALER_CONFIGURE_UPSCALE_KEY_FMINDISOCCLUSIONACCUMULATION, g_minDisocclusion,
                g_appliedMinDisocclusion, "min disocclusion accumulation",
                "-0.333 = AMD default; higher = less white-pixel flicker on swaying thin objects");

            static LARGE_INTEGER s_frequency = [] {
                LARGE_INTEGER freq{};
                ::QueryPerformanceFrequency(&freq);
                return freq;
            }();
            static LARGE_INTEGER s_lastFrameTime = [] {
                LARGE_INTEGER time{};
                ::QueryPerformanceCounter(&time);
                return time;
            }();
            LARGE_INTEGER now{};
            ::QueryPerformanceCounter(&now);
            float deltaMs = static_cast<float>(now.QuadPart - s_lastFrameTime.QuadPart) *
                            1000.0F / static_cast<float>(s_frequency.QuadPart);
            s_lastFrameTime = now;
            if (deltaMs < 0.0F) {
                deltaMs = 0.0F;
            }
            if (deltaMs > 100.0F) {
                deltaMs = 100.0F;
            }

            const auto camera = Platform::Fallout4Renderer::CameraSnapshot();
            const bool cameraValid = Platform::AreCameraConstantsValid(camera);

            FfxFsr3DispatchUpscaleDescription d{};
            d.commandList = ffxGetCommandListDX11(a_ctx);
            d.color = Wrap(reinterpret_cast<ID3D11Resource*>(a_colorInOut),
                FFX_RESOURCE_STATE_PIXEL_COMPUTE_READ);
            d.depth = Wrap(a_depth, FFX_RESOURCE_STATE_PIXEL_COMPUTE_READ);
            d.motionVectors = Wrap(a_mvec, FFX_RESOURCE_STATE_PIXEL_COMPUTE_READ);
            if (g_masksEnabled.load(std::memory_order_relaxed) && !g_masksRefused) {
                if (ID3D11Texture2D* const opaque = OpaqueCapture::TakeThisFrame(); opaque != nullptr) {
                    ID3D11Device* const device = g_device.load(std::memory_order_acquire);
                    if (EnsureReactiveMask(device, a_renderW, a_renderH)) {
                    FfxFsr3GenerateReactiveDescription gen{};
                    gen.commandList = ffxGetCommandListDX11(a_ctx);
                    gen.colorOpaqueOnly = Wrap(reinterpret_cast<ID3D11Resource*>(opaque),
                        FFX_RESOURCE_STATE_PIXEL_COMPUTE_READ);
                    gen.colorPreUpscale = Wrap(reinterpret_cast<ID3D11Resource*>(a_colorInOut),
                        FFX_RESOURCE_STATE_PIXEL_COMPUTE_READ);
                    gen.outReactive = Wrap(reinterpret_cast<ID3D11Resource*>(g_reactiveMask),
                        FFX_RESOURCE_STATE_UNORDERED_ACCESS);
                    gen.renderSize = { a_renderW, a_renderH };
                    gen.scale = 0.5F;
                    gen.flags = kAutoReactiveUseComponentsMax;
                        if (ffxFsr3ContextGenerateReactiveMask(g_context, &gen) == FFX_OK) {
                            d.reactive = Wrap(reinterpret_cast<ID3D11Resource*>(g_reactiveMask),
                                FFX_RESOURCE_STATE_PIXEL_COMPUTE_READ);
                        }
                    }
                    if (EncodeTransparency(a_ctx, device, opaque, a_colorInOut, a_renderW, a_renderH)) {
                        d.transparencyAndComposition = Wrap(
                            reinterpret_cast<ID3D11Resource*>(g_transparencyMask),
                            FFX_RESOURCE_STATE_PIXEL_COMPUTE_READ);
                    }
                }
            }
            d.upscaleOutput = Wrap(reinterpret_cast<ID3D11Resource*>(outTex),
                FFX_RESOURCE_STATE_UNORDERED_ACCESS);
            d.jitterOffset = { -a_jitterX, -a_jitterY };
            d.motionVectorScale = { static_cast<float>(a_renderW),
                static_cast<float>(a_renderH) };
            d.renderSize = { a_renderW, a_renderH };
            d.upscaleSize = { a_outW, a_outH };
            d.enableSharpening = a_sharpness > 0.0F;
            d.sharpness = a_sharpness > 1.0F ? 1.0F : (a_sharpness < 0.0F ? 0.0F : a_sharpness);
            d.frameTimeDelta = deltaMs;
            d.preExposure = 1.0F;
            d.reset = a_reset || a_outW != g_lastDispatchOutW || a_outH != g_lastDispatchOutH ||
                      a_renderW != g_lastDispatchRenderW || a_renderH != g_lastDispatchRenderH;
            d.cameraNear = cameraValid ? camera.nearPlane : 15.0F;
            d.cameraFar = cameraValid ? camera.farPlane : 353840.0F;
            d.cameraFovAngleVertical =
                (camera.fovVertical > 0.01F && camera.fovVertical < 3.13F) ? camera.fovVertical
                                                                           : 1.0F;
            d.viewSpaceToMetersFactor = 0.01428222656F;
            d.flags = 0;
            d.frameID = ++g_frameId;

            const FfxErrorCode err = ffxFsr3ContextDispatchUpscale(g_context, &d);
            if (err != FFX_OK) {
                MarkFailure(key);
                logger::error("[FSR] dispatch FAILED ({:#x}) — latched for {}x{}; frame stays "
                              "native (no AA fallback)",
                    static_cast<std::uint32_t>(err), a_outW, a_outH);
                (void)DestroyContext();
                return false;
            }

            g_lastDispatchOutW = a_outW;
            g_lastDispatchOutH = a_outH;
            g_lastDispatchRenderW = a_renderW;
            g_lastDispatchRenderH = a_renderH;

            if (a_superResOut == nullptr) {
                a_ctx->CopyResource(a_colorInOut, g_fsrOut);
            }

            static std::atomic<bool> s_loggedActive{ false };
            if (!s_loggedActive.exchange(true, std::memory_order_relaxed)) {
                logger::info("[FSR] ★ active: {} {}x{} -> {}x{} HDR={} (FSR {}.{}.{}, native "
                             "DX11) | backend texture-UAV creations: {} calls, {} failed{}",
                    a_outW > a_renderW ? "upscale" : "NativeAA", a_renderW, a_renderH, a_outW,
                    a_outH, a_colorIsHDR, FFX_FSR3UPSCALER_VERSION_MAJOR,
                    FFX_FSR3UPSCALER_VERSION_MINOR, FFX_FSR3UPSCALER_VERSION_PATCH,
                    g_ffxDx11TexUavCreateCalls, g_ffxDx11TexUavCreateFails,
                    g_ffxDx11TexUavCreateFails != 0
                        ? "  <-- silent null-UAV writes: the black-output mechanism"
                        : "");
            }
            return true;
        } catch (...) {
            static std::atomic<bool> s_logged{ false };
            if (!s_logged.exchange(true, std::memory_order_relaxed)) {
                logger::error("[FSR] C++ exception in Evaluate — frame stays native");
            }
            return false;
        }
    }

    bool DestroyContext() noexcept
    {
        try {
            g_pendingStable = 0;
            DestroyContextOnly();
            if (g_fsrOut != nullptr) {
                g_fsrOut->Release();
                g_fsrOut = nullptr;
                g_fsrOutKey = {};
            }
            return true;
        } catch (...) {
            return false;
        }
    }

    void RearmFailureLatch() noexcept
    {
        g_rearmRequested.store(true, std::memory_order_release);
    }

    void SetVelocityFactor(float a_value) noexcept
    {
        g_velocityFactor.store(Platform::ClampFsrVelocity(a_value), std::memory_order_relaxed);
    }

    float VelocityFactor() noexcept
    {
        return g_velocityFactor.load(std::memory_order_relaxed);
    }

    void SetReactiveness(float a_value) noexcept
    {
        g_reactiveness.store(Platform::ClampFsrReactiveness(a_value), std::memory_order_relaxed);
    }

    void SetShadingChange(float a_value) noexcept
    {
        g_shadingChange.store(Platform::ClampFsrShadingChange(a_value), std::memory_order_relaxed);
    }

    void SetAccumulation(float a_value) noexcept
    {
        g_accumulation.store(Platform::ClampFsrAccumulation(a_value), std::memory_order_relaxed);
    }

    void SetMinDisocclusion(float a_value) noexcept
    {
        g_minDisocclusion.store(Platform::ClampFsrMinDisocclusion(a_value), std::memory_order_relaxed);
    }

    void SetMasksEnabled(bool a_enabled) noexcept
    {
        g_masksEnabled.store(a_enabled, std::memory_order_relaxed);
    }

    void SetTransparencyScale(float a_value) noexcept
    {
        g_transparencyScale.store(Platform::ClampFsrTransparencyScale(a_value), std::memory_order_relaxed);
    }

    bool MasksEnabled() noexcept
    {
        return g_masksEnabled.load(std::memory_order_relaxed);
    }
}
