#include "PCH.h"

#include "Platform/FrameGenEngine.h"
#include "Platform/FsrFrameGen.h"

#include "Platform/D3D12Sidecar.h"
#include "Platform/DynamicMultiplier.h"
#include "Platform/FrameBufferResolve.h"
#include "Platform/Matrix4.h"
#include "Platform/NgxD3D12.h"
#include "Platform/PresentPolicy.h"
#include "Platform/RendererContracts.h"
#include "Platform/SidecarFrame.h"
#include "Platform/SidecarGuides.h"
#include "Platform/Streamline.h"
#include "UI/Menu.h"

#include "CSUiAlpha.h"

#include "RE/Bethesda/BSGraphics.h"

#include <atomic>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

namespace
{
    using namespace Platform;
    using namespace Platform::SidecarGuides;

    constexpr int kHudlessSlots = 3;
    constexpr int kMaxGenerated = static_cast<int>(Platform::FrameGenEngine::kMaxGeneratedFrames);
    constexpr int kOutputSlots = 4 * kMaxGenerated;
    constexpr int kEvaluateStrikes = 3;
    constexpr std::uint32_t kFrameBufferIndex = static_cast<std::uint32_t>(Fallout4RenderTargetIndex::kFrameBuffer);

    struct Camera
    {
        bool valid{ false };
        float viewToClip[16]{};
        float clipToView[16]{};
        float clipToPrevClip[16]{};
        float prevClipToClip[16]{};
        float pos[3]{}, up[3]{}, right[3]{}, fwd[3]{};
        float nearPlane{ 0 }, farPlane{ 0 }, fov{ 0 }, aspect{ 0 };
    };

    struct Guides
    {
        bool valid{ false };
        ID3D12Resource* depth{ nullptr };
        ID3D12Resource* mv{ nullptr };
        std::uint32_t renderW{ 0 }, renderH{ 0 };
        float jitterX{ 0 }, jitterY{ 0 };
        bool reset{ false };
    };

    struct FeatureKey
    {
        std::uint32_t width{ 0 }, height{ 0 }, renderW{ 0 }, renderH{ 0 };
        DXGI_FORMAT format{ DXGI_FORMAT_UNKNOWN };
        std::uint32_t frames{ 1 };
        [[nodiscard]] bool operator==(const FeatureKey&) const noexcept = default;
    };

    struct EngineState
    {
        std::atomic<bool> enabled{ false };
        std::atomic<std::uint32_t> framesRequested{ 1 };
        std::uint32_t frameCap{ static_cast<std::uint32_t>(kMaxGenerated) };
        std::atomic<std::uint32_t> snapFrames{ 1 };
        std::atomic<std::uint32_t> snapFrameCap{ static_cast<std::uint32_t>(kMaxGenerated) };
        bool framesLogged{ false };
        std::atomic<bool> dynamic{ false };
        std::atomic<std::uint32_t> dynamicTargetHz{ 0 };
        std::atomic<float> displayHz{ 0.0F };
        std::atomic<bool> dynamicRefused{ false };
        std::atomic<std::uint32_t> snapPairFrames{ 1 };
        std::atomic<float> snapPairIntervalMs{ 0.0F };
        std::uint32_t pairFrames{ 1 };
        std::uint32_t dynamicCandidate{ 1 };
        std::uint32_t dynamicCandidateRun{ 0 };
        std::uint32_t dynamicSinceChange{ 0 };
        double dynamicIntervalMs{ 0.0 };
        bool dynamicWaitingLogged{ false };
        bool dynamicEnteredLogged{ false };
        std::atomic<float> depthSeparation{ 40.0F };
        float depthSeparationApplied{ 40.0F };
        std::atomic<bool> latched{ false };
        std::atomic<bool> interpolating{ false };
        char reason[160]{};

        Camera camera{};
        Guides guides{};
        SidecarFrame::Cursor guidesCursor{};
        SidecarFrame::Cursor hudlessCursor{};
        bool haveHudless{ false };
        bool hudlessShared{ false };
        ID3D12Resource* sharedHudless{ nullptr };
        ID3D11Texture2D* sharedHudless11{ nullptr };
        std::uint32_t sharedHudlessW{ 0 }, sharedHudlessH{ 0 };
        DXGI_FORMAT sharedHudlessFmt{ DXGI_FORMAT_UNKNOWN };
        std::atomic<int> hudlessSource{ -1 };
        unsigned hudlessSeen{ 0 };
        int hudlessSlot{ 0 };
        int hudlessPresentSlot{ -1 };
        bool wasInterpolatingLastFrame{ false };
        bool pendingReset{ true };

        bool pathsResolved{ false };
        std::wstring dataPath;
        std::uint64_t sessionSerial{ 0 };
        Ngx::Parameter* params{ nullptr };
        Ngx::Handle* feature{ nullptr };
        ID3D12CommandAllocator* featureAllocator{ nullptr };
        FeatureKey key{};
        int strikes{ 0 };
        std::uint64_t evaluations{ 0 };
        std::uint32_t creates{ 0 };
        std::atomic<std::uint32_t> lastResult{ 0 };

        ID3D11Device* device11{ nullptr };
        D3D12Sidecar::SharedTexture hudless[kHudlessSlots]{};
        std::uint32_t hudlessW{ 0 }, hudlessH{ 0 };
        DXGI_FORMAT hudlessFmt{ DXGI_FORMAT_UNKNOWN };
        std::uint64_t hudlessReadFence[kHudlessSlots]{};
        bool uiAlphaRefused{ false };
        std::atomic<bool> snapUiAlphaRefused{ false };
        std::atomic<bool> snapUiAlphaActive{ false };
        bool haveUiAlpha{ false };
        int uiAlphaSlot{ 0 };
        D3D12Sidecar::SharedTexture uiAlpha[kHudlessSlots]{};
        ID3D11UnorderedAccessView* uiAlphaUav[kHudlessSlots]{};
        std::uint64_t uiAlphaReadFence[kHudlessSlots]{};
        std::uint32_t uiAlphaW{ 0 }, uiAlphaH{ 0 };
        ID3D11ComputeShader* csUiAlpha{ nullptr };
        unsigned uiAlphaLogged{ 0 };
        ID3D12Resource* output[kOutputSlots]{};
        int outputCount{ 0 };
        std::uint32_t outputW{ 0 }, outputH{ 0 };
        DXGI_FORMAT outputFmt{ DXGI_FORMAT_UNKNOWN };
        int outputSlot{ 0 };

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
            logger::warn("[FrameGen] {} — frame generation is OFF (real frames only) until 'Retry' or a restart", g.reason);
        }
    }

    void ReleaseFeatureHandle() noexcept
    {
        if (g.feature != nullptr) {
            if (NgxD3D12::SessionSerial() == g.sessionSerial) {
                const NgxD3D12::Outcome release = NgxD3D12::CoreRelease(g.feature);
                if (release.faultCode != 0) {
                    Latch("ReleaseFeature(11) raised an exception");
                }
            }
            g.feature = nullptr;
            logger::info("[FrameGen] feature 11 released after {} evaluation(s)", g.evaluations);
        }
        g.key = FeatureKey{};
    }

    void ReleaseUiAlphaRing() noexcept
    {
        for (int i = 0; i < kHudlessSlots; ++i) {
            ReleaseCom(g.uiAlphaUav[i]);
            D3D12Sidecar::ReleaseShared(g.uiAlpha[i]);
            g.uiAlphaReadFence[i] = 0;
        }
        g.uiAlphaW = g.uiAlphaH = 0;
        g.haveUiAlpha = false;
    }

    void ReleaseResources() noexcept
    {
        for (int i = 0; i < kHudlessSlots; ++i) {
            D3D12Sidecar::ReleaseShared(g.hudless[i]);
            g.hudlessReadFence[i] = 0;
        }
        g.hudlessW = g.hudlessH = 0;
        ReleaseUiAlphaRing();
        ReleaseCom(g.csUiAlpha);
        for (auto& out : g.output) {
            ReleaseCom(out);
        }
        g.outputW = g.outputH = 0;
        g.outputCount = 0;
        g.haveHudless = false;
        g.hudlessShared = false;
        g.sharedHudless = nullptr;
        g.sharedHudless11 = nullptr;
    }

    [[nodiscard]] std::uint32_t EffectiveFrames() noexcept
    {
        const std::uint32_t requested = g.framesRequested.load(std::memory_order_relaxed);
        const std::uint32_t clamped = requested < 1U ? 1U : requested > static_cast<std::uint32_t>(kMaxGenerated) ? static_cast<std::uint32_t>(kMaxGenerated) : requested;
        return clamped < g.frameCap ? clamped : g.frameCap;
    }

    [[nodiscard]] bool OutputsMatch(std::uint32_t a_width, std::uint32_t a_height, DXGI_FORMAT a_format, int a_count) noexcept
    {
        return g.output[0] != nullptr && g.outputW == a_width && g.outputH == a_height && g.outputFmt == a_format &&
               g.outputCount == a_count;
    }

    [[nodiscard]] bool EnsureOutputs(std::uint32_t a_width, std::uint32_t a_height, DXGI_FORMAT a_format, int a_count) noexcept
    {
        if (OutputsMatch(a_width, a_height, a_format, a_count)) {
            return true;
        }
        for (auto& out : g.output) {
            ReleaseCom(out);
        }
        ID3D12Device* device = D3D12Sidecar::Device();
        if (device == nullptr) {
            return false;
        }
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = a_width;
        desc.Height = a_height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = a_format;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        for (int i = 0; i < a_count; ++i) {
            auto& out = g.output[i];
            if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&out))) || out == nullptr) {
                for (auto& o : g.output) {
                    ReleaseCom(o);
                }
                return false;
            }
        }
        g.outputW = a_width;
        g.outputH = a_height;
        g.outputFmt = a_format;
        g.outputCount = a_count;
        g.outputSlot = 0;
        return true;
    }

    [[nodiscard]] bool EnsureRuntime() noexcept
    {
        if (!g.pathsResolved) {
            const auto folder = SelfModuleDirectory() / L"FO4GraphicsOverhaul" / L"Streamline";
            g.dataPath = (folder / L"").wstring();
            g.pathsResolved = true;
        }
        if (!NgxD3D12::LoadCore()) {
            Latch("the NGX core (_nvngx.dll) is unavailable");
            return false;
        }
        const NgxD3D12::Outcome session = NgxD3D12::InitSession(D3D12Sidecar::Device(), g.dataPath.c_str(), g.dataPath.c_str());
        if (!session.Ok()) {
            char text[128]{};
            NgxD3D12::DescribeOutcome(session, text, sizeof(text));
            char reason[160]{};
            std::snprintf(reason, sizeof(reason), "the NGX D3D12 session did not initialise (%s)", text);
            Latch(reason);
            return false;
        }
        if (NgxD3D12::SessionSerial() != g.sessionSerial) {
            g.feature = nullptr;
            g.params = nullptr;
            g.key = FeatureKey{};
            g.sessionSerial = NgxD3D12::SessionSerial();
        }
        if (NgxD3D12::FrameGenerationAvailable() == 0) {
            Latch("NGX reports frame generation unavailable on this adapter/driver (FrameGeneration.Available=0)");
            return false;
        }
        if (g.params == nullptr) {
            g.params = NgxD3D12::AllocateParameters();
            if (g.params == nullptr) {
                Latch("NGX would not allocate a parameter block for frame generation");
                return false;
            }
            g.depthSeparationApplied = kDepthSeparationDefault;
        }
        if (g.featureAllocator == nullptr) {
            if (FAILED(D3D12Sidecar::Device()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                    IID_PPV_ARGS(&g.featureAllocator)))) {
                Latch("a command allocator for the frame-generation feature could not be created");
                return false;
            }
        }
        return true;
    }

    void SetCreateParams(const FeatureKey& a_key) noexcept
    {
        Ngx::Parameter* p = g.params;
        p->Set("DLSSG.Width", static_cast<unsigned int>(a_key.width));
        p->Set("DLSSG.Height", static_cast<unsigned int>(a_key.height));
        p->Set("DLSSG.InternalWidth", static_cast<unsigned int>(a_key.renderW));
        p->Set("DLSSG.InternalHeight", static_cast<unsigned int>(a_key.renderH));
        p->Set("DLSSG.BackbufferFormat", static_cast<unsigned int>(a_key.format));
        p->Set("DLSSG.CmdQueue", static_cast<void*>(D3D12Sidecar::Queue()));
        p->Set("DLSSG.CmdAlloc", static_cast<void*>(g.featureAllocator));
        p->Set("DLSSG.MultiFrameCount", a_key.frames);
        p->Set("DLSSG.DynamicResolution", 0U);
        p->Set("DLSSG.EnableInterp", 1U);
        p->Set("CreationNodeMask", 1U);
        p->Set("VisibilityNodeMask", 1U);
    }

    void SetMatrix(const char* a_name, float* a_matrix) noexcept
    {
        g.params->Set(a_name, static_cast<void*>(a_matrix));
    }

    void SetEvaluateParams(ID3D12Resource* a_backbuffer, ID3D12Resource* a_output, std::uint32_t a_width,
        std::uint32_t a_height, std::uint64_t a_frameId, bool a_reset, ID3D12Resource* a_hudless,
        std::uint32_t a_hudlessW, std::uint32_t a_hudlessH, std::uint32_t a_frames, std::uint32_t a_index,
        ID3D12Resource* a_uiAlpha) noexcept
    {
        Ngx::Parameter* p = g.params;
        const Guides& gd = g.guides;
        Camera& cam = g.camera;
        p->Set("DLSSG.Backbuffer", a_backbuffer);
        p->Set("DLSSG.HUDLess", a_hudless);
        p->Set("DLSSG.MVecs", gd.mv);
        p->Set("DLSSG.Depth", gd.depth);
        p->Set("DLSSG.UI", static_cast<ID3D12Resource*>(nullptr));
        p->Set("DLSSG.UIAlpha", a_uiAlpha);
        p->Set("DLSSG.UIAlphaSubrectBaseX", 0U);
        p->Set("DLSSG.UIAlphaSubrectBaseY", 0U);
        p->Set("DLSSG.UIAlphaSubrectWidth", static_cast<unsigned int>(a_uiAlpha != nullptr ? a_width : 0U));
        p->Set("DLSSG.UIAlphaSubrectHeight", static_cast<unsigned int>(a_uiAlpha != nullptr ? a_height : 0U));
        p->Set("DLSSG.OutputInterpolated", a_output);
        p->Set("DLSSG.OutputReal", static_cast<ID3D12Resource*>(nullptr));
        p->Set("DLSSG.BackbufferSubrectBaseX", 0U);
        p->Set("DLSSG.BackbufferSubrectBaseY", 0U);
        p->Set("DLSSG.BackbufferSubrectWidth", static_cast<unsigned int>(a_width));
        p->Set("DLSSG.BackbufferSubrectHeight", static_cast<unsigned int>(a_height));
        p->Set("DLSSG.HUDLessSubrectBaseX", 0U);
        p->Set("DLSSG.HUDLessSubrectBaseY", 0U);
        p->Set("DLSSG.HUDLessSubrectWidth", static_cast<unsigned int>(a_hudlessW));
        p->Set("DLSSG.HUDLessSubrectHeight", static_cast<unsigned int>(a_hudlessH));
        p->Set("DLSSG.MVecsSubrectBaseX", 0U);
        p->Set("DLSSG.MVecsSubrectBaseY", 0U);
        p->Set("DLSSG.MVecsSubrectWidth", static_cast<unsigned int>(gd.renderW));
        p->Set("DLSSG.MVecsSubrectHeight", static_cast<unsigned int>(gd.renderH));
        p->Set("DLSSG.DepthSubrectBaseX", 0U);
        p->Set("DLSSG.DepthSubrectBaseY", 0U);
        p->Set("DLSSG.DepthSubrectWidth", static_cast<unsigned int>(gd.renderW));
        p->Set("DLSSG.DepthSubrectHeight", static_cast<unsigned int>(gd.renderH));
        p->Set("DLSSG.OutputInterpolatedSubrectBaseX", 0U);
        p->Set("DLSSG.OutputInterpolatedSubrectBaseY", 0U);
        p->Set("DLSSG.OutputInterpolatedSubrectWidth", static_cast<unsigned int>(a_width));
        p->Set("DLSSG.OutputInterpolatedSubrectHeight", static_cast<unsigned int>(a_height));
        p->Set("DLSSG.JitterOffsetX", -gd.jitterX);
        p->Set("DLSSG.JitterOffsetY", -gd.jitterY);
        p->Set("DLSSG.MvecScaleX", static_cast<float>(gd.renderW));
        p->Set("DLSSG.MvecScaleY", static_cast<float>(gd.renderH));
        p->Set("DLSSG.MvecDilated", 0U);
        p->Set("DLSSG.MvecJittered", 0U);
        p->Set("DLSSG.MvecInvalidValue", FLT_MIN);
        p->Set("DLSSG.ColorBuffersHDR", 0U);
        p->Set("DLSSG.DepthInverted", 0U);
        p->Set("DLSSG.OrthoProjection", 0U);
        p->Set("DLSSG.CameraMotionIncluded", 1U);
        SetMatrix("DLSSG.CameraViewToClip", cam.viewToClip);
        SetMatrix("DLSSG.ClipToCameraView", cam.clipToView);
        SetMatrix("DLSSG.ClipToPrevClip", cam.clipToPrevClip);
        SetMatrix("DLSSG.PrevClipToClip", cam.prevClipToClip);
        p->Set("DLSSG.CameraPosX", cam.pos[0]);
        p->Set("DLSSG.CameraPosY", cam.pos[1]);
        p->Set("DLSSG.CameraPosZ", cam.pos[2]);
        p->Set("DLSSG.CameraUpX", cam.up[0]);
        p->Set("DLSSG.CameraUpY", cam.up[1]);
        p->Set("DLSSG.CameraUpZ", cam.up[2]);
        p->Set("DLSSG.CameraRightX", cam.right[0]);
        p->Set("DLSSG.CameraRightY", cam.right[1]);
        p->Set("DLSSG.CameraRightZ", cam.right[2]);
        p->Set("DLSSG.CameraFwdX", cam.fwd[0]);
        p->Set("DLSSG.CameraFwdY", cam.fwd[1]);
        p->Set("DLSSG.CameraFwdZ", cam.fwd[2]);
        p->Set("DLSSG.CameraNear", cam.nearPlane);
        p->Set("DLSSG.CameraFar", cam.farPlane);
        p->Set("DLSSG.CameraFOV", cam.fov);
        p->Set("DLSSG.CameraAspectRatio", cam.aspect);
        p->Set("DLSSG.CameraPinholeOffsetX", 0.0F);
        p->Set("DLSSG.CameraPinholeOffsetY", 0.0F);
        p->Set("DLSSG.Reset", a_reset ? 1U : 0U);
        p->Set("DLSSG.EnableInterp", 1U);
        p->Set("DLSSG.MultiFrameCount", a_frames);
        p->Set("DLSSG.MultiFrameIndex", a_index);
        p->Set("DLSSG.BackbufferFrameID", static_cast<unsigned int>(a_frameId & 0xFFFFFFFFULL));
        p->Set("DLSSG.NotRenderingGameFrames", 0U);
        p->Set("DLSSG.EvalFlags", 0U);
        p->Set("DLSSG.MenuDetectionEnabled", 0U);
        p->Set("DLSSG.UserInterfaceRecompositionEnabled", 0U);
    }

    [[nodiscard]] bool EnsureFeature(const FeatureKey& a_key) noexcept
    {
        if (g.feature != nullptr && g.key == a_key) {
            return true;
        }
        if (g.feature != nullptr && D3D12Sidecar::IsOpen() && !D3D12Sidecar::Disabled() && !D3D12Sidecar::DrainGpu()) {
            return false;
        }
        ReleaseFeatureHandle();
        SetCreateParams(a_key);
        ID3D12GraphicsCommandList* list = nullptr;
        if (!D3D12Sidecar::BeginCommands(list)) {
            return false;
        }
        Ngx::Handle* handle = nullptr;
        const NgxD3D12::Outcome create = NgxD3D12::CoreCreate(list, Ngx::kFeatureFrameGeneration, g.params, &handle);
        if (create.faultCode != 0) {
            (void)D3D12Sidecar::AbandonCommands();
            g.lastResult.store(Ngx::Code(create.result), std::memory_order_relaxed);
            char text[128]{};
            NgxD3D12::DescribeOutcome(create, text, sizeof(text));
            char reason[160]{};
            std::snprintf(reason, sizeof(reason), "CreateFeature(11) raised %s", text);
            Latch(reason);
            return false;
        }
        bool listAccepted = false;
        const std::uint64_t value = D3D12Sidecar::EndCommands(&listAccepted);
        const bool completed = D3D12Sidecar::WaitForValue(value, 4000);
        if (!listAccepted) {
            if (handle != nullptr) {
                (void)NgxD3D12::CoreRelease(handle);
            }
            Latch("the frame-generation feature's creation list was dropped by the sidecar (its GPU initialisation never ran)");
            return false;
        }
        g.lastResult.store(Ngx::Code(create.result), std::memory_order_relaxed);
        char text[128]{};
        NgxD3D12::DescribeOutcome(create, text, sizeof(text));
        if (!completed) {
            D3D12Sidecar::Disable("frame-generation feature creation did not complete within 4 s");
            Latch("feature creation did not complete on the GPU");
            return false;
        }
        if (!Ngx::Succeeded(create.result) || handle == nullptr) {
            char reason[160]{};
            std::snprintf(reason, sizeof(reason), "CreateFeature(11) failed %s for %ux%u (render %ux%u, fmt %d) - is nvngx_dlssg.dll staged?",
                text, a_key.width, a_key.height, a_key.renderW, a_key.renderH, static_cast<int>(a_key.format));
            Latch(reason);
            return false;
        }
        g.feature = handle;
        g.key = a_key;
        g.strikes = 0;
        g.pendingReset = true;
        ++g.creates;
        g.snapCreates.store(g.creates, std::memory_order_relaxed);
        logger::info("[FrameGen] feature 11 created: {}x{} (render {}x{}, fmt {}) on the sidecar (create #{})",
            a_key.width, a_key.height, a_key.renderW, a_key.renderH, static_cast<int>(a_key.format), g.creates);
        return true;
    }

    void DeriveCamera(const CameraConstants& a_camera) noexcept
    {
        Camera& cam = g.camera;
        cam.valid = false;
        if (!a_camera.hasReprojection) {
            return;
        }
        float invView[16]{}, invCurVP[16]{};
        if (!Matrix4::Invert(invView, a_camera.viewMatrix) || !Matrix4::Invert(invCurVP, a_camera.curViewProj)) {
            return;
        }
        Matrix4::Multiply(cam.viewToClip, invView, a_camera.curViewProj);
        Matrix4::Multiply(cam.clipToPrevClip, invCurVP, a_camera.prevViewProj);
        if (!Matrix4::Invert(cam.clipToView, cam.viewToClip) || !Matrix4::Invert(cam.prevClipToClip, cam.clipToPrevClip)) {
            return;
        }
        for (int i = 0; i < 16; ++i) {
            if (!std::isfinite(cam.viewToClip[i]) || !std::isfinite(cam.clipToView[i]) ||
                !std::isfinite(cam.clipToPrevClip[i]) || !std::isfinite(cam.prevClipToClip[i])) {
                return;
            }
        }
        for (int i = 0; i < 3; ++i) {
            cam.pos[i] = a_camera.eyePosition[i];
            cam.up[i] = a_camera.cameraUp[i];
            cam.right[i] = a_camera.cameraRight[i];
            cam.fwd[i] = a_camera.cameraForward[i];
        }
        cam.nearPlane = a_camera.nearPlane;
        cam.farPlane = a_camera.farPlane;
        cam.fov = a_camera.fovVertical;
        cam.aspect = a_camera.aspectRatio;
        const auto finitePositive = [](float v) noexcept { return std::isfinite(v) && v > 0.0F; };
        const auto finiteVec = [](const float (&v)[3]) noexcept {
            return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
        };
        cam.valid = finitePositive(cam.nearPlane) && finitePositive(cam.farPlane) && cam.farPlane > cam.nearPlane &&
                    finitePositive(cam.fov) && finitePositive(cam.aspect) && finiteVec(cam.pos) && finiteVec(cam.up) &&
                    finiteVec(cam.right) && finiteVec(cam.fwd);
    }
}

namespace Platform::FrameGenEngine
{
    void SetFrames(std::uint32_t a_frames) noexcept
    {
        const std::uint32_t clamped = a_frames < 1U ? 1U : a_frames > kMaxGeneratedFrames ? kMaxGeneratedFrames : a_frames;
        if (g.framesRequested.exchange(clamped, std::memory_order_relaxed) != clamped) {
            logger::info("[FrameGen] multi-frame requested: {} generated frame(s) per real frame ({}x) — applies at the next feature create",
                clamped, clamped + 1);
        }
    }

    void SetDynamic(bool a_dynamic) noexcept
    {
        if (g.dynamic.exchange(a_dynamic, std::memory_order_relaxed) != a_dynamic) {
            logger::info("[FrameGen] dynamic multiplier {}", a_dynamic ? "requested: the controller takes over the count at the next present"
                                                                       : "off: the fixed multiplier applies again");
        }
    }

    void SetDynamicTargetHz(std::uint32_t a_hz) noexcept
    {
        const std::uint32_t clamped = ClampDynamicTargetHz(a_hz);
        if (g.dynamicTargetHz.exchange(clamped, std::memory_order_relaxed) != clamped) {
            if (clamped == 0U) {
                logger::info("[FrameGen] dynamic target: the display's refresh");
            } else {
                logger::info("[FrameGen] dynamic target: {} Hz", clamped);
            }
        }
    }

    void SetDisplayRefresh(float a_hz) noexcept
    {
        const float value = (a_hz > 1.0F && a_hz < 2000.0F) ? a_hz : 0.0F;
        const float before = g.displayHz.exchange(value, std::memory_order_relaxed);
        if (before != value && value > 0.0F) {
            logger::info("[FrameGen] display refresh {:.0f} Hz (the dynamic multiplier's default target)", value);
        }
    }

    void SetDepthSeparation(float a_separation) noexcept
    {
        g.depthSeparation.store(ClampDepthSeparation(a_separation), std::memory_order_relaxed);
    }

    void SetEnabled(bool a_enabled) noexcept
    {
        if (g.enabled.exchange(a_enabled, std::memory_order_relaxed) != a_enabled) {
            logger::info("[FrameGen] {}", a_enabled ? "ENABLED (interpolates when the DirectX 12 path presents through the proxy)" : "disabled");
            if (a_enabled) {
                FsrFrameGen::ReArmAfterEnable();
            }
        }
    }

    bool Enabled() noexcept { return g.enabled.load(std::memory_order_relaxed); }

    void RequestRetry() noexcept
    {
        if (g.latched.exchange(false, std::memory_order_relaxed)) {
            g.reason[0] = '\0';
            g.strikes = 0;
            logger::info("[FrameGen] retry requested: the latch is cleared");
        }
    }

    void NoteCamera(const CameraConstants& a_camera) noexcept
    {
        DeriveCamera(a_camera);
    }

    void CollectGuides() noexcept
    {
        Guides& gd = g.guides;
        SidecarFrame::Guides rec{};
        if (!SidecarFrame::TakeGuides(g.guidesCursor, rec)) {
            gd = Guides{};
            return;
        }
        gd.depth = rec.fgDepth != nullptr ? rec.fgDepth : rec.depth;
        gd.mv = rec.fgMv != nullptr ? rec.fgMv : rec.mv;
        gd.renderW = rec.renderWidth;
        gd.renderH = rec.renderHeight;
        gd.jitterX = rec.jitterX;
        gd.jitterY = rec.jitterY;
        gd.reset = rec.reset;
        gd.valid = true;
        if (rec.reset) {
            g.pendingReset = true;
        }
    }

    void NoteHudlessSource(int a_source) noexcept
    {
        g.hudlessSource.store(a_source, std::memory_order_relaxed);
        const unsigned bit = 1U << static_cast<unsigned>(a_source + 1);
        if ((g.hudlessSeen & bit) == 0U) {
            g.hudlessSeen |= bit;
            logger::info("[FrameGen] HUD-less source (first time): {}", a_source == 1
                ? "the neural pass's D3D12 output, taken on the sidecar (shared pipeline: no RT0 copy, no ring)"
                : a_source == 0 ? "RT0 copied into the shared HUD-less ring"
                                : "none (frame generation is not capturing: off, latched, or not the DirectX 12 path)");
        }
    }

    void* TakeHudlessForPresent(std::uint32_t a_width, std::uint32_t a_height, DXGI_FORMAT a_format) noexcept
    {
        g.hudlessPresentSlot = -1;
        if (!g.haveHudless) {
            return nullptr;
        }
        ID3D12Resource* const tex = g.hudlessShared ? g.sharedHudless : g.hudless[g.hudlessSlot].d3d12;
        const std::uint32_t w = g.hudlessShared ? g.sharedHudlessW : g.hudlessW;
        const std::uint32_t h = g.hudlessShared ? g.sharedHudlessH : g.hudlessH;
        const DXGI_FORMAT fmt = g.hudlessShared ? g.sharedHudlessFmt : g.hudlessFmt;
        if (tex == nullptr || w != a_width || h != a_height || fmt != a_format) {
            static std::atomic<bool> s_logged{ false };
            if (!s_logged.exchange(true, std::memory_order_relaxed)) {
                logger::info("[FrameGen] HUD-less {}x{} fmt={} ({}) != the presented backbuffer {}x{} fmt={}; FSR-FG interpolates the composited frame until they match",
                    w, h, static_cast<int>(fmt), g.hudlessShared ? "the neural output" : "RT0 capture",
                    a_width, a_height, static_cast<int>(a_format));
            }
            return nullptr;
        }
        if (!g.hudlessShared) {
            g.hudlessPresentSlot = g.hudlessSlot;
        }
        return tex;
    }

    void NoteHudlessPresented(std::uint64_t a_fence) noexcept
    {
        if (g.hudlessPresentSlot >= 0 && g.hudlessPresentSlot < kHudlessSlots) {
            if (a_fence != 0) {
                g.hudlessReadFence[g.hudlessPresentSlot] = a_fence;
            }
            g.hudlessPresentSlot = -1;
        }
    }

    void CaptureHudless() noexcept
    {
        if (!Enabled() || g.latched.load(std::memory_order_relaxed) || !D3D12Sidecar::IsOpen() || D3D12Sidecar::Disabled()) {
            NoteHudlessSource(-1);
            return;
        }
        if (Streamline::AppliedEffectiveEngine() != AaEffectiveEngine::kDlss && !FsrFrameGen::Engaged()) {
            NoteHudlessSource(-1);
            return;
        }
        g.hudlessShared = false;
        {
            SidecarFrame::Hudless rec{};
            const bool took = SidecarFrame::TakeHudless(g.hudlessCursor, rec);
            if (took) {
                g.sharedHudless = rec.colour;
                g.sharedHudless11 = rec.colour11;
                g.sharedHudlessW = rec.width;
                g.sharedHudlessH = rec.height;
                g.sharedHudlessFmt = rec.format;
                g.hudlessShared = true;
                g.haveHudless = true;
                NoteHudlessSource(1);
                return;
            }
        }
        NoteHudlessSource(0);
        try {
            auto* const data = RE::BSGraphics::RendererData::GetSingleton();
            if (data == nullptr || data->device == nullptr || data->context == nullptr) {
                return;
            }
            auto* const device = reinterpret_cast<ID3D11Device*>(data->device);
            if (D3D12Sidecar::BoundDevice() != device) {
                return;
            }
            if (g.device11 != device) {
                for (int i = 0; i < kHudlessSlots; ++i) {
                    D3D12Sidecar::ReleaseShared(g.hudless[i]);
                    g.hudlessReadFence[i] = 0;
                }
                g.hudlessW = g.hudlessH = 0;
                g.haveHudless = false;
                g.device11 = device;
            }
            auto& record = data->renderTargets[kFrameBufferIndex];
            ResolvedFrameBuffer frame{};
            ResolveFrameBufferTexture(record, frame);
            if (frame.texture == nullptr) {
                return;
            }
            D3D11_TEXTURE2D_DESC desc{};
            frame.texture->GetDesc(&desc);
            if (desc.SampleDesc.Count != 1) {
                return;
            }
            const DXGI_FORMAT format = TypedColorFormat(desc.Format);
            if (g.hudless[0].d3d11 == nullptr || g.hudlessW != desc.Width || g.hudlessH != desc.Height || g.hudlessFmt != format) {
                for (auto& slot : g.hudless) {
                    D3D12Sidecar::ReleaseShared(slot);
                }
                for (int i = 0; i < kHudlessSlots; ++i) {
                    char name[32]{};
                    std::snprintf(name, sizeof(name), "framegen hudless %d", i);
                    if (!D3D12Sidecar::CreateShared(g.hudless[i], name, desc.Width, desc.Height, format, false)) {
                        Latch("a shared HUD-less texture could not be created (see the [Sidecar] lines)");
                        return;
                    }
                }
                g.hudlessW = desc.Width;
                g.hudlessH = desc.Height;
                g.hudlessFmt = format;
            }
            g.hudlessSlot = (g.hudlessSlot + 1) % kHudlessSlots;
            const D3D12Sidecar::FrameLock frameLock;
            if (g.hudlessReadFence[g.hudlessSlot] != 0) {
                (void)D3D12Sidecar::WaitOnD3D11(g.hudlessReadFence[g.hudlessSlot]);
            }
            data->context->CopyResource(g.hudless[g.hudlessSlot].d3d11, frame.texture);
            g.haveHudless = true;
        } catch (...) {
            Latch("C++ exception capturing the HUD-less frame");
        }
    }

    [[nodiscard]] bool RealFramesOnly() noexcept
    {
        if (!Enabled()) {
            return true;
        }
        return UI::Menu::GetSingleton().IsOpen() || PresentPolicy::CurrentState().loadingScreenActive;
    }

    bool WantsInterpolation() noexcept
    {
        if (RealFramesOnly() || g.latched.load(std::memory_order_relaxed)) {
            return false;
        }
        if (Streamline::AppliedEffectiveEngine() != AaEffectiveEngine::kDlss) {
            return false;
        }
        if (!g.camera.valid || !g.guides.valid || !g.haveHudless) {
            return false;
        }
        return true;
    }

    constexpr std::uint32_t kDynamicSettlePresents = 20;
    constexpr std::uint32_t kDynamicDwellPresents = 30;
    constexpr double kDynamicMinIntervalMs = 2.0;
    constexpr double kDynamicMaxIntervalMs = 100.0;

    [[nodiscard]] std::uint32_t DynamicCap() noexcept
    {
        const std::uint32_t cap = g.frameCap < kMaxGeneratedFrames ? g.frameCap : kMaxGeneratedFrames;
        return cap < 1U ? 1U : cap;
    }

    std::uint32_t FramesThisFrame(float a_realIntervalMs, std::uint32_t a_syncInterval) noexcept
    {
        const std::uint32_t fixed = EffectiveFrames();
        if (!g.dynamic.load(std::memory_order_relaxed)) {
            g.pairFrames = fixed;
            g.snapPairFrames.store(fixed, std::memory_order_relaxed);
            return fixed;
        }
        g.dynamicIntervalMs = (a_realIntervalMs >= kDynamicMinIntervalMs && a_realIntervalMs <= kDynamicMaxIntervalMs)
                                  ? static_cast<double>(a_realIntervalMs) : 0.0;

        const std::uint32_t cap = DynamicCap();
        if (g.pairFrames > cap) {
            g.pairFrames = cap;
        }
        if (RealFramesOnly()) {
            g.dynamicCandidateRun = 0;
            g.dynamicSinceChange = 0;
            g.snapPairFrames.store(g.pairFrames, std::memory_order_relaxed);
            g.snapPairIntervalMs.store(static_cast<float>(g.dynamicIntervalMs), std::memory_order_relaxed);
            return g.dynamicRefused.load(std::memory_order_relaxed) ? g.pairFrames : cap;
        }
        const std::uint32_t targetSetting = g.dynamicTargetHz.load(std::memory_order_relaxed);
        const float displayHz = g.displayHz.load(std::memory_order_relaxed);
        const float asked = targetSetting != 0U ? static_cast<float>(targetSetting) : displayHz;
        const float target = DynamicMultiplier::EffectiveTargetHz(asked, DynamicMultiplier::PresentedCapHz(displayHz, a_syncInterval));
        const bool capped = target < asked;
        if (!g.dynamicEnteredLogged) {
            g.dynamicEnteredLogged = true;
            logger::info("[FrameGen] dynamic multiplier ON: the feature is created for up to {} generated frame(s); each real-frame "
                         "pair asks for the smallest count whose presented rate reaches the target (target {} Hz{}{})", cap, target,
                targetSetting != 0U ? ", the setting" : ", the display's refresh",
                capped ? " — capped by VSync at what the chain can present" : "");
        }
        const std::uint32_t ideal = DynamicMultiplier::IdealGeneratedFrames(target, g.dynamicIntervalMs, cap);
        if (ideal != 0U) {
            if (ideal == g.dynamicCandidate) {
                ++g.dynamicCandidateRun;
            } else {
                g.dynamicCandidate = ideal;
                g.dynamicCandidateRun = 1;
            }
            ++g.dynamicSinceChange;
            if (g.dynamicCandidate != g.pairFrames && g.dynamicCandidateRun >= kDynamicSettlePresents &&
                g.dynamicSinceChange >= kDynamicDwellPresents) {
                const std::uint32_t before = g.pairFrames;
                g.pairFrames += g.dynamicCandidate > g.pairFrames ? 1U : static_cast<std::uint32_t>(-1);
                g.dynamicSinceChange = 0;
                logger::info("[FrameGen] dynamic: {} generated frame(s) per real frame ({}x) — real frame period {:.2f} ms, target {} Hz{}: "
                             "{:.0f} Hz presented at {}x, {:.0f} Hz at {}x on this period",
                    g.pairFrames, g.pairFrames + 1U, g.dynamicIntervalMs, target, capped ? " (capped by VSync)" : "",
                    DynamicMultiplier::PresentedHz(before, g.dynamicIntervalMs), before + 1U,
                    DynamicMultiplier::PresentedHz(g.pairFrames, g.dynamicIntervalMs), g.pairFrames + 1U);
            }
        } else if (!g.dynamicWaitingLogged) {
            g.dynamicWaitingLogged = true;
            logger::info("[FrameGen] dynamic: holding {} generated frame(s) — {}{}{}", g.pairFrames,
                target <= 0.0F ? "no target yet (the display's refresh is not known and no target is set)" : "",
                target <= 0.0F && g.dynamicIntervalMs <= 0.0 ? "; " : "",
                g.dynamicIntervalMs <= 0.0 ? "no real-frame period yet (the present worker has not paced a packet)" : "");
        }
        g.snapPairFrames.store(g.pairFrames, std::memory_order_relaxed);
        g.snapPairIntervalMs.store(static_cast<float>(g.dynamicIntervalMs), std::memory_order_relaxed);
        return g.dynamicRefused.load(std::memory_order_relaxed) ? g.pairFrames : cap;
    }

    bool OutputsNeedRebuild(std::uint32_t a_width, std::uint32_t a_height, DXGI_FORMAT a_format, std::uint32_t a_frames) noexcept
    {
        return g.output[0] != nullptr && !OutputsMatch(a_width, a_height, a_format, 4 * static_cast<int>(a_frames));
    }

    bool Prepare(std::uint32_t a_width, std::uint32_t a_height, DXGI_FORMAT a_format, std::uint32_t a_frames) noexcept
    {
        if (!g.guides.valid) {
            return false;
        }
        try {
            if (!EnsureRuntime()) {
                return false;
            }
            const std::uint32_t frames = a_frames < 1U ? 1U : a_frames > kMaxGeneratedFrames ? kMaxGeneratedFrames : a_frames;
            if (!EnsureOutputs(a_width, a_height, a_format, 4 * static_cast<int>(frames))) {
                Latch("the frame-generation output textures could not be created");
                return false;
            }
            const FeatureKey key{ a_width, a_height, g.guides.renderW, g.guides.renderH, a_format, frames };
            if (!EnsureFeature(key)) {
                return false;
            }
            if (!g.framesLogged || g.snapFrames.load(std::memory_order_relaxed) != frames) {
                g.framesLogged = true;
                g.snapFrames.store(frames, std::memory_order_relaxed);
                if (g.dynamic.load(std::memory_order_relaxed)) {
                    logger::info("[FrameGen] multi-frame: the feature is created for up to {} generated frame(s) per real frame (dynamic; "
                                 "each pair asks for its own count)", frames);
                } else {
                    logger::info("[FrameGen] multi-frame: {} generated frame(s) per real frame ({}x){}", frames, frames + 1,
                        frames < g.framesRequested.load(std::memory_order_relaxed) ? " — capped by the runtime's refusal" : "");
                }
            }
            return true;
        } catch (...) {
            Latch("C++ exception preparing frame generation");
            return false;
        }
    }

    std::uint32_t Evaluate(ID3D12GraphicsCommandList* a_list, ID3D12Resource* a_backbuffer, std::uint32_t a_width,
        std::uint32_t a_height, DXGI_FORMAT a_format, std::uint64_t a_frameId, ID3D12Resource** a_outputs) noexcept
    {
        const bool camera = g.camera.valid, guides = g.guides.valid, hudless = g.haveHudless;
        const bool alphaReady = g.haveUiAlpha;
        g.camera.valid = false;
        g.guides.valid = false;
        g.haveHudless = false;
        g.haveUiAlpha = false;
        if (a_list == nullptr || a_backbuffer == nullptr || a_outputs == nullptr || !camera || !guides || !hudless) {
            g.wasInterpolatingLastFrame = false;
            return 0;
        }
        try {
            const bool dynamic = g.dynamic.load(std::memory_order_relaxed);
            const std::uint32_t created = dynamic ? (g.key.frames < 1U ? 1U : g.key.frames) : EffectiveFrames();
            const FeatureKey k{ a_width, a_height, g.guides.renderW, g.guides.renderH, a_format, created };
            const std::uint32_t frames = dynamic ? (g.pairFrames < created ? g.pairFrames : created) : created;
            if (g.feature == nullptr || !(g.key == k) || g.output[0] == nullptr) {
                g.wasInterpolatingLastFrame = false;
                return 0;
            }
            ID3D12Resource* const hudlessTex = g.hudlessShared ? g.sharedHudless : g.hudless[g.hudlessSlot].d3d12;
            const std::uint32_t hudlessW = g.hudlessShared ? g.sharedHudlessW : g.hudlessW;
            const std::uint32_t hudlessH = g.hudlessShared ? g.sharedHudlessH : g.hudlessH;
            const DXGI_FORMAT hudlessFmt = g.hudlessShared ? g.sharedHudlessFmt : g.hudlessFmt;
            if (hudlessTex == nullptr || hudlessW != a_width || hudlessH != a_height || hudlessFmt != a_format) {
                static std::atomic<bool> s_logged{ false };
                if (!s_logged.exchange(true, std::memory_order_relaxed)) {
                    logger::info("[FrameGen] HUD-less {}x{} fmt={} ({}) != backbuffer {}x{} fmt={}; real frames only until they match",
                        hudlessW, hudlessH, static_cast<int>(hudlessFmt), g.hudlessShared ? "the neural output" : "RT0 capture",
                        a_width, a_height, static_cast<int>(a_format));
                }
                g.wasInterpolatingLastFrame = false;
                return 0;
            }
            const bool reset = g.pendingReset || !g.wasInterpolatingLastFrame;
            ID3D12Resource* const uiAlpha = (alphaReady && !g.uiAlphaRefused && g.uiAlpha[g.uiAlphaSlot].Valid())
                                                ? g.uiAlpha[g.uiAlphaSlot].d3d12 : nullptr;
            g.snapUiAlphaActive.store(uiAlpha != nullptr, std::memory_order_relaxed);

            D3D12Sidecar::Barrier(a_list, a_backbuffer, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            D3D12Sidecar::Barrier(a_list, hudlessTex, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            if (uiAlpha != nullptr) {
                D3D12Sidecar::Barrier(a_list, uiAlpha, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            }
            D3D12Sidecar::Barrier(a_list, g.guides.depth, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            D3D12Sidecar::Barrier(a_list, g.guides.mv, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            {
                const float separation = g.depthSeparation.load(std::memory_order_relaxed);
                if (separation != g.depthSeparationApplied) {
                    g.params->Set("DLSSG.MinRelativeLinearDepthObjectSeparation", separation);
                    logger::info("[FrameGen] depth-edge separation {:.1f} pushed ({})", separation,
                        separation == kDepthSeparationDefault ? "the runtime's documented default, restored once after another value"
                                                              : "replaces the runtime's default 40 until changed again");
                    g.depthSeparationApplied = separation;
                }
            }
            std::uint32_t produced = 0;
            NgxD3D12::Outcome eval{};
            for (std::uint32_t index = 1; index <= frames; ++index) {
                g.outputSlot = (g.outputSlot + 1) % (g.outputCount > 0 ? g.outputCount : 1);
                ID3D12Resource* const output = g.output[g.outputSlot];
                SetEvaluateParams(a_backbuffer, output, a_width, a_height, a_frameId, reset, hudlessTex, hudlessW, hudlessH,
                    frames, index, uiAlpha);
                eval = NgxD3D12::CoreEvaluate(a_list, g.feature, g.params);
                D3D12Sidecar::Barrier(a_list, output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
                a_outputs[produced++] = output;
                if (eval.faultCode != 0 || !Ngx::Succeeded(eval.result)) {
                    break;
                }
            }
            D3D12Sidecar::Barrier(a_list, a_backbuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
            D3D12Sidecar::Barrier(a_list, hudlessTex, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
            if (uiAlpha != nullptr) {
                D3D12Sidecar::Barrier(a_list, uiAlpha, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
            }
            D3D12Sidecar::Barrier(a_list, g.guides.depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
            D3D12Sidecar::Barrier(a_list, g.guides.mv, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);

            g.lastResult.store(Ngx::Code(eval.result), std::memory_order_relaxed);
            const bool failed = eval.faultCode != 0 || !Ngx::Succeeded(eval.result);
            if (failed && eval.faultCode == 0 && uiAlpha != nullptr && !g.uiAlphaRefused) {
                g.uiAlphaRefused = true;
                g.snapUiAlphaRefused.store(true, std::memory_order_relaxed);
                logger::warn("[FrameGen] the runtime refused an evaluate that happened to carry DLSSG.UIAlpha ({:#010x} {}) — the UI alpha is OFF for this session (the alpha may or may not be the cause; a restart re-arms it); the next frame evaluates without it",
                    Ngx::Code(eval.result), Ngx::ResultName(eval.result));
                for (std::uint32_t i = 0; i < produced; ++i) {
                    D3D12Sidecar::Barrier(a_list, a_outputs[i], D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                    a_outputs[i] = nullptr;
                }
                g.wasInterpolatingLastFrame = false;
                return 0;
            }
            if (failed) {
                for (std::uint32_t i = 0; i < produced; ++i) {
                    D3D12Sidecar::Barrier(a_list, a_outputs[i], D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                    a_outputs[i] = nullptr;
                }
                g.wasInterpolatingLastFrame = false;
                if (eval.faultCode != 0) {
                    char text[128]{};
                    NgxD3D12::DescribeOutcome(eval, text, sizeof(text));
                    char reason[160]{};
                    std::snprintf(reason, sizeof(reason), "EvaluateFeature(11) raised %s", text);
                    Latch(reason);
                    return 0;
                }
                if (dynamic && frames != created && !g.dynamicRefused.load(std::memory_order_relaxed)) {
                    g.dynamicRefused.store(true, std::memory_order_relaxed);
                    g.pendingReset = true;
                    logger::warn("[FrameGen] dynamic: the runtime refused {} generated frame(s) on a feature created for {} ({:#010x} {}) — "
                                 "per-pair counts are not honoured by this build; the feature will be re-created on each count change instead",
                        frames, created, Ngx::Code(eval.result), Ngx::ResultName(eval.result));
                    return 0;
                }
                if (frames > 1U && g.frameCap > 1U) {
                    g.frameCap = 1U;
                    g.snapFrameCap.store(1U, std::memory_order_relaxed);
                    logger::warn("[FrameGen] the runtime refused {} generated frames per real frame ({:#010x} {}) — back to 2x for "
                                 "this session (above 2x needs Blackwell, or RTX 40 with the runtime's architecture gates rewritten — "
                                 "the [MFG] lines say whether they were)",
                        frames, Ngx::Code(eval.result), Ngx::ResultName(eval.result));
                    return 0;
                }
                if (++g.strikes <= kEvaluateStrikes) {
                    logger::error("[FrameGen] evaluate failed {:#010x} {} (strike {}/{})", Ngx::Code(eval.result),
                        Ngx::ResultName(eval.result), g.strikes, kEvaluateStrikes);
                }
                if (g.strikes >= kEvaluateStrikes) {
                    char reason[160]{};
                    std::snprintf(reason, sizeof(reason), "%d consecutive evaluate failures (last %#010x %s)",
                        kEvaluateStrikes, Ngx::Code(eval.result), Ngx::ResultName(eval.result));
                    Latch(reason);
                }
                return 0;
            }
            g.strikes = 0;
            g.pendingReset = false;
            g.wasInterpolatingLastFrame = true;
            ++g.evaluations;
            g.snapEvaluations.store(g.evaluations, std::memory_order_relaxed);
            if (g.evaluations == 1) {
                logger::info("[FrameGen] ★ first interpolated frame produced: {}x{} (render {}x{}), reset={}, {} per real frame", a_width, a_height,
                    k.renderW, k.renderH, reset ? 1 : 0, frames);
            }
            return produced;
        } catch (...) {
            Latch("C++ exception in the frame-generation evaluate");
            g.wasInterpolatingLastFrame = false;
            return 0;
        }
    }

    void NoteSubmitted(std::uint64_t a_fence) noexcept
    {
        if (!g.hudlessShared) {
            g.hudlessReadFence[g.hudlessSlot] = a_fence;
        }
        if (g.snapUiAlphaActive.load(std::memory_order_relaxed)) {
            g.uiAlphaReadFence[g.uiAlphaSlot] = a_fence;
        }
    }

    void ProduceUiAlpha(ID3D11DeviceContext* a_context, ID3D11Texture2D* a_backbuffer11, std::uint32_t a_width,
        std::uint32_t a_height) noexcept
    {
        g.haveUiAlpha = false;
        if (g.uiAlphaRefused || a_context == nullptr || a_backbuffer11 == nullptr || !g.haveHudless ||
            a_width == 0U || a_height == 0U) {
            return;
        }
        ID3D11Texture2D* const hudless11 = g.hudlessShared ? g.sharedHudless11 : g.hudless[g.hudlessSlot].d3d11;
        if (hudless11 == nullptr) {
            if ((g.uiAlphaLogged & 2U) == 0U) {
                g.uiAlphaLogged |= 2U;
                logger::warn("[FrameGen] UI alpha: this frame's HUD-less has no D3D11 side to diff against (one-shot line)");
            }
            return;
        }
        try {
            ID3D11Device* device = nullptr;
            a_context->GetDevice(&device);
            if (device == nullptr) {
                return;
            }
            const auto releaseDevice = [device]() { device->Release(); };
            if (g.csUiAlpha == nullptr &&
                FAILED(device->CreateComputeShader(g_csUiAlpha, sizeof(g_csUiAlpha), nullptr, &g.csUiAlpha))) {
                releaseDevice();
                g.uiAlphaRefused = true;
                g.snapUiAlphaRefused.store(true, std::memory_order_relaxed);
                logger::warn("[FrameGen] UI alpha: the coverage shader would not create — off for this session");
                return;
            }
            if (!g.uiAlpha[0].Valid() || g.uiAlphaW != a_width || g.uiAlphaH != a_height) {
                ReleaseUiAlphaRing();
                for (int i = 0; i < kHudlessSlots; ++i) {
                    char name[32]{};
                    std::snprintf(name, sizeof(name), "framegen ui alpha %d", i);
                    if (!D3D12Sidecar::CreateShared(g.uiAlpha[i], name, a_width, a_height, DXGI_FORMAT_R8_UNORM, true) ||
                        !CreateUav(device, g.uiAlpha[i].d3d11, DXGI_FORMAT_R8_UNORM, g.uiAlphaUav[i])) {
                        ReleaseUiAlphaRing();
                        releaseDevice();
                        g.uiAlphaRefused = true;
                        g.snapUiAlphaRefused.store(true, std::memory_order_relaxed);
                        logger::warn("[FrameGen] UI alpha: the shared R8 ring could not be created — off for this session (see the [Sidecar] lines)");
                        return;
                    }
                }
                g.uiAlphaW = a_width;
                g.uiAlphaH = a_height;
            }
            const int slot = (g.uiAlphaSlot + 1) % kHudlessSlots;
            if (g.uiAlphaReadFence[slot] != 0) {
                (void)D3D12Sidecar::WaitOnD3D11(g.uiAlphaReadFence[slot]);
            }
            ID3D11ShaderResourceView* srvBackbuffer = nullptr;
            ID3D11ShaderResourceView* srvHudless = nullptr;
            if (FAILED(device->CreateShaderResourceView(a_backbuffer11, nullptr, &srvBackbuffer)) ||
                FAILED(device->CreateShaderResourceView(hudless11, nullptr, &srvHudless))) {
                ReleaseCom(srvBackbuffer);
                ReleaseCom(srvHudless);
                releaseDevice();
                if ((g.uiAlphaLogged & 4U) == 0U) {
                    g.uiAlphaLogged |= 4U;
                    logger::warn("[FrameGen] UI alpha: a source view would not create (one-shot line); no alpha this frame");
                }
                return;
            }
            releaseDevice();
            {
                const ComputeBindingsScope csScope(a_context);
                a_context->CSSetShader(g.csUiAlpha, nullptr, 0);
                ID3D11ShaderResourceView* srvs[] = { srvBackbuffer, srvHudless };
                a_context->CSSetShaderResources(3, 2, srvs);
                ID3D11UnorderedAccessView* uavs[] = { g.uiAlphaUav[slot] };
                a_context->CSSetUnorderedAccessViews(3, 1, uavs, nullptr);
                a_context->Dispatch((a_width + 7U) / 8U, (a_height + 7U) / 8U, 1U);
                ID3D11ShaderResourceView* noSrvs[] = { nullptr, nullptr };
                a_context->CSSetShaderResources(3, 2, noSrvs);
                ID3D11UnorderedAccessView* noUavs[] = { nullptr };
                a_context->CSSetUnorderedAccessViews(3, 1, noUavs, nullptr);
            }
            ReleaseCom(srvBackbuffer);
            ReleaseCom(srvHudless);
            g.uiAlphaSlot = slot;
            g.haveUiAlpha = true;
            if ((g.uiAlphaLogged & 1U) == 0U) {
                g.uiAlphaLogged |= 1U;
                logger::info("[FrameGen] UI alpha: first coverage frame produced ({}x{} R8, {} HUD-less); DLSSG.UIAlpha is set from here (always on)",
                    a_width, a_height, g.hudlessShared ? "the neural output's D3D11 side" : "the RT0 capture ring");
            }
        } catch (...) {
            g.haveUiAlpha = false;
        }
    }

    void DropFrameInputs() noexcept
    {
        g.camera.valid = false;
        g.guides.valid = false;
        g.haveHudless = false;
        g.haveUiAlpha = false;
        g.hudlessShared = false;
        g.wasInterpolatingLastFrame = false;
    }

    void RestoreOutputState(ID3D12GraphicsCommandList* a_list, ID3D12Resource* a_output) noexcept
    {
        D3D12Sidecar::Barrier(a_list, a_output, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    bool Interpolating() noexcept { return g.interpolating.load(std::memory_order_relaxed); }
    void NoteInterpolating(bool a_on) noexcept
    {
        if (g.interpolating.exchange(a_on, std::memory_order_relaxed) != a_on) {
            logger::info("[FrameGen] {}", a_on ? "interpolating: generated frames between the real ones"
                                              : "real frames only");
        }
    }

    void LatchPresentWorkerFailure(const char* a_reason) noexcept
    {
        DropFrameInputs();
        Latch(a_reason != nullptr ? a_reason : "the present worker failed");
    }

    bool HasResources() noexcept
    {
        return g.feature != nullptr || g.output[0] != nullptr || g.hudless[0].d3d11 != nullptr;
    }

    void Release() noexcept
    {
        ReleaseFeatureHandle();
        ReleaseResources();
        if (g.params != nullptr && NgxD3D12::SessionSerial() == g.sessionSerial) {
            NgxD3D12::DestroyParameters(g.params);
        }
        g.params = nullptr;
        ReleaseCom(g.featureAllocator);
        g.interpolating.store(false, std::memory_order_relaxed);
        g.wasInterpolatingLastFrame = false;
    }

    void AbandonAfterUnretiredSubmission() noexcept
    {
        bool owned = g.feature != nullptr || g.params != nullptr || g.featureAllocator != nullptr ||
                     g.csUiAlpha != nullptr;
        for (const auto& slot : g.hudless) {
            owned = owned || slot.d3d11 != nullptr || slot.d3d12 != nullptr || slot.handle != nullptr;
        }
        for (int i = 0; i < kHudlessSlots; ++i) {
            const auto& slot = g.uiAlpha[i];
            owned = owned || slot.d3d11 != nullptr || slot.d3d12 != nullptr || slot.handle != nullptr ||
                    g.uiAlphaUav[i] != nullptr;
        }
        for (const auto* output : g.output) {
            owned = owned || output != nullptr;
        }
        if (!owned) {
            g.interpolating.store(false, std::memory_order_relaxed);
            g.wasInterpolatingLastFrame = false;
            return;
        }

        g.feature = nullptr;
        g.params = nullptr;
        g.featureAllocator = nullptr;
        g.key = FeatureKey{};
        g.camera = Camera{};
        g.guides = Guides{};
        g.haveHudless = false;
        g.hudlessShared = false;
        g.sharedHudless = nullptr;
        g.sharedHudless11 = nullptr;
        g.sharedHudlessW = g.sharedHudlessH = 0;
        g.sharedHudlessFmt = DXGI_FORMAT_UNKNOWN;
        g.hudlessPresentSlot = -1;
        g.device11 = nullptr;
        for (int i = 0; i < kHudlessSlots; ++i) {
            g.hudless[i] = D3D12Sidecar::SharedTexture{};
            g.hudlessReadFence[i] = 0;
            g.uiAlpha[i] = D3D12Sidecar::SharedTexture{};
            g.uiAlphaUav[i] = nullptr;
            g.uiAlphaReadFence[i] = 0;
        }
        g.hudlessW = g.hudlessH = 0;
        g.hudlessFmt = DXGI_FORMAT_UNKNOWN;
        g.uiAlphaW = g.uiAlphaH = 0;
        g.csUiAlpha = nullptr;
        g.haveUiAlpha = false;
        for (auto& output : g.output) {
            output = nullptr;
        }
        g.outputCount = 0;
        g.outputW = g.outputH = 0;
        g.outputFmt = DXGI_FORMAT_UNKNOWN;
        g.interpolating.store(false, std::memory_order_relaxed);
        g.wasInterpolatingLastFrame = false;
        Latch("the present worker submitted GPU work whose retirement could not be proved; restart required");
        logger::warn("[FrameGen] NGX handles, allocators, shared rings, views and generated outputs are retained "
                     "until process exit; none was released after the unretired submission");
    }

    State Snapshot() noexcept
    {
        State state{};
        state.enabled = Enabled();
        state.latched = g.latched.load(std::memory_order_relaxed);
        std::snprintf(state.reason, sizeof(state.reason), "%s", g.reason);
        state.interpolating = g.interpolating.load(std::memory_order_relaxed);
        state.evaluations = g.snapEvaluations.load(std::memory_order_relaxed);
        state.creates = g.snapCreates.load(std::memory_order_relaxed);
        state.lastResult = g.lastResult.load(std::memory_order_relaxed);
        state.capability = NgxD3D12::FrameGenerationAvailable();
        state.hudlessSource = g.hudlessSource.load(std::memory_order_relaxed);
        state.frames = g.snapFrames.load(std::memory_order_relaxed);
        state.frameCap = g.snapFrameCap.load(std::memory_order_relaxed);
        state.uiAlphaActive = g.snapUiAlphaActive.load(std::memory_order_relaxed);
        state.uiAlphaRefused = g.snapUiAlphaRefused.load(std::memory_order_relaxed);
        state.dynamic = g.dynamic.load(std::memory_order_relaxed);
        state.pairFrames = g.snapPairFrames.load(std::memory_order_relaxed);
        state.pairIntervalMs = g.snapPairIntervalMs.load(std::memory_order_relaxed);
        state.dynamicTargetHz = g.dynamicTargetHz.load(std::memory_order_relaxed);
        state.displayHz = g.displayHz.load(std::memory_order_relaxed);
        state.dynamicRefused = g.dynamicRefused.load(std::memory_order_relaxed);
        state.depthSeparation = g.depthSeparation.load(std::memory_order_relaxed);
        return state;
    }
}
