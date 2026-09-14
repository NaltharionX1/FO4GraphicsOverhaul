// SPDX-License-Identifier: GPL-3.0-or-later
// Portions adapted from Community Shaders for Fallout 4 (northaxosky), GPL-3.0.

#include "PCH.h"

#include "Platform/Dlss12Engine.h"

#include "Platform/D3D12Sidecar.h"
#include "Platform/SidecarFrame.h"
#include "Platform/NgxD3D12.h"
#include "Platform/SidecarGuides.h"

#include <d3d12.h>
#include <dxgi1_4.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

#include "CSNeuralColor.h"
#include "CSNeuralDepth.h"
#include "CSFgDepthConditioned.h"
#include "CSFgMotionConditioned.h"
#include "CSDilateMotion.h"
#include "Platform/FirstPersonMask.h"

namespace
{
    using namespace Platform;
    using namespace Platform::SidecarGuides;

    constexpr int kEvaluateStrikes = 3;
    constexpr std::uint32_t kTimingWindow = 600;
    constexpr unsigned int kNvidiaVendorId = 0x10DEU;

    constexpr int kFlagIsHdr = 1;
    constexpr int kFlagMvLowRes = 2;
    constexpr int kFlagAutoExposure = 64;

    struct FeatureKey
    {
        std::uint32_t renderW{ 0 }, renderH{ 0 };
        std::uint32_t outW{ 0 }, outH{ 0 };
        int perfQuality{ 0 };
        int flags{ 0 };
        std::uint32_t preset{ 0 };
        DXGI_FORMAT carrier{ DXGI_FORMAT_UNKNOWN };
        std::uint32_t depthW{ 0 }, depthH{ 0 };
        std::uint32_t mvW{ 0 }, mvH{ 0 };
        DXGI_FORMAT mvFormat{ DXGI_FORMAT_UNKNOWN };

        [[nodiscard]] bool operator==(const FeatureKey&) const noexcept = default;
    };

    struct EngineState
    {
        std::atomic<bool> latched{ false };
        char reason[160]{};

        bool pathsResolved{ false };
        std::wstring dataPath;

        ID3D11Device* device{ nullptr };
        ID3D12Device* owner12{ nullptr };
        bool lastTeardownAbandoned{ false };
        std::uint64_t sessionSerial{ 0 };
        Ngx::Parameter* params{ nullptr };
        Ngx::Handle* feature{ nullptr };
        FeatureKey key{};

        D3D12Sidecar::SharedTexture colorIn{};
        D3D12Sidecar::SharedTexture colorOut{};
        D3D12Sidecar::SharedTexture depth{};
        D3D12Sidecar::SharedTexture mv{};
        D3D12Sidecar::SharedTexture fgDepth{};
        D3D12Sidecar::SharedTexture fgMv{};
        D3D12Sidecar::SharedTexture exposure{};
        bool exposureSeeded{ false };

        bool fp16{ false };
        bool carrierFallbackTried{ false };
        ID3D11ComputeShader* csDepth{ nullptr };
        ID3D11ComputeShader* csColor{ nullptr };
        ID3D11ComputeShader* csFgDepth{ nullptr };
        ID3D11ComputeShader* csFgMotion{ nullptr };
        ID3D11ComputeShader* csDilate{ nullptr };
        ID3D11Buffer* dilateParams{ nullptr };
        ID3D11UnorderedAccessView* depthUav{ nullptr };
        ID3D11UnorderedAccessView* mvUav{ nullptr };
        ID3D11UnorderedAccessView* fgDepthUav{ nullptr };
        ID3D11UnorderedAccessView* fgMvUav{ nullptr };
        ID3D11UnorderedAccessView* colorInUav{ nullptr };
        ID3D11ShaderResourceView* colorOutSrv{ nullptr };
        ID3D11Texture2D* outScratch{ nullptr };
        ID3D11UnorderedAccessView* outScratchUav{ nullptr };
        ID3D11ShaderResourceView* ownDepthSrv{ nullptr };
        ID3D11Texture2D* ownDepthSrvTexture{ nullptr };
        ID3D11ShaderResourceView* ownColorInSrv{ nullptr };
        ID3D11Texture2D* ownColorInSrvTexture{ nullptr };
        ID3D11ShaderResourceView* ownMvSrv{ nullptr };
        ID3D11Texture2D* ownMvSrvTexture{ nullptr };
        std::uint64_t maskCursor{ 0 };
        std::atomic<bool> fgConditioned{ false };
        std::atomic<bool> dilated{ false };
        std::uint32_t p48Refused{ 0 };

        int strikes{ 0 };
        std::uint64_t evaluations{ 0 };
        std::uint32_t creates{ 0 };
        std::atomic<std::uint32_t> lastResult{ 0 };

        LONGLONG qpf{ 0 };
        LONGLONG cpuTicks{ 0 };
        std::uint64_t windowFrames{ 0 };
        std::atomic<float> cpuMsAvg{ 0.0F };

        std::atomic<bool> featureReady{ false };
        std::atomic<std::uint32_t> snapRenderW{ 0 }, snapRenderH{ 0 }, snapOutW{ 0 }, snapOutH{ 0 };
        std::atomic<std::uint64_t> snapEvaluations{ 0 };
        std::atomic<std::uint32_t> snapCreates{ 0 };
    };

    EngineState g;

    [[nodiscard]] std::string Narrow(const wchar_t* a_text)
    {
        if (a_text == nullptr) {
            return {};
        }
        const int needed = ::WideCharToMultiByte(CP_UTF8, 0, a_text, -1, nullptr, 0, nullptr, nullptr);
        if (needed <= 1) {
            return {};
        }
        std::string out(static_cast<std::size_t>(needed - 1), '\0');
        ::WideCharToMultiByte(CP_UTF8, 0, a_text, -1, out.data(), needed, nullptr, nullptr);
        return out;
    }

    [[nodiscard]] std::filesystem::path SelfModuleDirectory()
    {
        wchar_t buffer[MAX_PATH]{};
        HMODULE module = nullptr;
        ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&g), &module);
        ::GetModuleFileNameW(module, buffer, MAX_PATH);
        return std::filesystem::path{ buffer }.parent_path();
    }

    void Latch(const char* a_reason) noexcept
    {
        if (!g.latched.exchange(true, std::memory_order_relaxed)) {
            std::snprintf(g.reason, sizeof(g.reason), "%s", a_reason != nullptr ? a_reason : "unknown");
            logger::warn("[DLSS12] {} — DX12 mode is OFF (the frame stays native, no AA) until the next staged "
                         "recreate (engine re-select, preset/exposure/mode change) or a restart",
                g.reason);
            Platform::SidecarFrame::InvalidateGuides();
            g.fgConditioned.store(false, std::memory_order_relaxed);
            g.dilated.store(false, std::memory_order_relaxed);
        }
    }

    void Rearm() noexcept
    {
        g.latched.store(false, std::memory_order_relaxed);
        g.reason[0] = '\0';
        g.strikes = 0;
    }

    void ReleaseViews() noexcept
    {
        ReleaseCom(g.mvUav);
        ReleaseCom(g.fgDepthUav);
        ReleaseCom(g.fgMvUav);
        ReleaseCom(g.depthUav);
        ReleaseCom(g.colorInUav);
        ReleaseCom(g.colorOutSrv);
        ReleaseCom(g.outScratchUav);
        ReleaseCom(g.outScratch);
    }

    void ReleaseOwnViews() noexcept
    {
        ReleaseCom(g.ownMvSrv);
        g.ownMvSrvTexture = nullptr;
        ReleaseCom(g.ownDepthSrv);
        g.ownDepthSrvTexture = nullptr;
        ReleaseCom(g.ownColorInSrv);
        g.ownColorInSrvTexture = nullptr;
    }

    [[nodiscard]] bool HasAnything() noexcept
    {
        return g.feature != nullptr || g.colorIn.Valid() || g.colorOut.Valid() || g.depth.Valid() ||
               g.mv.Valid() || g.exposure.Valid();
    }

    void ReleaseFeatureHandle() noexcept
    {
        if (g.feature == nullptr) {
            return;
        }
        if (NgxD3D12::SessionSerial() == g.sessionSerial) {
            const NgxD3D12::Outcome release = NgxD3D12::CoreRelease(g.feature);
            if (release.faultCode != 0) {
                Latch("ReleaseFeature(1) raised an exception");
            }
        }
        g.feature = nullptr;
        g.featureReady.store(false, std::memory_order_relaxed);
    }

    void ReleaseOwner12() noexcept
    {
        if (g.owner12 != nullptr) {
            g.owner12->Release();
            g.owner12 = nullptr;
        }
    }

    void AbandonShared() noexcept
    {
        g.lastTeardownAbandoned = true;
        Platform::SidecarFrame::InvalidateGuides();
        g.feature = nullptr;
        g.featureReady.store(false, std::memory_order_relaxed);
        ReleaseViews();
        g.fgConditioned.store(false, std::memory_order_relaxed);
        g.dilated.store(false, std::memory_order_relaxed);
        D3D12Sidecar::ForgetShared(g.fgDepth);
        D3D12Sidecar::ForgetShared(g.fgMv);
        D3D12Sidecar::ForgetShared(g.colorIn);
        D3D12Sidecar::ForgetShared(g.colorOut);
        D3D12Sidecar::ForgetShared(g.depth);
        D3D12Sidecar::ForgetShared(g.mv);
        D3D12Sidecar::ForgetShared(g.exposure);
        g.exposureSeeded = false;
        g.key = FeatureKey{};
        ReleaseOwner12();
    }

    [[nodiscard]] bool TeardownShared() noexcept
    {
        if (D3D12Sidecar::IsOpen() && !D3D12Sidecar::Disabled()) {
            (void)D3D12Sidecar::PollHealth();
        }
        const bool onClosedSession = g.owner12 != nullptr && (!D3D12Sidecar::IsOpen() || g.owner12 != D3D12Sidecar::Device());
        if (onClosedSession && !D3D12Sidecar::SessionRetired(g.owner12)) {
            logger::warn("[DLSS12] the session the DX12 engine's objects were created on closed without proving retirement: its "
                         "feature and shared textures are RETAINED (leaked), never released under the GPU; the engine forgets them");
            AbandonShared();
            return true;
        }
        if (!onClosedSession && D3D12Sidecar::IsOpen() && HasAnything() && !D3D12Sidecar::ProveRetired()) {
            if (!D3D12Sidecar::Disabled()) {
                return false;
            }
            logger::warn("[DLSS12] the sidecar is disabled ({}) and did not prove retirement of the DX12 engine's work: its "
                         "feature and shared textures are RETAINED (leaked), never released under the GPU; the engine forgets them",
                D3D12Sidecar::DisableReason());
            AbandonShared();
            return true;
        }
        g.lastTeardownAbandoned = false;
        Platform::SidecarFrame::InvalidateGuides();
        ReleaseFeatureHandle();
        ReleaseViews();
        g.fgConditioned.store(false, std::memory_order_relaxed);
        g.dilated.store(false, std::memory_order_relaxed);
        D3D12Sidecar::ReleaseShared(g.fgDepth);
        D3D12Sidecar::ReleaseShared(g.fgMv);
        D3D12Sidecar::ReleaseShared(g.colorIn);
        D3D12Sidecar::ReleaseShared(g.colorOut);
        D3D12Sidecar::ReleaseShared(g.depth);
        D3D12Sidecar::ReleaseShared(g.mv);
        D3D12Sidecar::ReleaseShared(g.exposure);
        g.exposureSeeded = false;
        g.key = FeatureKey{};
        ReleaseOwner12();
        return true;
    }

    void ForgetSession() noexcept
    {
        g.feature = nullptr;
        g.featureReady.store(false, std::memory_order_relaxed);
        g.params = nullptr;
        g.key = FeatureKey{};
    }

    [[nodiscard]] bool CreateShadersOnce(ID3D11Device* a_device) noexcept
    {
        const auto refuseOnce = [](std::uint32_t a_bit, const char* a_what, const char* a_loses) noexcept {
            if ((g.p48Refused & a_bit) == 0) {
                g.p48Refused |= a_bit;
                logger::warn("[DLSS12] {} would not create — {} is off for this session", a_what, a_loses);
            }
        };
        if (g.csFgDepth == nullptr && (g.p48Refused & 1U) == 0 &&
            FAILED(a_device->CreateComputeShader(g_csFgDepthConditioned, sizeof(g_csFgDepthConditioned), nullptr, &g.csFgDepth))) {
            refuseOnce(1U, "the conditioned-depth shader", "first-person conditioning for frame generation");
        }
        if (g.csFgMotion == nullptr && (g.p48Refused & 2U) == 0 &&
            FAILED(a_device->CreateComputeShader(g_csFgMotionConditioned, sizeof(g_csFgMotionConditioned), nullptr, &g.csFgMotion))) {
            refuseOnce(2U, "the conditioned-motion shader", "first-person conditioning for frame generation");
        }
        if (g.csDilate == nullptr && (g.p48Refused & 4U) == 0 &&
            FAILED(a_device->CreateComputeShader(g_csDilateMotion, sizeof(g_csDilateMotion), nullptr, &g.csDilate))) {
            refuseOnce(4U, "the motion-dilation shader", "motion-vector dilation");
        }
        if (g.dilateParams == nullptr && (g.p48Refused & 8U) == 0) {
            D3D11_BUFFER_DESC cb{};
            cb.ByteWidth = 16;
            cb.Usage = D3D11_USAGE_DEFAULT;
            cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            if (FAILED(a_device->CreateBuffer(&cb, nullptr, &g.dilateParams))) {
                refuseOnce(8U, "the dilation's parameter buffer", "motion-vector dilation");
            }
        }
        if (g.csDepth == nullptr &&
            FAILED(a_device->CreateComputeShader(g_csNeuralDepth, sizeof(g_csNeuralDepth), nullptr, &g.csDepth))) {
            Latch("the depth conversion shader would not create");
            return false;
        }
        if (g.csColor == nullptr &&
            FAILED(a_device->CreateComputeShader(g_csNeuralColor, sizeof(g_csNeuralColor), nullptr, &g.csColor))) {
            Latch("the colour carrier shader would not create");
            return false;
        }
        return true;
    }

    [[nodiscard]] bool GetDesc(ID3D11Resource* a_resource, D3D11_TEXTURE2D_DESC& a_desc) noexcept
    {
        ID3D11Texture2D* texture = nullptr;
        if (a_resource == nullptr ||
            FAILED(a_resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&texture))) ||
            texture == nullptr) {
            return false;
        }
        texture->GetDesc(&a_desc);
        texture->Release();
        return true;
    }

    void SetCreateParams(const FeatureKey& a_key) noexcept
    {
        Ngx::Parameter* p = g.params;
        p->Set("Width", static_cast<unsigned int>(a_key.renderW));
        p->Set("Height", static_cast<unsigned int>(a_key.renderH));
        p->Set("OutWidth", static_cast<unsigned int>(a_key.outW));
        p->Set("OutHeight", static_cast<unsigned int>(a_key.outH));
        p->Set("PerfQualityValue", a_key.perfQuality);
        p->Set("DLSS.Feature.Create.Flags", a_key.flags);
        p->Set("CreationNodeMask", 1U);
        p->Set("VisibilityNodeMask", 1U);
        p->Set("DLSS.Enable.Output.Subrects", 0U);
        p->Set("DLSS.Hint.Render.Preset.DLAA", static_cast<unsigned int>(a_key.preset));
        p->Set("DLSS.Hint.Render.Preset.Quality", static_cast<unsigned int>(a_key.preset));
        p->Set("DLSS.Hint.Render.Preset.Balanced", static_cast<unsigned int>(a_key.preset));
        p->Set("DLSS.Hint.Render.Preset.Performance", static_cast<unsigned int>(a_key.preset));
        p->Set("DLSS.Hint.Render.Preset.UltraPerformance", static_cast<unsigned int>(a_key.preset));
        p->Set("DLSS.Hint.Render.Preset.UltraQuality", static_cast<unsigned int>(a_key.preset));
    }

    void SetEvaluateParams(const Dlss12Engine::Inputs& a_in) noexcept
    {
        Ngx::Parameter* p = g.params;
        p->Set("Color", g.colorIn.d3d12);
        p->Set("Output", g.colorOut.d3d12);
        p->Set("Depth", g.depth.d3d12);
        p->Set("MotionVectors", g.mv.d3d12);
        p->Set("ExposureTexture", a_in.autoExposure ? static_cast<ID3D12Resource*>(nullptr) : g.exposure.d3d12);
        p->Set("TransparencyMask", static_cast<ID3D12Resource*>(nullptr));
        p->Set("DLSS.Input.Bias.Current.Color.Mask", static_cast<ID3D12Resource*>(nullptr));
        p->Set("Jitter.Offset.X", -a_in.jitterX);
        p->Set("Jitter.Offset.Y", -a_in.jitterY);
        p->Set("MV.Scale.X", static_cast<float>(a_in.renderW));
        p->Set("MV.Scale.Y", static_cast<float>(a_in.renderH));
        p->Set("Reset", a_in.reset ? 1 : 0);
        p->Set("Sharpness", 0.0F);
        p->Set("DLSS.Render.Subrect.Dimensions.Width", static_cast<unsigned int>(a_in.renderW));
        p->Set("DLSS.Render.Subrect.Dimensions.Height", static_cast<unsigned int>(a_in.renderH));
        p->Set("DLSS.Input.Color.Subrect.Base.X", 0U);
        p->Set("DLSS.Input.Color.Subrect.Base.Y", 0U);
        p->Set("DLSS.Input.Depth.Subrect.Base.X", 0U);
        p->Set("DLSS.Input.Depth.Subrect.Base.Y", 0U);
        p->Set("DLSS.Input.MV.Subrect.Base.X", 0U);
        p->Set("DLSS.Input.MV.Subrect.Base.Y", 0U);
        p->Set("DLSS.Output.Subrect.Base.X", 0U);
        p->Set("DLSS.Output.Subrect.Base.Y", 0U);
        p->Set("DLSS.Pre.Exposure", 1.0F);
        p->Set("DLSS.Exposure.Scale", a_in.exposureScale);
    }

    enum class RebuildOutcome
    {
        kBuilt,
        kTeardownPending,
        kRefused,
    };

    [[nodiscard]] RebuildOutcome Rebuild(ID3D11Device* a_device, const FeatureKey& a_key,
        const D3D11_TEXTURE2D_DESC& a_inDesc, const D3D11_TEXTURE2D_DESC& a_outDesc) noexcept
    {
        if (!TeardownShared()) {
            return RebuildOutcome::kTeardownPending;
        }
        if (!CreateShadersOnce(a_device)) {
            return RebuildOutcome::kRefused;
        }
        if (!g.fp16 && !D3D12Sidecar::TypedUavStoreSupported(TypedColorFormat(a_inDesc.Format))) {
            g.fp16 = true;
            logger::warn("[DLSS12] the D3D12 side has no typed UAV store for colour fmt={}; using the RGBA16F carrier",
                static_cast<int>(a_inDesc.Format));
        }
        for (int attempt = 0; attempt < 2; ++attempt) {
            const DXGI_FORMAT carrier = g.fp16 ? DXGI_FORMAT_R16G16B16A16_FLOAT : TypedColorFormat(a_inDesc.Format);
            if (g.owner12 == nullptr && D3D12Sidecar::Device() != nullptr) {
                g.owner12 = D3D12Sidecar::Device();
                g.owner12->AddRef();
            }
            const bool shared =
                D3D12Sidecar::CreateShared(g.colorIn, "dlss12 colour in", a_inDesc.Width, a_inDesc.Height, carrier, g.fp16) &&
                D3D12Sidecar::CreateShared(g.colorOut, "dlss12 colour out", a_outDesc.Width, a_outDesc.Height, carrier, true) &&
                D3D12Sidecar::CreateShared(g.depth, "dlss12 depth", a_key.depthW, a_key.depthH, DXGI_FORMAT_R32_FLOAT, true) &&
                D3D12Sidecar::CreateShared(g.mv, "dlss12 motion", a_key.mvW, a_key.mvH, a_key.mvFormat, true) &&
                D3D12Sidecar::CreateShared(g.fgDepth, "framegen depth (conditioned)", a_key.depthW, a_key.depthH, DXGI_FORMAT_R32_FLOAT, true) &&
                D3D12Sidecar::CreateShared(g.fgMv, "framegen motion (conditioned)", a_key.mvW, a_key.mvH, a_key.mvFormat, true) &&
                ((a_key.flags & kFlagAutoExposure) != 0 ||
                    D3D12Sidecar::CreateShared(g.exposure, "dlss12 exposure", 1, 1, DXGI_FORMAT_R32_FLOAT, false));
            if (shared) {
                break;
            }
            (void)TeardownShared();
            if (!g.fp16 && !g.carrierFallbackTried) {
                g.fp16 = true;
                g.carrierFallbackTried = true;
                logger::warn("[DLSS12] the driver refused to share the colour format; retrying with the RGBA16F carrier");
                continue;
            }
            Latch("a shared texture could not be created on this driver (see the [Sidecar] lines)");
            return RebuildOutcome::kRefused;
        }
        if (!g.colorIn.Valid()) {
            Latch("a shared texture could not be created on this driver (see the [Sidecar] lines)");
            return RebuildOutcome::kRefused;
        }
        if (!CreateUav(a_device, g.depth.d3d11, DXGI_FORMAT_R32_FLOAT, g.depthUav)) {
            Latch("a UAV on the shared depth texture would not create");
            (void)TeardownShared();
            return RebuildOutcome::kRefused;
        }
        if (!CreateUav(a_device, g.mv.d3d11, a_key.mvFormat, g.mvUav)) {
            ReleaseCom(g.mvUav);
            if ((g.p48Refused & 16U) == 0) {
                g.p48Refused |= 16U;
                logger::warn("[DLSS12] a UAV on the shared motion texture (fmt={}) would not create — motion-vector dilation is off for this session",
                    static_cast<int>(a_key.mvFormat));
            }
        }
        if (!CreateUav(a_device, g.fgDepth.d3d11, DXGI_FORMAT_R32_FLOAT, g.fgDepthUav) ||
            !CreateUav(a_device, g.fgMv.d3d11, a_key.mvFormat, g.fgMvUav)) {
            ReleaseCom(g.fgDepthUav);
            ReleaseCom(g.fgMvUav);
            if ((g.p48Refused & 32U) == 0) {
                g.p48Refused |= 32U;
                logger::warn("[DLSS12] a UAV on the conditioned textures would not create — first-person conditioning for frame generation is off for this session");
            }
        }
        if (g.fp16) {
            D3D11_TEXTURE2D_DESC scratch{};
            scratch.Width = a_outDesc.Width;
            scratch.Height = a_outDesc.Height;
            scratch.MipLevels = 1;
            scratch.ArraySize = 1;
            scratch.Format = a_outDesc.Format;
            scratch.SampleDesc.Count = 1;
            scratch.Usage = D3D11_USAGE_DEFAULT;
            scratch.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
            if (!CreateUav(a_device, g.colorIn.d3d11, DXGI_FORMAT_R16G16B16A16_FLOAT, g.colorInUav) ||
                FAILED(a_device->CreateShaderResourceView(g.colorOut.d3d11, nullptr, &g.colorOutSrv)) ||
                FAILED(a_device->CreateTexture2D(&scratch, nullptr, &g.outScratch)) ||
                !CreateUav(a_device, g.outScratch, TypedColorFormat(a_outDesc.Format), g.outScratchUav)) {
                Latch("the RGBA16F carrier views would not create");
                (void)TeardownShared();
                return RebuildOutcome::kRefused;
            }
        }

        SetCreateParams(a_key);
        ID3D12GraphicsCommandList* list = nullptr;
        if (!D3D12Sidecar::BeginCommands(list)) {
            Latch(D3D12Sidecar::Disabled() ? D3D12Sidecar::DisableReason() : "the sidecar command list would not begin");
            (void)TeardownShared();
            return RebuildOutcome::kRefused;
        }
        Ngx::Handle* handle = nullptr;
        const NgxD3D12::Outcome create = NgxD3D12::CoreCreate(list, Ngx::kFeatureSuperSampling, g.params, &handle);
        if (create.faultCode != 0) {
            (void)D3D12Sidecar::AbandonCommands();
            g.lastResult.store(Ngx::Code(create.result), std::memory_order_relaxed);
            char text[128]{};
            NgxD3D12::DescribeOutcome(create, text, sizeof(text));
            char reason[160]{};
            std::snprintf(reason, sizeof(reason), "CreateFeature(1) raised %s", text);
            Latch(reason);
            (void)TeardownShared();
            return RebuildOutcome::kRefused;
        }
        bool listAccepted = false;
        const std::uint64_t value = D3D12Sidecar::EndCommands(&listAccepted);
        const bool completed = D3D12Sidecar::WaitForValue(value, 4000);
        if (!listAccepted) {
            if (handle != nullptr) {
                (void)NgxD3D12::CoreRelease(handle);
            }
            Latch("the DLSS feature's creation list was dropped by the sidecar (its GPU initialisation never ran)");
            (void)TeardownShared();
            return RebuildOutcome::kRefused;
        }
        g.lastResult.store(Ngx::Code(create.result), std::memory_order_relaxed);
        char text[128]{};
        NgxD3D12::DescribeOutcome(create, text, sizeof(text));
        if (!completed) {
            D3D12Sidecar::Disable("DLSS feature creation did not complete within 4 s");
            Latch("DLSS feature creation did not complete on the GPU");
            return RebuildOutcome::kRefused;
        }
        if (!Ngx::Succeeded(create.result) || handle == nullptr) {
            char reason[160]{};
            std::snprintf(reason, sizeof(reason),
                "CreateFeature(1) failed %s for render %ux%u -> out %ux%u perfQuality=%d flags=%d preset=%u "
                "(is nvngx_dlss.dll staged in the NGX folder?)",
                text, a_key.renderW, a_key.renderH, a_key.outW, a_key.outH, a_key.perfQuality, a_key.flags, a_key.preset);
            Latch(reason);
            (void)TeardownShared();
            return RebuildOutcome::kRefused;
        }
        g.feature = handle;
        g.sessionSerial = NgxD3D12::SessionSerial();
        g.key = a_key;
        g.key.carrier = g.fp16 ? DXGI_FORMAT_R16G16B16A16_FLOAT : TypedColorFormat(a_inDesc.Format);
        g.strikes = 0;
        ++g.creates;
        g.snapCreates.store(g.creates, std::memory_order_relaxed);
        g.featureReady.store(true, std::memory_order_relaxed);
        logger::info("[DLSS12] feature 1 created: render {}x{} -> out {}x{} perfQuality={} flags={} preset={} carrier {} | depth {}x{} | motion {}x{} fmt={} (create #{})",
            a_key.renderW, a_key.renderH, a_key.outW, a_key.outH, a_key.perfQuality, a_key.flags, a_key.preset,
            g.fp16 ? "RGBA16F" : "native", a_key.depthW, a_key.depthH, a_key.mvW, a_key.mvH,
            static_cast<int>(a_key.mvFormat), g.creates);
        return RebuildOutcome::kBuilt;
    }

    void ReportTiming() noexcept
    {
        if (++g.windowFrames < kTimingWindow || g.qpf == 0) {
            return;
        }
        const float ms = static_cast<float>(g.cpuTicks) * 1000.0F / static_cast<float>(g.qpf) /
                         static_cast<float>(g.windowFrames);
        g.cpuMsAvg.store(ms, std::memory_order_relaxed);
        g.cpuTicks = 0;
        g.windowFrames = 0;
        static ULONGLONG s_lastLog = 0;
        const ULONGLONG now = ::GetTickCount64();
        if (now - s_lastLog >= 30000) {
            s_lastLog = now;
            logger::info("[DLSS12] timing: {:.2f} ms CPU/frame at the AA seam (copy-in, fence hops, evaluate, copy-back); {} evaluations",
                ms, g.evaluations);
        }
    }
}

namespace Platform::Dlss12Engine
{
    AaAvailability ProbeFirstDevice(ID3D11Device* a_device) noexcept
    {
        if (a_device == nullptr) {
            return AaAvailability::kUnavailable;
        }
        bool nvidia = false;
        bool d3d12 = false;
        try {
            IDXGIDevice* dxgiDevice = nullptr;
            if (SUCCEEDED(a_device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDevice))) &&
                dxgiDevice != nullptr) {
                IDXGIAdapter* adapter = nullptr;
                if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter)) && adapter != nullptr) {
                    DXGI_ADAPTER_DESC desc{};
                    if (SUCCEEDED(adapter->GetDesc(&desc))) {
                        nvidia = desc.VendorId == kNvidiaVendorId;
                    }
                    d3d12 = SUCCEEDED(::D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), nullptr));
                    adapter->Release();
                }
                dxgiDevice->Release();
            }
        } catch (...) {
            nvidia = false;
        }
        const bool core = nvidia && NgxD3D12::LoadCore();
        const bool sidecar = !D3D12Sidecar::Unavailable();
        const bool available = nvidia && d3d12 && core && sidecar;
        logger::info("[DLSS12] first-device probe: nvidia={} d3d12={} ngxCore={} sidecar={} -> DX12 mode {}",
            nvidia, d3d12, core, sidecar, available ? "available" : "unavailable");
        if (!sidecar) {
            logger::warn("[DLSS12] the sidecar refused at device creation ({}): the DirectX 12 engine is unavailable this session",
                D3D12Sidecar::DisableReason());
        }
        return available ? AaAvailability::kAvailable : AaAvailability::kUnavailable;
    }

    bool Evaluate(ID3D11DeviceContext* a_context, const Inputs& a_in) noexcept
    {
        if (a_context == nullptr || a_in.colorIn == nullptr || a_in.colorOut == nullptr || a_in.depth == nullptr ||
            a_in.motionVectors == nullptr || a_in.renderW == 0 || a_in.renderH == 0 || a_in.outW == 0 ||
            a_in.outH == 0) {
            return false;
        }
        if (g.latched.load(std::memory_order_relaxed)) {
            return false;
        }
        try {
            if (g.qpf == 0) {
                LARGE_INTEGER f{};
                ::QueryPerformanceFrequency(&f);
                g.qpf = f.QuadPart;
            }
            LARGE_INTEGER t0{};
            ::QueryPerformanceCounter(&t0);

            ID3D11Device* device = nullptr;
            a_context->GetDevice(&device);
            if (device == nullptr) {
                return false;
            }
            device->Release();

            if (D3D12Sidecar::Disabled()) {
                Latch(D3D12Sidecar::DisableReason());
                return false;
            }
            if (!D3D12Sidecar::IsOpen()) {
                if (!D3D12Sidecar::Open(device, a_context)) {
                    Latch(D3D12Sidecar::Disabled() ? D3D12Sidecar::DisableReason() : "the D3D12 sidecar did not open");
                    return false;
                }
            } else if (D3D12Sidecar::BoundDevice() != device) {
                (void)TeardownShared();
                ForgetSession();
                Latch("the D3D12 sidecar is bound to another D3D11 device (device recreated); DX12 mode needs a restart");
                return false;
            }
            if (!D3D12Sidecar::PollHealth()) {
                Latch(D3D12Sidecar::DisableReason());
                return false;
            }
            if (g.device != device) {
                ForgetSession();
                ReleaseViews();
                ReleaseOwnViews();
                g.device = device;
            }

            if (!g.pathsResolved) {
                const auto folder = SelfModuleDirectory() / L"FO4GraphicsOverhaul" / L"Streamline";
                g.dataPath = (folder / L"").wstring();
                g.pathsResolved = true;
                logger::info("[DLSS12] payload folder: {}", Narrow(g.dataPath.c_str()));
            }
            if (!NgxD3D12::LoadCore()) {
                Latch("the NGX core (_nvngx.dll) is unavailable - is the NVIDIA driver installed?");
                return false;
            }
            const NgxD3D12::Outcome session =
                NgxD3D12::InitSession(D3D12Sidecar::Device(), g.dataPath.c_str(), g.dataPath.c_str());
            if (!session.Ok()) {
                char text[128]{};
                NgxD3D12::DescribeOutcome(session, text, sizeof(text));
                char reason[160]{};
                std::snprintf(reason, sizeof(reason), "the NGX D3D12 session did not initialise (%s)", text);
                Latch(reason);
                return false;
            }
            if (NgxD3D12::SessionSerial() != g.sessionSerial) {
                ForgetSession();
                g.sessionSerial = NgxD3D12::SessionSerial();
            }
            if (NgxD3D12::SuperSamplingAvailable() == 0) {
                Latch("NGX reports DLSS Super Resolution unavailable on this adapter/driver (SuperSampling.Available=0)");
                return false;
            }
            if (g.params == nullptr) {
                g.params = NgxD3D12::AllocateParameters();
                if (g.params == nullptr) {
                    Latch(NgxD3D12::CoreFaulted() ? "NGX raised an exception allocating the parameter block"
                                                  : "NGX would not allocate a parameter block");
                    return false;
                }
            }

            D3D11_TEXTURE2D_DESC inDesc{}, outDesc{}, depthDesc{}, mvDesc{};
            a_in.colorIn->GetDesc(&inDesc);
            a_in.colorOut->GetDesc(&outDesc);
            if (!GetDesc(a_in.depth, depthDesc) || !GetDesc(a_in.motionVectors, mvDesc)) {
                Latch("the depth or motion-vector input is not a 2D texture");
                return false;
            }
            if (inDesc.SampleDesc.Count != 1 || outDesc.SampleDesc.Count != 1 || depthDesc.SampleDesc.Count != 1 ||
                mvDesc.SampleDesc.Count != 1) {
                Latch("a multisampled input cannot be shared with the D3D12 side");
                return false;
            }
            if (inDesc.Format != outDesc.Format) {
                Latch("the colour input and output formats differ (the shared tail expects one format)");
                return false;
            }
            if (a_in.renderW > inDesc.Width || a_in.renderH > inDesc.Height || a_in.outW != outDesc.Width ||
                a_in.outH != outDesc.Height) {
                Latch("the render/output extents do not fit the textures handed in");
                return false;
            }
            FeatureKey key{};
            key.renderW = a_in.renderW;
            key.renderH = a_in.renderH;
            key.outW = a_in.outW;
            key.outH = a_in.outH;
            key.perfQuality = a_in.perfQuality;
            key.flags = (a_in.colorIsHDR ? kFlagIsHdr : 0) | kFlagMvLowRes | (a_in.autoExposure ? kFlagAutoExposure : 0);
            key.preset = a_in.preset;
            key.carrier = g.fp16 ? DXGI_FORMAT_R16G16B16A16_FLOAT : TypedColorFormat(inDesc.Format);
            key.depthW = depthDesc.Width;
            key.depthH = depthDesc.Height;
            key.mvW = mvDesc.Width;
            key.mvH = mvDesc.Height;
            key.mvFormat = MotionViewFormat(mvDesc.Format);

            if (g.feature == nullptr || !(g.key == key)) {
                const RebuildOutcome rebuilt = Rebuild(device, key, inDesc, outDesc);
                if (rebuilt == RebuildOutcome::kTeardownPending) {
                    return false;
                }
                if (rebuilt == RebuildOutcome::kRefused) {
                    return false;
                }
                key.carrier = g.key.carrier;
            }
            g.snapRenderW.store(key.renderW, std::memory_order_relaxed);
            g.snapRenderH.store(key.renderH, std::memory_order_relaxed);
            g.snapOutW.store(key.outW, std::memory_order_relaxed);
            g.snapOutH.store(key.outH, std::memory_order_relaxed);

            const D3D12Sidecar::FrameLock frameLock;
            {
                const ComputeBindingsScope csScope(a_context);
                ID3D11Texture2D* depthTexture = nullptr;
                (void)a_in.depth->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&depthTexture));
                ID3D11ShaderResourceView* depthSrv = nullptr;
                if (depthTexture != nullptr) {
                    depthSrv = EnsureOwnSrv(device, depthTexture, DepthViewFormat(depthDesc.Format), g.ownDepthSrv,
                        g.ownDepthSrvTexture);
                    depthTexture->Release();
                }
                if (depthSrv == nullptr) {
                    Latch("the engine's depth texture cannot be read as a shader resource (format not viewable)");
                    return false;
                }
                Dispatch(a_context, g.csDepth, 0, depthSrv, g.depthUav, key.depthW, key.depthH);
                ID3D11ShaderResourceView* mvSrv = nullptr;
                {
                    ID3D11Texture2D* mvTexture = nullptr;
                    (void)a_in.motionVectors->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&mvTexture));
                    if (mvTexture != nullptr) {
                        mvSrv = EnsureOwnSrv(device, mvTexture, key.mvFormat, g.ownMvSrv, g.ownMvSrvTexture);
                        mvTexture->Release();
                    }
                }
                const bool dilate = g.csDilate != nullptr &&
                                    g.dilateParams != nullptr && g.mvUav != nullptr && mvSrv != nullptr;
                if (dilate) {
                    const float params[4]{ a_in.nearPlane, a_in.farPlane,
                        (a_in.nearPlane > 0.0F && a_in.farPlane > a_in.nearPlane) ? 1.0F : 0.0F, 0.0F };
                    a_context->UpdateSubresource(g.dilateParams, 0, nullptr, params, sizeof(params), sizeof(params));
                    a_context->CSSetShader(g.csDilate, nullptr, 0);
                    a_context->CSSetConstantBuffers(0, 1, &g.dilateParams);
                    ID3D11ShaderResourceView* srvs[] = { mvSrv, depthSrv };
                    a_context->CSSetShaderResources(10, 2, srvs);
                    a_context->CSSetUnorderedAccessViews(7, 1, &g.mvUav, nullptr);
                    a_context->Dispatch((key.mvW + 7U) / 8U, (key.mvH + 7U) / 8U, 1U);
                    ID3D11UnorderedAccessView* noUav = nullptr;
                    a_context->CSSetUnorderedAccessViews(7, 1, &noUav, nullptr);
                    ID3D11ShaderResourceView* noSrvs[] = { nullptr, nullptr };
                    a_context->CSSetShaderResources(10, 2, noSrvs);
                } else {
                    a_context->CopyResource(g.mv.d3d11, a_in.motionVectors);
                }
                bool conditioned = false;
                {
                    ID3D11ShaderResourceView* maskSrv = nullptr;
                    std::uint32_t maskW = 0, maskH = 0;
                    if (g.csFgDepth != nullptr && g.csFgMotion != nullptr && g.fgDepthUav != nullptr && g.fgMvUav != nullptr &&
                        mvSrv != nullptr && FirstPersonMask::TakeMask(g.maskCursor, maskSrv, maskW, maskH) &&
                        maskW >= key.depthW && maskH >= key.depthH) {
                        a_context->CSSetShader(g.csFgDepth, nullptr, 0);
                        ID3D11ShaderResourceView* srvs[] = { depthSrv, maskSrv };
                        a_context->CSSetShaderResources(7, 2, srvs);
                        a_context->CSSetUnorderedAccessViews(5, 1, &g.fgDepthUav, nullptr);
                        a_context->Dispatch((key.depthW + 7U) / 8U, (key.depthH + 7U) / 8U, 1U);
                        ID3D11UnorderedAccessView* noUav = nullptr;
                        a_context->CSSetUnorderedAccessViews(5, 1, &noUav, nullptr);
                        a_context->CSSetShader(g.csFgMotion, nullptr, 0);
                        a_context->CSSetShaderResources(9, 1, &mvSrv);
                        a_context->CSSetUnorderedAccessViews(6, 1, &g.fgMvUav, nullptr);
                        a_context->Dispatch((key.mvW + 7U) / 8U, (key.mvH + 7U) / 8U, 1U);
                        a_context->CSSetUnorderedAccessViews(6, 1, &noUav, nullptr);
                        ID3D11ShaderResourceView* noSrvs[] = { nullptr, nullptr, nullptr };
                        a_context->CSSetShaderResources(7, 3, noSrvs);
                        conditioned = true;
                    }
                }
                g.fgConditioned.store(conditioned, std::memory_order_relaxed);
                g.dilated.store(dilate, std::memory_order_relaxed);
                if (g.fp16) {
                    ID3D11ShaderResourceView* colorSrv = EnsureOwnSrv(device, a_in.colorIn, TypedColorFormat(inDesc.Format),
                        g.ownColorInSrv, g.ownColorInSrvTexture);
                    if (colorSrv == nullptr) {
                        Latch("the colour scratch cannot be read as a shader resource");
                        return false;
                    }
                    Dispatch(a_context, g.csColor, 2, colorSrv, g.colorInUav, inDesc.Width, inDesc.Height);
                } else {
                    a_context->CopyResource(g.colorIn.d3d11, a_in.colorIn);
                }
                if (!a_in.autoExposure && !g.exposureSeeded) {
                    const float one = 1.0F;
                    a_context->UpdateSubresource(g.exposure.d3d11, 0, nullptr, &one, sizeof(one), sizeof(one));
                    g.exposureSeeded = true;
                }
            }

            const std::uint64_t vIn = D3D12Sidecar::SignalFromD3D11();
            D3D12Sidecar::WaitOnD3D12(vIn);
            ID3D12GraphicsCommandList* list = nullptr;
            if (!D3D12Sidecar::BeginCommands(list)) {
                Latch(D3D12Sidecar::Disabled() ? D3D12Sidecar::DisableReason() : "the sidecar command list would not begin");
                return false;
            }
            D3D12Sidecar::Barrier(list, g.colorIn.d3d12, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            D3D12Sidecar::Barrier(list, g.depth.d3d12, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            D3D12Sidecar::Barrier(list, g.mv.d3d12, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            D3D12Sidecar::Barrier(list, g.exposure.d3d12, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            D3D12Sidecar::Barrier(list, g.colorOut.d3d12, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            SetEvaluateParams(a_in);
            const NgxD3D12::Outcome eval = NgxD3D12::CoreEvaluate(list, g.feature, g.params);
            if (eval.faultCode != 0) {
                (void)D3D12Sidecar::AbandonCommands();
                g.lastResult.store(Ngx::Code(eval.result), std::memory_order_relaxed);
                char text[128]{};
                NgxD3D12::DescribeOutcome(eval, text, sizeof(text));
                char reason[160]{};
                std::snprintf(reason, sizeof(reason), "EvaluateFeature(1) raised %s", text);
                Latch(reason);
                return false;
            }
            D3D12Sidecar::Barrier(list, g.colorIn.d3d12, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
            D3D12Sidecar::Barrier(list, g.depth.d3d12, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
            D3D12Sidecar::Barrier(list, g.mv.d3d12, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
            D3D12Sidecar::Barrier(list, g.exposure.d3d12, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
            D3D12Sidecar::Barrier(list, g.colorOut.d3d12, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
            bool listAccepted = false;
            const std::uint64_t vOut = D3D12Sidecar::EndCommands(&listAccepted);
            const bool outputOrdered = D3D12Sidecar::WaitOnD3D11(vOut);

            g.lastResult.store(Ngx::Code(eval.result), std::memory_order_relaxed);
            if (!listAccepted) {
                if (++g.strikes <= kEvaluateStrikes) {
                    logger::error("[DLSS12] the evaluate's list was dropped by the sidecar - the frame stays native (strike {}/{})",
                        g.strikes, kEvaluateStrikes);
                }
                if (g.strikes >= kEvaluateStrikes) {
                    Latch("the sidecar dropped the evaluate's command list repeatedly");
                }
                return false;
            }
            if (!Ngx::Succeeded(eval.result)) {
                if (++g.strikes <= kEvaluateStrikes) {
                    logger::error("[DLSS12] evaluate failed {:#010x} {} (strike {}/{})", Ngx::Code(eval.result),
                        Ngx::ResultName(eval.result), g.strikes, kEvaluateStrikes);
                }
                if (g.strikes >= kEvaluateStrikes) {
                    char reason[160]{};
                    std::snprintf(reason, sizeof(reason), "%d consecutive evaluate failures (last %#010x %s)",
                        kEvaluateStrikes, Ngx::Code(eval.result), Ngx::ResultName(eval.result));
                    Latch(reason);
                }
                return false;
            }
            g.strikes = 0;
            ++g.evaluations;
            g.snapEvaluations.store(g.evaluations, std::memory_order_relaxed);
            {
                Platform::SidecarFrame::Guides guides{};
                guides.depth = g.depth.d3d12;
                guides.mv = g.mv.d3d12;
                guides.depthWidth = key.depthW;
                guides.depthHeight = key.depthH;
                guides.mvWidth = key.mvW;
                guides.mvHeight = key.mvH;
                guides.renderWidth = key.renderW;
                guides.renderHeight = key.renderH;
                guides.depthFormat = DXGI_FORMAT_R32_FLOAT;
                guides.mvFormat = key.mvFormat;
                guides.jitterX = a_in.jitterX;
                guides.jitterY = a_in.jitterY;
                guides.reset = a_in.reset;
                guides.motion = g.dilated.load(std::memory_order_relaxed) ? Platform::SidecarFrame::GuideMotion::kDilated
                                                                          : Platform::SidecarFrame::GuideMotion::kRaw;
                if (g.fgConditioned.load(std::memory_order_relaxed)) {
                    guides.fgDepth = g.fgDepth.d3d12;
                    guides.fgMv = g.fgMv.d3d12;
                }
                Platform::SidecarFrame::PublishGuides(guides);
            }

            if (!outputOrdered) {
                return false;
            }
            if (g.fp16) {
                const ComputeBindingsScope csScope(a_context);
                Dispatch(a_context, g.csColor, 2, g.colorOutSrv, g.outScratchUav, outDesc.Width, outDesc.Height);
                a_context->CopyResource(a_in.colorOut, g.outScratch);
            } else {
                a_context->CopyResource(a_in.colorOut, g.colorOut.d3d11);
            }

            LARGE_INTEGER t1{};
            ::QueryPerformanceCounter(&t1);
            g.cpuTicks += t1.QuadPart - t0.QuadPart;
            if (g.evaluations == 1) {
                logger::info("[DLSS12] ★ first DX12 DLSS frame delivered: render {}x{} -> out {}x{}, carrier {}, reset={}, jitter=({:.4f},{:.4f})",
                    key.renderW, key.renderH, key.outW, key.outH, g.fp16 ? "RGBA16F" : "native", a_in.reset ? 1 : 0,
                    a_in.jitterX, a_in.jitterY);
            }
            ReportTiming();
            return true;
        } catch (...) {
            Latch("C++ exception in the DX12 DLSS engine");
            return false;
        }
    }

    AaTeardownResult DestroyFeature() noexcept
    {
        try {
            if (!HasAnything()) {
                ReleaseViews();
                Rearm();
                return AaTeardownResult::kAlreadyAbsent;
            }
            const std::uint64_t evaluations = g.evaluations;
            if (!TeardownShared()) {
                return AaTeardownResult::kFailed;
            }
            Rearm();
            logger::info("[DLSS12] feature 1 {} after {} evaluation(s) (staged teardown; the next evaluate recreates it)",
                g.lastTeardownAbandoned ? "RETAINED (abandoned: retirement unproven, leaked)" : "released", evaluations);
            return AaTeardownResult::kDestroyed;
        } catch (...) {
            return AaTeardownResult::kFailed;
        }
    }

    void RearmExposure() noexcept
    {
        g.exposureSeeded = false;
    }

    State Snapshot() noexcept
    {
        State state{};
        state.latched = g.latched.load(std::memory_order_relaxed);
        std::snprintf(state.reason, sizeof(state.reason), "%s", g.reason);
        state.featureReady = g.featureReady.load(std::memory_order_relaxed);
        state.renderW = g.snapRenderW.load(std::memory_order_relaxed);
        state.renderH = g.snapRenderH.load(std::memory_order_relaxed);
        state.outW = g.snapOutW.load(std::memory_order_relaxed);
        state.outH = g.snapOutH.load(std::memory_order_relaxed);
        state.evaluations = g.snapEvaluations.load(std::memory_order_relaxed);
        state.creates = g.snapCreates.load(std::memory_order_relaxed);
        state.cpuMs = g.cpuMsAvg.load(std::memory_order_relaxed);
        state.fp16Carrier = g.fp16;
        state.lastResult = g.lastResult.load(std::memory_order_relaxed);
        return state;
    }

    bool MotionDilationApplied() noexcept { return g.dilated.load(std::memory_order_relaxed); }
    bool FirstPersonConditionedLastFrame() noexcept { return g.fgConditioned.load(std::memory_order_relaxed); }

}
