#include "PCH.h"

#include "Platform/NeuralPass.h"

#include "Platform/D3D12Sidecar.h"
#include "Platform/Fallout4Renderer.h"
#include "Platform/FrameBufferResolve.h"
#include "Platform/NeuralMailbox.h"
#include "Platform/NeuralRenderer.h"
#include "Platform/OutputMergerScope.h"
#include "Platform/RendererContracts.h"
#include "Platform/SidecarCompute.h"
#include "Platform/SidecarFrame.h"
#include "Platform/SidecarGuides.h"
#include "Platform/Streamline.h"

#include "RE/Bethesda/BSGraphics.h"

#include <d3d11.h>

#include <algorithm>
#include <bit>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "CSNeuralColor.h"
#include "CSNeuralDepth.h"
#include "CSDilateMotion.h"

namespace
{
    using namespace Platform;
    using namespace Platform::SidecarGuides;

    constexpr std::uint32_t kFrameBufferIndex = static_cast<std::uint32_t>(Platform::Fallout4RenderTargetIndex::kFrameBuffer);
    constexpr std::uint32_t kMainDepthIndex = 2;
    constexpr std::uint32_t kMotionVectorIndex = 29;
    constexpr std::uint32_t kSettleFrames = 6;
    constexpr std::uint32_t kTimingWindow = 600;

    struct ShapeKey
    {
        std::uint32_t rt0W{ 0 }, rt0H{ 0 };
        DXGI_FORMAT rt0Fmt{ DXGI_FORMAT_UNKNOWN };
        std::uint32_t depthW{ 0 }, depthH{ 0 };
        DXGI_FORMAT depthFmt{ DXGI_FORMAT_UNKNOWN };
        std::uint32_t mvW{ 0 }, mvH{ 0 };
        DXGI_FORMAT mvFmt{ DXGI_FORMAT_UNKNOWN };
        std::uint32_t renderW{ 0 }, renderH{ 0 };
        std::uint32_t carrier{ 0 };
        std::uint32_t modelW{ 0 }, modelH{ 0 };

        [[nodiscard]] bool operator==(const ShapeKey&) const noexcept = default;
    };

    [[nodiscard]] bool SameColourShape(const ShapeKey& a, const ShapeKey& b) noexcept
    {
        return a.rt0W == b.rt0W && a.rt0H == b.rt0H && a.rt0Fmt == b.rt0Fmt && a.carrier == b.carrier &&
               a.modelW == b.modelW && a.modelH == b.modelH;
    }

    [[nodiscard]] bool SameGuideAllocation(const ShapeKey& a, const ShapeKey& b) noexcept
    {
        return a.depthW == b.depthW && a.depthH == b.depthH && a.depthFmt == b.depthFmt &&
               a.mvW == b.mvW && a.mvH == b.mvH && a.mvFmt == b.mvFmt;
    }

    Neural::CascadeSettings g_settings{};
    std::atomic<bool> g_enabled{ false };
    std::uint64_t g_pumpFrame{ 0 };
    std::atomic<std::uint32_t> g_activePasses{ 0 };
    std::atomic<std::uint32_t> g_lastCompletedPasses{ 0 };
    std::uint32_t g_frameColourCopies = 0;
    std::uint32_t g_frameCrossApiTrips = 0;
    std::uint32_t g_frameDelivered = 0;
    std::atomic<std::uint32_t> g_lastColourCopies{ 0 };
    std::atomic<std::uint32_t> g_lastCrossApiTrips{ 0 };
    std::uint64_t g_frameCopyBytes = 0;
    std::atomic<std::uint64_t> g_lastCopyBytes{ 0 };

    struct D3D11Bracket
    {
        static constexpr std::uint32_t kSlots = 8;
        struct Slot
        {
            ID3D11Query* disjoint{ nullptr };
            ID3D11Query* begin{ nullptr };
            ID3D11Query* end{ nullptr };
            bool inFlight{ false };
        };
        Slot slots[kSlots]{};
        std::uint32_t next{ 0 };
        std::uint32_t oldest{ 0 };
        std::uint32_t inFlight{ 0 };
        int open{ -1 };
        ID3D11Device* device{ nullptr };
        bool ready{ false };
        Neural::TimingWindow window{};
        std::atomic<std::uint64_t> retired{ 0 };
        std::atomic<float> median{ 0.0F };
    };
    D3D11Bracket g_prepBracket{};
    D3D11Bracket g_returnBracket{};
    std::atomic<bool> g_d3d11Timed{ false };
    std::atomic<std::uint32_t> g_d3d11Windows{ 0 };
    std::uint64_t g_transferWindowFrames = 0;
    [[nodiscard]] bool WindowNearlyFull(const D3D11Bracket& b) noexcept
    {
        return b.window.count + Neural::kMaxPasses > Neural::TimingWindow::kCapacity;
    }

    void ReleaseBracket(D3D11Bracket& b) noexcept
    {
        for (auto& slot : b.slots) {
            if (slot.disjoint != nullptr) slot.disjoint->Release();
            if (slot.begin != nullptr) slot.begin->Release();
            if (slot.end != nullptr) slot.end->Release();
            slot = D3D11Bracket::Slot{};
        }
        if (b.device != nullptr) b.device->Release();
        b.device = nullptr;
        b.ready = false;
        b.next = b.oldest = b.inFlight = 0;
        b.open = -1;
        b.window.Clear();
        b.median.store(0.0F, std::memory_order_relaxed);
        b.retired.store(0, std::memory_order_relaxed);
    }

    void ReleaseBrackets() noexcept
    {
        ReleaseBracket(g_prepBracket);
        ReleaseBracket(g_returnBracket);
        g_d3d11Timed.store(false, std::memory_order_relaxed);
        g_d3d11Windows.store(0, std::memory_order_relaxed);
        g_transferWindowFrames = 0;
    }

    [[nodiscard]] bool EnsureBracket(D3D11Bracket& b, ID3D11Device* a_device) noexcept
    {
        if (b.ready && b.device == a_device) return true;
        ReleaseBracket(b);
        if (a_device == nullptr) return false;
        const D3D11_QUERY_DESC disjoint{ D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
        const D3D11_QUERY_DESC stamp{ D3D11_QUERY_TIMESTAMP, 0 };
        for (auto& slot : b.slots) {
            if (FAILED(a_device->CreateQuery(&disjoint, &slot.disjoint)) || FAILED(a_device->CreateQuery(&stamp, &slot.begin)) ||
                FAILED(a_device->CreateQuery(&stamp, &slot.end))) {
                ReleaseBracket(b);
                return false;
            }
        }
        b.device = a_device;
        b.device->AddRef();
        b.ready = true;
        return true;
    }

    void BracketBegin(D3D11Bracket& b, ID3D11DeviceContext* a_context) noexcept
    {
        if (!b.ready || a_context == nullptr) return;
        if (b.open >= 0) {
            a_context->End(b.slots[b.open].disjoint);
            b.open = -1;
        }
        if (b.inFlight >= D3D11Bracket::kSlots) return;
        auto& slot = b.slots[b.next];
        a_context->Begin(slot.disjoint);
        a_context->End(slot.begin);
        b.open = static_cast<int>(b.next);
    }

    void BracketEnd(D3D11Bracket& b, ID3D11DeviceContext* a_context) noexcept
    {
        if (b.open < 0 || a_context == nullptr) return;
        auto& slot = b.slots[b.open];
        a_context->End(slot.end);
        a_context->End(slot.disjoint);
        slot.inFlight = true;
        b.next = (static_cast<std::uint32_t>(b.open) + 1U) % D3D11Bracket::kSlots;
        ++b.inFlight;
        b.open = -1;
    }

    void BracketRetire(D3D11Bracket& b, ID3D11DeviceContext* a_context) noexcept
    {
        if (!b.ready || a_context == nullptr) return;
        while (b.inFlight != 0U) {
            auto& slot = b.slots[b.oldest];
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT period{};
            const HRESULT hr = a_context->GetData(slot.disjoint, &period, sizeof(period), D3D11_ASYNC_GETDATA_DONOTFLUSH);
            if (hr == S_FALSE) return;
            if (hr == S_OK && !period.Disjoint && period.Frequency != 0ULL) {
                UINT64 t0 = 0;
                UINT64 t1 = 0;
                if (a_context->GetData(slot.begin, &t0, sizeof(t0), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
                    a_context->GetData(slot.end, &t1, sizeof(t1), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK && t1 >= t0) {
                    b.window.Push(static_cast<float>(1000.0 * static_cast<double>(t1 - t0) / static_cast<double>(period.Frequency)));
                    b.retired.fetch_add(1U, std::memory_order_relaxed);
                }
            }
            slot.inFlight = false;
            b.oldest = (b.oldest + 1U) % D3D11Bracket::kSlots;
            --b.inFlight;
        }
    }

    struct BracketScope
    {
        D3D11Bracket* bracket{ nullptr };
        ID3D11DeviceContext* context{ nullptr };
        BracketScope(D3D11Bracket& a_bracket, ID3D11DeviceContext* a_context, bool a_on) noexcept
        {
            if (a_on) {
                bracket = &a_bracket;
                context = a_context;
                BracketBegin(a_bracket, a_context);
            }
        }
        void Close() noexcept
        {
            if (bracket != nullptr) BracketEnd(*bracket, context);
            bracket = nullptr;
        }
        ~BracketScope() { Close(); }
        BracketScope(const BracketScope&) = delete;
        BracketScope& operator=(const BracketScope&) = delete;
    };

    [[nodiscard]] std::uint64_t CopyBytes(std::uint32_t a_width, std::uint32_t a_height, DXGI_FORMAT a_format) noexcept
    {
        std::uint64_t bytesPerPixel = 0;
        switch (a_format) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8X8_TYPELESS:
        case DXGI_FORMAT_B8G8R8X8_UNORM:
        case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        case DXGI_FORMAT_R10G10B10A2_UNORM:
        case DXGI_FORMAT_R11G11B10_FLOAT:
            bytesPerPixel = 4;
            break;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
        case DXGI_FORMAT_R16G16B16A16_UNORM:
            bytesPerPixel = 8;
            break;
        default:
            break;
        }
        return static_cast<std::uint64_t>(a_width) * a_height * bytesPerPixel;
    }
    std::atomic<bool> g_lastResetFlag{ false };
    char g_blockReason[160]{};
    std::atomic<std::uint32_t> g_modelW{ 0 }, g_modelH{ 0 };
    char g_modelReason[160]{};
    std::uint32_t g_modelLoggedPercent{ 0 };
    char g_modelLoggedReason[160]{};

    struct PassContext
    {
        std::uint32_t index{ 0 };
        ID3D11Device* owner11{ nullptr };
        ID3D12Device* owner12{ nullptr };
        std::atomic<bool> failed{ false };
        char failReason[160]{};

        bool built{ false };
        ShapeKey activeKey{};
        Neural::Settings activeSettings{};
        ShapeKey pendingKey{};
        std::uint32_t pendingFrames{ 0 };
        bool adoptedGuides{ false };
        std::uint32_t carrierResolved{ Neural::kCarrierNative };
        bool carrierFallbackTried{ false };
        SidecarFrame::Cursor guidesCursor{};
        std::atomic<int> guidesSource{ -1 };
        unsigned guidesSeen{ 0 };
        bool guideMismatchLogged{ false };
        bool statelessLogged{ false };

        D3D12Sidecar::SharedTexture colorIn{};
        D3D12Sidecar::SharedTexture colorOut{};
        D3D12Sidecar::SharedTexture depth{};
        D3D12Sidecar::SharedTexture mv{};
        ID3D12Resource* modelIn{ nullptr };
        ID3D12Resource* modelOut{ nullptr };
        ID3D11ComputeShader* csDepth{ nullptr };
        ID3D11ComputeShader* csColor{ nullptr };
        ID3D11ComputeShader* csDilate{ nullptr };
        ID3D11Buffer* dilateParams{ nullptr };
        ID3D11UnorderedAccessView* depthUav{ nullptr };
        ID3D11UnorderedAccessView* mvUav{ nullptr };
        ID3D11UnorderedAccessView* colorInUav{ nullptr };
        ID3D11ShaderResourceView* colorOutSrv{ nullptr };
        ID3D11Texture2D* rt0Scratch{ nullptr };
        ID3D11UnorderedAccessView* rt0ScratchUav{ nullptr };
        ID3D11ShaderResourceView* ownDepthSrv{ nullptr };
        ID3D11Texture2D* ownDepthSrvTexture{ nullptr };
        ID3D11ShaderResourceView* ownMvSrv{ nullptr };
        ID3D11Texture2D* ownMvSrvTexture{ nullptr };
        ID3D11ShaderResourceView* ownRt0Srv{ nullptr };
        ID3D11Texture2D* ownRt0SrvTexture{ nullptr };

        bool needReset{ true };
        std::uint64_t lastHistorySerial{ 0 };
        Neural::HistoryEpochConsumer epoch{};
        AaEffectiveEngine lastEngine{ AaEffectiveEngine::kUnresolved };
        std::uint32_t rebuilds{ 0 };
        std::uint32_t rebuildsInWindow{ 0 };
        ULONGLONG rebuildWindowStart{ 0 };
        ULONGLONG pausedUntil{ 0 };
        std::uint32_t pauseCycles{ 0 };

        std::uint64_t frames{ 0 };
        std::uint64_t evaluated{ 0 };
        LONGLONG qpf{ 0 };
        LONGLONG cpuTicks{ 0 };
        std::uint64_t windowFrames{ 0 };
        std::atomic<float> cpuMsAvg{ 0.0F };
        Neural::TimingWindow gpuWindow{};
        std::atomic<float> gpuMsMedian{ 0.0F };
        std::atomic<float> gpuMsP99{ 0.0F };
        std::atomic<bool> activeLastFrame{ false };
        std::atomic<std::uint32_t> snapshotOutW{ 0 }, snapshotOutH{ 0 }, snapshotRenderW{ 0 }, snapshotRenderH{ 0 };

    };
    std::array<PassContext, Neural::kMaxPasses> g_passes{};

    struct ChainFrame
    {
        struct Pending
        {
            bool historyChanged{ false };
            std::uint64_t historySerial{ 0 };
            bool epochChanged{ false };
            std::uint64_t epoch{ 0 };
            bool engineChanged{ false };
            AaEffectiveEngine engine{ AaEffectiveEngine::kUnresolved };
            bool reset{ false };
            ShapeKey key{};
        };
        bool active{ false };
        std::uint32_t delivered{ 0 };
        std::uint64_t lastToken{ 0 };
        PassContext* last{ nullptr };
        ID3D12Resource* previousColour{ nullptr };
        ID3D12Resource* base{ nullptr };
        ID3D12Resource* firstModelIn{ nullptr };
        ID3D12Resource* guideDepth{ nullptr };
        ID3D12Resource* guideMv{ nullptr };
        ShapeKey key{};
        std::array<Pending, Neural::kMaxPasses> pending{};
    };
    ChainFrame g_chain{};

    void InvalidateHistoryFrom(std::uint32_t a_first) noexcept
    {
        const std::uint32_t mask = Neural::HistoryTailMask(a_first);
        for (std::uint32_t i = 0; i < Neural::kMaxPasses; ++i) {
            if ((mask & (1U << i)) != 0U) g_passes[i].needReset = true;
        }
    }

    struct HistoryFrameScope
    {
        std::uint32_t completed{ 0 };
        ~HistoryFrameScope() noexcept { InvalidateHistoryFrom(completed); }
    };

    template <class T>
    void SafeRelease(T*& a_ptr) noexcept
    {
        if (a_ptr != nullptr) {
            a_ptr->Release();
            a_ptr = nullptr;
        }
    }

    void Fail(PassContext& g, const char* a_reason) noexcept
    {
        if (!g.failed.exchange(true, std::memory_order_relaxed)) {
            std::snprintf(g.failReason, sizeof(g.failReason), "%s", a_reason != nullptr ? a_reason : "unknown");
            logger::warn("[Neural] pass {} stopped: {}; the preceding image is retained until retry", g.index + 1U, g.failReason);
            SidecarFrame::InvalidateHudless();
        }
    }

    void NoteGuidesSource(PassContext& g, int a_source) noexcept
    {
        g.guidesSource.store(a_source, std::memory_order_relaxed);
        const unsigned bit = 1U << static_cast<unsigned>(a_source + 1);
        if ((g.guidesSeen & bit) == 0U) {
            g.guidesSeen |= bit;
            logger::info("[Neural] pass {} guides (first time): {}", g.index + 1U, a_source == 1
                ? "the DirectX 12 DLSS engine's shared depth/MV (dilated motion), taken on the sidecar (no conversion dispatches)"
                : a_source == 2 ? "own derivation, dilated motion (the engine's motion-vector kernel on the pass's own textures)"
                                : "none (the neural pass is not running: off or latched)");
        }
    }

    [[nodiscard]] bool CreateShadersOnce(PassContext& g, ID3D11Device* a_device) noexcept
    {
        if (g.owner11 != nullptr && g.owner11 != a_device) {
            Fail(g, "the stage still owns another device's shaders");
            return false;
        }
        if (g.owner11 == nullptr) {
            g.owner11 = a_device;
            a_device->AddRef();
        }
        if (g.csDepth != nullptr && g.csColor != nullptr && g.csDilate != nullptr && g.dilateParams != nullptr) {
            return true;
        }
        if (g.csDepth == nullptr &&
            FAILED(a_device->CreateComputeShader(g_csNeuralDepth, sizeof(g_csNeuralDepth), nullptr, &g.csDepth))) {
            Fail(g, "the depth guide shader would not create");
            return false;
        }
        if (g.csColor == nullptr &&
            FAILED(a_device->CreateComputeShader(g_csNeuralColor, sizeof(g_csNeuralColor), nullptr, &g.csColor))) {
            Fail(g, "the colour carrier shader would not create");
            return false;
        }
        if (g.csDilate == nullptr &&
            FAILED(a_device->CreateComputeShader(g_csDilateMotion, sizeof(g_csDilateMotion), nullptr, &g.csDilate))) {
            Fail(g, "the motion dilation shader would not create");
            return false;
        }
        if (g.dilateParams == nullptr) {
            D3D11_BUFFER_DESC cb{};
            cb.ByteWidth = 16;
            cb.Usage = D3D11_USAGE_DEFAULT;
            cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            if (FAILED(a_device->CreateBuffer(&cb, nullptr, &g.dilateParams))) {
                Fail(g, "the motion dilation constants would not create");
                return false;
            }
        }
        return true;
    }

    void DispatchDilate(ID3D11DeviceContext* a_context, PassContext& g, ID3D11ShaderResourceView* a_mvSrv,
        ID3D11ShaderResourceView* a_depthSrv, const ShapeKey& a_key) noexcept
    {
        const CameraConstants cam = Fallout4Renderer::CameraSnapshot();
        const bool useNear = std::isfinite(cam.nearPlane) && std::isfinite(cam.farPlane) &&
                             cam.nearPlane > 0.0F && cam.farPlane > cam.nearPlane;
        const float params[4]{ useNear ? cam.nearPlane : 0.0F, useNear ? cam.farPlane : 0.0F, useNear ? 1.0F : 0.0F, 0.0F };
        a_context->UpdateSubresource(g.dilateParams, 0, nullptr, params, sizeof(params), sizeof(params));
        a_context->CSSetShader(g.csDilate, nullptr, 0);
        a_context->CSSetConstantBuffers(0, 1, &g.dilateParams);
        ID3D11ShaderResourceView* srvs[]{ a_mvSrv, a_depthSrv };
        a_context->CSSetShaderResources(10, 2, srvs);
        a_context->CSSetUnorderedAccessViews(7, 1, &g.mvUav, nullptr);
        a_context->Dispatch((a_key.mvW + 7U) / 8U, (a_key.mvH + 7U) / 8U, 1U);
        ID3D11UnorderedAccessView* noUav = nullptr;
        a_context->CSSetUnorderedAccessViews(7, 1, &noUav, nullptr);
        ID3D11ShaderResourceView* noSrvs[]{ nullptr, nullptr };
        a_context->CSSetShaderResources(10, 2, noSrvs);
    }

    void ReleaseViews(PassContext& g) noexcept
    {
        SafeRelease(g.depthUav);
        SafeRelease(g.mvUav);
        SafeRelease(g.colorInUav);
        SafeRelease(g.colorOutSrv);
        SafeRelease(g.rt0ScratchUav);
        SafeRelease(g.rt0Scratch);
    }

    [[nodiscard]] bool HasGpuResources(const PassContext& g) noexcept
    {
        return g.owner12 != nullptr || g.colorIn.Valid() || g.colorOut.Valid() || g.modelIn != nullptr ||
            g.modelOut != nullptr || g.depth.Valid() || g.mv.Valid() || NeuralRenderer::HasResources(g.index);
    }

    void AbandonStage(PassContext& g) noexcept
    {
        logger::warn("[Neural] pass {} RETAINED: the GPU did not prove retirement of its work after a forced release; its feature and "
                     "images are leaked, never freed under the GPU ('Reset & retry filter' proves retirement before rebuilding)",
            g.index + 1U);
        SidecarFrame::InvalidateHudless();
        NeuralRenderer::AbandonPass(g.index);
        ReleaseViews(g);
        D3D12Sidecar::ForgetShared(g.colorIn);
        D3D12Sidecar::ForgetShared(g.colorOut);
        g.modelIn = nullptr;
        g.modelOut = nullptr;
        D3D12Sidecar::ForgetShared(g.depth);
        D3D12Sidecar::ForgetShared(g.mv);
        SafeRelease(g.owner12);
        g.built = false;
    }

    [[nodiscard]] bool RetireStages(std::uint32_t a_mask) noexcept
    {
        bool drain = false;
        std::uint32_t onClosedSession = 0;
        for (std::uint32_t i = 0; i < Neural::kMaxPasses; ++i) {
            const auto& stage = g_passes[i];
            if ((a_mask & (1U << i)) == 0U || !HasGpuResources(stage)) continue;
            if (stage.owner12 != nullptr && FAILED(stage.owner12->GetDeviceRemovedReason())) continue;
            if (!D3D12Sidecar::IsOpen() || (stage.owner12 != nullptr && stage.owner12 != D3D12Sidecar::Device())) {
                onClosedSession |= 1U << i;
                continue;
            }
            drain = true;
        }
        if (drain && !D3D12Sidecar::ProveRetired()) {
            if (!D3D12Sidecar::Disabled()) {
                std::snprintf(g_blockReason, sizeof(g_blockReason), "GPU retirement pending; neural resources retained");
                return false;
            }
            for (std::uint32_t i = 0; i < Neural::kMaxPasses; ++i) {
                auto& stage = g_passes[i];
                if ((a_mask & (1U << i)) != 0U && stage.owner12 != nullptr && stage.owner12 == D3D12Sidecar::Device()) {
                    AbandonStage(stage);
                }
            }
        }
        for (std::uint32_t i = 0; i < Neural::kMaxPasses; ++i) {
            if ((onClosedSession & (1U << i)) != 0U && !D3D12Sidecar::SessionRetired(g_passes[i].owner12)) AbandonStage(g_passes[i]);
        }
        return true;
    }

    [[nodiscard]] bool TeardownShared(PassContext& g, bool a_retired = false) noexcept
    {
        if (!a_retired && !RetireStages(1U << g.index)) return false;
        SidecarFrame::InvalidateHudless();
        NeuralRenderer::ReleasePass(g.index);
        ReleaseViews(g);
        D3D12Sidecar::ReleaseShared(g.colorIn);
        D3D12Sidecar::ReleaseShared(g.colorOut);
        SafeRelease(g.modelIn);
        SafeRelease(g.modelOut);
        D3D12Sidecar::ReleaseShared(g.depth);
        D3D12Sidecar::ReleaseShared(g.mv);
        SafeRelease(g.owner12);
        g.built = false;
        return true;
    }

    [[nodiscard]] bool TeardownOwnGuides(PassContext& g) noexcept
    {
        if (!g.depth.Valid() && !g.mv.Valid()) {
            return true;
        }
        if (!D3D12Sidecar::DrainGpu()) {
            return false;
        }
        SafeRelease(g.depthUav);
        SafeRelease(g.mvUav);
        D3D12Sidecar::ReleaseShared(g.depth);
        D3D12Sidecar::ReleaseShared(g.mv);
        return true;
    }

    [[nodiscard]] bool TeardownAll(PassContext& g, bool a_retired = false) noexcept
    {
        if (!TeardownShared(g, a_retired)) {
            return false;
        }
        SafeRelease(g.ownDepthSrv);
        g.ownDepthSrvTexture = nullptr;
        SafeRelease(g.ownMvSrv);
        g.ownMvSrvTexture = nullptr;
        SafeRelease(g.ownRt0Srv);
        g.ownRt0SrvTexture = nullptr;
        g.activeKey = ShapeKey{};
        g.pendingFrames = 0;
        SafeRelease(g.csDepth);
        SafeRelease(g.csColor);
        SafeRelease(g.csDilate);
        SafeRelease(g.dilateParams);
        SafeRelease(g.owner11);
        return true;
    }

    [[nodiscard]] bool EnsureOwnGuides(PassContext& g, ID3D11Device* a_device, const ShapeKey& a_key) noexcept
    {
        if (g.depth.Valid() && g.mv.Valid() && g.depthUav != nullptr && g.mvUav != nullptr) {
            return true;
        }
        if (!D3D12Sidecar::CreateShared(g.depth, "neural depth", a_key.depthW, a_key.depthH, DXGI_FORMAT_R32_FLOAT, true) ||
            !D3D12Sidecar::CreateShared(g.mv, "neural motion", a_key.mvW, a_key.mvH, DXGI_FORMAT_R16G16_FLOAT, true) ||
            !CreateUav(a_device, g.depth.d3d11, DXGI_FORMAT_R32_FLOAT, g.depthUav) ||
            !CreateUav(a_device, g.mv.d3d11, DXGI_FORMAT_R16G16_FLOAT, g.mvUav)) {
            Fail(g, "the pass's own guide textures could not be created (see the [Sidecar] lines)");
            return false;
        }
        logger::info("[Neural] pass {} own guide textures built: depth {}x{}, motion {}x{} (this frame has no DirectX 12 DLSS guides to take)",
            g.index + 1U, a_key.depthW, a_key.depthH, a_key.mvW, a_key.mvH);
        return true;
    }

    enum class RebuildOutcome
    {
        kBuilt,
        kTeardownPending,
        kRefused,
        kCarrierRefused,
    };

    [[nodiscard]] RebuildOutcome Rebuild(PassContext& g, ID3D11Device* a_device, const ShapeKey& a_key, const Neural::Settings& a_settings) noexcept
    {
        if (!TeardownShared(g)) {
            return RebuildOutcome::kTeardownPending;
        }
        g.owner12 = D3D12Sidecar::Device();
        if (g.owner12 == nullptr) return RebuildOutcome::kRefused;
        g.owner12->AddRef();
        const bool fp16 = a_key.carrier == Neural::kCarrierFp16;
        const DXGI_FORMAT carrierFormat = fp16 ? DXGI_FORMAT_R16G16B16A16_FLOAT : TypedColorFormat(a_key.rt0Fmt);

        if (!D3D12Sidecar::CreateShared(g.colorIn, "neural colour in", a_key.rt0W, a_key.rt0H, carrierFormat, fp16) ||
            !D3D12Sidecar::CreateShared(g.colorOut, "neural colour out", a_key.rt0W, a_key.rt0H, carrierFormat, true)) {
            if (!fp16 && !g.carrierFallbackTried) {
                logger::warn("[Neural] pass {} the native colour carrier's shared texture would not create (see the [Sidecar] lines); FP16 next",
                    g.index + 1U);
                (void)TeardownShared(g);
                return RebuildOutcome::kCarrierRefused;
            }
            Fail(g, "a shared texture could not be created on this driver (see the [Sidecar] lines)");
            (void)TeardownShared(g);
            return RebuildOutcome::kRefused;
        }
        if (fp16) {
            if (!CreateUav(a_device, g.colorIn.d3d11, carrierFormat, g.colorInUav) ||
                FAILED(a_device->CreateShaderResourceView(g.colorOut.d3d11, nullptr, &g.colorOutSrv))) {
                Fail(g, "the FP16 carrier views would not create");
                (void)TeardownShared(g);
                return RebuildOutcome::kRefused;
            }
            D3D11_TEXTURE2D_DESC scratch{};
            scratch.Width = a_key.rt0W;
            scratch.Height = a_key.rt0H;
            scratch.MipLevels = 1;
            scratch.ArraySize = 1;
            scratch.Format = a_key.rt0Fmt;
            scratch.SampleDesc.Count = 1;
            scratch.Usage = D3D11_USAGE_DEFAULT;
            scratch.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
            if (FAILED(a_device->CreateTexture2D(&scratch, nullptr, &g.rt0Scratch)) ||
                !CreateUav(a_device, g.rt0Scratch, TypedColorFormat(a_key.rt0Fmt), g.rt0ScratchUav)) {
                Fail(g, "the frame-buffer scratch for the FP16 carrier would not create");
                (void)TeardownShared(g);
                return RebuildOutcome::kRefused;
            }
        }
        if (!D3D12Sidecar::TypedUavStoreSupported(carrierFormat)) {
            logger::warn("[Neural] the D3D12 side reports no typed UAV store for carrier fmt={} — the runtime may refuse it",
                static_cast<int>(carrierFormat));
        }
        const bool resized = a_key.modelW != a_key.rt0W || a_key.modelH != a_key.rt0H;
        if (resized) {
            if (!SidecarCompute::CreateTexture(g.owner12, a_key.modelW, a_key.modelH, carrierFormat, L"neural model in", g.modelIn) ||
                !SidecarCompute::CreateTexture(g.owner12, a_key.modelW, a_key.modelH, carrierFormat, L"neural model out", g.modelOut)) {
                Fail(g, "the model-extent images could not be created (see the log)");
                (void)TeardownShared(g);
                return RebuildOutcome::kRefused;
            }
        }
        const Neural::Shape shape{ a_key.modelW, a_key.modelH, a_key.renderW, a_key.renderH };
        const Neural::Resources resources{ resized ? g.modelIn : g.colorIn.d3d12, resized ? g.modelOut : g.colorOut.d3d12,
            g.depth.d3d12, g.mv.d3d12 };
        const NeuralRenderer::FeatureKey featureKey{ a_key.modelW, a_key.modelH, carrierFormat,
            Neural::ClampPreset(a_settings.preset) };
        if (!NeuralRenderer::EnsureFeature(featureKey, shape, a_settings, resources, g.index)) {
            return (!fp16 && !g.carrierFallbackTried) ? RebuildOutcome::kCarrierRefused : RebuildOutcome::kRefused;
        }
        g.built = true;
        g.needReset = true;
        ++g.rebuilds;
        logger::info("[Neural] pass {} built: frame {}x{} fmt={} carrier {} | model {}x{} | depth {}x{} fmt={} | motion {}x{} fmt={} | guides {}x{} (rebuild #{})",
            g.index + 1U, a_key.rt0W, a_key.rt0H, static_cast<int>(a_key.rt0Fmt), fp16 ? "FP16" : "native", a_key.modelW, a_key.modelH, a_key.depthW,
            a_key.depthH, static_cast<int>(a_key.depthFmt), a_key.mvW, a_key.mvH, static_cast<int>(a_key.mvFmt),
            a_key.renderW, a_key.renderH, g.rebuilds);
        logger::info("[Neural] pass {} colour contract: RT0 fmt={} ({}), {} — the encoded display-referred values as stored",
            g.index + 1U, static_cast<int>(a_key.rt0Fmt), Neural::IsSrgbFormat(static_cast<std::uint32_t>(a_key.rt0Fmt)) ? "sRGB-TYPED resource" : "not sRGB-typed",
            fp16 ? fmt::format("read through the pass's own typed view fmt={} into the FP16 carrier",
                       Neural::NonSrgbFormat(static_cast<std::uint32_t>(TypedColorFormat(a_key.rt0Fmt))))
                 : "copied as the native carrier");
        return RebuildOutcome::kBuilt;
    }

    void ClearStageLatch(PassContext& g) noexcept
    {
        g.failed.store(false, std::memory_order_relaxed);
        g.failReason[0] = '\0';
        g.pauseCycles = 0;
        g.pausedUntil = 0;
        g.carrierFallbackTried = false;
        g.carrierResolved = Neural::kCarrierNative;
        NeuralRenderer::RearmLatch(1U << g.index);
    }

    [[nodiscard]] NeuralTeardownResult DestroyStages(std::uint32_t a_mask) noexcept
    {
        bool had = false;
        for (std::uint32_t i = 0; i < Neural::kMaxPasses; ++i) {
            if ((a_mask & (1U << i)) != 0U) had = had || HasGpuResources(g_passes[i]);
        }
        if (!RetireStages(a_mask)) return NeuralTeardownResult::kFailed;
        for (std::uint32_t i = 0; i < Neural::kMaxPasses; ++i) {
            if ((a_mask & (1U << i)) != 0U) {
                (void)TeardownAll(g_passes[i], true);
            }
        }
        if (a_mask == Neural::kAllPasses) {
            ReleaseBrackets();
            SidecarCompute::Release();
        }
        return had ? NeuralTeardownResult::kDestroyed : NeuralTeardownResult::kAlreadyAbsent;
    }

    void WireEffects(NeuralMailbox& a_mailbox) noexcept
    {
        for (std::uint32_t i = 0; i < Neural::kMaxPasses; ++i) g_passes[i].index = i;
        NeuralEffects effects{};
        effects.destroyFeature = [](std::uint32_t mask) noexcept { return DestroyStages(mask); };
        effects.rearmLatch = []() noexcept {
            for (auto& stage : g_passes) ClearStageLatch(stage);
            NeuralRenderer::RearmLatch();
            D3D12Sidecar::Rearm();
        };
        effects.logCommit = [](const NeuralCommitInfo& info) noexcept {
            for (std::uint32_t i = 0; i < Neural::kMaxPasses; ++i) {
                auto& stage = g_passes[i];
                if ((info.historyResetMask & (1U << i)) != 0U) stage.needReset = true;
                if ((info.scope & (kNeuralScopeArm | kNeuralScopeRetry)) != 0U ||
                    (i >= g_settings.passCount && i < info.settings.passCount) ||
                    Neural::RequiresRecreate(g_settings.passes[i], info.settings.passes[i])) {
                    ClearStageLatch(stage);
                }
            }
            static ULONGLONG lastLive = 0;
            const ULONGLONG now = ::GetTickCount64();
            if (info.scope == kNeuralScopeLive && now - lastLive < 1000) return;
            if (info.scope == kNeuralScopeLive) lastLive = now;
            char scope[64]{};
            NeuralMailbox::ScopeName(info.scope, scope, sizeof(scope));
            logger::info("[Neural] COMMITTED #{} ({}): enabled={}, {} requested pass(es), history reset mask={:#x}",
                info.generation, scope, info.settings.enabled, info.settings.passCount, info.historyResetMask);
        };
        a_mailbox.SetEffects(std::move(effects));
    }

    NeuralMailbox& Mailbox() noexcept
    {
        static NeuralMailbox mailbox{};
        static const bool wired = (WireEffects(mailbox), true);
        (void)wired;
        return mailbox;
    }

    void ReportTiming(PassContext& g) noexcept
    {
        for (double gpuMs = 0.0; D3D12Sidecar::TimestampTake(g.index, gpuMs);) {
            g.gpuWindow.Push(static_cast<float>(gpuMs));
        }
        if (g.windowFrames < kTimingWindow || g.qpf == 0) {
            return;
        }
        const double ms = 1000.0 * static_cast<double>(g.cpuTicks) / static_cast<double>(g.qpf) /
                          static_cast<double>(g.windowFrames);
        g.cpuMsAvg.store(static_cast<float>(ms), std::memory_order_relaxed);
        const float gpuMedian = g.gpuWindow.Percentile(0.5F);
        const float gpuP99 = g.gpuWindow.Percentile(0.99F);
        g.gpuMsMedian.store(gpuMedian, std::memory_order_relaxed);
        g.gpuMsP99.store(gpuP99, std::memory_order_relaxed);
        const auto health = D3D12Sidecar::HealthSnapshot();
        if (g.gpuWindow.count != 0U) {
            logger::info("[Neural] pass {}: {} frames: pass CPU {:.2f} ms/frame | GPU median {:.2f} ms, p99 {:.2f} ms ({} brackets; the evaluate and its barriers, not the D3D11 side) | {} evaluated total | {} rebuilds | d3d12 {}/{} ({} behind)",
                g.index + 1U, g.windowFrames, ms, gpuMedian, gpuP99, g.gpuWindow.count, g.evaluated, g.rebuilds, health.completed,
                health.submitted, health.submitted > health.completed ? health.submitted - health.completed : 0ULL);
        } else {
            logger::info("[Neural] pass {}: {} frames: pass CPU {:.2f} ms/frame | GPU {} | {} evaluated total | {} rebuilds | d3d12 {}/{} ({} behind)",
                g.index + 1U, g.windowFrames, ms, D3D12Sidecar::TimestampsAvailable() ? "no bracket retired this period" : "unavailable",
                g.evaluated, g.rebuilds, health.completed, health.submitted,
                health.submitted > health.completed ? health.submitted - health.completed : 0ULL);
        }
        g.gpuWindow.Clear();
        g.windowFrames = 0;
        g.cpuTicks = 0;
    }

    [[nodiscard]] std::uint32_t FinaliseChain(bool a_finalImage) noexcept
    {
        if (!g_chain.active) {
            return 0;
        }
        g_chain.active = false;
        const std::uint32_t delivered = g_chain.delivered;
        if (delivered == 0U || g_chain.last == nullptr) {
            return 0;
        }
        try {
            auto* const data = RE::BSGraphics::RendererData::GetSingleton();
            if (data == nullptr || data->context == nullptr) {
                return 0;
            }
            ResolvedFrameBuffer frame{};
            ResolveFrameBufferTexture(data->renderTargets[kFrameBufferIndex], frame);
            bool outputOrdered = false;
            if (frame.texture != nullptr && g_chain.lastToken != 0U && !D3D12Sidecar::Disabled()) {
                ++g_frameCrossApiTrips;
                outputOrdered = D3D12Sidecar::WaitOnD3D11(g_chain.lastToken);
            }
            if (!outputOrdered || D3D12Sidecar::Disabled()) {
                return 0;
            }
            for (std::uint32_t i = 0; i < delivered; ++i) {
                auto& g = g_passes[i];
                const auto& pending = g_chain.pending[i];
                g.needReset = false;
                g.adoptedGuides = false;
                if (pending.historyChanged) {
                    g.lastHistorySerial = pending.historySerial;
                }
                if (pending.epochChanged) {
                    g.epoch.Acknowledge(pending.epoch);
                    if (i == 0U) {
                        logger::info("[Neural] pass 1 history reset: epoch {} ({})", pending.epoch, Fallout4Renderer::HistoryEpochReason());
                    }
                }
                if (pending.engineChanged) {
                    g.lastEngine = pending.engine;
                }
                ++g.evaluated;
                if (g.evaluated == 1) {
                    logger::info("[Neural] pass {} ★ first neural frame delivered: {}x{} (model {}x{}, guides {}x{}), carrier native, reset={}{}",
                        i + 1U, pending.key.rt0W, pending.key.rt0H, pending.key.modelW, pending.key.modelH, pending.key.renderW, pending.key.renderH, pending.reset ? 1 : 0,
                        i > 0U ? " (chained on the sidecar: no D3D11 return between stages)" : "");
                }
                g.activeLastFrame.store(true, std::memory_order_relaxed);
            }
            PassContext& last = *g_chain.last;
            const ShapeKey& key = g_chain.pending[delivered - 1U].key;
            const OutputMergerUnbindScope omScope(data->context);
            {
                BracketScope returnLeg(g_returnBracket, data->context, true);
                data->context->CopyResource(frame.texture, last.colorOut.d3d11);
                returnLeg.Close();
            }
            ++g_frameColourCopies;
            g_frameCopyBytes += CopyBytes(key.rt0W, key.rt0H, TypedColorFormat(key.rt0Fmt));
            if (a_finalImage) {
                SidecarFrame::Hudless hudless{};
                hudless.colour = last.colorOut.d3d12;
                hudless.colour11 = last.colorOut.d3d11;
                hudless.width = key.rt0W;
                hudless.height = key.rt0H;
                hudless.format = TypedColorFormat(key.rt0Fmt);
                SidecarFrame::PublishHudless(hudless);
            }
            g_frameDelivered += delivered;
            return delivered;
        } catch (...) {
            logger::error("[Neural] C++ exception finalising the chained cascade; the current image is retained");
            return 0;
        }
    }

    [[nodiscard]] bool ChainThisStage(const PassContext& g, const ShapeKey& a_key) noexcept
    {
        if (a_key.carrier != Neural::kCarrierNative) {
            return false;
        }
        if (g.index == 0U) {
            g_chain = ChainFrame{};
            g_chain.active = true;
            g_chain.key = a_key;
            return true;
        }
        return g_chain.active && a_key == g_chain.key;
    }

    [[nodiscard]] bool ExecuteStage(PassContext& g, const Neural::Settings& settings) noexcept
    {
        if (g.failed.load(std::memory_order_relaxed)) {
            NoteGuidesSource(g, -1);
            return false;
        }
        try {
            LARGE_INTEGER t0{};
            ::QueryPerformanceCounter(&t0);
            if (g.qpf == 0) {
                LARGE_INTEGER f{};
                ::QueryPerformanceFrequency(&f);
                g.qpf = f.QuadPart;
            }

            auto* const data = RE::BSGraphics::RendererData::GetSingleton();
            if (data == nullptr || data->device == nullptr || data->context == nullptr) {
                return false;
            }
            auto* const device = reinterpret_cast<ID3D11Device*>(data->device);
            auto* const context = data->context;

            if (!D3D12Sidecar::PollHealth()) {
                Fail(g, D3D12Sidecar::DisableReason());
                return false;
            }
            if (!CreateShadersOnce(g, device)) {
                return false;
            }

            auto& rt0Record = data->renderTargets[kFrameBufferIndex];
            ResolvedFrameBuffer frame{};
            ResolveFrameBufferTexture(rt0Record, frame);
            auto& depthRecord = data->depthStencilTargets[kMainDepthIndex];
            auto& mvRecord = data->renderTargets[kMotionVectorIndex];
            if (frame.texture == nullptr || depthRecord.texture == nullptr || mvRecord.texture == nullptr) {
                static std::atomic<bool> s_logged{ false };
                if (!s_logged.exchange(true, std::memory_order_relaxed)) {
                    logger::warn("[Neural] inputs missing (frame={} depth={} motion={}); idling until they exist (one-shot line)",
                        fmt::ptr(frame.texture), fmt::ptr(depthRecord.texture), fmt::ptr(mvRecord.texture));
                }
                return false;
            }
            D3D11_TEXTURE2D_DESC rt0Desc{}, depthDesc{}, mvDesc{};
            frame.texture->GetDesc(&rt0Desc);
            depthRecord.texture->GetDesc(&depthDesc);
            mvRecord.texture->GetDesc(&mvDesc);
            if (rt0Desc.SampleDesc.Count > 1 || depthDesc.SampleDesc.Count > 1 || mvDesc.SampleDesc.Count > 1) {
                Fail(g, "the frame is multisampled; the neural pass cannot read MSAA buffers");
                return false;
            }
            const FrameInputs inputs = Fallout4Renderer::Snapshot();
            std::uint32_t renderW = inputs.renderWidth != 0 ? inputs.renderWidth : rt0Desc.Width;
            std::uint32_t renderH = inputs.renderHeight != 0 ? inputs.renderHeight : rt0Desc.Height;
            renderW = std::min(renderW, std::min(depthDesc.Width, mvDesc.Width));
            renderH = std::min(renderH, std::min(depthDesc.Height, mvDesc.Height));

            if (!g.carrierFallbackTried) {
                g.carrierResolved = Neural::kCarrierNative;
            }
            ShapeKey key{};
            key.rt0W = rt0Desc.Width;
            key.rt0H = rt0Desc.Height;
            key.rt0Fmt = rt0Desc.Format;
            key.depthW = depthDesc.Width;
            key.depthH = depthDesc.Height;
            key.depthFmt = depthDesc.Format;
            key.mvW = mvDesc.Width;
            key.mvH = mvDesc.Height;
            key.mvFmt = mvDesc.Format;
            key.renderW = renderW;
            key.renderH = renderH;
            key.carrier = g.carrierResolved;
            g.snapshotOutW.store(key.rt0W, std::memory_order_relaxed);
            g.snapshotOutH.store(key.rt0H, std::memory_order_relaxed);
            key.modelW = key.rt0W;
            key.modelH = key.rt0H;
            {
                const std::uint32_t percent = Neural::EffectiveModelPercent(g_settings);
                const char* why = nullptr;
                const Neural::ModelExtent extent = Neural::ComputeModelExtent(key.rt0W, key.rt0H, percent);
                if (percent == 100U) {
                } else if (!extent.Valid()) {
                    why = "the requested extent is below the 32 px shape floor at this output size";
                } else if (key.carrier == Neural::kCarrierFp16) {
                    why = "the FP16 carrier does not resize";
                } else if (Neural::IsSrgbFormat(static_cast<std::uint32_t>(TypedColorFormat(key.rt0Fmt)))) {
                    why = "the frame buffer is an sRGB-typed format; the extent kernels need a UNORM carrier";
                } else if (!SidecarCompute::Ensure(D3D12Sidecar::Device())) {
                    why = SidecarCompute::Reason();
                } else {
                    key.modelW = extent.width;
                    key.modelH = extent.height;
                }
                if (g.index == 0U) {
                    g_modelW.store(key.modelW, std::memory_order_relaxed);
                    g_modelH.store(key.modelH, std::memory_order_relaxed);
                    std::snprintf(g_modelReason, sizeof(g_modelReason), "%s", why != nullptr ? why : "");
                    if (percent != g_modelLoggedPercent || std::strcmp(g_modelReason, g_modelLoggedReason) != 0) {
                        g_modelLoggedPercent = percent;
                        std::snprintf(g_modelLoggedReason, sizeof(g_modelLoggedReason), "%s", g_modelReason);
                        if (why != nullptr) {
                            logger::warn("[Neural] model extent {} % NOT applied: {} — the model works at 100 % ({}x{})", percent, why, key.rt0W, key.rt0H);
                        } else if (percent != 100U) {
                            logger::info("[Neural] model extent {} %: the model works on {}x{} of the {}x{} frame; its residual is {} back onto the untouched base",
                                percent, key.modelW, key.modelH, key.rt0W, key.rt0H, percent < 100U ? "enlarged (Catmull-Rom)" : "reduced (area)");
                        } else {
                            logger::info("[Neural] model extent 100 %: the exact path ({}x{})", key.rt0W, key.rt0H);
                        }
                    }
                }
            }

            if (key.rt0W < 32U || key.rt0H < 32U || renderW < 32U || renderH < 32U) {
                return false;
            }

            const ULONGLONG now = ::GetTickCount64();
            if (g.pausedUntil != 0) {
                if (now < g.pausedUntil) {
                    return false;
                }
                g.pausedUntil = 0;
                g.rebuildsInWindow = 0;
                g.rebuildWindowStart = now;
            }
            const bool colourChanged = !g.built || !SameColourShape(key, g.activeKey);
            const bool guidesChanged = g.built && !colourChanged && !(key == g.activeKey);
            const bool shapeChanged = colourChanged || guidesChanged;
            const bool settingsRecreate = g.built && Neural::RequiresRecreate(g.activeSettings, settings);
            if (shapeChanged || settingsRecreate) {
                if (shapeChanged) {
                    if (key == g.pendingKey) {
                        ++g.pendingFrames;
                    } else {
                        g.pendingKey = key;
                        g.pendingFrames = 1;
                    }
                    if (g.built && g.pendingFrames < kSettleFrames) {
                        return false;
                    }
                }
                if (guidesChanged && !settingsRecreate) {
                    if (!SameGuideAllocation(key, g.activeKey) && !TeardownOwnGuides(g)) {
                        return false;
                    }
                    logger::info("[Neural] pass {} guides adopted without a rebuild: render {}x{} -> {}x{}, depth {}x{} -> {}x{}, motion {}x{} -> {}x{} (the feature stays; the history restarts once)",
                        g.index + 1U, g.activeKey.renderW, g.activeKey.renderH, key.renderW, key.renderH,
                        g.activeKey.depthW, g.activeKey.depthH, key.depthW, key.depthH,
                        g.activeKey.mvW, g.activeKey.mvH, key.mvW, key.mvH);
                    g.activeKey = key;
                    g.needReset = true;
                    g.adoptedGuides = true;
                    g.pendingFrames = 0;
                } else {
                    if (now - g.rebuildWindowStart > 2000) {
                        g.rebuildWindowStart = now;
                        g.rebuildsInWindow = 0;
                    }
                    if (++g.rebuildsInWindow > 8) {
                        if (++g.pauseCycles >= 5) {
                            Fail(g, "the render shape never settles (five pauses); stopping rather than rebuilding forever");
                            return false;
                        }
                        logger::warn("[Neural] pass {} render shape changed {} times in two seconds; standing aside for 3 s", g.index + 1U, g.rebuildsInWindow);
                        g.pausedUntil = now + 3000;
                        return false;
                    }
                    const RebuildOutcome rebuilt = Rebuild(g, device, key, settings);
                    if (rebuilt == RebuildOutcome::kTeardownPending) {
                        return false;
                    }
                    if (rebuilt == RebuildOutcome::kCarrierRefused) {
                        g.carrierFallbackTried = true;
                        g.carrierResolved = Neural::kCarrierFp16;
                        NeuralRenderer::RearmLatch(1U << g.index);
                        logger::warn("[Neural] pass {} the native colour carrier was refused; retrying with the FP16 carrier", g.index + 1U);
                        g.pendingFrames = 0;
                        return false;
                    }
                    if (rebuilt == RebuildOutcome::kRefused) {
                        if (!g.failed.load(std::memory_order_relaxed)) {
                            const auto state = NeuralRenderer::Snapshot(g.index);
                            Fail(g, state.reason[0] != '\0' ? state.reason : "the neural feature could not be created for this shape");
                        }
                        return false;
                    }
                    g.activeKey = key;
                    g.activeSettings = settings;
                    g.pendingFrames = 0;
                    g.adoptedGuides = false;
                }
            }
            if (!NeuralRenderer::FeatureReady(g.index)) {
                return false;
            }

            const AaRequest applied = Streamline::AppliedRequest();
            const AaEffectiveEngine engine = Streamline::AppliedEffectiveEngine();
            const bool historyChanged = applied.historyResetSerial != g.lastHistorySerial;
            const bool engineChanged = engine != g.lastEngine;
            const std::uint64_t epoch = Fallout4Renderer::HistoryEpoch();
            const bool epochChanged = g.epoch.Pending(epoch);
            const bool stateless = g.index > 0U;
            if (stateless && !g.statelessLogged) {
                g.statelessLogged = true;
                logger::info("[Neural] pass {} runs stateless (reset every frame): it repaints pass {}'s temporally stable image; a second history on top of the first flickers",
                    g.index + 1U, g.index);
            }
            const bool reset = stateless || g.needReset || historyChanged || engineChanged || epochChanged;
            if (reset && !stateless) InvalidateHistoryFrom(g.index + 1U);

            SidecarFrame::Guides shared{};
            const bool taken = engine == AaEffectiveEngine::kDlss &&
                               SidecarFrame::TakeGuides(g.guidesCursor, shared);
            const bool shapeMatch = taken && shared.depthWidth == key.depthW && shared.depthHeight == key.depthH &&
                                    shared.mvWidth == key.mvW && shared.mvHeight == key.mvH &&
                                    shared.renderWidth == key.renderW && shared.renderHeight == key.renderH &&
                                    shared.depthFormat == DXGI_FORMAT_R32_FLOAT && shared.mvFormat == DXGI_FORMAT_R16G16_FLOAT;
            const bool sharedGuides = shapeMatch && shared.motion == SidecarFrame::GuideMotion::kDilated;
            if (taken && !shapeMatch) {
                if (!g.guideMismatchLogged) {
                    g.guideMismatchLogged = true;
                    logger::warn("[Neural] the DirectX 12 DLSS engine's guides do not match this pass's shape (engine: depth {}x{} fmt {}, "
                                 "MV {}x{} fmt {}, render {}x{}; pass: depth {}x{}, MV {}x{}, render {}x{}) — own derivation stays "
                                 "(one-shot line; the shared pipeline's guide hand-off is inactive here)",
                        shared.depthWidth, shared.depthHeight, static_cast<int>(shared.depthFormat), shared.mvWidth, shared.mvHeight,
                        static_cast<int>(shared.mvFormat), shared.renderWidth, shared.renderHeight, key.depthW, key.depthH, key.mvW,
                        key.mvH, key.renderW, key.renderH);
                }
            }
            NoteGuidesSource(g, sharedGuides ? 1 : 2);

            const bool chained = ChainThisStage(g, key);
            if (!chained && g_chain.active) {
                const std::uint32_t before = g_chain.delivered;
                if (FinaliseChain(false) == 0U && before != 0U) {
                    return false;
                }
            }
            const bool chainedInput = chained && g.index > 0U;

            const OutputMergerUnbindScope omScope(context);
            BracketScope prepLeg(g_prepBracket, context, !chainedInput);
            {
                const ComputeBindingsScope csScope(context);
                ID3D11ShaderResourceView* depthSrv = depthRecord.srViewDepth;
                if (depthSrv == nullptr) {
                    depthSrv = EnsureOwnSrv(device, depthRecord.texture, DepthViewFormat(depthDesc.Format), g.ownDepthSrv, g.ownDepthSrvTexture);
                }
                ID3D11ShaderResourceView* mvSrv = mvRecord.srView;
                if (mvSrv == nullptr) {
                    mvSrv = EnsureOwnSrv(device, mvRecord.texture, MotionViewFormat(mvDesc.Format), g.ownMvSrv, g.ownMvSrvTexture);
                }
                if (depthSrv == nullptr || mvSrv == nullptr) {
                    Fail(g, "the engine's depth or motion-vector texture cannot be read as a shader resource (format not viewable)");
                    return false;
                }
                if (!sharedGuides && !chainedInput) {
                    if (!EnsureOwnGuides(g, device, key)) {
                        return false;
                    }
                    Dispatch(context, g.csDepth, 0, depthSrv, g.depthUav, key.depthW, key.depthH);
                    DispatchDilate(context, g, mvSrv, depthSrv, key);
                }
                if (key.carrier == Neural::kCarrierFp16) {
                    ID3D11ShaderResourceView* rt0Srv = EnsureOwnSrv(device, frame.texture,
                        static_cast<DXGI_FORMAT>(Neural::NonSrgbFormat(static_cast<std::uint32_t>(TypedColorFormat(rt0Desc.Format)))),
                        g.ownRt0Srv, g.ownRt0SrvTexture);
                    if (rt0Srv == nullptr) {
                        Fail(g, "the frame buffer cannot be read as a shader resource");
                        return false;
                    }
                    Dispatch(context, g.csColor, 2, rt0Srv, g.colorInUav, key.rt0W, key.rt0H);
                } else if (!chainedInput) {
                    context->CopyResource(g.colorIn.d3d11, frame.texture);
                }
            }
            prepLeg.Close();
            if (!chainedInput) {
                ++g_frameColourCopies;
                g_frameCopyBytes += CopyBytes(key.rt0W, key.rt0H,
                    key.carrier == Neural::kCarrierFp16 ? DXGI_FORMAT_R16G16B16A16_FLOAT : TypedColorFormat(key.rt0Fmt));
            }

            ID3D12Resource* const depth12 = chainedInput ? g_chain.guideDepth : sharedGuides ? shared.depth : g.depth.d3d12;
            ID3D12Resource* const mv12 = chainedInput ? g_chain.guideMv : sharedGuides ? shared.mv : g.mv.d3d12;
            const bool resized = key.modelW != key.rt0W || key.modelH != key.rt0H;
            ID3D12Resource* const inputColour = chainedInput ? g_chain.previousColour : (resized ? g.modelIn : g.colorIn.d3d12);
            ID3D12Resource* const modelOutput = resized ? g.modelOut : g.colorOut.d3d12;
            const DXGI_FORMAT modelView = TypedColorFormat(key.rt0Fmt);

            if (!chainedInput) {
                const std::uint64_t vIn = D3D12Sidecar::SignalFromD3D11();
                D3D12Sidecar::WaitOnD3D12(vIn);
            }
            ID3D12GraphicsCommandList* list = nullptr;
            if (!D3D12Sidecar::BeginCommands(list)) {
                Fail(g, D3D12Sidecar::Disabled() ? D3D12Sidecar::DisableReason() : "the sidecar command list would not begin");
                return false;
            }
            D3D12Sidecar::TimestampBegin(list, g.index);
            if (resized && !chainedInput) {
                D3D12Sidecar::Barrier(list, g.colorIn.d3d12, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                D3D12Sidecar::Barrier(list, g.modelIn, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                if (!SidecarCompute::Resample(list, g.colorIn.d3d12, g.modelIn, modelView, SidecarCompute::FilterFor(key.rt0W, key.modelW))) {
                    (void)D3D12Sidecar::AbandonCommands();
                    Fail(g, "the model-extent resample could not be recorded");
                    return false;
                }
                D3D12Sidecar::Barrier(list, g.modelIn, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                D3D12Sidecar::Barrier(list, g.colorIn.d3d12, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
            } else {
                D3D12Sidecar::Barrier(list, inputColour, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            }
            D3D12Sidecar::Barrier(list, depth12, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            D3D12Sidecar::Barrier(list, mv12, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            D3D12Sidecar::Barrier(list, modelOutput, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            const Neural::Shape shape{ key.modelW, key.modelH, key.renderW, key.renderH };
            const Neural::Resources resources{ inputColour, modelOutput, depth12, mv12 };
            const bool evaluated = NeuralRenderer::Evaluate(list, shape, settings, resources, reset, g.index);
            if (g.index == 0U) g_lastResetFlag.store(reset, std::memory_order_relaxed);
            if (!evaluated) {
                (void)D3D12Sidecar::AbandonCommands();
                ++g.frames;
                ++g.windowFrames;
                if (g.adoptedGuides) {
                    g.adoptedGuides = false;
                    g.built = false;
                    NeuralRenderer::RearmLatch(1U << g.index);
                    logger::warn("[Neural] pass {} the runtime refused the adopted guide extent; rebuilding the feature", g.index + 1U);
                    return false;
                }
                const auto state = NeuralRenderer::Snapshot(g.index);
                if (state.latched || state.faulted) {
                    Fail(g, state.reason[0] != '\0' ? state.reason : "the neural feature latched");
                }
                return false;
            }
            D3D12Sidecar::Barrier(list, inputColour, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
            D3D12Sidecar::Barrier(list, depth12, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
            D3D12Sidecar::Barrier(list, mv12, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
            D3D12Sidecar::Barrier(list, modelOutput, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
            if (resized) {
                ID3D12Resource* const base = chainedInput ? g_chain.base : g.colorIn.d3d12;
                ID3D12Resource* const firstModelIn = chainedInput ? g_chain.firstModelIn : g.modelIn;
                D3D12Sidecar::Barrier(list, base, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                D3D12Sidecar::Barrier(list, firstModelIn, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                D3D12Sidecar::Barrier(list, g.modelOut, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                D3D12Sidecar::Barrier(list, g.colorOut.d3d12, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                if (!SidecarCompute::Compose(list, base, firstModelIn, g.modelOut, g.colorOut.d3d12, modelView,
                        SidecarCompute::FilterFor(key.modelW, key.rt0W))) {
                    (void)D3D12Sidecar::AbandonCommands();
                    Fail(g, "the model-extent compose could not be recorded");
                    return false;
                }
                D3D12Sidecar::Barrier(list, g.colorOut.d3d12, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
                D3D12Sidecar::Barrier(list, g.modelOut, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
                D3D12Sidecar::Barrier(list, firstModelIn, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
                D3D12Sidecar::Barrier(list, base, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
            }
            D3D12Sidecar::TimestampEnd(list, g.index);
            bool listAccepted = false;
            const std::uint64_t vOut = D3D12Sidecar::EndCommands(&listAccepted);
            if (chained) {
                ++g.frames;
                ++g.windowFrames;
                if (!listAccepted || vOut == 0U) {
                    Fail(g, "the neural command list was rejected");
                    return false;
                }
                auto& pending = g_chain.pending[g.index];
                pending.historyChanged = historyChanged;
                pending.historySerial = applied.historyResetSerial;
                pending.epochChanged = epochChanged;
                pending.epoch = epoch;
                pending.engineChanged = engineChanged;
                pending.engine = engine;
                pending.reset = reset;
                pending.key = key;
                if (g.index == 0U) {
                    g_chain.guideDepth = depth12;
                    g_chain.guideMv = mv12;
                    g_chain.base = g.colorIn.d3d12;
                    g_chain.firstModelIn = resized ? g.modelIn : nullptr;
                }
                g_chain.previousColour = modelOutput;
                g_chain.lastToken = vOut;
                g_chain.last = &g;
                g_chain.delivered = g.index + 1U;
                LARGE_INTEGER tChained{};
                ::QueryPerformanceCounter(&tChained);
                g.cpuTicks += tChained.QuadPart - t0.QuadPart;
                ReportTiming(g);
                return true;
            }
            bool outputOrdered = false;
            if (vOut != 0U && !D3D12Sidecar::Disabled()) {
                ++g_frameCrossApiTrips;
                outputOrdered = D3D12Sidecar::WaitOnD3D11(vOut);
            }

            ++g.frames;
            ++g.windowFrames;
            if (!Neural::CanUseStageOutput(evaluated, listAccepted, vOut, outputOrdered) || D3D12Sidecar::Disabled()) {
                if (!listAccepted) Fail(g, "the neural command list was rejected");
                return false;
            }
            g.needReset = false;
            g.adoptedGuides = false;
            if (historyChanged) {
                g.lastHistorySerial = applied.historyResetSerial;
            }
            if (epochChanged) {
                g.epoch.Acknowledge(epoch);
                if (g.index == 0U) {
                    logger::info("[Neural] pass 1 history reset: epoch {} ({})", epoch, Fallout4Renderer::HistoryEpochReason());
                }
            }
            if (engineChanged) {
                g.lastEngine = engine;
            }
            ++g.evaluated;

            BracketScope returnLeg(g_returnBracket, context, true);
            if (key.carrier == Neural::kCarrierFp16) {
                const ComputeBindingsScope csScope(context);
                Dispatch(context, g.csColor, 2, g.colorOutSrv, g.rt0ScratchUav, key.rt0W, key.rt0H);
                context->CopyResource(frame.texture, g.rt0Scratch);
                returnLeg.Close();
            } else {
                context->CopyResource(frame.texture, g.colorOut.d3d11);
                returnLeg.Close();
                SidecarFrame::Hudless hudless{};
                hudless.colour = g.colorOut.d3d12;
                hudless.colour11 = g.colorOut.d3d11;
                hudless.width = key.rt0W;
                hudless.height = key.rt0H;
                hudless.format = TypedColorFormat(key.rt0Fmt);
                SidecarFrame::PublishHudless(hudless);
            }
            ++g_frameColourCopies;
            g_frameCopyBytes += CopyBytes(key.rt0W, key.rt0H, TypedColorFormat(key.rt0Fmt));

            LARGE_INTEGER t1{};
            ::QueryPerformanceCounter(&t1);
            g.cpuTicks += t1.QuadPart - t0.QuadPart;
            if (g.evaluated == 1) {
                logger::info("[Neural] pass {} ★ first neural frame delivered: {}x{} (model {}x{}, guides {}x{}), carrier {}, reset={}",
                    g.index + 1U, key.rt0W, key.rt0H, key.modelW, key.modelH, key.renderW, key.renderH,
                    key.carrier == Neural::kCarrierFp16 ? "FP16" : "native", reset ? 1 : 0);
            }
            ReportTiming(g);
            g.activeLastFrame.store(true, std::memory_order_relaxed);
            ++g_frameDelivered;
            return true;
        } catch (...) {
            Fail(g, "C++ exception in the neural pass");
            return false;
        }
    }

    [[nodiscard]] bool EnsureEnvironment(ID3D11Device* a_device, ID3D11DeviceContext* a_context) noexcept
    {
        if (D3D12Sidecar::Disabled()) {
            std::snprintf(g_blockReason, sizeof(g_blockReason), "%s", D3D12Sidecar::DisableReason());
            return false;
        }
        if (D3D12Sidecar::IsOpen() && D3D12Sidecar::BoundDevice() != a_device) {
            std::snprintf(g_blockReason, sizeof(g_blockReason), "the sidecar is bound to another game device");
            return false;
        }
        if (!D3D12Sidecar::IsOpen() && !D3D12Sidecar::Open(a_device, a_context)) {
            std::snprintf(g_blockReason, sizeof(g_blockReason), "%s", D3D12Sidecar::DisableReason());
            return false;
        }
        bool changed = false;
        for (const auto& stage : g_passes) {
            changed = changed || (stage.owner11 != nullptr && stage.owner11 != a_device) ||
                (stage.owner12 != nullptr && stage.owner12 != D3D12Sidecar::Device());
        }
        if (changed) {
            if (DestroyStages(Neural::kAllPasses) == NeuralTeardownResult::kFailed) return false;
            NeuralRenderer::ReleaseRuntime();
        }
        if (!NeuralRenderer::EnsureRuntime(D3D12Sidecar::Device())) {
            const auto state = NeuralRenderer::Snapshot();
            std::snprintf(g_blockReason, sizeof(g_blockReason), "%s", state.reason);
            return false;
        }
        const bool timed = EnsureBracket(g_prepBracket, a_device) && EnsureBracket(g_returnBracket, a_device);
        if (timed != g_d3d11Timed.load(std::memory_order_relaxed)) {
            g_d3d11Timed.store(timed, std::memory_order_relaxed);
            logger::info("[Neural] D3D11 transfer brackets {}", timed ? "armed on the game's context (the preparation and return legs, retired without flushing)"
                                                                       : "unavailable (timestamp queries could not be created); the D3D11 legs run untimed");
        }
        return true;
    }
}

namespace Platform::NeuralPass
{
    void ExecuteAfterEffectRange() noexcept
    {
        HistoryFrameScope history;
        g_activePasses.store(0, std::memory_order_relaxed);
        g_lastColourCopies.store(0, std::memory_order_relaxed);
        g_lastCrossApiTrips.store(0, std::memory_order_relaxed);
        g_lastCopyBytes.store(0, std::memory_order_relaxed);
        g_frameColourCopies = 0;
        g_frameCrossApiTrips = 0;
        g_frameCopyBytes = 0;
        g_frameDelivered = 0;
        g_chain = ChainFrame{};
        g_blockReason[0] = '\0';
        for (auto& stage : g_passes) stage.activeLastFrame.store(false, std::memory_order_relaxed);
        SidecarFrame::InvalidateHudless();
        if (D3D12Sidecar::TakeRearmRequest()) RequestRetry();
        const auto pump = Mailbox().Pump(++g_pumpFrame);
        g_settings = Mailbox().Applied().settings;
        g_enabled.store(g_settings.enabled, std::memory_order_relaxed);
        if (!g_settings.enabled) return;
        std::uint32_t limit = Neural::kMaxPasses;
        {
            const auto diag = Mailbox().MakeDiagnosticSnapshot();
            if (diag.damagedStages != 0U) {
                limit = static_cast<std::uint32_t>(std::countr_zero(diag.damagedStages));
            }
        }
        {
            static std::uint32_t s_lastLimit{ Neural::kMaxPasses };
            if (limit != s_lastLimit) {
                s_lastLimit = limit;
                if (limit < Neural::ClampPassCount(g_settings.passCount)) {
                    logger::info("[Neural] cascade runs {} of {} stage(s) while a transition settles (stage {} is torn down and waits for its commit)",
                        limit, Neural::ClampPassCount(g_settings.passCount), limit + 1U);
                }
            }
        }
        try {
            auto* data = RE::BSGraphics::RendererData::GetSingleton();
            if (data == nullptr || data->device == nullptr || data->context == nullptr) return;
            auto* device = reinterpret_cast<ID3D11Device*>(data->device);
            if (!EnsureEnvironment(device, data->context)) return;
            const D3D12Sidecar::FrameLock frameLock;
            g_lastResetFlag.store(false, std::memory_order_relaxed);
            const std::uint32_t submitted = Neural::RunCascade(g_settings, limit,
                [](std::uint32_t) noexcept { SidecarFrame::InvalidateHudless(); },
                [](std::uint32_t index) noexcept { return ExecuteStage(g_passes[index], g_settings.passes[index]); });
            if (g_chain.active) {
                (void)FinaliseChain(true);
            }
            const std::uint32_t completed = g_frameDelivered < submitted ? g_frameDelivered : submitted;
            history.completed = completed;
            g_lastColourCopies.store(g_frameColourCopies, std::memory_order_relaxed);
            g_lastCrossApiTrips.store(g_frameCrossApiTrips, std::memory_order_relaxed);
            g_lastCopyBytes.store(g_frameCopyBytes, std::memory_order_relaxed);
            BracketRetire(g_prepBracket, data->context);
            BracketRetire(g_returnBracket, data->context);
            ++g_transferWindowFrames;
            if (g_transferWindowFrames >= kTimingWindow || WindowNearlyFull(g_prepBracket) || WindowNearlyFull(g_returnBracket)) {
                const float prep = g_prepBracket.window.Percentile(0.5F);
                const float ret = g_returnBracket.window.Percentile(0.5F);
                g_prepBracket.median.store(prep, std::memory_order_relaxed);
                g_returnBracket.median.store(ret, std::memory_order_relaxed);
                g_d3d11Windows.fetch_add(1U, std::memory_order_relaxed);
                float modelSum = 0.0F;
                for (const auto& stage : g_passes) {
                    if (stage.activeLastFrame.load(std::memory_order_relaxed)) modelSum += stage.gpuMsMedian.load(std::memory_order_relaxed);
                }
                if (g_prepBracket.window.count != 0U || g_returnBracket.window.count != 0U) {
                    logger::info("[Neural] transfers, window of {} frames: D3D11 preparation median {:.3f} ms ({} brackets), return median "
                                 "{:.3f} ms ({} brackets) — the game's context, each leg on its own, not a latency; model {:.2f} ms = the "
                                 "active stages' own medians added up (not a latency either) | this frame: {} stage(s) delivered, "
                                 "{} colour copies ({:.1f} MB), {} cross-API return(s)",
                        g_transferWindowFrames, prep, g_prepBracket.window.count, ret, g_returnBracket.window.count, modelSum,
                        completed, g_frameColourCopies, static_cast<double>(g_frameCopyBytes) / 1048576.0, g_frameCrossApiTrips);
                }
                g_prepBracket.window.Clear();
                g_returnBracket.window.Clear();
                g_transferWindowFrames = 0;
            }
            if (D3D12Sidecar::Disabled() || !NeuralRenderer::RuntimeReady()) {
                SidecarFrame::InvalidateHudless();
                for (auto& stage : g_passes) stage.activeLastFrame.store(false, std::memory_order_relaxed);
                const auto runtime = NeuralRenderer::Snapshot();
                std::snprintf(g_blockReason, sizeof(g_blockReason), "%s",
                    D3D12Sidecar::Disabled() ? D3D12Sidecar::DisableReason() : runtime.reason);
                return;
            }
            g_activePasses.store(completed, std::memory_order_relaxed);
            if (completed != 0U) g_lastCompletedPasses.store(completed, std::memory_order_relaxed);
        } catch (...) {
            std::snprintf(g_blockReason, sizeof(g_blockReason), "C++ exception in the neural cascade");
            logger::error("[Neural] {}; current image retained", g_blockReason);
        }
    }

    void ApplyMenuSettings(const Settings::MenuSettings& a_settings) noexcept
    {
        auto next = a_settings.neural;
        for (auto& pass : next.passes) pass.preset = 0U;
        const auto previous = Mailbox().PeekPublished();
        const auto generation = Mailbox().Publish([&next](NeuralRequest& request) { request.settings = next; });
        if (previous.settings.enabled != next.enabled || previous.settings.passCount != next.passCount) {
            logger::info("[Neural] requested #{}: {}, {} pass(es)", generation,
                next.enabled ? "ON" : "OFF", Neural::ClampPassCount(next.passCount));
        }
    }

    Neural::CascadeSettings CurrentSettings() noexcept { return Mailbox().Applied().settings; }

    bool LastResetFlag() noexcept
    {
        return g_lastResetFlag.load(std::memory_order_relaxed);
    }

    void RequestRetry() noexcept
    {
        const auto generation = Mailbox().Publish([](NeuralRequest& request) { ++request.retrySerial; });
        logger::info("[Neural] cascade retry requested (#{})", generation);
    }

    State Snapshot() noexcept
    {
        State state{};
        const auto mailbox = Mailbox().MakeDiagnosticSnapshot();
        state.enabled = g_enabled.load(std::memory_order_relaxed);
        state.requestedPasses = mailbox.requested.passCount;
        state.activePasses = g_activePasses.load(std::memory_order_relaxed);
        state.active = state.activePasses != 0U;
        state.pending = mailbox.pending;
        state.appliedGeneration = mailbox.appliedGeneration;
        state.latestGeneration = mailbox.latestGeneration;
        if (state.pending) NeuralMailbox::ScopeName(mailbox.pendingScope, state.pendingScope, sizeof(state.pendingScope));
        const auto adapter = D3D12Sidecar::Adapter();
        const auto family = Neural::FamilyFromAdapter(adapter.vendorId, adapter.deviceId, adapter.description);
        state.family = Neural::FamilyName(family);
        state.expectation = Neural::FamilyExpectation(family);
        const auto runtime = NeuralRenderer::Snapshot();
        std::memcpy(state.runtimeVersion, runtime.runtimeVersion, sizeof(state.runtimeVersion));
        std::memcpy(state.runtimeSignature, runtime.runtimeSignature, sizeof(state.runtimeSignature));
        std::memcpy(state.runtimeSha, runtime.runtimeSha, sizeof(state.runtimeSha));
        state.profileLabel = runtime.profileLabel;
        state.profileValidated = runtime.profileValidated;
        const auto measured = g_lastCompletedPasses.load(std::memory_order_relaxed);
        for (std::uint32_t i = 0; i < Neural::kMaxPasses; ++i) {
            const auto& stage = g_passes[i];
            const auto renderer = NeuralRenderer::Snapshot(i);
            auto& out = state.passes[i];
            out.active = stage.activeLastFrame.load(std::memory_order_relaxed);
            out.ready = renderer.featureReady;
            out.latched = stage.failed.load(std::memory_order_relaxed) || renderer.latched;
            out.cpuMs = stage.cpuMsAvg.load(std::memory_order_relaxed);
            out.gpuMsMedian = stage.gpuMsMedian.load(std::memory_order_relaxed);
            out.gpuMsP99 = stage.gpuMsP99.load(std::memory_order_relaxed);
            out.evaluations = stage.evaluated;
            out.creates = renderer.creates;
            out.handleAddress = renderer.handleAddress;
            out.carrier = stage.carrierResolved == Neural::kCarrierFp16 ? "FP16" : "native";
            std::snprintf(out.reason, sizeof(out.reason), "%s",
                stage.failed.load(std::memory_order_relaxed) ? stage.failReason : renderer.reason);
            state.evaluations += stage.evaluated;
            state.rebuilds += stage.rebuilds;
            if (i < measured) {
                state.cpuMs += out.cpuMs;
                state.gpuMsMedianSum += out.gpuMsMedian;
            }
        }
        state.frames = g_passes[0].frames;
        state.gpuTimed = D3D12Sidecar::TimestampsAvailable();
        const std::uint32_t last = state.activePasses != 0U ? state.activePasses - 1U : 0U;
        const auto& stage = g_passes[last];
        const auto renderer = NeuralRenderer::Snapshot(last);
        state.lastResult = renderer.lastResult;
        state.lastResultName = renderer.lastFault != 0 ? "exception" : Ngx::ResultName(static_cast<Ngx::Result>(renderer.lastResult));
        state.carrier = state.passes[last].carrier;
        state.guidesSource = stage.guidesSource.load(std::memory_order_relaxed);
        state.outWidth = stage.snapshotOutW.load(std::memory_order_relaxed);
        state.outHeight = stage.snapshotOutH.load(std::memory_order_relaxed);
        state.modelPercent = Neural::EffectiveModelPercent(mailbox.applied);
        state.modelWidth = g_modelW.load(std::memory_order_relaxed);
        state.modelHeight = g_modelH.load(std::memory_order_relaxed);
        std::snprintf(state.modelReason, sizeof(state.modelReason), "%s", g_modelReason);
        state.frameStagesDelivered = g_activePasses.load(std::memory_order_relaxed);
        state.frameColourCopies = g_lastColourCopies.load(std::memory_order_relaxed);
        state.frameCrossApiTrips = g_lastCrossApiTrips.load(std::memory_order_relaxed);
        state.frameCopyBytes = g_lastCopyBytes.load(std::memory_order_relaxed);
        state.d3d11Timed = g_d3d11Timed.load(std::memory_order_relaxed);
        state.d3d11PrepMsMedian = g_prepBracket.median.load(std::memory_order_relaxed);
        state.d3d11ReturnMsMedian = g_returnBracket.median.load(std::memory_order_relaxed);
        state.d3d11PrepBrackets = g_prepBracket.retired.load(std::memory_order_relaxed);
        state.d3d11ReturnBrackets = g_returnBracket.retired.load(std::memory_order_relaxed);
        state.d3d11Windows = g_d3d11Windows.load(std::memory_order_relaxed);
        state.latched = state.passes[0].latched;
        if (!state.enabled) {
            std::snprintf(state.status, sizeof(state.status), "Off");
        } else if (g_blockReason[0] != '\0') {
            std::snprintf(state.status, sizeof(state.status), "%s", g_blockReason);
        } else if (state.pending) {
            std::snprintf(state.status, sizeof(state.status), "Applying %u-pass configuration", state.requestedPasses);
        } else if (state.activePasses == state.requestedPasses) {
            if (state.modelReason[0] != '\0') {
                std::snprintf(state.status, sizeof(state.status), "Active: %u pass(es), model 100%% (%u%% not applied: %.80s), output %ux%u",
                    state.activePasses, state.modelPercent, state.modelReason, state.outWidth, state.outHeight);
            } else {
                std::snprintf(state.status, sizeof(state.status), "Active: %u pass(es), model %ux%u (%u%%), output %ux%u",
                    state.activePasses, state.modelWidth, state.modelHeight, state.modelPercent, state.outWidth, state.outHeight);
            }
        } else {
            const std::uint32_t stopped = state.activePasses < Neural::kMaxPasses ? state.activePasses : 0U;
            std::snprintf(state.status, sizeof(state.status), "%u/%u pass(es): pass %u %.110s",
                state.activePasses, state.requestedPasses, stopped + 1U,
                state.passes[stopped].reason[0] != '\0' ? state.passes[stopped].reason : "waiting for valid inputs");
        }
        return state;
    }
}
