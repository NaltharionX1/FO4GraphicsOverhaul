#include "PCH.h"

#include "Platform/PresentProxy.h"
#include "Platform/AdaptiveSync.h"
#include "Platform/Reflex.h"

#include "Platform/D3D12Sidecar.h"
#include "Platform/PresentPolicy.h"
#include "Platform/DynamicMultiplier.h"
#include "Platform/FsrFrameGen.h"
#include "Platform/FrameGenEngine.h"
#include "Platform/FrameFingerprint.h"
#include "Platform/Streamline.h"

#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

namespace
{
    using namespace Platform;

    constexpr int kRingSlots = 4;
    constexpr UINT kMaxFrameLatency = 2;
    constexpr std::uint32_t kTimingWindow = 600;
    constexpr int kThreadAllocators = 2;
    constexpr std::size_t kMaxPacketsInFlight = 2;

    template <class T>
    void ReleaseCom(T*& a_ptr) noexcept
    {
        if (a_ptr != nullptr) {
            a_ptr->Release();
            a_ptr = nullptr;
        }
    }

    [[nodiscard]] LONGLONG Qpc() noexcept
    {
        LARGE_INTEGER v{};
        ::QueryPerformanceCounter(&v);
        return v.QuadPart;
    }

    struct ProxyState
    {
        std::atomic<bool> active{ false };
        std::atomic<bool> d3d11Fallback{ false };
        std::atomic<bool> fullscreenIgnored{ false };
        char reason[160]{};
        std::mutex reasonMutex;
        std::mutex takeOverMutex;

        std::atomic<std::uint32_t> width{ 0 }, height{ 0 }, bufferCount{ 0 }, resizes{ 0 };
        std::atomic<bool> tearing{ false };
        std::atomic<std::uint64_t> presents{ 0 };
        std::atomic<std::uint64_t> interpolated{ 0 };
        std::atomic<float> cpuMsAvg{ 0.0F };
        std::atomic<float> intervalMs{ 0.0F };
        std::atomic<float> cadenceLongestMs{ 0.0F };
        std::atomic<std::uint32_t> cadencePresents{ 0 };
        std::atomic<std::uint32_t> cadenceOverFloorWindow{ 0 };
        std::atomic<std::uint64_t> cadenceOverFloorLifetime{ 0 };
        std::atomic<float> cadenceGapSumMs{ 0.0F };
        std::atomic<float> cadenceStepSumMs{ 0.0F };
        std::atomic<std::uint32_t> cadenceSteps{ 0 };
    };
    ProxyState g;

    void SetReason(const char* a_reason) noexcept
    {
        try {
            const std::scoped_lock lock(g.reasonMutex);
            std::snprintf(g.reason, sizeof(g.reason), "%s", a_reason != nullptr ? a_reason : "unknown");
        } catch (...) {
        }
    }

    [[nodiscard]] IDXGIFactory2* FactoryOf(ID3D11Device* a_device) noexcept
    {
        IDXGIFactory2* factory = nullptr;
        IDXGIDevice* dxgiDevice = nullptr;
        IDXGIAdapter* adapter = nullptr;
        if (SUCCEEDED(a_device->QueryInterface(IID_PPV_ARGS(&dxgiDevice))) && dxgiDevice != nullptr) {
            dxgiDevice->GetAdapter(&adapter);
            dxgiDevice->Release();
        }
        if (adapter != nullptr) {
            adapter->GetParent(IID_PPV_ARGS(&factory));
            adapter->Release();
        }
        return factory;
    }

    [[nodiscard]] float QueryDisplayRefreshHz(HWND a_hwnd) noexcept
    {
        if (a_hwnd == nullptr) {
            return 0.0F;
        }
        const HMONITOR monitor = ::MonitorFromWindow(a_hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFOEXW info{};
        info.cbSize = sizeof(info);
        if (monitor == nullptr || ::GetMonitorInfoW(monitor, &info) == 0) {
            return 0.0F;
        }
        DEVMODEW mode{};
        mode.dmSize = sizeof(mode);
        if (::EnumDisplaySettingsExW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode, 0) == 0) {
            return 0.0F;
        }
        return mode.dmDisplayFrequency > 1 ? static_cast<float>(mode.dmDisplayFrequency) : 0.0F;
    }

    struct Packet
    {
        int slot{ 0 };
        std::uint64_t fence{ 0 };
        ID3D12Resource* outputs[FrameGenEngine::kMaxGeneratedFrames]{};
        std::uint32_t count{ 0 };
        UINT syncInterval{ 0 };
        UINT flags{ 0 };
        LONGLONG arrival{ 0 };
        LONGLONG blocked{ 0 };
    };

    enum class WorkerCopyFault : std::uint32_t
    {
        kNone = 0,
        kBegin,
        kCopyTarget,
        kSubmission,
        kDrain,
        kRecorderUnavailable,
        kPacketException
    };

    [[nodiscard]] const char* WorkerFaultReason(WorkerCopyFault a_fault) noexcept
    {
        switch (a_fault) {
        case WorkerCopyFault::kBegin: return "the present worker could not begin command recording";
        case WorkerCopyFault::kCopyTarget: return "the present worker had no valid source/current backbuffer for its copy";
        case WorkerCopyFault::kSubmission: return "the present worker's copy list was rejected or could not be retired";
        case WorkerCopyFault::kDrain: return "the inline path could not take ownership from the present worker within 2 s";
        case WorkerCopyFault::kRecorderUnavailable: return "the present worker recorder is unavailable";
        case WorkerCopyFault::kPacketException: return "the present worker raised a C++ exception while processing a packet";
        default: return "unknown present-worker failure";
        }
    }

    class SwapChainProxy final : public IDXGISwapChain4
    {
    public:
        SwapChainProxy() = default;
        SwapChainProxy(const SwapChainProxy&) = delete;
        SwapChainProxy& operator=(const SwapChainProxy&) = delete;

        [[nodiscard]] bool Build(ID3D11Device* a_device, ID3D11DeviceContext* a_context,
            const DXGI_SWAP_CHAIN_DESC& a_policyDesc) noexcept;

        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID a_riid, void** a_out) override
        {
            if (a_out == nullptr) {
                return E_POINTER;
            }
            if (a_riid == __uuidof(IUnknown) || a_riid == __uuidof(IDXGIObject) ||
                a_riid == __uuidof(IDXGIDeviceSubObject) || a_riid == __uuidof(IDXGISwapChain) ||
                a_riid == __uuidof(IDXGISwapChain1) || a_riid == __uuidof(IDXGISwapChain2) ||
                a_riid == __uuidof(IDXGISwapChain3) || a_riid == __uuidof(IDXGISwapChain4)) {
                *a_out = static_cast<IDXGISwapChain4*>(this);
                AddRef();
                return S_OK;
            }
            *a_out = nullptr;
            return E_NOINTERFACE;
        }
        ULONG STDMETHODCALLTYPE AddRef() override { return refs_.fetch_add(1, std::memory_order_acq_rel) + 1; }
        ULONG STDMETHODCALLTYPE Release() override
        {
            const ULONG left = refs_.fetch_sub(1, std::memory_order_acq_rel) - 1;
            if (left == 0) {
                delete this;
            }
            return left;
        }

        HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID a_name, UINT a_size, const void* a_data) override
        {
            return real_ ? real_->SetPrivateData(a_name, a_size, a_data) : E_FAIL;
        }
        HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID a_name, const IUnknown* a_unknown) override
        {
            return real_ ? real_->SetPrivateDataInterface(a_name, a_unknown) : E_FAIL;
        }
        HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID a_name, UINT* a_size, void* a_data) override
        {
            return real_ ? real_->GetPrivateData(a_name, a_size, a_data) : E_FAIL;
        }
        HRESULT STDMETHODCALLTYPE GetParent(REFIID a_riid, void** a_parent) override
        {
            return real_ ? real_->GetParent(a_riid, a_parent) : E_FAIL;
        }

        HRESULT STDMETHODCALLTYPE GetDevice(REFIID a_riid, void** a_device) override
        {
            if (a_device == nullptr) {
                return E_POINTER;
            }
            if (device11_ != nullptr && SUCCEEDED(device11_->QueryInterface(a_riid, a_device))) {
                return S_OK;
            }
            return real_ ? real_->GetDevice(a_riid, a_device) : E_NOINTERFACE;
        }

        HRESULT STDMETHODCALLTYPE Present(UINT a_syncInterval, UINT a_flags) override
        {
            return ProxyPresent(a_syncInterval, a_flags);
        }
        HRESULT STDMETHODCALLTYPE GetBuffer(UINT, REFIID a_riid, void** a_surface) override
        {
            if (a_surface == nullptr) {
                return E_POINTER;
            }
            if (fake_ == nullptr) {
                *a_surface = nullptr;
                return DXGI_ERROR_INVALID_CALL;
            }
            return fake_->QueryInterface(a_riid, a_surface);
        }
        HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL a_fullscreen, IDXGIOutput*) override
        {
            if (a_fullscreen != FALSE && !g.fullscreenIgnored.exchange(true, std::memory_order_relaxed)) {
                logger::info("[Proxy] SetFullscreenState(TRUE) ABSORBED: the presenting chain is windowed by "
                             "policy and cannot serve exclusive fullscreen (AMD's frame-generation chain "
                             "carries ALLOW_TEARING). The game runs borderless fullscreen; nothing is lost but "
                             "the mode switch. The menu's proxy line says so too.");
            }
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE GetFullscreenState(BOOL* a_fullscreen, IDXGIOutput** a_target) override
        {
            return real_ ? real_->GetFullscreenState(a_fullscreen, a_target) : E_FAIL;
        }
        HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC* a_desc) override
        {
            return real_ ? real_->GetDesc(a_desc) : E_FAIL;
        }
        HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT a_count, UINT a_width, UINT a_height, DXGI_FORMAT a_format,
            UINT a_flags) override
        {
            return ProxyResize(a_count, a_width, a_height, a_format, a_flags);
        }
        HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC* a_mode) override
        {
            return real_ ? real_->ResizeTarget(a_mode) : E_FAIL;
        }
        HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput** a_output) override
        {
            return real_ ? real_->GetContainingOutput(a_output) : E_FAIL;
        }
        HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS* a_stats) override
        {
            return real_ ? real_->GetFrameStatistics(a_stats) : E_FAIL;
        }
        HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT* a_count) override
        {
            return real_ ? real_->GetLastPresentCount(a_count) : E_FAIL;
        }

        HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_SWAP_CHAIN_DESC1* a_desc) override
        {
            return real_ ? real_->GetDesc1(a_desc) : E_FAIL;
        }
        HRESULT STDMETHODCALLTYPE GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC* a_desc) override
        {
            return real_ ? real_->GetFullscreenDesc(a_desc) : E_FAIL;
        }
        HRESULT STDMETHODCALLTYPE GetHwnd(HWND* a_hwnd) override { return real_ ? real_->GetHwnd(a_hwnd) : E_FAIL; }
        HRESULT STDMETHODCALLTYPE GetCoreWindow(REFIID a_riid, void** a_unk) override
        {
            return real_ ? real_->GetCoreWindow(a_riid, a_unk) : E_FAIL;
        }
        HRESULT STDMETHODCALLTYPE Present1(UINT a_syncInterval, UINT a_flags, const DXGI_PRESENT_PARAMETERS*) override
        {
            return ProxyPresent(a_syncInterval, a_flags);
        }
        BOOL STDMETHODCALLTYPE IsTemporaryMonoSupported() override
        {
            return real_ ? real_->IsTemporaryMonoSupported() : FALSE;
        }
        HRESULT STDMETHODCALLTYPE GetRestrictToOutput(IDXGIOutput** a_output) override
        {
            return real_ ? real_->GetRestrictToOutput(a_output) : E_FAIL;
        }
        HRESULT STDMETHODCALLTYPE SetBackgroundColor(const DXGI_RGBA* a_color) override
        {
            return real_ ? real_->SetBackgroundColor(a_color) : E_FAIL;
        }
        HRESULT STDMETHODCALLTYPE GetBackgroundColor(DXGI_RGBA* a_color) override
        {
            return real_ ? real_->GetBackgroundColor(a_color) : E_FAIL;
        }
        HRESULT STDMETHODCALLTYPE SetRotation(DXGI_MODE_ROTATION a_rotation) override
        {
            return real_ ? real_->SetRotation(a_rotation) : E_FAIL;
        }
        HRESULT STDMETHODCALLTYPE GetRotation(DXGI_MODE_ROTATION* a_rotation) override
        {
            return real_ ? real_->GetRotation(a_rotation) : E_FAIL;
        }

        HRESULT STDMETHODCALLTYPE SetSourceSize(UINT a_width, UINT a_height) override
        {
            return real_ ? real_->SetSourceSize(a_width, a_height) : E_FAIL;
        }
        HRESULT STDMETHODCALLTYPE GetSourceSize(UINT* a_width, UINT* a_height) override
        {
            return real_ ? real_->GetSourceSize(a_width, a_height) : E_FAIL;
        }
        HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT a_latency) override
        {
            return real_ ? real_->SetMaximumFrameLatency(a_latency) : E_FAIL;
        }
        HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT* a_latency) override
        {
            return real_ ? real_->GetMaximumFrameLatency(a_latency) : E_FAIL;
        }
        HANDLE STDMETHODCALLTYPE GetFrameLatencyWaitableObject() override
        {
            return real_ ? real_->GetFrameLatencyWaitableObject() : nullptr;
        }
        HRESULT STDMETHODCALLTYPE SetMatrixTransform(const DXGI_MATRIX_3X2_F* a_matrix) override
        {
            return real_ ? real_->SetMatrixTransform(a_matrix) : E_FAIL;
        }
        HRESULT STDMETHODCALLTYPE GetMatrixTransform(DXGI_MATRIX_3X2_F* a_matrix) override
        {
            return real_ ? real_->GetMatrixTransform(a_matrix) : E_FAIL;
        }

        UINT STDMETHODCALLTYPE GetCurrentBackBufferIndex() override
        {
            return real_ ? real_->GetCurrentBackBufferIndex() : 0;
        }
        HRESULT STDMETHODCALLTYPE CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE a_space, UINT* a_support) override
        {
            return real_ ? real_->CheckColorSpaceSupport(a_space, a_support) : E_FAIL;
        }
        HRESULT STDMETHODCALLTYPE SetColorSpace1(DXGI_COLOR_SPACE_TYPE a_space) override
        {
            return real_ ? real_->SetColorSpace1(a_space) : E_FAIL;
        }
        HRESULT STDMETHODCALLTYPE ResizeBuffers1(UINT a_count, UINT a_width, UINT a_height, DXGI_FORMAT a_format,
            UINT a_flags, const UINT*, IUnknown* const*) override
        {
            return this->ResizeBuffers(a_count, a_width, a_height, a_format, a_flags);
        }

        HRESULT STDMETHODCALLTYPE SetHDRMetaData(DXGI_HDR_METADATA_TYPE a_type, UINT a_size, void* a_data) override
        {
            return real_ ? real_->SetHDRMetaData(a_type, a_size, a_data) : E_FAIL;
        }

    private:
        ~SwapChainProxy() { Teardown(); }

        [[nodiscard]] bool CreateFake(std::uint32_t a_width, std::uint32_t a_height, DXGI_FORMAT a_format) noexcept;
        [[nodiscard]] bool CreateD3D12Resources() noexcept;
        void ReleaseD3D12Resources() noexcept;
        void AbandonD3D12Resources() noexcept;
        void Teardown() noexcept;
        [[nodiscard]] bool SidecarUsable() noexcept;
        [[nodiscard]] bool HasTrustedSidecarTimeline() const noexcept;
        [[nodiscard]] bool ProveSidecarQueueRetired(unsigned long a_timeoutMs) noexcept;
        [[nodiscard]] bool SwitchToD3D11Fallback(const char* a_reason) noexcept;
        [[nodiscard]] HRESULT PresentViaD3D11(UINT a_syncInterval, UINT a_flags) noexcept;
        [[nodiscard]] HRESULT PresentViaD3D12(UINT a_syncInterval, UINT a_flags) noexcept;
        [[nodiscard]] HRESULT PresentReal(UINT a_syncInterval, UINT a_flags) noexcept;
        [[nodiscard]] HRESULT ProxyPresent(UINT a_syncInterval, UINT a_flags) noexcept;
        [[nodiscard]] HRESULT ProxyResize(UINT a_count, UINT a_width, UINT a_height, DXGI_FORMAT a_format, UINT a_flags) noexcept;
        void ReportTiming(LONGLONG a_ticks) noexcept;
        void NoteDelivered() noexcept;
        std::atomic<LONGLONG> lastDeliveredQpc_{ 0 };
        std::atomic<float> lastGapMs_{ -1.0F };
        void WaitLatency() noexcept;
        void NoteInterpolated() noexcept;

        [[nodiscard]] bool StartPresentThread() noexcept;
        void ReleaseChain(bool a_orderly) noexcept;
        void StopPresentThread() noexcept;
        [[nodiscard]] bool DrainPresentThread() noexcept;
        void MarkWorkerCopyFault(WorkerCopyFault a_fault) noexcept;
        [[nodiscard]] bool RecoverWorkerCopyFault() noexcept;
        ULONGLONG lastRebuildDeferredLog_{ 0 };
        HWND hwnd_{ nullptr };
        ULONGLONG lastRefreshQuery_{ 0 };
        [[nodiscard]] bool EnqueuePacket(Packet& a_packet) noexcept;
        void PresentThreadMain() noexcept;
        [[nodiscard]] bool ThreadBeginList() noexcept;
        [[nodiscard]] std::uint64_t ThreadEndList(bool* a_listAccepted = nullptr) noexcept;
        [[nodiscard]] bool ThreadCopyToBackbuffer(ID3D12Resource* a_source, D3D12_RESOURCE_STATES a_sourceState) noexcept;
        void PaceUntil(LONGLONG a_target) noexcept;

        std::atomic<ULONG> refs_{ 1 };
        ID3D11Device* device11_{ nullptr };
        ID3D11DeviceContext* context11_{ nullptr };
        ID3D12CommandQueue* sidecarQueue_{ nullptr };
        ID3D12Fence* sidecarFence_{ nullptr };
        IDXGISwapChain4* real_{ nullptr };
        bool d3d11Fallback_{ false };
        DXGI_SWAP_CHAIN_DESC policyDesc_{};
        HANDLE waitable_{ nullptr };
        UINT chainFlags_{ 0 };
        bool fsrFrameGenChain_{ false };
        bool lastFsrSelected_{ false };
        ULONGLONG lastSwitchDeferredLog_{ 0 };
        bool fsrHudlessPending_{ false };
        DXGI_FORMAT format_{ DXGI_FORMAT_UNKNOWN };
        std::uint32_t width_{ 0 }, height_{ 0 }, bufferCount_{ 0 };
        ID3D12Resource* backbuffers_[DXGI_MAX_SWAP_CHAIN_BUFFERS]{};
        ID3D11Texture2D* fake_{ nullptr };
        D3D12Sidecar::SharedTexture ring_[kRingSlots]{};
        std::atomic<std::uint64_t> ringReadFence_[kRingSlots]{};
        std::uint64_t presentCount_{ 0 };
        bool staleLogged_{ false };
        bool recoveryRequiresRestart_{ false };
        bool badIndexLogged_{ false };
        bool waitTimeoutLogged_{ false };
        LONGLONG qpf_{ 0 };
        LONGLONG ticks_{ 0 };
        std::uint32_t windowFrames_{ 0 };

        std::thread thread_;
        std::mutex queueMutex_;
        std::condition_variable queueCv_;
        std::condition_variable drainedCv_;
        std::deque<Packet> queue_;
        std::size_t inFlight_{ 0 };
        std::atomic<bool> stop_{ false };
        std::atomic<WorkerCopyFault> workerCopyFault_{ WorkerCopyFault::kNone };
        std::atomic<bool> workerSubmissionUnretired_{ false };
        ULONGLONG lastWaitTimeoutLog_{ 0 };
        ULONGLONG lastThreadListLog_{ 0 };
        ID3D12CommandAllocator* threadAllocators_[kThreadAllocators]{};
        std::uint64_t threadAllocatorFence_[kThreadAllocators]{};
        int threadSlot_{ 0 };
        ID3D12GraphicsCommandList* threadList_{ nullptr };
        HANDLE threadEvent_{ nullptr };
        LONGLONG lastArrival_{ 0 };
        LONGLONG prevBlocked_{ 0 };
        LONGLONG paceSleep_{ 0 };
        LONGLONG nextPresentAt_{ 0 };
        double intervalTicks_{ 0.0 };
        HANDLE paceTimer_{ nullptr };
        bool paceTimerLogged_{ false };
        bool threadLogged_{ false };
    };

    [[nodiscard]] HRESULT MarkedPresent(IDXGISwapChain* a_chain, UINT a_syncInterval, UINT a_flags) noexcept
    {
        Reflex::RenderSubmitEnd();
        Reflex::PresentStart();
        const HRESULT hr = a_chain->Present(a_syncInterval, a_flags);
        Reflex::PresentEnd();
        return hr;
    }

    bool SwapChainProxy::Build(ID3D11Device* a_device, ID3D11DeviceContext* a_context,
        const DXGI_SWAP_CHAIN_DESC& a_policyDesc) noexcept
    {
        try {
            device11_ = a_device;
            device11_->AddRef();
            context11_ = a_context;
            context11_->AddRef();
            policyDesc_ = a_policyDesc;

            if (!D3D12Sidecar::Open(a_device, a_context)) {
                SetReason(D3D12Sidecar::Disabled() ? D3D12Sidecar::DisableReason() : "the D3D12 sidecar did not open");
                return false;
            }
            sidecarQueue_ = D3D12Sidecar::Queue();
            sidecarFence_ = D3D12Sidecar::Fence12();
            if (sidecarQueue_ == nullptr || sidecarFence_ == nullptr) {
                SetReason("the D3D12 sidecar opened without a stable queue/fence identity");
                return false;
            }
            sidecarQueue_->AddRef();
            sidecarFence_->AddRef();
            IDXGIFactory2* factory = FactoryOf(a_device);
            if (factory == nullptr) {
                SetReason("the DXGI factory behind the game's adapter could not be reached");
                return false;
            }
            const bool flip = a_policyDesc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD ||
                              a_policyDesc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
            DXGI_SWAP_CHAIN_DESC1 desc1{};
            desc1.Width = a_policyDesc.BufferDesc.Width;
            desc1.Height = a_policyDesc.BufferDesc.Height;
            desc1.Format = a_policyDesc.BufferDesc.Format;
            desc1.SampleDesc.Count = 1;
            desc1.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            desc1.BufferCount = flip ? a_policyDesc.BufferCount : 3U;
            if (desc1.BufferCount < 2U) {
                desc1.BufferCount = 2U;
            }
            desc1.SwapEffect = flip ? a_policyDesc.SwapEffect : DXGI_SWAP_EFFECT_FLIP_DISCARD;
            desc1.Scaling = DXGI_SCALING_STRETCH;
            desc1.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
            desc1.Flags = a_policyDesc.Flags | DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
            const HWND hwnd = a_policyDesc.OutputWindow;
            hwnd_ = hwnd;

            const bool wantFsrFg = FsrFrameGen::Available();
            bool fsrChain = false;
            if (wantFsrFg) {
                fsrChain = FsrFrameGen::CreateSwapChain(factory, D3D12Sidecar::Queue(), hwnd, &desc1, &real_);
                if (!fsrChain) {
                    logger::warn("[Proxy] FSR frame generation asked for its own swap chain and was refused — "
                                 "this session presents through the normal DirectX 12 chain");
                }
            }
            if (!fsrChain) {
                IDXGISwapChain1* chain1 = nullptr;
                const HRESULT hr = factory->CreateSwapChainForHwnd(D3D12Sidecar::Queue(), hwnd, &desc1, nullptr, nullptr, &chain1);
                if (FAILED(hr) || chain1 == nullptr) {
                    char reason[160]{};
                    std::snprintf(reason, sizeof(reason), "CreateSwapChainForHwnd on the D3D12 queue failed 0x%08lx", static_cast<unsigned long>(hr));
                    SetReason(reason);
                    factory->Release();
                    return false;
                }
                factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
                factory->Release();
                const HRESULT hr4 = chain1->QueryInterface(IID_PPV_ARGS(&real_));
                chain1->Release();
                if (FAILED(hr4) || real_ == nullptr) {
                    SetReason("the D3D12 chain does not expose IDXGISwapChain4");
                    return false;
                }
            } else {
                factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
                factory->Release();
            }
            fsrFrameGenChain_ = fsrChain;
            if (fsrChain) {
                (void)FsrFrameGen::EnsureContext(D3D12Sidecar::Device(), desc1.Width, desc1.Height,
                    static_cast<std::uint32_t>(desc1.Format));
            }
            DXGI_SWAP_CHAIN_DESC1 built{};
            if (SUCCEEDED(real_->GetDesc1(&built)) &&
                (built.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) != 0U) {
                real_->SetMaximumFrameLatency(kMaxFrameLatency);
                waitable_ = real_->GetFrameLatencyWaitableObject();
            }
            chainFlags_ = desc1.Flags;
            DXGI_SWAP_CHAIN_DESC1 created{};
            real_->GetDesc1(&created);
            if (!CreateFake(created.Width, created.Height, created.Format) || !CreateD3D12Resources()) {
                return false;
            }
            if (!StartPresentThread()) {
                return false;
            }
            g.tearing.store((chainFlags_ & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) != 0U, std::memory_order_relaxed);
            lastDeliveredQpc_.store(0, std::memory_order_relaxed);
            AdaptiveSync::Observe(hwnd);
            logger::info("[Proxy] taking over the swap chain: {}x{} fmt={} buffers={} effect={} flags={:#x} -> the game presents to the proxy, the D3D12 chain presents to the window",
                created.Width, created.Height, static_cast<int>(created.Format), created.BufferCount,
                static_cast<int>(created.SwapEffect), created.Flags);
            return true;
        } catch (...) {
            SetReason("C++ exception building the present proxy");
            return false;
        }
    }

    bool SwapChainProxy::CreateFake(std::uint32_t a_width, std::uint32_t a_height, DXGI_FORMAT a_format) noexcept
    {
        ReleaseCom(fake_);
        width_ = a_width;
        height_ = a_height;
        format_ = a_format;
        D3D11_TEXTURE2D_DESC fake{};
        fake.Width = a_width;
        fake.Height = a_height;
        fake.MipLevels = 1;
        fake.ArraySize = 1;
        fake.Format = a_format;
        fake.SampleDesc.Count = 1;
        fake.Usage = D3D11_USAGE_DEFAULT;
        fake.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(device11_->CreateTexture2D(&fake, nullptr, &fake_)) || fake_ == nullptr) {
            fake_ = nullptr;
            SetReason("the fake backbuffer texture could not be created");
            return false;
        }
        g.width.store(a_width, std::memory_order_relaxed);
        g.height.store(a_height, std::memory_order_relaxed);
        return true;
    }

    bool SwapChainProxy::CreateD3D12Resources() noexcept
    {
        ReleaseD3D12Resources();
        bool ok = false;
        do {
            DXGI_SWAP_CHAIN_DESC1 desc{};
            if (FAILED(real_->GetDesc1(&desc))) {
                SetReason("the D3D12 chain refused GetDesc1");
                break;
            }
            bufferCount_ = desc.BufferCount;
            bool acquired = true;
            for (UINT i = 0; i < bufferCount_ && i < DXGI_MAX_SWAP_CHAIN_BUFFERS; ++i) {
                if (FAILED(real_->GetBuffer(i, IID_PPV_ARGS(&backbuffers_[i]))) || backbuffers_[i] == nullptr) {
                    acquired = false;
                    break;
                }
            }
            if (!acquired) {
                SetReason("a D3D12 backbuffer could not be acquired");
                break;
            }
            bool shared = true;
            for (int i = 0; i < kRingSlots; ++i) {
                char name[32]{};
                std::snprintf(name, sizeof(name), "present ring %d", i);
                if (!D3D12Sidecar::CreateShared(ring_[i], name, width_, height_, format_, false)) {
                    shared = false;
                    break;
                }
                ringReadFence_[i].store(0, std::memory_order_relaxed);
            }
            if (!shared) {
                SetReason("a shared present-ring texture could not be created (see the [Sidecar] lines)");
                break;
            }
            ok = true;
        } while (false);
        if (!ok) {
            ReleaseD3D12Resources();
            return false;
        }
        g.bufferCount.store(bufferCount_, std::memory_order_relaxed);
        return true;
    }

    void SwapChainProxy::ReleaseD3D12Resources() noexcept
    {
        for (auto& bb : backbuffers_) {
            ReleaseCom(bb);
        }
        for (auto& slot : ring_) {
            D3D12Sidecar::ReleaseShared(slot);
        }
        for (auto& fence : ringReadFence_) {
            fence.store(0, std::memory_order_relaxed);
        }
    }

    void SwapChainProxy::AbandonD3D12Resources() noexcept
    {
        bool owned = false;
        for (auto& bb : backbuffers_) {
            owned = owned || bb != nullptr;
            bb = nullptr;
        }
        for (auto& slot : ring_) {
            owned = owned || slot.d3d11 != nullptr || slot.d3d12 != nullptr || slot.handle != nullptr;
            slot = D3D12Sidecar::SharedTexture{};
        }
        for (auto& fence : ringReadFence_) {
            fence.store(0, std::memory_order_relaxed);
        }
        if (owned) {
            logger::warn("[Proxy] D3D12 backbuffers and present-ring resources abandoned until process exit: GPU "
                         "retirement could not be proved");
        }
    }

    void SwapChainProxy::Teardown() noexcept
    {
        g.active.store(false, std::memory_order_relaxed);
        AdaptiveSync::Restore();
        StopPresentThread();
        if (waitable_ != nullptr) {
            ::CloseHandle(waitable_);
            waitable_ = nullptr;
        }
        bool d3d12Retired = d3d11Fallback_;
        if (!d3d11Fallback_ && HasTrustedSidecarTimeline()) {
            d3d12Retired = D3D12Sidecar::DrainGpu() && HasTrustedSidecarTimeline();
        }
        if (d3d12Retired) {
            FrameGenEngine::Release();
            ReleaseD3D12Resources();
            ReleaseChain(true);
        } else {
            FrameGenEngine::AbandonAfterUnretiredSubmission();
            AbandonD3D12Resources();
            ReleaseChain(false);
        }
        if (d3d12Retired) {
            ReleaseCom(sidecarFence_);
            ReleaseCom(sidecarQueue_);
        } else {
            sidecarFence_ = nullptr;
            sidecarQueue_ = nullptr;
        }
        ReleaseCom(fake_);
        ReleaseCom(context11_);
        ReleaseCom(device11_);
        logger::info("[Proxy] released after {} present(s)", presentCount_);
    }

    void SwapChainProxy::ReleaseChain(bool a_orderly) noexcept
    {
        if (!a_orderly) {
            if (fsrFrameGenChain_) {
                logger::warn("[Proxy] abandoning AMD's frame-generation chain without destroying it: GPU "
                             "retirement is unproved and its teardown may wait forever (leaked for the rest of "
                             "the session, deliberately)");
                FsrFrameGen::Abandon();
            } else if (real_ != nullptr) {
                logger::warn("[Proxy] abandoning the D3D12 presenting chain until process exit: an unretired "
                             "submission may still name its backbuffers");
            }
            real_ = nullptr;
            fsrFrameGenChain_ = false;
            return;
        }
        if (fsrFrameGenChain_) {
            real_ = nullptr;
            FsrFrameGen::Release();
            fsrFrameGenChain_ = false;
            return;
        }
        ReleaseCom(real_);
    }

    bool SwapChainProxy::SidecarUsable() noexcept
    {
        return !d3d11Fallback_ && fake_ != nullptr && ring_[0].Valid() && D3D12Sidecar::IsOpen() &&
               !D3D12Sidecar::Disabled() && D3D12Sidecar::BoundDevice() == device11_ &&
               D3D12Sidecar::Queue() == sidecarQueue_ && D3D12Sidecar::Fence12() == sidecarFence_ &&
               D3D12Sidecar::PollHealth();
    }

    bool SwapChainProxy::HasTrustedSidecarTimeline() const noexcept
    {
        return D3D12Sidecar::IsOpen() && !D3D12Sidecar::Disabled() &&
               D3D12Sidecar::BoundDevice() == device11_ &&
               D3D12Sidecar::Queue() == sidecarQueue_ && D3D12Sidecar::Fence12() == sidecarFence_ &&
               !workerSubmissionUnretired_.load(std::memory_order_acquire);
    }

    bool SwapChainProxy::ProveSidecarQueueRetired(unsigned long a_timeoutMs) noexcept
    {
        if (sidecarQueue_ == nullptr) {
            return false;
        }
        ID3D12Device* device = nullptr;
        if (FAILED(sidecarQueue_->GetDevice(IID_PPV_ARGS(&device))) || device == nullptr) {
            return false;
        }
        ID3D12Fence* fence = nullptr;
        const HRESULT created = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
        device->Release();
        if (FAILED(created) || fence == nullptr) {
            return false;
        }
        bool retired = false;
        if (fence->GetCompletedValue() != UINT64_MAX && SUCCEEDED(sidecarQueue_->Signal(fence, 1))) {
            HANDLE event = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (event != nullptr) {
                if (SUCCEEDED(fence->SetEventOnCompletion(1, event))) {
                    const DWORD wait = ::WaitForSingleObject(event, a_timeoutMs);
                    const UINT64 done = fence->GetCompletedValue();
                    retired = wait == WAIT_OBJECT_0 && done >= 1 && done != UINT64_MAX;
                }
                ::CloseHandle(event);
            }
        }
        if (retired) {
            fence->Release();
        } else {
            logger::warn("[Proxy] the sidecar queue did not retire a private fence within {} ms; the fence is "
                         "retained (a leak is the honest outcome, freeing it under a pending Signal is not)",
                a_timeoutMs);
        }
        return retired;
    }

    bool SwapChainProxy::SwitchToD3D11Fallback(const char* a_reason) noexcept
    {
        if (recoveryRequiresRestart_) return false;
        try {
            logger::error("[Proxy] the D3D12 presentation path is gone ({}); switching to a DirectX 11 chain for the rest of the session",
                a_reason != nullptr ? a_reason : "unknown");
            SetReason(a_reason);
            StopPresentThread();
            lastDeliveredQpc_.store(0, std::memory_order_relaxed);
            if (waitable_ != nullptr) {
                ::CloseHandle(waitable_);
                waitable_ = nullptr;
            }
            FrameGenEngine::NoteInterpolating(false);
            bool d3d12Retired = false;
            if (HasTrustedSidecarTimeline()) {
                d3d12Retired = D3D12Sidecar::DrainGpu() && HasTrustedSidecarTimeline();
            }
            if (!d3d12Retired && D3D12Sidecar::BoundDevice() == device11_) {
                d3d12Retired = ProveSidecarQueueRetired(5000);
                if (d3d12Retired) {
                    logger::info("[Proxy] the sidecar queue retired on a private fence after the session was "
                                 "disabled; releasing the D3D12 path in order and building the DirectX 11 chain");
                }
            }
            if (d3d12Retired) {
                FrameGenEngine::Release();
                ReleaseD3D12Resources();
                ReleaseChain(true);
            } else {
                FrameGenEngine::AbandonAfterUnretiredSubmission();
                AbandonD3D12Resources();
                ReleaseChain(false);
            }
            if (d3d12Retired) {
                ReleaseCom(sidecarFence_);
                ReleaseCom(sidecarQueue_);
            } else {
                sidecarFence_ = nullptr;
                sidecarQueue_ = nullptr;
                logger::warn("[Proxy] GPU retirement unproved: the D3D12 chain and resources are retained; "
                             "attempting the DirectX 11 chain regardless");
            }
            IDXGIFactory2* factory = FactoryOf(device11_);
            if (factory == nullptr) {
                return false;
            }
            DXGI_SWAP_CHAIN_DESC desc = policyDesc_;
            desc.BufferDesc.Width = width_;
            desc.BufferDesc.Height = height_;
            desc.BufferDesc.Format = format_;
            desc.Windowed = TRUE;
            IDXGISwapChain* chain = nullptr;
            const HRESULT hr = factory->CreateSwapChain(device11_, &desc, &chain);
            if (SUCCEEDED(hr) && chain != nullptr) {
                factory->MakeWindowAssociation(desc.OutputWindow, DXGI_MWA_NO_ALT_ENTER);
                chain->QueryInterface(IID_PPV_ARGS(&real_));
                chain->Release();
            }
            factory->Release();
            if (real_ == nullptr) {
                if (!d3d12Retired) {
                    recoveryRequiresRestart_ = true;
                    SetReason("GPU retirement unproved and the retained chain refused a replacement; a game restart is required");
                    logger::error("[Proxy] display recovery requires a game restart: the retained flip chain still owns "
                                  "the window ({:#x} creating a replacement)", static_cast<unsigned>(hr));
                    return false;
                }
                logger::error("[Proxy] the DirectX 11 fallback chain could not be created ({:#x}); presents are stale from here on",
                    static_cast<unsigned>(hr));
                return false;
            }
            chainFlags_ = desc.Flags;
            d3d11Fallback_ = true;
            g.d3d11Fallback.store(true, std::memory_order_relaxed);
            g.tearing.store((chainFlags_ & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) != 0U, std::memory_order_relaxed);
            return true;
        } catch (...) {
            return false;
        }
    }

    HRESULT SwapChainProxy::PresentViaD3D11(UINT a_syncInterval, UINT a_flags) noexcept
    {
        ID3D11Texture2D* backbuffer = nullptr;
        if (SUCCEEDED(real_->GetBuffer(0, IID_PPV_ARGS(&backbuffer))) && backbuffer != nullptr) {
            context11_->CopyResource(backbuffer, fake_);
            backbuffer->Release();
        }
        const HRESULT hr = MarkedPresent(real_, a_syncInterval, a_flags);
        NoteDelivered();
        ++presentCount_;
        g.presents.store(presentCount_, std::memory_order_relaxed);
        return hr;
    }

    void SwapChainProxy::NoteInterpolated() noexcept
    {
        g.interpolated.fetch_add(1, std::memory_order_relaxed);
    }

    void SwapChainProxy::WaitLatency() noexcept
    {
        if (waitable_ == nullptr || stop_.load(std::memory_order_relaxed)) {
            return;
        }
        if (::WaitForSingleObject(waitable_, 1000) != WAIT_OBJECT_0) {
            if (FrameGenEngine::Interpolating()) {
                Reflex::NotePresentStall();
            }
            const ULONGLONG now = ::GetTickCount64();
            if (now - lastWaitTimeoutLog_ >= 10000) {
                lastWaitTimeoutLog_ = now;
                char why[400]{};
                D3D12Sidecar::DescribeStall(why, sizeof(why), 0);
                logger::warn("[Proxy] the frame-latency waitable object timed out (1 s); presenting anyway — {}", why);
            }
        }
    }

    bool SwapChainProxy::StartPresentThread() noexcept
    {
        try {
            ID3D12Device* device = D3D12Sidecar::Device();
            if (device == nullptr || D3D12Sidecar::Queue() != sidecarQueue_ ||
                D3D12Sidecar::Fence12() != sidecarFence_) {
                SetReason("no matching D3D12 device/queue/fence timeline for the present thread");
                return false;
            }
            for (auto& alloc : threadAllocators_) {
                if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)))) {
                    SetReason("the present thread's command allocator could not be created");
                    return false;
                }
            }
            if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, threadAllocators_[0], nullptr,
                    IID_PPV_ARGS(&threadList_)))) {
                SetReason("the present thread's command list could not be created");
                return false;
            }
            threadList_->Close();
            threadEvent_ = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (threadEvent_ == nullptr) {
                SetReason("the present thread's fence event could not be created");
                return false;
            }
            stop_.store(false, std::memory_order_relaxed);
            lastDeliveredQpc_.store(0, std::memory_order_relaxed);
            lastArrival_ = 0;
            prevBlocked_ = 0;
            paceSleep_ = 0;
            nextPresentAt_ = 0;
            intervalTicks_ = 0.0;
            threadLogged_ = false;
            g.intervalMs.store(0.0F, std::memory_order_relaxed);
            thread_ = std::thread([this]() { PresentThreadMain(); });
            return true;
        } catch (...) {
            SetReason("the present thread could not start");
            return false;
        }
    }

    void SwapChainProxy::StopPresentThread() noexcept
    {
        try {
            if (thread_.joinable()) {
                (void)DrainPresentThread();
                stop_.store(true, std::memory_order_release);
                queueCv_.notify_all();
                thread_.join();
            }
        } catch (...) {
        }
        const bool hasRecorder = threadList_ != nullptr ||
                                 std::any_of(std::begin(threadAllocators_), std::end(threadAllocators_),
                                     [](const auto* a_allocator) { return a_allocator != nullptr; });
        if (hasRecorder) {
            const bool timelineChanged = D3D12Sidecar::Queue() != sidecarQueue_ ||
                                         D3D12Sidecar::Fence12() != sidecarFence_;
            if (timelineChanged) {
                workerSubmissionUnretired_.store(true, std::memory_order_release);
            }
            std::uint64_t last = 0;
            for (const auto fence : threadAllocatorFence_) {
                if (fence > last) {
                    last = fence;
                }
            }
            ID3D12Fence* const fence12 = D3D12Sidecar::Fence12();
            const bool deviceLost = fence12 != nullptr && fence12->GetCompletedValue() == UINT64_MAX;
            const bool executeUnretired = workerSubmissionUnretired_.load(std::memory_order_acquire);
            const bool retirementUnproved = executeUnretired ||
                                             D3D12Sidecar::Disabled() || fence12 == nullptr || deviceLost;
            bool retired = !retirementUnproved && last == 0;
            if (!retirementUnproved && !retired) {
                retired = D3D12Sidecar::WaitForValue(last, 2000) && !D3D12Sidecar::Disabled();
            }
            if (retired) {
                ReleaseCom(threadList_);
                for (auto& alloc : threadAllocators_) {
                    ReleaseCom(alloc);
                }
            } else {
                workerSubmissionUnretired_.store(true, std::memory_order_release);
                logger::warn("[Proxy] the present thread's command list and allocators are RETAINED, not freed: their last "
                             "submission (#{}) could not be proven retired ({}); a leak is the honest outcome, freeing them "
                             "under the GPU is not",
                    last, timelineChanged ? "the sidecar queue/fence timeline changed"
                              : executeUnretired
                              ? "ExecuteCommandLists ran without a trustworthy retirement value"
                              : D3D12Sidecar::Disabled()
                                  ? "the sidecar is disabled, so a fence value may have been forced from the CPU"
                                  : deviceLost ? "the completion fence reports device removal"
                                               : fence12 == nullptr ? "the completion fence is unavailable"
                                                                    : "the wait timed out");
                threadList_ = nullptr;
                for (auto& alloc : threadAllocators_) {
                    alloc = nullptr;
                }
            }
        }
        for (auto& fence : threadAllocatorFence_) {
            fence = 0;
        }
        threadSlot_ = 0;
        try {
            const std::scoped_lock lock(queueMutex_);
            queue_.clear();
            inFlight_ = 0;
        } catch (...) {
        }
        if (paceTimer_ != nullptr) {
            ::CloseHandle(paceTimer_);
            paceTimer_ = nullptr;
        }
        if (threadEvent_ != nullptr) {
            ::CloseHandle(threadEvent_);
            threadEvent_ = nullptr;
        }
    }

    bool SwapChainProxy::DrainPresentThread() noexcept
    {
        try {
            std::unique_lock lock(queueMutex_);
            if (!drainedCv_.wait_for(lock, std::chrono::seconds(2), [this]() { return queue_.empty() && inFlight_ == 0; })) {
                logger::warn("[Proxy] the present thread did not drain within 2 s");
                return false;
            }
            return true;
        } catch (...) {
            return false;
        }
    }

    void SwapChainProxy::MarkWorkerCopyFault(WorkerCopyFault a_fault) noexcept
    {
        if (a_fault == WorkerCopyFault::kNone) {
            return;
        }
        WorkerCopyFault expected = WorkerCopyFault::kNone;
        if (workerCopyFault_.compare_exchange_strong(expected, a_fault, std::memory_order_acq_rel)) {
            logger::error("[Proxy] present-worker fault: {}; packet intake is closed until the render thread "
                          "safely drains and rebuilds the worker",
                WorkerFaultReason(a_fault));
        }
        queueCv_.notify_all();
    }

    bool SwapChainProxy::RecoverWorkerCopyFault() noexcept
    {
        const WorkerCopyFault fault = workerCopyFault_.load(std::memory_order_acquire);
        if (fault == WorkerCopyFault::kNone) {
            return true;
        }

        FrameGenEngine::NoteInterpolating(false);
        if (!DrainPresentThread()) {
            return false;
        }
        if (fault == WorkerCopyFault::kDrain) {
            workerCopyFault_.store(WorkerCopyFault::kNone, std::memory_order_release);
            logger::info("[Proxy] the present worker drained on retry after a timeout; the held frame is over and "
                         "frame generation continues (no rebuild, no latch)");
            return true;
        }
        if (D3D12Sidecar::Queue() != sidecarQueue_ || D3D12Sidecar::Fence12() != sidecarFence_) {
            workerSubmissionUnretired_.store(true, std::memory_order_release);
            logger::warn("[Proxy] present-worker recovery refused: the sidecar queue/fence timeline changed; "
                         "resources remain owned for fallback abandonment");
            return false;
        }
        if (!D3D12Sidecar::DrainGpu()) {
            logger::warn("[Proxy] present-worker recovery deferred: the sidecar queue did not drain; all worker "
                         "and frame-generation resources remain owned");
            return false;
        }

        workerSubmissionUnretired_.store(false, std::memory_order_release);
        StopPresentThread();
        if (workerSubmissionUnretired_.load(std::memory_order_acquire) || D3D12Sidecar::Disabled() ||
            D3D12Sidecar::Queue() != sidecarQueue_ || D3D12Sidecar::Fence12() != sidecarFence_) {
            logger::warn("[Proxy] present-worker recovery stopped after recorder teardown: retirement or timeline "
                         "identity became untrusted; generated resources remain owned for fallback abandonment");
            return false;
        }
        FrameGenEngine::Release();
        FrameGenEngine::LatchPresentWorkerFailure(WorkerFaultReason(fault));
        workerCopyFault_.store(WorkerCopyFault::kNone, std::memory_order_release);

        if (!StartPresentThread()) {
            StopPresentThread();
            logger::error("[Proxy] the present worker could not be rebuilt; real frames continue inline and "
                          "DLSS-G stays latched until Retry or restart");
        } else {
            logger::warn("[Proxy] present worker drained and rebuilt; DLSS-G is latched off after '{}'; real "
                         "frames continue on the existing presenting chain",
                WorkerFaultReason(fault));
        }
        return true;
    }

    bool SwapChainProxy::EnqueuePacket(Packet& a_packet) noexcept
    {
        try {
            if (workerCopyFault_.load(std::memory_order_acquire) != WorkerCopyFault::kNone || !thread_.joinable()) {
                if (!thread_.joinable()) {
                    MarkWorkerCopyFault(WorkerCopyFault::kRecorderUnavailable);
                }
                return false;
            }
            const LONGLONG beforeWait = Qpc();
            std::unique_lock lock(queueMutex_);
            if (!queueCv_.wait_for(lock, std::chrono::seconds(1),
                    [this]() {
                        return stop_.load(std::memory_order_relaxed) ||
                               workerCopyFault_.load(std::memory_order_acquire) != WorkerCopyFault::kNone ||
                               queue_.size() + inFlight_ < kMaxPacketsInFlight + 1;
                    })) {
                return false;
            }
            a_packet.blocked = Qpc() - beforeWait;
            if (stop_.load(std::memory_order_relaxed) ||
                workerCopyFault_.load(std::memory_order_acquire) != WorkerCopyFault::kNone) {
                return false;
            }
            queue_.push_back(a_packet);
            queueCv_.notify_all();
            return true;
        } catch (...) {
            return false;
        }
    }

    bool SwapChainProxy::ThreadBeginList() noexcept
    {
        if (D3D12Sidecar::Disabled()) {
            return false;
        }
        if (D3D12Sidecar::Queue() != sidecarQueue_ || D3D12Sidecar::Fence12() != sidecarFence_) {
            logger::error("[Proxy] the present worker refused command recording: the sidecar queue/fence "
                          "identity changed underneath the proxy");
            workerSubmissionUnretired_.store(true, std::memory_order_release);
            return false;
        }
        const int slot = threadSlot_;
        const std::uint64_t retire = threadAllocatorFence_[slot];
        ID3D12Fence* fence = D3D12Sidecar::Fence12();
        if (fence == nullptr) {
            return false;
        }
        if (retire != 0 && fence->GetCompletedValue() < retire) {
            fence->SetEventOnCompletion(retire, threadEvent_);
            const DWORD budget = stop_.load(std::memory_order_relaxed) ? 200U : 2000U;
            if (::WaitForSingleObject(threadEvent_, budget) != WAIT_OBJECT_0 || fence->GetCompletedValue() < retire) {
                if (!stop_.load(std::memory_order_relaxed)) {
                    const ULONGLONG now = ::GetTickCount64();
                    if (now - lastThreadListLog_ >= 5000) {
                        lastThreadListLog_ = now;
                        char why[400]{};
                        D3D12Sidecar::DescribeStall(why, sizeof(why), retire);
                        logger::error("[Proxy] the present thread's allocator did not retire within 2 s; the GPU "
                                      "stopped completing sidecar work — {}", why);
                    }
                    D3D12Sidecar::Disable("the GPU stopped completing the present thread's work");
                }
                return false;
            }
        }
        if (D3D12Sidecar::Disabled()) {
            return false;
        }
        if (threadAllocators_[slot] == nullptr || threadList_ == nullptr) {
            logger::error("[Proxy] the present worker's recorder is missing (allocator={}, list={})",
                fmt::ptr(threadAllocators_[slot]), fmt::ptr(threadList_));
            return false;
        }
        const HRESULT allocatorReset = threadAllocators_[slot]->Reset();
        if (FAILED(allocatorReset)) {
            logger::error("[Proxy] the present worker's allocator slot {} Reset failed ({:#010x}); no copy or "
                          "Present is issued",
                slot, static_cast<unsigned>(allocatorReset));
            return false;
        }
        const HRESULT listReset = threadList_->Reset(threadAllocators_[slot], nullptr);
        if (FAILED(listReset)) {
            logger::error("[Proxy] the present worker's command-list Reset failed on slot {} ({:#010x}); no copy "
                          "or Present is issued",
                slot, static_cast<unsigned>(listReset));
            return false;
        }
        return true;
    }

    std::uint64_t SwapChainProxy::ThreadEndList(bool* a_listAccepted) noexcept
    {
        if (D3D12Sidecar::Queue() != sidecarQueue_ || D3D12Sidecar::Fence12() != sidecarFence_) {
            workerSubmissionUnretired_.store(true, std::memory_order_release);
            (void)D3D12Sidecar::CloseOrDrop(threadList_, "the present thread's list after a timeline change");
            if (a_listAccepted != nullptr) {
                *a_listAccepted = false;
            }
            return 0;
        }
        ID3D12CommandList* const closed = D3D12Sidecar::CloseOrDrop(threadList_, "the present thread's list");
        const std::uint64_t value = D3D12Sidecar::ExecuteAndSignal(closed);
        if (closed != nullptr && value == 0) {
            workerSubmissionUnretired_.store(true, std::memory_order_release);
        }
        if (a_listAccepted != nullptr) {
            *a_listAccepted = closed != nullptr && value != 0 && !D3D12Sidecar::Disabled();
        }
        threadAllocatorFence_[threadSlot_] = value;
        if (closed == nullptr) {
            (void)D3D12Sidecar::ReplaceInvalidList(threadList_, threadAllocators_[threadSlot_], L"FO4GraphicsOverhaul present-thread list");
        }
        threadSlot_ = (threadSlot_ + 1) % kThreadAllocators;
        return value;
    }

    bool SwapChainProxy::ThreadCopyToBackbuffer(ID3D12Resource* a_source, D3D12_RESOURCE_STATES a_sourceState) noexcept
    {
        const UINT back = real_->GetCurrentBackBufferIndex();
        ID3D12Resource* target = back < bufferCount_ ? backbuffers_[back] : nullptr;
        if (target == nullptr || a_source == nullptr) {
            return false;
        }
        D3D12Sidecar::Barrier(threadList_, a_source, a_sourceState, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12Sidecar::Barrier(threadList_, target, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
        threadList_->CopyResource(target, a_source);
        D3D12Sidecar::Barrier(threadList_, target, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
        D3D12Sidecar::Barrier(threadList_, a_source, D3D12_RESOURCE_STATE_COPY_SOURCE, a_sourceState);
        return true;
    }

    void SwapChainProxy::PaceUntil(LONGLONG a_target) noexcept
    {
        if (paceTimer_ == nullptr) {
            paceTimer_ = ::CreateWaitableTimerExW(nullptr, nullptr,
                CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
            if (paceTimer_ == nullptr) {
                paceTimer_ = ::CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
                if (!paceTimerLogged_) {
                    paceTimerLogged_ = true;
                    logger::warn("[Proxy] the high-resolution pacing timer was refused; falling back to {} — "
                                 "expect coarser frame placement",
                        paceTimer_ != nullptr ? "an ordinary waitable timer" : "thread sleeps");
                }
            } else if (!paceTimerLogged_) {
                paceTimerLogged_ = true;
                logger::info("[Proxy] pacing on a high-resolution waitable timer (sub-millisecond placement)");
            }
        }
        const LONGLONG spin = qpf_ > 0 ? qpf_ / 4000 : 0;
        const LONGLONG entered = Qpc();
        for (;;) {
            const LONGLONG now = Qpc();
            const LONGLONG remaining = a_target - now;
            if (remaining <= 0 || stop_.load(std::memory_order_relaxed)) {
                paceSleep_ += now - entered;
                return;
            }
            if (remaining > spin) {
                bool waited = false;
                if (paceTimer_ != nullptr && qpf_ > 0) {
                    LARGE_INTEGER due{};
                    due.QuadPart = -((remaining - spin) * 10000000LL / qpf_);
                    if (due.QuadPart < 0 &&
                        ::SetWaitableTimerEx(paceTimer_, &due, 0, nullptr, nullptr, nullptr, 0) != 0) {
                        (void)::WaitForSingleObject(paceTimer_, 100);
                        waited = true;
                    }
                }
                if (!waited) {
                    std::this_thread::sleep_for(std::chrono::microseconds(250));
                }
            } else {
                ::YieldProcessor();
            }
        }
    }

    void SwapChainProxy::PresentThreadMain() noexcept
    {
        ::SetThreadDescription(::GetCurrentThread(), L"FO4GraphicsOverhaul present thread");
        for (;;) {
            Packet packet{};
            {
                std::unique_lock lock(queueMutex_);
                queueCv_.wait(lock, [this]() { return stop_.load(std::memory_order_relaxed) || !queue_.empty(); });
                if (stop_.load(std::memory_order_relaxed) && queue_.empty()) {
                    return;
                }
                packet = queue_.front();
                queue_.pop_front();
                inFlight_ = 1;
                queueCv_.notify_all();
            }
            try {
                paceSleep_ = 0;
                if (!threadLogged_) {
                    threadLogged_ = true;
                    logger::info("[Proxy] present thread: showing generated frames between real ones (paced at even fractions of the measured frame interval)");
                }
                const LONGLONG rawSpan = lastArrival_ != 0 ? packet.arrival - lastArrival_ : 0;
                const LONGLONG span = rawSpan > prevBlocked_ ? rawSpan - prevBlocked_ : rawSpan;

                if (lastArrival_ != 0 && intervalTicks_ > 0.0 &&
                    static_cast<double>(span) > intervalTicks_ * 4.0) {
                    lastArrival_ = 0;
                    prevBlocked_ = 0;
                    nextPresentAt_ = 0;
                }
                if (lastArrival_ != 0) {
                    const double delta = static_cast<double>(span);
                    intervalTicks_ = intervalTicks_ == 0.0 ? delta : intervalTicks_ * 0.85 + delta * 0.15;
                    const double minTicks = static_cast<double>(qpf_) * 0.002, maxTicks = static_cast<double>(qpf_) * 0.1;
                    if (intervalTicks_ < minTicks) intervalTicks_ = minTicks;
                    if (intervalTicks_ > maxTicks) intervalTicks_ = maxTicks;
                    g.intervalMs.store(static_cast<float>(intervalTicks_ * 1000.0 / static_cast<double>(qpf_)), std::memory_order_relaxed);
                }
                lastArrival_ = packet.arrival;
                prevBlocked_ = packet.blocked;

                const double slots = static_cast<double>(packet.count) + 1.0;
                const LONGLONG slotTicks = intervalTicks_ > 0.0
                    ? static_cast<LONGLONG>(intervalTicks_ / slots)
                    : 0;
                if (D3D12Sidecar::Queue() != sidecarQueue_ || D3D12Sidecar::Fence12() != sidecarFence_) {
                    workerSubmissionUnretired_.store(true, std::memory_order_release);
                    MarkWorkerCopyFault(WorkerCopyFault::kSubmission);
                } else {
                    D3D12Sidecar::WaitOnQueue(packet.fence);
                }
                const LONGLONG ready = Qpc();

                LONGLONG target = nextPresentAt_;
                const LONGLONG interval = static_cast<LONGLONG>(intervalTicks_);
                if (target == 0 || target < ready) {
                    target = ready;
                } else if (interval > 0 && target - ready > interval) {
                    target = ready + interval;
                }
                const LONGLONG anchor = target;

                const auto place = [&](LONGLONG a_target) noexcept {
                    WaitLatency();
                    PaceUntil(a_target);
                };
                for (std::uint32_t k = 0; k < packet.count; ++k) {
                    if (workerCopyFault_.load(std::memory_order_acquire) != WorkerCopyFault::kNone) {
                        break;
                    }
                    place(target);
                    if (!ThreadBeginList()) {
                        if (!stop_.load(std::memory_order_relaxed)) {
                            MarkWorkerCopyFault(WorkerCopyFault::kBegin);
                        }
                        break;
                    }
                    const bool copied = ThreadCopyToBackbuffer(packet.outputs[k], D3D12_RESOURCE_STATE_COPY_SOURCE);
                    if (copied) {
                        FrameGenEngine::RestoreOutputState(threadList_, packet.outputs[k]);
                    }
                    bool listAccepted = false;
                    (void)ThreadEndList(&listAccepted);
                    if (!copied) {
                        MarkWorkerCopyFault(WorkerCopyFault::kCopyTarget);
                    } else if (!listAccepted) {
                        MarkWorkerCopyFault(WorkerCopyFault::kSubmission);
                    }
                    if (copied && listAccepted &&
                        workerCopyFault_.load(std::memory_order_acquire) == WorkerCopyFault::kNone) {
                        real_->Present(packet.syncInterval, packet.flags);
                        NoteDelivered();
                        NoteInterpolated();
                    }
                    const LONGLONG now = Qpc();
                    target = (target < now ? now : target) + slotTicks;
                }
                if (workerCopyFault_.load(std::memory_order_acquire) == WorkerCopyFault::kNone) {
                    place(target);
                    if (!ThreadBeginList()) {
                        if (!stop_.load(std::memory_order_relaxed)) {
                            MarkWorkerCopyFault(WorkerCopyFault::kBegin);
                        }
                    } else {
                        const bool copied = ThreadCopyToBackbuffer(
                            ring_[packet.slot].d3d12, D3D12_RESOURCE_STATE_COMMON);
                        bool listAccepted = false;
                        const std::uint64_t value = ThreadEndList(&listAccepted);
                        if (!copied) {
                            MarkWorkerCopyFault(WorkerCopyFault::kCopyTarget);
                        } else if (!listAccepted) {
                            MarkWorkerCopyFault(WorkerCopyFault::kSubmission);
                        }
                        if (copied && listAccepted &&
                            workerCopyFault_.load(std::memory_order_acquire) == WorkerCopyFault::kNone) {
                            ringReadFence_[packet.slot].store(value, std::memory_order_release);
                            real_->Present(packet.syncInterval, packet.flags);
                            NoteDelivered();
                        }
                    }
                }
                nextPresentAt_ = interval > 0 ? anchor + interval : 0;
                prevBlocked_ = DynamicMultiplier::OwnBackPressure(prevBlocked_, paceSleep_);
            } catch (...) {
                prevBlocked_ = 0;
                if (!stop_.load(std::memory_order_relaxed)) {
                    MarkWorkerCopyFault(WorkerCopyFault::kPacketException);
                }
            }
            {
                const std::scoped_lock lock(queueMutex_);
                inFlight_ = 0;
            }
            drainedCv_.notify_all();
            queueCv_.notify_all();
        }
    }

    HRESULT SwapChainProxy::PresentReal(UINT a_syncInterval, UINT a_flags) noexcept
    {
        const HRESULT hr = MarkedPresent(real_, a_syncInterval, a_flags);
        NoteDelivered();
        if (fsrHudlessPending_) {
            fsrHudlessPending_ = false;
            FrameGenEngine::NoteHudlessPresented(D3D12Sidecar::SignalMarker());
        }
        return hr;
    }

    HRESULT SwapChainProxy::PresentViaD3D12(UINT a_syncInterval, UINT a_flags) noexcept
    {
        const D3D12Sidecar::FrameLock frameLock;
        if (!RecoverWorkerCopyFault()) {
            FrameGenEngine::DropFrameInputs();
            Reflex::CloseFrameWithoutPresent();
            return S_OK;
        }
        const int slot = static_cast<int>(presentCount_ % kRingSlots);
        FrameGenEngine::CollectGuides();
        const bool fsrSelected = FsrFrameGen::Selected();
        if (fsrSelected != lastFsrSelected_) {
            if (fsrSelected) {
                if (!DrainPresentThread() || !D3D12Sidecar::DrainGpu()) {
                    const ULONGLONG now = ::GetTickCount64();
                    if (now - lastSwitchDeferredLog_ >= 10000) {
                        lastSwitchDeferredLog_ = now;
                        logger::warn("[Proxy] frame-generation backend switch deferred: the present thread or the "
                                     "sidecar queue did not drain, so DLSS-G's outputs stay allocated and in use "
                                     "(retrying every present; real frames until it clears)");
                    }
                } else {
                    lastFsrSelected_ = fsrSelected;
                    lastDeliveredQpc_.store(0, std::memory_order_relaxed);
                    FrameGenEngine::NoteInterpolating(false);
                    FrameGenEngine::Release();
                    logger::info("[Proxy] frame-generation backend -> FSR: DLSS-G drained and released; AMD's chain {}",
                        FrameGenEngine::Enabled() ? "interpolates from this present"
                                                  : "stays a passthrough until frame generation is switched on");
                }
            } else {
                lastFsrSelected_ = fsrSelected;
                lastDeliveredQpc_.store(0, std::memory_order_relaxed);
                logger::info("[Proxy] frame-generation backend -> DLSS-G: AMD's interpolation is configured off "
                             "from this present; DLSS-G recreates its feature on demand");
            }
        }
        if (fsrFrameGenChain_) {
            FsrFrameGen::RetirePendingRelease();
        }
        if (fsrFrameGenChain_ && FsrFrameGen::ContextReady()) {
            const bool fsrOn = lastFsrSelected_ && !FrameGenEngine::RealFramesOnly();
            void* const hudless = fsrOn ? FrameGenEngine::TakeHudlessForPresent(width_, height_, format_) : nullptr;
            fsrHudlessPending_ = hudless != nullptr;
            FsrFrameGen::ConfigureFrame(real_, fsrOn, hudless);
        }
        bool interpolate = !(fsrSelected && fsrFrameGenChain_) && FrameGenEngine::WantsInterpolation();
        if (const ULONGLONG now = ::GetTickCount64(); now - lastRefreshQuery_ >= 2000ULL) {
            lastRefreshQuery_ = now;
            FrameGenEngine::SetDisplayRefresh(QueryDisplayRefreshHz(hwnd_));
        }
        const std::uint32_t frames = FrameGenEngine::FramesThisFrame(g.intervalMs.load(std::memory_order_relaxed), a_syncInterval);
        if (interpolate && FrameGenEngine::OutputsNeedRebuild(width_, height_, format_, frames)) {
            interpolate = DrainPresentThread() && D3D12Sidecar::DrainGpu();
            if (!interpolate) {
                const ULONGLONG now = ::GetTickCount64();
                if (now - lastRebuildDeferredLog_ >= 10000) {
                    lastRebuildDeferredLog_ = now;
                    logger::warn("[Proxy] frame-generation output rebuild deferred: the present thread or the sidecar queue did not drain (real frames until it does; repeats every 10 s while it persists)");
                }
            }
        }
        interpolate = interpolate && FrameGenEngine::Prepare(width_, height_, format_, frames);
        const std::uint64_t readFence = ringReadFence_[slot].load(std::memory_order_acquire);
        if (readFence != 0) {
            (void)D3D12Sidecar::WaitOnD3D11(readFence);
        }
        context11_->CopyResource(ring_[slot].d3d11, fake_);
        if (interpolate) {
            FrameGenEngine::ProduceUiAlpha(context11_, ring_[slot].d3d11, width_, height_);
        }
        const std::uint64_t vIn = D3D12Sidecar::SignalFromD3D11();
        D3D12Sidecar::WaitOnD3D12(vIn);

        ID3D12GraphicsCommandList* list = nullptr;
        if (!D3D12Sidecar::BeginCommands(list)) {
            FrameGenEngine::DropFrameInputs();
            static std::atomic<std::uint32_t> reports{ 0 };
            if (reports.fetch_add(1, std::memory_order_relaxed) < 5U) {
                logger::warn("[Proxy] command recording did not begin; this frame is not presented (the previous image holds)");
            }
            Reflex::CloseFrameWithoutPresent();
            return S_OK;
        }

        ID3D12Resource* generated[FrameGenEngine::kMaxGeneratedFrames]{};
        std::uint32_t generatedCount = 0;
        if (interpolate) {
            generatedCount = FrameGenEngine::Evaluate(list, ring_[slot].d3d12, width_, height_, format_, presentCount_, generated);
        } else {
            FrameGenEngine::DropFrameInputs();
        }
        if (generatedCount > 0) {
            bool listAccepted = false;
            const std::uint64_t submitted = D3D12Sidecar::EndCommands(&listAccepted);
            FrameGenEngine::NoteSubmitted(submitted);
            (void)D3D12Sidecar::WaitOnD3D11(submitted);
            if (listAccepted) {
                Packet packet{};
                packet.slot = slot;
                packet.fence = submitted;
                for (std::uint32_t i = 0; i < generatedCount; ++i) {
                    packet.outputs[i] = generated[i];
                }
                packet.count = generatedCount;
                packet.syncInterval = a_syncInterval;
                packet.flags = a_flags;
                packet.arrival = Qpc();
                if (EnqueuePacket(packet)) {
                    Reflex::CloseFrameWithoutPresent();
                    FrameGenEngine::NoteInterpolating(true);
                    ++presentCount_;
                    g.presents.store(presentCount_, std::memory_order_relaxed);
                    if (presentCount_ == 1) {
                        logger::info("[Proxy] ★ first proxied present (interpolated): {}x{} via ring slot {}", width_, height_, slot);
                    }
                    return S_OK;
                }
                logger::warn("[Proxy] the present thread refused a packet; presenting this frame directly");
            } else {
                logger::warn("[Proxy] the generation list was dropped by the sidecar; presenting this real frame directly (no generated frames this present)");
                generatedCount = 0;
            }
            list = nullptr;
            if (!D3D12Sidecar::BeginCommands(list)) {
                logger::warn("[Proxy] fallback command recording did not begin; the uncopied frame is not presented");
                FrameGenEngine::DropFrameInputs();
                Reflex::CloseFrameWithoutPresent();
                return S_OK;
            }
        }

        FrameGenEngine::NoteInterpolating(false);
        if (!DrainPresentThread()) {
            MarkWorkerCopyFault(WorkerCopyFault::kDrain);
            (void)D3D12Sidecar::AbandonCommands();
            FrameGenEngine::DropFrameInputs();
            Reflex::CloseFrameWithoutPresent();
            return S_OK;
        }
        const UINT back = real_->GetCurrentBackBufferIndex();
        ID3D12Resource* target = back < bufferCount_ ? backbuffers_[back] : nullptr;
        if (target != nullptr) {
            D3D12Sidecar::Barrier(list, ring_[slot].d3d12, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12Sidecar::Barrier(list, target, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
            list->CopyResource(target, ring_[slot].d3d12);
            D3D12Sidecar::Barrier(list, target, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
            D3D12Sidecar::Barrier(list, ring_[slot].d3d12, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
        } else if (!badIndexLogged_) {
            badIndexLogged_ = true;
            logger::error("[Proxy] GetCurrentBackBufferIndex returned {} with {} buffers; the frame was not copied", back, bufferCount_);
        }
        for (std::uint32_t i = 0; i < generatedCount; ++i) {
            FrameGenEngine::RestoreOutputState(list, generated[i]);
        }
        bool listAccepted = false;
        const std::uint64_t value = D3D12Sidecar::EndCommands(&listAccepted);
        ringReadFence_[slot].store(value, std::memory_order_release);
        const bool copied = target != nullptr && listAccepted;
        if (!listAccepted) {
            logger::warn("[Proxy] the pass-through list was dropped by the sidecar; this frame is not presented (the previous frame holds)");
        }
        if (interpolate) {
            FrameGenEngine::NoteSubmitted(value);
            (void)D3D12Sidecar::WaitOnD3D11(value);
        }
        WaitLatency();
        if (!interpolate && FrameGenEngine::HasResources() &&
            (!FrameGenEngine::Enabled() ||
             (Streamline::AppliedEffectiveEngine() != AaEffectiveEngine::kDlss && !FsrFrameGen::Engaged()))) {
            if (D3D12Sidecar::DrainGpu()) {
                FrameGenEngine::Release();
                logger::info("[Proxy] frame generation resources released (not wanted on this path)");
            }
        }
        if (!copied) Reflex::CloseFrameWithoutPresent();
        const HRESULT hr = copied ? PresentReal(a_syncInterval, a_flags) : S_OK;
        ++presentCount_;
        g.presents.store(presentCount_, std::memory_order_relaxed);
        if (presentCount_ == 1) {
            logger::info("[Proxy] ★ first proxied present: {}x{} via ring slot {} (hr={:#x}{})", width_, height_, slot,
                static_cast<unsigned>(hr),
                fsrFrameGenChain_ ? " — from AMD's wrapper, which returns S_OK unconditionally: NOT proof the frame was displayed"
                                  : "");
        }
        return hr;
    }

    HRESULT SwapChainProxy::ProxyPresent(UINT a_syncInterval, UINT a_flags) noexcept
    {
        if (real_ == nullptr) {
            return DXGI_ERROR_INVALID_CALL;
        }
        try {
            const LONGLONG t0 = Qpc();
            FrameFingerprint::EndFrame(this);
            if (qpf_ == 0) {
                LARGE_INTEGER f{};
                ::QueryPerformanceFrequency(&f);
                qpf_ = f.QuadPart;
            }
            HRESULT hr = S_OK;
            if (d3d11Fallback_) {
                FrameGenEngine::DropFrameInputs();
                hr = PresentViaD3D11(a_syncInterval, a_flags);
            } else if (SidecarUsable()) {
                hr = PresentViaD3D12(a_syncInterval, a_flags);
            } else {
                FrameGenEngine::DropFrameInputs();
                const char* reason = fake_ == nullptr || !ring_[0].Valid() ? "the proxy's resources are gone"
                                     : D3D12Sidecar::Disabled()             ? D3D12Sidecar::DisableReason()
                                     : D3D12Sidecar::BoundDevice() != device11_ ? "the sidecar moved to another D3D11 device"
                                                                              : "the sidecar is unhealthy";
                if (fake_ != nullptr && SwitchToD3D11Fallback(reason)) {
                    hr = PresentViaD3D11(a_syncInterval, a_flags);
                } else {
                    if (!staleLogged_) {
                        staleLogged_ = true;
                        SetReason(reason);
                        logger::error("[Proxy] cannot present new frames ({}) and the DirectX 11 fallback is unavailable; the window is stale from here on", reason);
                    }
                    hr = real_ != nullptr
                             ? real_->Present(a_syncInterval, a_flags & ~static_cast<UINT>(DXGI_PRESENT_ALLOW_TEARING))
                             : DXGI_ERROR_DEVICE_REMOVED;
                    if (real_ != nullptr) {
                        NoteDelivered();
                    }
                }
            }
            ReportTiming(Qpc() - t0);
            return hr;
        } catch (...) {
            static std::atomic<bool> s_logged{ false };
            if (!s_logged.exchange(true)) {
                logger::error("[Proxy] C++ exception in the proxied present; falling through to the real present");
            }
            if (real_ == nullptr) {
                return DXGI_ERROR_DEVICE_REMOVED;
            }
            const HRESULT fallbackHr = real_->Present(a_syncInterval, a_flags & ~static_cast<UINT>(DXGI_PRESENT_ALLOW_TEARING));
            NoteDelivered();
            return fallbackHr;
        }
    }

    HRESULT SwapChainProxy::ProxyResize(UINT a_count, UINT a_width, UINT a_height, DXGI_FORMAT a_format, UINT a_flags) noexcept
    {
        if (real_ == nullptr) {
            return DXGI_ERROR_INVALID_CALL;
        }
        try {
            (void)a_flags;
            const UINT count = a_count != 0U ? a_count : bufferCount_;
            const DXGI_FORMAT format = a_format != DXGI_FORMAT_UNKNOWN ? a_format : format_;
            if (!DrainPresentThread()) {
                logger::error("[Proxy] resize refused: the present thread did not drain (review RC: nothing is released while it may still present)");
                return DXGI_ERROR_INVALID_CALL;
            }
            FrameGenEngine::NoteInterpolating(false);
            const bool sidecarForResize = !d3d11Fallback_;
            if (sidecarForResize && (!HasTrustedSidecarTimeline() || !D3D12Sidecar::DrainGpu() ||
                                    !HasTrustedSidecarTimeline())) {
                logger::error("[Proxy] resize refused: the presenting queue/fence timeline is untrusted or did not "
                              "drain; all old buffers remain owned");
                return DXGI_ERROR_INVALID_CALL;
            }
            const D3D12Sidecar::FrameLock frameLock;
            lastDeliveredQpc_.store(0, std::memory_order_relaxed);
            FrameGenEngine::Release();
            ReleaseD3D12Resources();
            ReleaseCom(fake_);
            if (fsrFrameGenChain_) {
                FsrFrameGen::WaitForPresents();
            }
            const HRESULT hr = real_->ResizeBuffers(count, a_width, a_height, format, chainFlags_);
            if (FAILED(hr)) {
                logger::error("[Proxy] the presenting chain refused ResizeBuffers({}, {}x{}, fmt={}): {:#x}", count, a_width, a_height,
                    static_cast<int>(format), static_cast<unsigned>(hr));
                if (!CreateFake(width_, height_, format_) || (sidecarForResize && !CreateD3D12Resources())) {
                    (void)SwitchToD3D11Fallback("the proxy's buffers could not be restored after a refused resize");
                }
                return hr;
            }
            DXGI_SWAP_CHAIN_DESC created{};
            real_->GetDesc(&created);
            if (!CreateFake(created.BufferDesc.Width, created.BufferDesc.Height, created.BufferDesc.Format)) {
                logger::error("[Proxy] the fake backbuffer could not be recreated after the resize ({}); restoring the previous size", g.reason);
                if (!CreateFake(width_, height_, format_) || (sidecarForResize && !CreateD3D12Resources())) {
                    (void)SwitchToD3D11Fallback("the proxy's buffers could not be restored after a failed resize");
                }
                return E_FAIL;
            }
            if (sidecarForResize && !CreateD3D12Resources()) {
                logger::warn("[Proxy] the D3D12 present resources could not be recreated after the resize ({}); the next present falls back to DirectX 11",
                    g.reason);
            }
            if (fsrFrameGenChain_) {
                FsrFrameGen::OnChainResized();
                (void)FsrFrameGen::EnsureContext(D3D12Sidecar::Device(), created.BufferDesc.Width, created.BufferDesc.Height,
                    static_cast<std::uint32_t>(created.BufferDesc.Format));
            }
            bufferCount_ = created.BufferCount;
            logger::info("[Proxy] resized: {}x{} fmt={} buffers={} (asked {}x{}); the fake backbuffer and the D3D12 "
                         "resources were rebuilt with it",
                created.BufferDesc.Width, created.BufferDesc.Height, static_cast<int>(created.BufferDesc.Format),
                created.BufferCount, a_width, a_height);
            g.bufferCount.store(bufferCount_, std::memory_order_relaxed);
            g.resizes.fetch_add(1, std::memory_order_relaxed);
            logger::info("[Proxy] resized: {}x{} fmt={} buffers={} ({})", created.BufferDesc.Width, created.BufferDesc.Height,
                static_cast<int>(created.BufferDesc.Format), created.BufferCount,
                d3d11Fallback_ ? "DirectX 11 fallback chain" : "D3D12 chain");
            return S_OK;
        } catch (...) {
            logger::error("[Proxy] C++ exception in the proxied resize");
            return E_FAIL;
        }
    }

    void SwapChainProxy::NoteDelivered() noexcept
    {
        if (PresentPolicy::LoadingScreenActive()) {
            lastDeliveredQpc_.store(0, std::memory_order_relaxed);
            lastGapMs_.store(-1.0F, std::memory_order_relaxed);
            return;
        }
        const LONGLONG now = Qpc();
        const LONGLONG last = lastDeliveredQpc_.exchange(now, std::memory_order_relaxed);
        if (last == 0 || qpf_ <= 0) {
            lastGapMs_.store(-1.0F, std::memory_order_relaxed);
            return;
        }
        const float gapMs = static_cast<float>(now - last) * 1000.0F / static_cast<float>(qpf_);
        g.cadenceGapSumMs.fetch_add(gapMs, std::memory_order_relaxed);
        const float prevGap = lastGapMs_.exchange(gapMs, std::memory_order_relaxed);
        if (prevGap >= 0.0F) {
            g.cadenceStepSumMs.fetch_add(gapMs > prevGap ? gapMs - prevGap : prevGap - gapMs, std::memory_order_relaxed);
            g.cadenceSteps.fetch_add(1, std::memory_order_relaxed);
        }
        float prev = g.cadenceLongestMs.load(std::memory_order_relaxed);
        while (gapMs > prev && !g.cadenceLongestMs.compare_exchange_weak(prev, gapMs, std::memory_order_relaxed)) {
        }
        g.cadencePresents.fetch_add(1, std::memory_order_relaxed);
        if (gapMs > AdaptiveSync::FloorMs()) {
            g.cadenceOverFloorWindow.fetch_add(1, std::memory_order_relaxed);
            g.cadenceOverFloorLifetime.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void SwapChainProxy::ReportTiming(LONGLONG a_ticks) noexcept
    {
        ticks_ += a_ticks;
        if (++windowFrames_ < kTimingWindow || qpf_ == 0) {
            return;
        }
        const float ms = static_cast<float>(ticks_) * 1000.0F / static_cast<float>(qpf_) / static_cast<float>(windowFrames_);
        g.cpuMsAvg.store(ms, std::memory_order_relaxed);
        ticks_ = 0;
        windowFrames_ = 0;
        static ULONGLONG s_lastLog = 0;
        const ULONGLONG now = ::GetTickCount64();
        if (now - s_lastLog >= 30000) {
            s_lastLog = now;
            logger::info("[Proxy] timing: {:.2f} ms CPU per present on the render thread — OUR HAND-OFF ONLY ({}), not the frame time: [Perf] measures the whole hook; {} real presents, {} generated by DLSS-G, {} generated by FSR-FG, interval {:.2f} ms",
                ms, d3d11Fallback_ ? "DirectX 11 fallback: copy + present" : "copy-in, fence hops, D3D12 work, present/enqueue",
                presentCount_, g.interpolated.load(std::memory_order_relaxed), FsrFrameGen::Generated(),
                g.intervalMs.load(std::memory_order_relaxed));
            const float longest = g.cadenceLongestMs.exchange(0.0F, std::memory_order_relaxed);
            const std::uint32_t presents = g.cadencePresents.exchange(0, std::memory_order_relaxed);
            const std::uint32_t over = g.cadenceOverFloorWindow.exchange(0, std::memory_order_relaxed);
            const std::uint32_t repeats = AdaptiveSync::SampleRepeats();
            const float gapSum = g.cadenceGapSumMs.exchange(0.0F, std::memory_order_relaxed);
            const float stepSum = g.cadenceStepSumMs.exchange(0.0F, std::memory_order_relaxed);
            const std::uint32_t steps = g.cadenceSteps.exchange(0, std::memory_order_relaxed);
            const float meanGap = presents != 0 ? gapSum / static_cast<float>(presents) : 0.0F;
            const float regularity = (steps != 0 && meanGap > 0.0F) ? 100.0F * (stepSum / static_cast<float>(steps)) / meanGap : 0.0F;
            logger::info("[Proxy] cadence: longest gap {:.2f} ms over {} presents to the panel this window | mean gap {:.2f} ms, regularity {:.1f}% (mean step between consecutive panel presents; 0 = even) | {} past the {:.1f} ms floor ({}), {} this session | the driver showed the last flip {} time(s)",
                longest, presents, meanGap, regularity, over, AdaptiveSync::FloorMs(),
                AdaptiveSync::FloorIsMeasured() ? "the driver's number" : "assumed: 48 Hz",
                g.cadenceOverFloorLifetime.load(std::memory_order_relaxed), repeats);
        }
    }
}

namespace Platform::PresentProxy
{
    bool Active() noexcept { return g.active.load(std::memory_order_relaxed); }

    bool TakeOver(ID3D11Device* a_device, ID3D11DeviceContext* a_context, const DXGI_SWAP_CHAIN_DESC& a_policyDesc,
        IDXGISwapChain** a_out) noexcept
    {
        if (a_out == nullptr) {
            return false;
        }
        *a_out = nullptr;
        if (a_device == nullptr || a_context == nullptr || a_policyDesc.OutputWindow == nullptr) {
            SetReason("no device/context/window to proxy");
            return false;
        }
        try {
            const std::scoped_lock lock(g.takeOverMutex);
            if (Active()) {
                SetReason("a proxy already exists for this session (a second swap chain is not proxied)");
                return false;
            }
            SwapChainProxy* proxy = new SwapChainProxy();
            if (!proxy->Build(a_device, a_context, a_policyDesc)) {
                logger::warn("[Proxy] takeover fell back to the game's own swap chain: {}", g.reason);
                proxy->Release();
                return false;
            }
            g.active.store(true, std::memory_order_relaxed);
            SetReason("");
            *a_out = proxy;
            return true;
        } catch (...) {
            SetReason("exception creating the proxy");
            return false;
        }
    }

    State Snapshot() noexcept
    {
        State state{};
        state.active = g.active.load(std::memory_order_relaxed);
        state.d3d11Fallback = g.d3d11Fallback.load(std::memory_order_relaxed);
        state.fullscreenIgnored = g.fullscreenIgnored.load(std::memory_order_relaxed);
        state.cadenceLongestMs = g.cadenceLongestMs.load(std::memory_order_relaxed);
        state.cadencePresents = g.cadencePresents.load(std::memory_order_relaxed);
        state.cadenceOverFloorWindow = g.cadenceOverFloorWindow.load(std::memory_order_relaxed);
        state.cadenceOverFloorLifetime = g.cadenceOverFloorLifetime.load(std::memory_order_relaxed);
        state.cadenceFloorMs = AdaptiveSync::FloorMs();
        state.cadenceFloorMeasured = AdaptiveSync::FloorIsMeasured();
        try {
            const std::scoped_lock lock(g.reasonMutex);
            std::snprintf(state.reason, sizeof(state.reason), "%s", g.reason);
        } catch (...) {
        }
        state.width = g.width.load(std::memory_order_relaxed);
        state.height = g.height.load(std::memory_order_relaxed);
        state.bufferCount = g.bufferCount.load(std::memory_order_relaxed);
        state.tearing = g.tearing.load(std::memory_order_relaxed);
        state.presents = g.presents.load(std::memory_order_relaxed);
        state.interpolated = g.interpolated.load(std::memory_order_relaxed);
        state.intervalMs = g.intervalMs.load(std::memory_order_relaxed);
        state.resizes = g.resizes.load(std::memory_order_relaxed);
        state.cpuMs = g.cpuMsAvg.load(std::memory_order_relaxed);
        return state;
    }
}
