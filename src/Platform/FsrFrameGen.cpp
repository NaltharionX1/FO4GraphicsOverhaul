// SPDX-License-Identifier: GPL-3.0-or-later
// Portions adapted from Community Shaders for Fallout 4 (northaxosky), GPL-3.0.

#include "PCH.h"

#include "Platform/FsrFrameGen.h"

#include "Platform/D3D12Sidecar.h"
#include "Platform/Fallout4Renderer.h"
#include "Platform/SidecarGuides.h"

#include "CSNeuralDepth.h"

#include <d3d12.h>
#include <dxgi1_4.h>

#include <ffx_api.h>
#include <ffx_api_loader.h>
#include <ffx_framegeneration.h>
#include <dx12/ffx_api_framegeneration_dx12.h>

#include <atomic>
#include <cstdio>
#include <cstring>

namespace
{
    std::atomic<bool> g_probed{ false };
    std::atomic<bool> g_available{ false };
    std::atomic<bool> g_selected{ false };
    char g_reason[128]{};

    ffxContext g_fgContext{ nullptr };
    std::atomic<std::uint64_t> g_frameId{ 0 };
    std::uint64_t g_lastEpoch{ 0 };
    bool g_preparedLastFrame{ false };
    std::atomic<std::uint64_t> g_prepared{ 0 };
    std::atomic<std::uint64_t> g_configureFails{ 0 };
    std::atomic<std::uint64_t> g_dispatchFails{ 0 };
    std::atomic<std::uint64_t> g_generated{ 0 };
    std::atomic<std::uint32_t> g_consecutiveDispatchFails{ 0 };
    std::atomic<bool> g_runtimeFailed{ false };

    ID3D11Device* g_captureDevice{ nullptr };
    Platform::D3D12Sidecar::SharedTexture g_sharedDepth{};
    Platform::D3D12Sidecar::SharedTexture g_sharedMotion{};
    ID3D11ComputeShader* g_depthCs{ nullptr };
    ID3D11ShaderResourceView* g_depthSrv{ nullptr };
    ID3D11UnorderedAccessView* g_depthUav{ nullptr };
    ID3D11Resource* g_depthSrvOwner{ nullptr };
    std::uint32_t g_captureW{ 0 };
    std::uint32_t g_captureH{ 0 };
    std::uint64_t g_lastPrepareFence{ 0 };
    std::atomic<bool> g_releasePending{ false };

    HMODULE g_loader{ nullptr };
    HMODULE g_effect{ nullptr };
    ffxFunctions g_ffx{};
    ffxContext g_swapChainContext{ nullptr };
    IDXGISwapChain4* g_chain{ nullptr };

    std::atomic<float> g_safetyMarginMs{ 0.1F };
    std::atomic<float> g_varianceFactor{ 0.1F };
    std::atomic<bool> g_hybridSpin{ false };
    std::atomic<std::uint32_t> g_hybridSpinTime{ 2U };
    std::atomic<bool> g_waitOnFence{ false };

    ffxReturnCode_t FrameGenerationDispatch(ffxDispatchDescFrameGeneration* a_params, void*)
    {
        ffxReturnCode_t rc = FFX_API_RETURN_ERROR;
        GUARD_BEGIN
        a_params->backbufferTransferFunction = FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
        rc = g_ffx.Dispatch(&g_fgContext, &a_params->header);
        if (rc != FFX_API_RETURN_OK) {
            g_dispatchFails.fetch_add(1, std::memory_order_relaxed);
            static std::atomic<std::uint32_t> logs{ 0 };
            if (logs.fetch_add(1, std::memory_order_relaxed) < 3) {
                logger::error("[FSR-FG] generation dispatch failed (ffx rc={})", static_cast<int>(rc));
            }
            if (g_consecutiveDispatchFails.fetch_add(1, std::memory_order_relaxed) >= 60 &&
                !g_runtimeFailed.exchange(true, std::memory_order_relaxed)) {
                logger::error("[FSR-FG] interpolation disabled after 60 consecutive dispatch failures — the "
                              "session keeps presenting real frames through AMD's chain");
            }
        } else {
            g_consecutiveDispatchFails.store(0, std::memory_order_relaxed);
            g_generated.fetch_add(1, std::memory_order_relaxed);
        }
        GUARD_END("fsrframegen.dispatch")
        return rc;
    }

    void ReleaseCaptureResources() noexcept
    {
        Platform::SidecarGuides::ReleaseCom(g_depthUav);
        Platform::SidecarGuides::ReleaseCom(g_depthSrv);
        g_depthSrvOwner = nullptr;
        Platform::D3D12Sidecar::ReleaseShared(g_sharedDepth);
        Platform::D3D12Sidecar::ReleaseShared(g_sharedMotion);
        g_captureW = 0;
        g_captureH = 0;
        g_captureDevice = nullptr;
        Platform::SidecarGuides::ReleaseCom(g_depthCs);
    }

    [[nodiscard]] bool EnsureCapture(ID3D11Device* a_device, ID3D11Resource* a_depth11, ID3D11Resource* a_motion11,
        std::uint32_t a_w, std::uint32_t a_h) noexcept
    {
        if (a_device == nullptr || a_depth11 == nullptr || a_motion11 == nullptr || a_w == 0U || a_h == 0U) {
            return false;
        }
        if (g_captureW != a_w || g_captureH != a_h || g_captureDevice != a_device) {
            if (g_captureW != 0U && !Platform::D3D12Sidecar::DrainGpu()) {
                return false;
            }
            ReleaseCaptureResources();
            g_lastPrepareFence = 0U;
            g_captureDevice = a_device;
            D3D11_TEXTURE2D_DESC mv{};
            {
                ID3D11Texture2D* tex = nullptr;
                if (FAILED(a_motion11->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex))) ||
                    tex == nullptr) {
                    return false;
                }
                tex->GetDesc(&mv);
                tex->Release();
            }
            if (!Platform::D3D12Sidecar::CreateShared(g_sharedDepth, "fsr-fg depth", a_w, a_h, DXGI_FORMAT_R32_FLOAT, true) ||
                !Platform::D3D12Sidecar::CreateShared(g_sharedMotion, "fsr-fg motion", mv.Width, mv.Height, mv.Format, false)) {
                ReleaseCaptureResources();
                return false;
            }
            g_captureW = a_w;
            g_captureH = a_h;
        }
        if (g_depthCs == nullptr) {
            if (FAILED(a_device->CreateComputeShader(g_csNeuralDepth, sizeof(g_csNeuralDepth), nullptr, &g_depthCs))) {
                g_depthCs = nullptr;
                return false;
            }
        }
        if (g_depthUav == nullptr && !Platform::SidecarGuides::CreateUav(a_device, g_sharedDepth.d3d11, DXGI_FORMAT_R32_FLOAT,
                                          g_depthUav)) {
            return false;
        }
        if (g_depthSrv == nullptr || g_depthSrvOwner != a_depth11) {
            Platform::SidecarGuides::ReleaseCom(g_depthSrv);
            g_depthSrvOwner = nullptr;
            ID3D11Texture2D* tex = nullptr;
            if (FAILED(a_depth11->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex))) ||
                tex == nullptr) {
                return false;
            }
            D3D11_TEXTURE2D_DESC dd{};
            tex->GetDesc(&dd);
            D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Format = Platform::SidecarGuides::DepthViewFormat(dd.Format);
            sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            sd.Texture2D.MipLevels = 1;
            const HRESULT hr = a_device->CreateShaderResourceView(tex, &sd, &g_depthSrv);
            tex->Release();
            if (FAILED(hr)) {
                g_depthSrv = nullptr;
                return false;
            }
            g_depthSrvOwner = a_depth11;
        }
        return true;
    }

    void Note(const char* a_text) noexcept
    {
        std::snprintf(g_reason, sizeof(g_reason), "%s", a_text);
    }

    [[nodiscard]] HMODULE LoadFromFfxFolder(const wchar_t* a_leaf) noexcept
    {
        static const int s_anchor = 0;
        HMODULE self = nullptr;
        if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&s_anchor), &self) ||
            self == nullptr) {
            return nullptr;
        }
        wchar_t path[MAX_PATH]{};
        const DWORD length = ::GetModuleFileNameW(self, path, MAX_PATH);
        if (length == 0 || length >= MAX_PATH) {
            return nullptr;
        }
        wchar_t* const slash = std::wcsrchr(path, L'\\');
        if (slash == nullptr) {
            return nullptr;
        }
        slash[1] = L'\0';
        if (::wcscat_s(path, L"FO4GraphicsOverhaul\\FidelityFX\\") != 0 || ::wcscat_s(path, a_leaf) != 0) {
            return nullptr;
        }
        return ::LoadLibraryW(path);
    }

    void PushPacing() noexcept
    {
        if (g_swapChainContext == nullptr || g_ffx.Configure == nullptr) {
            return;
        }
        FfxApiSwapchainFramePacingTuning tuning{};
        tuning.safetyMarginInMs = g_safetyMarginMs.load(std::memory_order_relaxed);
        tuning.varianceFactor = g_varianceFactor.load(std::memory_order_relaxed);
        tuning.allowHybridSpin = g_hybridSpin.load(std::memory_order_relaxed);
        tuning.hybridSpinTime = g_hybridSpinTime.load(std::memory_order_relaxed);
        tuning.allowWaitForSingleObjectOnFence = g_waitOnFence.load(std::memory_order_relaxed);

        ffxConfigureDescFrameGenerationSwapChainKeyValueDX12 kv{};
        kv.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_KEYVALUE_DX12;
        kv.key = FFX_API_CONFIGURE_FG_SWAPCHAIN_KEY_FRAMEPACINGTUNING;
        kv.ptr = &tuning;
        (void)g_ffx.Configure(&g_swapChainContext, &kv.header);
    }
}

namespace Platform::FsrFrameGen
{
    [[nodiscard]] bool Prepare(void* a_cmdList, void* a_depth12, void* a_motion12, std::uint32_t a_renderW,
        std::uint32_t a_renderH, float a_jitterX, float a_jitterY, float a_frameDeltaMs) noexcept;

    void Probe() noexcept
    {
        if (g_probed.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        GUARD_BEGIN
        g_effect = LoadFromFfxFolder(L"amd_fidelityfx_framegeneration_dx12.dll");
        g_loader = LoadFromFfxFolder(L"amd_fidelityfx_loader_dx12.dll");
        if (g_effect == nullptr || g_loader == nullptr) {
            Note("the FidelityFX runtime is missing from FO4GraphicsOverhaul\\FidelityFX\\");
            logger::info("[FSR-FG] unavailable: {} — FSR frame generation cannot be selected this session",
                g_reason);
            return;
        }
        ffxLoadFunctions(&g_ffx, g_loader);
        if (g_ffx.CreateContext == nullptr || g_ffx.DestroyContext == nullptr || g_ffx.Configure == nullptr ||
            g_ffx.Dispatch == nullptr) {
            Note("the FidelityFX loader exports are incomplete");
            logger::warn("[FSR-FG] unavailable: {}", g_reason);
            return;
        }
        g_available.store(true, std::memory_order_release);
        logger::info("[FSR-FG] runtime loaded (amd_fidelityfx_loader_dx12.dll); AMD's chain will present this "
                     "session and FSR frame generation can be switched on and off live");
        GUARD_END("fsrframegen.probe")
    }

    bool Available() noexcept
    {
        return g_available.load(std::memory_order_acquire);
    }

    bool Selected() noexcept
    {
        return g_selected.load(std::memory_order_relaxed);
    }

    void SetSelected(bool a_selected) noexcept
    {
        const bool was = g_selected.exchange(a_selected, std::memory_order_acq_rel);
        if (was == a_selected) {
            return;
        }
        GUARD_BEGIN
        if (a_selected) {
            if (g_runtimeFailed.exchange(false, std::memory_order_relaxed)) {
                logger::info("[FSR-FG] re-armed by a fresh selection (the dispatch-failure latch is cleared)");
            }
            g_consecutiveDispatchFails.store(0, std::memory_order_relaxed);
            g_lastEpoch = ~static_cast<std::uint64_t>(0);
        } else {
            g_releasePending.store(true, std::memory_order_release);
        }
        GUARD_END("fsrframegen.setselected")
    }

    void ReArmAfterEnable() noexcept
    {
        if (!Selected()) {
            return;
        }
        GUARD_BEGIN
        if (g_runtimeFailed.exchange(false, std::memory_order_relaxed)) {
            logger::info("[FSR-FG] re-armed by the frame-generation toggle (the dispatch-failure latch is cleared)");
        }
        g_consecutiveDispatchFails.store(0, std::memory_order_relaxed);
        g_lastEpoch = ~static_cast<std::uint64_t>(0);
        GUARD_END("fsrframegen.rearmafterenable")
    }

    void RetirePendingRelease() noexcept
    {
        if (!g_releasePending.load(std::memory_order_acquire)) {
            return;
        }
        GUARD_BEGIN
        if (g_captureW == 0U || Platform::D3D12Sidecar::DrainGpu()) {
            ReleaseCaptureResources();
            g_lastPrepareFence = 0U;
            g_releasePending.store(false, std::memory_order_release);
        }
        GUARD_END("fsrframegen.retirepending")
    }

    void OnChainResized() noexcept
    {
        GUARD_BEGIN
        if (g_fgContext != nullptr && g_ffx.DestroyContext != nullptr) {
            (void)g_ffx.DestroyContext(&g_fgContext, nullptr);
            logger::info("[FSR-FG] generation context destroyed for the resize; re-created at the new size on the next frame");
        }
        g_fgContext = nullptr;
        ReleaseCaptureResources();
        g_lastPrepareFence = 0U;
        g_preparedLastFrame = false;
        g_lastEpoch = ~static_cast<std::uint64_t>(0);
        GUARD_END("fsrframegen.onchainresized")
    }

    void WaitForPresents() noexcept
    {
        if (g_swapChainContext == nullptr || g_ffx.Dispatch == nullptr) {
            return;
        }
        GUARD_BEGIN
        ffxDispatchDescFrameGenerationSwapChainWaitForPresentsDX12 wait{};
        wait.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_WAIT_FOR_PRESENTS_DX12;
        (void)g_ffx.Dispatch(&g_swapChainContext, &wait.header);
        GUARD_END("fsrframegen.waitforpresents")
    }

    bool CreateSwapChain(IDXGIFactory* a_factory, ID3D12CommandQueue* a_queue, void* a_hwnd,
        const DXGI_SWAP_CHAIN_DESC1* a_desc, IDXGISwapChain4** a_outChain) noexcept
    {
        if (a_outChain == nullptr) {
            return false;
        }
        *a_outChain = nullptr;
        if (!Available() || a_factory == nullptr || a_queue == nullptr || a_hwnd == nullptr || a_desc == nullptr) {
            return false;
        }
        bool created = false;
        GUARD_BEGIN
        DXGI_SWAP_CHAIN_DESC1 desc = *a_desc;
        ffxCreateContextDescFrameGenerationSwapChainForHwndDX12 chainDesc{};
        chainDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_FOR_HWND_DX12;
        chainDesc.swapchain = &g_chain;
        chainDesc.hwnd = static_cast<HWND>(a_hwnd);
        chainDesc.desc = &desc;
        chainDesc.fullscreenDesc = nullptr;
        chainDesc.dxgiFactory = a_factory;
        chainDesc.gameQueue = a_queue;

        const ffxReturnCode_t rc = g_ffx.CreateContext(&g_swapChainContext, &chainDesc.header, nullptr);
        if (rc != FFX_API_RETURN_OK || g_chain == nullptr) {
            std::snprintf(g_reason, sizeof(g_reason), "ffxCreateContext(FG swap chain) returned %d", static_cast<int>(rc));
            logger::error("[FSR-FG] {} — the session falls back to the normal DirectX 12 chain", g_reason);
            g_swapChainContext = nullptr;
            g_chain = nullptr;
        } else {
            PushPacing();
            *a_outChain = g_chain;
            created = true;
            logger::info("[FSR-FG] AMD's frame-generation swap chain created ({}x{} fmt={}); it owns presentation "
                         "and paces the generated frames itself",
                desc.Width, desc.Height, static_cast<unsigned>(desc.Format));
        }
        GUARD_END("fsrframegen.createswapchain")
        return created;
    }

    bool EnsureContext(void* a_device12, std::uint32_t a_displayW, std::uint32_t a_displayH,
        std::uint32_t a_backBufferFormat) noexcept
    {
        if (g_fgContext != nullptr) {
            return true;
        }
        if (!Available() || a_device12 == nullptr || a_displayW == 0U || a_displayH == 0U) {
            return false;
        }
        bool ok = false;
        GUARD_BEGIN
        ffxCreateContextDescFrameGeneration fg{};
        fg.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
        fg.flags = 0;
        fg.displaySize = { a_displayW, a_displayH };
        fg.maxRenderSize = { a_displayW, a_displayH };
        fg.backBufferFormat = ffxApiGetSurfaceFormatDX12(static_cast<DXGI_FORMAT>(a_backBufferFormat));

        ffxCreateBackendDX12Desc backend{};
        backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
        backend.device = static_cast<ID3D12Device*>(a_device12);
        fg.header.pNext = &backend.header;

        const ffxReturnCode_t rc = g_ffx.CreateContext(&g_fgContext, &fg.header, nullptr);
        if (rc != FFX_API_RETURN_OK) {
            g_fgContext = nullptr;
            std::snprintf(g_reason, sizeof(g_reason), "the frame-generation context refused creation (ffx rc=%d)",
                static_cast<int>(rc));
            logger::error("[FSR-FG] {} — the session presents real frames only", g_reason);
        } else {
            ok = true;
            logger::info("[FSR-FG] frame-generation context created ({}x{}, backbuffer format {}); interpolation "
                         "is possible from the next prepared frame",
                a_displayW, a_displayH, a_backBufferFormat);
        }
        GUARD_END("fsrframegen.ensurecontext")
        return ok;
    }

    bool Prepare(void* a_cmdList, void* a_depth12, void* a_motion12, std::uint32_t a_renderW,
        std::uint32_t a_renderH, float a_jitterX, float a_jitterY, float a_frameDeltaMs) noexcept
    {
        if (g_fgContext == nullptr || a_cmdList == nullptr || a_depth12 == nullptr || a_motion12 == nullptr ||
            a_renderW == 0U || a_renderH == 0U || g_runtimeFailed.load(std::memory_order_relaxed)) {
            return false;
        }
        bool prepared = false;
        GUARD_BEGIN
        const CameraConstants cam = Fallout4Renderer::CameraSnapshot();
        ffxDispatchDescFrameGenerationPrepareV2 prep{};
        prep.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
        prep.frameID = g_frameId.fetch_add(1, std::memory_order_relaxed) + 1U;
        prep.flags = 0;
        prep.commandList = a_cmdList;
        prep.renderSize = { a_renderW, a_renderH };
        prep.jitterOffset = { -a_jitterX, -a_jitterY };
        prep.motionVectorScale = { static_cast<float>(a_renderW), static_cast<float>(a_renderH) };
        prep.frameTimeDelta = a_frameDeltaMs;
        const std::uint64_t epoch = Fallout4Renderer::HistoryEpoch();
        prep.reset = (epoch != g_lastEpoch) || !g_preparedLastFrame;
        g_lastEpoch = epoch;
        prep.cameraNear = cam.nearPlane;
        prep.cameraFar = cam.farPlane;
        prep.cameraFovAngleVertical = cam.fovVertical;
        prep.viewSpaceToMetersFactor = 0.01428222656F;
        prep.depth = ffxApiGetResourceDX12(static_cast<ID3D12Resource*>(a_depth12));
        prep.motionVectors = ffxApiGetResourceDX12(static_cast<ID3D12Resource*>(a_motion12));
        std::memcpy(prep.cameraPosition, cam.eyePosition, sizeof(prep.cameraPosition));
        std::memcpy(prep.cameraUp, cam.cameraUp, sizeof(prep.cameraUp));
        std::memcpy(prep.cameraRight, cam.cameraRight, sizeof(prep.cameraRight));
        std::memcpy(prep.cameraForward, cam.cameraForward, sizeof(prep.cameraForward));

        const ffxReturnCode_t rc = g_ffx.Dispatch(&g_fgContext, &prep.header);
        if (rc != FFX_API_RETURN_OK) {
            static std::atomic<std::uint32_t> logs{ 0 };
            if (logs.fetch_add(1, std::memory_order_relaxed) < 3) {
                logger::error("[FSR-FG] prepare dispatch failed (ffx rc={}) — no frame is generated from it",
                    static_cast<int>(rc));
            }
        } else {
            prepared = true;
            g_preparedLastFrame = true;
            const std::uint64_t n = g_prepared.fetch_add(1, std::memory_order_relaxed) + 1U;
            if (n == 1U) {
                logger::info("[FSR-FG] ★ first prepared frame: depth + motion vectors + camera are live at "
                             "{}x{}",
                    a_renderW, a_renderH);
            }
        }
        GUARD_END("fsrframegen.prepare")
        return prepared;
    }

    void ConfigureFrame(void* a_swapChain, bool a_enabled, void* a_hudless12) noexcept
    {
        if (g_fgContext == nullptr || a_swapChain == nullptr) {
            return;
        }
        GUARD_BEGIN
        const bool on = a_enabled && !g_runtimeFailed.load(std::memory_order_relaxed);
        ffxConfigureDescFrameGeneration cfg{};
        cfg.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
        cfg.swapChain = a_swapChain;
        cfg.presentCallback = nullptr;
        cfg.presentCallbackUserContext = nullptr;
        cfg.frameGenerationCallback = &FrameGenerationDispatch;
        cfg.frameGenerationCallbackUserContext = nullptr;
        cfg.frameGenerationEnabled = on;
        cfg.allowAsyncWorkloads = false;
        if (a_hudless12 != nullptr) {
            cfg.HUDLessColor = ffxApiGetResourceDX12(static_cast<ID3D12Resource*>(a_hudless12));
        }
        cfg.flags = 0;
        cfg.onlyPresentGenerated = false;
        cfg.generationRect = {};
        cfg.frameID = g_frameId.load(std::memory_order_relaxed);

        const ffxReturnCode_t rc = g_ffx.Configure(&g_fgContext, &cfg.header);
        if (rc != FFX_API_RETURN_OK) {
            g_configureFails.fetch_add(1, std::memory_order_relaxed);
            static std::atomic<std::uint32_t> logs{ 0 };
            if (logs.fetch_add(1, std::memory_order_relaxed) < 3) {
                logger::error("[FSR-FG] configure failed (ffx rc={})", static_cast<int>(rc));
            }
        } else if (on) {
            static bool s_loggedOn = false;
            static int s_hudlessState = -1;
            static ULONGLONG s_hudlessStateLog = 0;
            static std::uint32_t s_hudlessUnlogged = 0;
            const int state = a_hudless12 != nullptr ? 1 : 0;
            if (!s_loggedOn) {
                s_loggedOn = true;
                s_hudlessState = state;
                logger::info("[FSR-FG] ★ interpolation ON (HUD-less {})", state == 1 ? "supplied" : "not supplied this frame");
            } else if (state != s_hudlessState) {
                s_hudlessState = state;
                const ULONGLONG now = ::GetTickCount64();
                if (now - s_hudlessStateLog >= 10000ULL) {
                    logger::info("[FSR-FG] HUD-less {}{}",
                        state == 1 ? "supplied: generated frames interpolate the scene without the HUD, and AMD recomposes the HUD onto them"
                                   : "NOT supplied: generated frames interpolate the composited frame, HUD included, until it returns",
                        s_hudlessUnlogged > 0U ? " (and earlier changes within 10 s went unlogged)" : "");
                    s_hudlessStateLog = now;
                    s_hudlessUnlogged = 0;
                } else {
                    ++s_hudlessUnlogged;
                }
            }
        }
        GUARD_END("fsrframegen.configure")
    }

    void CaptureAndPrepare(void* a_context11, void* a_depth11, void* a_motion11, std::uint32_t a_renderW,
        std::uint32_t a_renderH, float a_jitterX, float a_jitterY) noexcept
    {
        const float a_frameDeltaMs = []() noexcept {
            static LARGE_INTEGER freq{};
            static LARGE_INTEGER last{};
            if (freq.QuadPart == 0) {
                ::QueryPerformanceFrequency(&freq);
                ::QueryPerformanceCounter(&last);
                return 16.6F;
            }
            LARGE_INTEGER now{};
            ::QueryPerformanceCounter(&now);
            const double ms = static_cast<double>(now.QuadPart - last.QuadPart) * 1000.0 /
                              static_cast<double>(freq.QuadPart);
            last = now;
            return static_cast<float>(ms < 0.1 ? 0.1 : (ms > 200.0 ? 200.0 : ms));
        }();
        if (g_fgContext == nullptr || !Selected() || a_context11 == nullptr || a_depth11 == nullptr ||
            a_motion11 == nullptr || g_runtimeFailed.load(std::memory_order_relaxed) || !D3D12Sidecar::IsOpen() ||
            D3D12Sidecar::Disabled()) {
            g_preparedLastFrame = false;
            return;
        }
        GUARD_BEGIN
        auto* const ctx = static_cast<ID3D11DeviceContext*>(a_context11);
        auto* const depth11 = static_cast<ID3D11Resource*>(a_depth11);
        auto* const motion11 = static_cast<ID3D11Resource*>(a_motion11);
        ID3D11Device* device = nullptr;
        ctx->GetDevice(&device);
        if (device == nullptr) {
            g_preparedLastFrame = false;
            return;
        }
        const bool ready = EnsureCapture(device, depth11, motion11, a_renderW, a_renderH);
        device->Release();
        if (!ready) {
            g_preparedLastFrame = false;
            return;
        }

        if (g_lastPrepareFence != 0U) {
            (void)Platform::D3D12Sidecar::WaitOnD3D11(g_lastPrepareFence);
        }

        {
            const SidecarGuides::ComputeBindingsScope csScope(ctx);
            ctx->CSSetShader(g_depthCs, nullptr, 0);
            ctx->CSSetShaderResources(0, 1, &g_depthSrv);
            ctx->CSSetUnorderedAccessViews(0, 1, &g_depthUav, nullptr);
            ctx->Dispatch((a_renderW + 7) / 8, (a_renderH + 7) / 8, 1);
        }
        ctx->CopyResource(g_sharedMotion.d3d11, motion11);

        const std::uint64_t vIn = D3D12Sidecar::SignalFromD3D11();
        D3D12Sidecar::WaitOnD3D12(vIn);
        ID3D12GraphicsCommandList* list = nullptr;
        if (!D3D12Sidecar::BeginCommands(list) || list == nullptr) {
            g_preparedLastFrame = false;
            return;
        }
        const bool prepared = Prepare(list, g_sharedDepth.d3d12, g_sharedMotion.d3d12, a_renderW, a_renderH,
            a_jitterX, a_jitterY, a_frameDeltaMs);
        if (prepared) {
            bool accepted = false;
            const std::uint64_t retire = D3D12Sidecar::EndCommands(&accepted);
            if (retire != 0U) {
                g_lastPrepareFence = retire;
            }
        } else {
            (void)D3D12Sidecar::AbandonCommands();
        }
        GUARD_END("fsrframegen.captureandprepare")
    }

    bool ContextReady() noexcept
    {
        return g_fgContext != nullptr;
    }

    bool Engaged() noexcept
    {
        return Selected() && g_fgContext != nullptr;
    }

    std::uint64_t Generated() noexcept
    {
        return g_generated.load(std::memory_order_relaxed);
    }

    void Abandon() noexcept
    {
        GUARD_BEGIN
        g_depthUav = nullptr;
        g_depthSrv = nullptr;
        g_depthSrvOwner = nullptr;
        g_depthCs = nullptr;
        g_sharedDepth = Platform::D3D12Sidecar::SharedTexture{};
        g_sharedMotion = Platform::D3D12Sidecar::SharedTexture{};
        g_captureW = g_captureH = 0;
        g_captureDevice = nullptr;
        g_lastPrepareFence = 0;
        g_releasePending.store(false, std::memory_order_relaxed);
        g_swapChainContext = nullptr;
        g_fgContext = nullptr;
        g_chain = nullptr;
        logger::warn("[FSR-FG] contexts and capture resources retained until process exit: GPU retirement is unproved");
        GUARD_END("fsrframegen.abandon")
    }

    void Release() noexcept
    {
        GUARD_BEGIN
        if (g_fgContext != nullptr && g_ffx.DestroyContext != nullptr) {
            (void)g_ffx.DestroyContext(&g_fgContext, nullptr);
        }
        g_fgContext = nullptr;
        ReleaseCaptureResources();
        SidecarGuides::ReleaseCom(g_depthCs);
        if (g_swapChainContext != nullptr && g_ffx.DestroyContext != nullptr) {
            (void)g_ffx.DestroyContext(&g_swapChainContext, nullptr);
        }
        g_swapChainContext = nullptr;
        g_chain = nullptr;
        GUARD_END("fsrframegen.release")
    }

    void SetPacingSafetyMarginMs(float a_ms) noexcept
    {
        g_safetyMarginMs.store(a_ms, std::memory_order_relaxed);
        PushPacing();
    }

    void SetPacingVarianceFactor(float a_factor) noexcept
    {
        g_varianceFactor.store(a_factor, std::memory_order_relaxed);
        PushPacing();
    }

    void SetPacingHybridSpin(bool a_enabled) noexcept
    {
        g_hybridSpin.store(a_enabled, std::memory_order_relaxed);
        PushPacing();
    }

    void SetPacingHybridSpinTime(std::uint32_t a_units) noexcept
    {
        g_hybridSpinTime.store(a_units < 2U ? 2U : a_units, std::memory_order_relaxed);
        PushPacing();
    }

    void SetPacingWaitOnFence(bool a_enabled) noexcept
    {
        g_waitOnFence.store(a_enabled, std::memory_order_relaxed);
        PushPacing();
    }

    State Snapshot() noexcept
    {
        State state{};
        state.available = g_available.load(std::memory_order_relaxed);
        state.chainCreated = g_chain != nullptr;
        state.contextReady = g_fgContext != nullptr;
        state.latched = g_runtimeFailed.load(std::memory_order_relaxed);
        state.generated = g_generated.load(std::memory_order_relaxed);
        std::snprintf(state.reason, sizeof(state.reason), "%s", g_reason);
        return state;
    }
}
