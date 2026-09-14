#include "PCH.h"

#include "Platform/Reflex.h"

#include "Platform/D3D12Sidecar.h"
#include "Platform/FrameGenEngine.h"
#include "Platform/PresentPolicy.h"
#include "Platform/PresentProxy.h"

#include "RE/Bethesda/BSGraphics.h"

#include <d3d11.h>
#include <d3d12.h>
#pragma warning(push, 0)
#include <nvapi.h>
#pragma warning(pop)

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace
{
    using namespace Platform;

    int CaptureCode(unsigned long a_code, unsigned long* a_out) noexcept
    {
        *a_out = a_code;
        return EXCEPTION_EXECUTE_HANDLER;
    }

    int SehInitialize(unsigned long* a_code) noexcept
    {
        __try {
            return static_cast<int>(NvAPI_Initialize());
        } __except (CaptureCode(GetExceptionCode(), a_code)) {
            return -1;
        }
    }

    int SehSetSleepMode(IUnknown* a_device, NV_SET_SLEEP_MODE_PARAMS* a_params, unsigned long* a_code) noexcept
    {
        __try {
            return static_cast<int>(NvAPI_D3D_SetSleepMode(a_device, a_params));
        } __except (CaptureCode(GetExceptionCode(), a_code)) {
            return -1;
        }
    }

    int SehSleep(IUnknown* a_device, unsigned long* a_code) noexcept
    {
        __try {
            return static_cast<int>(NvAPI_D3D_Sleep(a_device));
        } __except (CaptureCode(GetExceptionCode(), a_code)) {
            return -1;
        }
    }

    int SehSetLatencyMarker(IUnknown* a_device, NV_LATENCY_MARKER_PARAMS* a_params, unsigned long* a_code) noexcept
    {
        __try {
            return static_cast<int>(NvAPI_D3D_SetLatencyMarker(a_device, a_params));
        } __except (CaptureCode(GetExceptionCode(), a_code)) {
            return -1;
        }
    }

    int SehGetLatency(IUnknown* a_device, NV_LATENCY_RESULT_PARAMS* a_params, unsigned long* a_code) noexcept
    {
        __try {
            return static_cast<int>(NvAPI_D3D_GetLatency(a_device, a_params));
        } __except (CaptureCode(GetExceptionCode(), a_code)) {
            return -1;
        }
    }

    void SehErrorMessage(int a_status, char* a_out, unsigned long* a_code) noexcept
    {
        __try {
            NvAPI_GetErrorMessage(static_cast<NvAPI_Status>(a_status), a_out);
        } __except (CaptureCode(GetExceptionCode(), a_code)) {
            a_out[0] = '\0';
        }
    }

    using PerformInputFn = void (*)(RE::BSInputEventReceiver*, const RE::InputEvent*);
    PerformInputFn g_performInputOriginal = nullptr;
    std::atomic<bool> g_inputHookAttempted{ false };

    void PerformInputThunk(RE::BSInputEventReceiver* a_this, const RE::InputEvent* a_queueHead)
    {
        Platform::Reflex::GameFrameBoundary();
        g_performInputOriginal(a_this, a_queueHead);
    }

    [[nodiscard]] bool IsExecutableAddress(std::uintptr_t a_addr) noexcept
    {
        if (a_addr == 0) {
            return false;
        }
        MEMORY_BASIC_INFORMATION mbi{};
        if (::VirtualQuery(reinterpret_cast<const void*>(a_addr), &mbi, sizeof(mbi)) != sizeof(mbi)) {
            return false;
        }
        constexpr DWORD kExec = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        return mbi.State == MEM_COMMIT && (mbi.Protect & kExec) != 0;
    }

    struct ReflexState
    {
        std::atomic<std::uint32_t> mode{ 1 };
        std::atomic<bool> optionsDirty{ true };
        std::atomic<bool> retry{ false };

        bool pacedByFrameGen{ false };
        bool nvapiTried{ false };
        bool nvapiOk{ false };
        bool nvapiFaulted{ false };
        char nvapiReason[160]{};
        IUnknown* bound{ nullptr };
        int boundKind{ 0 };
        std::uint32_t appliedMode{ 0xFFFFFFFFU };
        std::uint64_t frameId{ 0 };
        std::uint64_t sleeps{ 0 };
        std::uint64_t tailSleeps{ 0 };
        std::atomic<std::uint64_t> markers{ 0 };
        bool simOpen{ false };
        std::atomic<std::uint64_t> frameCounter{ 0 };
        std::atomic<std::uint32_t> gameThreadId{ 0 };
        std::atomic<bool> updateSinceBoundary{ false };
        std::atomic<long long> lastBoundaryQpc{ 0 };
        std::atomic<bool> driverReady{ false };
        std::atomic<std::uint32_t> snapMode{ 0 };
        std::atomic<bool> gameSimOpen{ false };
        std::atomic<std::uint64_t> gameFrameId{ 0 };
        std::atomic<std::uint64_t> gameSleeps{ 0 };
        std::atomic<long long> capPaceTicks{ 0 };
        std::atomic<long long> capSleepTicks{ 0 };
        std::atomic<std::uint64_t> capFrames{ 0 };
        std::atomic<long long> freeSleepTicks{ 0 };
        std::atomic<std::uint64_t> freeFrames{ 0 };
        std::atomic<bool> gameSleepLogged{ false };
        std::atomic<bool> tailSleptSinceBoundary{ false };
        std::atomic<bool> seamLiveSnap{ false };
        std::atomic<unsigned long> pendingFaultCode{ 0 };
        char pendingFaultCall[64]{};
        std::mutex bindMutex;
        static constexpr std::uint32_t kRing = 8;
        std::uint64_t ring[kRing]{};
        std::atomic<std::uint32_t> ringWrite{ 0 };
        std::atomic<std::uint32_t> ringRead{ 0 };
        std::atomic<std::uint32_t> ringDepthMax{ 0 };
        std::atomic<std::uint64_t> ringEmptyFinds{ 0 };
        std::atomic<std::uint64_t> ringFullDrops{ 0 };
        ULONGLONG lastReportTick{ 0 };
        std::atomic<bool> markerFailureLogged{ false };
        bool frameOpen{ false };
        bool frameUnmarked{ false };
        std::atomic<bool> presentStalled{ false };
        std::atomic<std::uint32_t> presentStallStrikes{ 0 };
        char lastIdle[160]{};

        std::atomic<bool> latched{ false };
        std::atomic<bool> idle{ false };
        std::mutex reasonMutex;
        char reason[160]{};

        std::atomic<std::uint32_t> renderThreadId{ 0 };

        std::atomic<int> snapBound{ 0 };
        std::atomic<std::uint64_t> snapSleeps{ 0 };
        std::atomic<std::uint64_t> snapTailSleeps{ 0 };
        std::atomic<std::uint64_t> snapMarkers{ 0 };
        std::atomic<float> latencyMs{ 0.0F };
        std::atomic<float> gpuFrameMs{ 0.0F };
        std::atomic<std::uint32_t> reportedFrames{ 0 };
        std::atomic<bool> snapPaced{ false };
    };
    ReflexState g;

    void CloseSimIfOpen() noexcept;
    void CloseGameSimLocked() noexcept;

    [[nodiscard]] const char* KindName(int a_kind) noexcept
    {
        return a_kind == 1 ? "the DirectX 12 presenter (the sidecar device)"
             : a_kind == 2 ? "the DirectX 11 game device"
                           : "no device";
    }

    void SetReasonText(const char* a_text) noexcept
    {
        try {
            const std::scoped_lock lock(g.reasonMutex);
            std::snprintf(g.reason, sizeof(g.reason), "%s", a_text != nullptr ? a_text : "unknown");
        } catch (...) {
        }
    }

    void Latch(const char* a_reason) noexcept
    {
        g.driverReady.store(false, std::memory_order_release);
        CloseSimIfOpen();
        if (!g.latched.exchange(true, std::memory_order_relaxed)) {
            SetReasonText(a_reason);
            g.idle.store(false, std::memory_order_relaxed);
            logger::warn("[Reflex] {} — Reflex (NVAPI) is OFF until Retry{}", a_reason != nullptr ? a_reason : "unknown",
                g.nvapiFaulted ? " (a fault: stays off until the game restarts)" : "");
        }
    }

    void Fault(const char* a_call, unsigned long a_code) noexcept
    {
        g.nvapiFaulted = true;
        g.nvapiOk = false;
        char reason[160]{};
        std::snprintf(reason, sizeof(reason), "%s raised an exception (%#010lx) inside the driver", a_call, a_code);
        std::snprintf(g.nvapiReason, sizeof(g.nvapiReason), "%s", reason);
        Latch(reason);
    }

    void SetIdle(const char* a_why) noexcept
    {
        g.idle.store(true, std::memory_order_relaxed);
        if (std::strncmp(g.lastIdle, a_why, sizeof(g.lastIdle)) != 0) {
            std::snprintf(g.lastIdle, sizeof(g.lastIdle), "%s", a_why);
            SetReasonText(a_why);
            logger::info("[Reflex] idle: {}", a_why);
        }
    }

    void ClearIdle() noexcept
    {
        if (g.idle.exchange(false, std::memory_order_relaxed)) {
            g.lastIdle[0] = '\0';
        }
    }

    void DescribeStatus(int a_status, char* a_text  ) noexcept
    {
        a_text[0] = '\0';
        unsigned long code = 0;
        SehErrorMessage(a_status, a_text, &code);
        if (code != 0) {
            Fault("NvAPI_GetErrorMessage", code);
        }
    }

    [[nodiscard]] bool EnsureNvapi() noexcept
    {
        if (g.nvapiFaulted) {
            Latch(g.nvapiReason);
            return false;
        }
        if (g.nvapiTried) {
            if (!g.nvapiOk) {
                Latch(g.nvapiReason);
            }
            return g.nvapiOk;
        }
        g.nvapiTried = true;
        unsigned long code = 0;
        const int status = SehInitialize(&code);
        if (code != 0) {
            Fault("NvAPI_Initialize", code);
            return false;
        }
        g.nvapiOk = status == static_cast<int>(NVAPI_OK);
        if (g.nvapiOk) {
            logger::info("[Reflex] NVAPI initialised (the driver's nvapi64.dll)");
            return true;
        }
        char text[64]{};
        DescribeStatus(status, text);
        std::snprintf(g.nvapiReason, sizeof(g.nvapiReason), "NvAPI_Initialize failed (%d: %s) — no NVIDIA driver NVAPI here", status, text);
        Latch(g.nvapiReason);
        return false;
    }

    [[nodiscard]] bool ApplySleepMode(std::uint32_t a_mode, bool& a_deferred) noexcept
    {
        a_deferred = false;
        NV_SET_SLEEP_MODE_PARAMS params{};
        params.version = NV_SET_SLEEP_MODE_PARAMS_VER;
        params.bLowLatencyMode = a_mode != 0U ? 1 : 0;
        params.bLowLatencyBoost = a_mode == 2U ? 1 : 0;
        params.minimumIntervalUs = 0;
        params.bUseMarkersToOptimize = a_mode != 0U ? 1 : 0;
        unsigned long code = 0;
        int status = 0;
        {
            std::unique_lock lock(g.bindMutex, std::try_to_lock);
            if (!lock.owns_lock()) {
                a_deferred = true;
                return true;
            }
            status = SehSetSleepMode(g.bound, &params, &code);
        }
        if (code != 0) {
            Fault("NvAPI_D3D_SetSleepMode", code);
            return false;
        }
        if (status != static_cast<int>(NVAPI_OK)) {
            char text[64]{};
            DescribeStatus(status, text);
            char reason[160]{};
            std::snprintf(reason, sizeof(reason), "NvAPI_D3D_SetSleepMode failed on %s (%d: %s)", KindName(g.boundKind), status, text);
            Latch(reason);
            return false;
        }
        logger::info("[Reflex] sleep mode {} on {} (low latency {}, boost {}, markers {}; the frame cap is never the driver's - the mod's own pacer waits it out, on the game thread before input where the player-update detour names that thread, at the present otherwise)",
            a_mode == 0U ? "OFF" : a_mode == 1U ? "ON" : "ON + BOOST", KindName(g.boundKind),
            params.bLowLatencyMode != 0 ? "on" : "off", params.bLowLatencyBoost != 0 ? "on" : "off",
            params.bUseMarkersToOptimize != 0 ? "on" : "off");
        return true;
    }

    [[nodiscard]] bool Unbind() noexcept
    {
        if (g.bound == nullptr) {
            return true;
        }
        g.driverReady.store(false, std::memory_order_release);
        std::unique_lock lock(g.bindMutex, std::try_to_lock);
        if (!lock.owns_lock()) {
            return false;
        }
        CloseGameSimLocked();
        CloseSimIfOpen();
        if (g.nvapiOk && !g.nvapiFaulted && g.appliedMode != 0U && g.appliedMode != 0xFFFFFFFFU) {
            NV_SET_SLEEP_MODE_PARAMS params{};
            params.version = NV_SET_SLEEP_MODE_PARAMS_VER;
            unsigned long code = 0;
            const int status = SehSetSleepMode(g.bound, &params, &code);
            if (code != 0) {
                Fault("NvAPI_D3D_SetSleepMode (unbind)", code);
            } else if (status != static_cast<int>(NVAPI_OK)) {
                char text[64]{};
                DescribeStatus(status, text);
                logger::warn("[Reflex] low latency could not be turned off while unbinding from {} ({}: {}); the "
                             "device is released as it stands", KindName(g.boundKind), status, text);
            }
        }
        logger::info("[Reflex] unbound from {}", KindName(g.boundKind));
        g.bound->Release();
        g.bound = nullptr;
        g.boundKind = 0;
        g.appliedMode = 0xFFFFFFFFU;
        g.snapMode.store(0, std::memory_order_relaxed);
        g.ringRead.store(g.ringWrite.load(std::memory_order_acquire), std::memory_order_release);
        g.frameOpen = false;
        g.frameUnmarked = false;
        g.simOpen = false;
        g.pacedByFrameGen = false;
        g.snapPaced.store(false, std::memory_order_relaxed);
        g.snapBound.store(0, std::memory_order_relaxed);
        return true;
    }

    [[nodiscard]] int ResolveBindingKind() noexcept
    {
        const auto proxy = PresentProxy::Snapshot();
        if (proxy.active && !proxy.d3d11Fallback && D3D12Sidecar::IsOpen() && !D3D12Sidecar::Disabled()) {
            return 1;
        }
        return 2;
    }

    [[nodiscard]] IUnknown* DeviceFor(int a_kind) noexcept
    {
        if (a_kind == 1) {
            return (D3D12Sidecar::IsOpen() && !D3D12Sidecar::Disabled()) ? D3D12Sidecar::Device() : nullptr;
        }
        ID3D11Device* device = D3D12Sidecar::BoundDevice();
        if (device == nullptr) {
            auto* const data = RE::BSGraphics::RendererData::GetSingleton();
            device = data != nullptr ? reinterpret_cast<ID3D11Device*>(data->device) : nullptr;
        }
        return device;
    }

    [[nodiscard]] bool EmitMarker(IUnknown* a_dev, NV_LATENCY_MARKER_TYPE a_type, std::uint64_t a_id, unsigned long* a_code) noexcept
    {
        NV_LATENCY_MARKER_PARAMS params{};
        params.version = NV_LATENCY_MARKER_PARAMS_VER;
        params.frameID = a_id;
        params.markerType = a_type;
        const int status = SehSetLatencyMarker(a_dev, &params, a_code);
        if (*a_code != 0) {
            return false;
        }
        if (status == static_cast<int>(NVAPI_OK)) {
            g.snapMarkers.store(g.markers.fetch_add(1, std::memory_order_relaxed) + 1, std::memory_order_relaxed);
        } else if (!g.markerFailureLogged.exchange(true, std::memory_order_relaxed)) {
            char text[64]{};
            DescribeStatus(status, text);
            logger::warn("[Reflex] NvAPI_D3D_SetLatencyMarker failed ({}: {}) — markers off, the sleep stays (one-shot line)", status, text);
        }
        return true;
    }

    void Marker(NV_LATENCY_MARKER_TYPE a_type) noexcept
    {
        if (g.bound == nullptr || !g.nvapiOk || g.nvapiFaulted || g.latched.load(std::memory_order_relaxed) || g.pacedByFrameGen ||
            g.frameUnmarked) {
            return;
        }
        unsigned long code = 0;
        if (!EmitMarker(g.bound, a_type, g.frameId, &code)) {
            Fault("NvAPI_D3D_SetLatencyMarker", code);
        }
    }

    void CloseGameSimLocked() noexcept
    {
        if (g.gameSimOpen.exchange(false, std::memory_order_acq_rel) && g.bound != nullptr && g.nvapiOk && !g.nvapiFaulted) {
            unsigned long code = 0;
            if (!EmitMarker(g.bound, SIMULATION_END, g.gameFrameId.load(std::memory_order_relaxed), &code)) {
                Fault("NvAPI_D3D_SetLatencyMarker", code);
            }
        }
    }

    thread_local bool t_inGameSeam = false;
    struct GameSeamGuard
    {
        GameSeamGuard() noexcept { t_inGameSeam = true; }
        ~GameSeamGuard() { t_inGameSeam = false; }
    };

    [[nodiscard]] bool CloseGameSimAndHandOver(IUnknown* a_dev) noexcept
    {
        if (!g.gameSimOpen.exchange(false, std::memory_order_acq_rel)) {
            return true;
        }
        const std::uint64_t closing = g.gameFrameId.load(std::memory_order_relaxed);
        unsigned long code = 0;
        if (!EmitMarker(a_dev, SIMULATION_END, closing, &code)) {
            std::snprintf(g.pendingFaultCall, sizeof(g.pendingFaultCall), "NvAPI_D3D_SetLatencyMarker (game thread)");
            g.driverReady.store(false, std::memory_order_release);
            g.pendingFaultCode.store(code, std::memory_order_release);
            return false;
        }
        const auto w = g.ringWrite.load(std::memory_order_relaxed);
        if (w - g.ringRead.load(std::memory_order_acquire) < ReflexState::kRing) {
            g.ring[w % ReflexState::kRing] = closing;
            g.ringWrite.store(w + 1, std::memory_order_release);
        } else {
            g.ringFullDrops.fetch_add(1, std::memory_order_relaxed);
        }
        return true;
    }

    [[nodiscard]] bool GameSeamLive() noexcept
    {
        const long long last = g.lastBoundaryQpc.load(std::memory_order_acquire);
        if (last == 0) {
            return false;
        }
        static const long long freq = [] {
            LARGE_INTEGER f{};
            ::QueryPerformanceFrequency(&f);
            return f.QuadPart;
        }();
        LARGE_INTEGER now{};
        ::QueryPerformanceCounter(&now);
        return (now.QuadPart - last) < freq / 2;
    }

    void CloseGameSpanIfStranded() noexcept
    {
        g.ringRead.store(g.ringWrite.load(std::memory_order_acquire), std::memory_order_release);
        if (!g.gameSimOpen.load(std::memory_order_acquire)) {
            return;
        }
        std::unique_lock lock(g.bindMutex, std::try_to_lock);
        if (!lock.owns_lock()) {
            return;
        }
        CloseGameSimLocked();
    }

    void CloseSimIfOpen() noexcept
    {
        if (g.simOpen) {
            Marker(SIMULATION_END);
            g.simOpen = false;
        }
    }

    [[nodiscard]] bool ReconcileOptions() noexcept
    {
        const bool paced = FrameGenEngine::Interpolating() && g.presentStalled.load(std::memory_order_relaxed);
        if (paced != g.pacedByFrameGen) {
            if (paced) {
                CloseSimIfOpen();
                g.driverReady.store(false, std::memory_order_release);
                const std::scoped_lock lock(g.bindMutex);
                CloseGameSimLocked();
                g.ringRead.store(g.ringWrite.load(std::memory_order_acquire), std::memory_order_release);
            }
            g.pacedByFrameGen = paced;
            g.snapPaced.store(paced, std::memory_order_relaxed);
            g.optionsDirty.store(true, std::memory_order_relaxed);
            logger::info("[Reflex] {} — low-latency mode {} on {}",
                paced ? "frame generation is pacing and the stall fallback is engaged"
                      : "frame generation no longer forces Reflex aside",
                paced ? "OFF and no markers" : "back to the user's mode", KindName(g.boundKind));
        }
        const std::uint32_t mode = paced ? 0U : g.mode.load(std::memory_order_relaxed);
        if (g.optionsDirty.exchange(false, std::memory_order_acq_rel) || g.appliedMode != mode) {
            bool deferred = false;
            if (!ApplySleepMode(mode, deferred)) {
                return false;
            }
            if (deferred) {
                g.optionsDirty.store(true, std::memory_order_relaxed);
                return true;
            }
            g.appliedMode = mode;
        }
        g.snapMode.store(g.appliedMode, std::memory_order_relaxed);
        return true;
    }

    void ReadLatencyReport() noexcept
    {
        const ULONGLONG now = ::GetTickCount64();
        if (now - g.lastReportTick < 1000ULL) {
            return;
        }
        g.lastReportTick = now;
        static NV_LATENCY_RESULT_PARAMS s_report{};
        std::memset(&s_report, 0, sizeof(s_report));
        s_report.version = NV_LATENCY_RESULT_PARAMS_VER;
        unsigned long code = 0;
        const int status = SehGetLatency(g.bound, &s_report, &code);
        if (code != 0) {
            Fault("NvAPI_D3D_GetLatency", code);
            return;
        }
        if (status != static_cast<int>(NVAPI_OK)) {
            return;
        }
        double latencyUs = 0.0, gpuUs = 0.0;
        std::uint32_t count = 0;
        for (const auto& frame : s_report.frameReport) {
            if (frame.frameID == 0 || frame.simStartTime == 0 || frame.gpuRenderEndTime <= frame.simStartTime) {
                continue;
            }
            latencyUs += static_cast<double>(frame.gpuRenderEndTime - frame.simStartTime);
            gpuUs += static_cast<double>(frame.gpuFrameTimeUs);
            ++count;
        }
        g.reportedFrames.store(count, std::memory_order_relaxed);
        if (count > 0) {
            g.latencyMs.store(static_cast<float>(latencyUs / count / 1000.0), std::memory_order_relaxed);
            g.gpuFrameMs.store(static_cast<float>(gpuUs / count / 1000.0), std::memory_order_relaxed);
        }
    }
}

namespace Platform::Reflex
{
    void SetMode(std::uint32_t a_mode) noexcept
    {
        const std::uint32_t clamped = a_mode > 2U ? 1U : a_mode;
        if (g.mode.exchange(clamped, std::memory_order_relaxed) != clamped) {
            g.optionsDirty.store(true, std::memory_order_relaxed);
        }
    }

    std::uint32_t Mode() noexcept { return g.mode.load(std::memory_order_relaxed); }

    void RequestRetry() noexcept
    {
        g.retry.store(true, std::memory_order_relaxed);
        g.optionsDirty.store(true, std::memory_order_relaxed);
        logger::info("[Reflex] retry requested");
    }

    void FrameStart() noexcept
    {
        try {
            g.renderThreadId.store(static_cast<std::uint32_t>(::GetCurrentThreadId()), std::memory_order_relaxed);
            g.frameUnmarked = false;
            if (const auto code = g.pendingFaultCode.exchange(0, std::memory_order_acq_rel); code != 0) {
                Fault(g.pendingFaultCall, code);
            }
            if (g.retry.exchange(false, std::memory_order_relaxed)) {
                if (!g.nvapiFaulted) {
                    g.latched.store(false, std::memory_order_relaxed);
                    g.nvapiTried = false;
                    g.markerFailureLogged.store(false, std::memory_order_relaxed);
                }
            }
            const bool live = GameSeamLive();
            g.seamLiveSnap.store(live, std::memory_order_relaxed);
            if (!live || g.latched.load(std::memory_order_relaxed)) {
                CloseGameSpanIfStranded();
            }
            if (g.latched.load(std::memory_order_relaxed)) {
                return;
            }
            if (!PresentProxy::Active()) {
                if (!Unbind()) {
                    return;
                }
                SetIdle("the game presents its own swap chain (no proxied present) — Reflex needs the proxy's present for its markers");
                return;
            }
            const int kind = ResolveBindingKind();
            IUnknown* device = DeviceFor(kind);
            if (device == nullptr) {
                if (!Unbind()) {
                    return;
                }
                SetIdle(kind == 1 ? "waiting for the DirectX 12 sidecar to open" : "waiting for the game's D3D11 device");
                return;
            }
            if (!EnsureNvapi()) {
                return;
            }
            if (device != g.bound) {
                if (!Unbind()) {
                    return;
                }
                device->AddRef();
                {
                    const std::scoped_lock lock(g.bindMutex);
                    g.bound = device;
                    g.boundKind = kind;
                }
                g.snapBound.store(kind, std::memory_order_relaxed);
                g.optionsDirty.store(true, std::memory_order_relaxed);
                g.markerFailureLogged.store(false, std::memory_order_relaxed);
                logger::info("[Reflex] bound to {}", KindName(kind));
            }
            ClearIdle();
            if (!ReconcileOptions()) {
                return;
            }
            if (g.pacedByFrameGen) {
                g.frameOpen = false;
                g.simOpen = false;
                g.driverReady.store(false, std::memory_order_release);
                return;
            }
            g.driverReady.store(true, std::memory_order_release);
            const std::uint32_t mode = g.appliedMode;
            if (live) {
                CloseSimIfOpen();
                const auto r = g.ringRead.load(std::memory_order_relaxed);
                const std::uint32_t depth = g.ringWrite.load(std::memory_order_acquire) - r;
                if (depth == 0) {
                    g.ringEmptyFinds.fetch_add(1, std::memory_order_relaxed);
                    g.frameUnmarked = true;
                    g.frameOpen = false;
                    ReadLatencyReport();
                    return;
                }
                {
                    std::uint32_t seen = g.ringDepthMax.load(std::memory_order_relaxed);
                    while (depth > seen && !g.ringDepthMax.compare_exchange_weak(seen, depth, std::memory_order_relaxed)) {
                    }
                }
                g.frameId = g.ring[r % ReflexState::kRing];
                g.ringRead.store(r + 1, std::memory_order_release);
                Marker(RENDERSUBMIT_START);
                g.frameOpen = true;
                ReadLatencyReport();
                return;
            }
            if (!g.simOpen) {
                g.frameId = g.frameCounter.fetch_add(1, std::memory_order_relaxed) + 1;
                if (mode != 0U) {
                    g.tailSleptSinceBoundary.store(true, std::memory_order_release);
                    unsigned long code = 0;
                    const int status = SehSleep(g.bound, &code);
                    if (code != 0) {
                        Fault("NvAPI_D3D_Sleep", code);
                        return;
                    }
                    if (status == static_cast<int>(NVAPI_OK)) {
                        ++g.sleeps;
                        g.snapSleeps.store(g.sleeps, std::memory_order_relaxed);
                    }
                }
                Marker(SIMULATION_START);
                g.simOpen = true;
            }
            Marker(SIMULATION_END);
            g.simOpen = false;
            Marker(RENDERSUBMIT_START);
            g.frameOpen = true;
            ReadLatencyReport();
        } catch (...) {
            Latch("C++ exception in the Reflex frame start");
        }
    }

    void AfterPresent() noexcept
    {
        try {
            if (g.latched.load(std::memory_order_relaxed) || g.bound == nullptr || !g.nvapiOk || g.nvapiFaulted) {
                return;
            }
            if (static_cast<std::uint32_t>(::GetCurrentThreadId()) != g.renderThreadId.load(std::memory_order_relaxed)) {
                return;
            }
            if (!ReconcileOptions()) {
                return;
            }
            if (g.pacedByFrameGen) {
                return;
            }
            CloseSimIfOpen();
            const bool live = GameSeamLive();
            g.seamLiveSnap.store(live, std::memory_order_relaxed);
            if (live) {
                return;
            }
            CloseGameSpanIfStranded();
            g.frameUnmarked = false;
            g.frameId = g.frameCounter.fetch_add(1, std::memory_order_relaxed) + 1;
            if (g.appliedMode != 0U && g.appliedMode != 0xFFFFFFFFU) {
                g.tailSleptSinceBoundary.store(true, std::memory_order_release);
                unsigned long code = 0;
                const int status = SehSleep(g.bound, &code);
                if (code != 0) {
                    Fault("NvAPI_D3D_Sleep", code);
                    return;
                }
                if (status == static_cast<int>(NVAPI_OK)) {
                    ++g.sleeps;
                    ++g.tailSleeps;
                    g.snapSleeps.store(g.sleeps, std::memory_order_relaxed);
                    g.snapTailSleeps.store(g.tailSleeps, std::memory_order_relaxed);
                }
            }
            Marker(SIMULATION_START);
            g.simOpen = true;
        } catch (...) {
            Latch("C++ exception at the Reflex present tail");
        }
    }

    void InstallInputHook() noexcept
    {
        if (g_inputHookAttempted.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        try {
            REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE::PlayerControls[0] };
            const auto existing = *reinterpret_cast<const std::uintptr_t*>(vtbl.address());
            if (!IsExecutableAddress(existing)) {
                logger::warn("[Reflex] the input hook is NOT installed: PlayerControls vtable slot 0 holds {:#x}, which is "
                             "not an executable address — the sleep and the frame cap fall back to the present's tail",
                    existing);
                return;
            }
            g_performInputOriginal = reinterpret_cast<PerformInputFn>(existing);
            vtbl.write_vfunc(0, PerformInputThunk);
            logger::info("[Reflex] the input hook is installed on PlayerControls::PerformInputProcessing (vtable {:#x}, "
                         "slot 0 was {:#x}, chained through whatever held it) — the driver's sleep and the frame cap run "
                         "here, on the game thread, right before the game consumes its input",
                vtbl.address(), existing);
        } catch (...) {
            logger::warn("[Reflex] the input hook threw during install and is NOT installed — the sleep and the frame "
                         "cap fall back to the present's tail");
        }
    }

    void NoteGameUpdate() noexcept
    {
        g.gameThreadId.store(static_cast<std::uint32_t>(::GetCurrentThreadId()), std::memory_order_relaxed);
        g.updateSinceBoundary.store(true, std::memory_order_release);
    }

    void NoteGameUpdateEnd() noexcept
    {
        if (!g.gameSimOpen.load(std::memory_order_acquire)) {
            return;
        }
        if (t_inGameSeam) {
            return;
        }
        const GameSeamGuard guard;
        try {
            if (!g.driverReady.load(std::memory_order_acquire) || g.pendingFaultCode.load(std::memory_order_acquire) != 0 ||
                g.snapPaced.load(std::memory_order_relaxed)) {
                return;
            }
            std::unique_lock lock(g.bindMutex, std::try_to_lock);
            if (!lock.owns_lock()) {
                return;
            }
            IUnknown* const dev = g.bound;
            if (dev == nullptr) {
                return;
            }
            (void)CloseGameSimAndHandOver(dev);
        } catch (...) {
        }
    }

    void GameFrameBoundary() noexcept
    {
        if (t_inGameSeam) {
            return;
        }
        const GameSeamGuard guard;
        try {
            if (static_cast<std::uint32_t>(::GetCurrentThreadId()) != g.gameThreadId.load(std::memory_order_relaxed)) {
                return;
            }
            if (!g.updateSinceBoundary.exchange(false, std::memory_order_acq_rel)) {
                return;
            }
            LARGE_INTEGER paceStart{}, paceEnd{};
            ::QueryPerformanceCounter(&paceStart);
            const bool capWaited = PresentPolicy::PaceGameFrame();
            ::QueryPerformanceCounter(&paceEnd);
            const long long paceTicks = paceEnd.QuadPart - paceStart.QuadPart;
            {
                LARGE_INTEGER stamp{};
                ::QueryPerformanceCounter(&stamp);
                g.lastBoundaryQpc.store(stamp.QuadPart, std::memory_order_release);
            }
            if (!g.driverReady.load(std::memory_order_acquire)) {
                return;
            }
            if (g.pendingFaultCode.load(std::memory_order_acquire) != 0) {
                return;
            }
            if (g.snapPaced.load(std::memory_order_relaxed)) {
                return;
            }
            std::unique_lock lock(g.bindMutex, std::try_to_lock);
            if (!lock.owns_lock()) {
                return;
            }
            IUnknown* const dev = g.bound;
            if (dev == nullptr) {
                return;
            }
            unsigned long code = 0;
            if (!CloseGameSimAndHandOver(dev)) {
                return;
            }
            const bool tailCoveredThisFrame = g.tailSleptSinceBoundary.exchange(false, std::memory_order_acq_rel);
            if (!tailCoveredThisFrame && g.snapMode.load(std::memory_order_relaxed) != 0U) {
                LARGE_INTEGER sleepStart{}, sleepEnd{};
                ::QueryPerformanceCounter(&sleepStart);
                const int status = SehSleep(dev, &code);
                ::QueryPerformanceCounter(&sleepEnd);
                if (capWaited) {
                    g.capPaceTicks.fetch_add(paceTicks, std::memory_order_relaxed);
                    g.capSleepTicks.fetch_add(sleepEnd.QuadPart - sleepStart.QuadPart, std::memory_order_relaxed);
                    g.capFrames.fetch_add(1, std::memory_order_relaxed);
                } else {
                    g.freeSleepTicks.fetch_add(sleepEnd.QuadPart - sleepStart.QuadPart, std::memory_order_relaxed);
                    g.freeFrames.fetch_add(1, std::memory_order_relaxed);
                }
                if (code != 0) {
                    std::snprintf(g.pendingFaultCall, sizeof(g.pendingFaultCall), "NvAPI_D3D_Sleep (game thread)");
                    g.driverReady.store(false, std::memory_order_release);
                    g.pendingFaultCode.store(code, std::memory_order_release);
                    return;
                }
                if (status == static_cast<int>(NVAPI_OK)) {
                    g.gameSleeps.fetch_add(1, std::memory_order_relaxed);
                    if (!g.gameSleepLogged.exchange(true, std::memory_order_relaxed)) {
                        logger::info("[Reflex] the GAME thread ({}) took the sleep before input. "
                                     "The driver's latency report spans the whole frame: simulation start on this "
                                     "thread, before input, to that same frame's GPU render end (each simulation "
                                     "closes at the end of the player update, so the id the render carries is its own).",
                            ::GetCurrentThreadId());
                    }
                }
            }
            const std::uint64_t id = g.frameCounter.fetch_add(1, std::memory_order_relaxed) + 1;
            g.gameFrameId.store(id, std::memory_order_relaxed);
            if (!EmitMarker(dev, SIMULATION_START, id, &code)) {
                std::snprintf(g.pendingFaultCall, sizeof(g.pendingFaultCall), "NvAPI_D3D_SetLatencyMarker (game thread)");
                g.driverReady.store(false, std::memory_order_release);
                g.pendingFaultCode.store(code, std::memory_order_release);
                return;
            }
            g.gameSimOpen.store(true, std::memory_order_release);
        } catch (...) {
        }
    }

    void RenderSubmitEnd() noexcept
    {
        if (g.simOpen) {
            Marker(SIMULATION_END);
            g.simOpen = false;
            if (!g.frameOpen) {
                Marker(RENDERSUBMIT_START);
                g.frameOpen = true;
            }
        }
        Marker(RENDERSUBMIT_END);
    }
    void PresentStart() noexcept { Marker(PRESENT_START); }
    void PresentEnd() noexcept
    {
        Marker(PRESENT_END);
        g.frameOpen = false;
    }

    void NotePresentStall() noexcept
    {
        if (g.presentStallStrikes.fetch_add(1, std::memory_order_relaxed) == 0U) {
            logger::warn("[Reflex] the frame-latency wait timed out once while frame generation was driving "
                         "Reflex — watching; one more and low-latency mode steps aside for the session");
            return;
        }
        if (!g.presentStalled.exchange(true, std::memory_order_relaxed)) {
            logger::warn("[Reflex] the flip queue stalled while frame generation was driving Reflex — low-latency "
                         "mode steps aside for generated frames for the REST OF THIS SESSION (real frames keep "
                         "the user's mode). If this line never appears, Reflex and "
                         "DLSS Frame Generation are running together as intended.");
        }
    }

    bool SteppedAsideAfterStall() noexcept { return g.presentStalled.load(std::memory_order_relaxed); }

    void CloseFrameWithoutPresent() noexcept
    {
        if (g.simOpen) {
            Marker(SIMULATION_END);
            g.simOpen = false;
            if (!g.frameOpen) {
                Marker(RENDERSUBMIT_START);
                g.frameOpen = true;
            }
        }
        if (!g.frameOpen) {
            return;
        }
        Marker(RENDERSUBMIT_END);
        Marker(PRESENT_START);
        Marker(PRESENT_END);
        g.frameOpen = false;
    }

    State Snapshot() noexcept
    {
        State state{};
        state.mode = Mode();
        state.boundDevice = g.snapBound.load(std::memory_order_relaxed);
        state.latched = g.latched.load(std::memory_order_relaxed);
        state.faulted = g.nvapiFaulted;
        state.idle = g.idle.load(std::memory_order_relaxed);
        try {
            const std::scoped_lock lock(g.reasonMutex);
            std::snprintf(state.reason, sizeof(state.reason), "%s", g.reason);
        } catch (...) {
        }
        state.sleeps = g.snapSleeps.load(std::memory_order_relaxed) + g.gameSleeps.load(std::memory_order_relaxed);
        state.tailSleeps = g.snapTailSleeps.load(std::memory_order_relaxed);
        state.gameSleeps = g.gameSleeps.load(std::memory_order_relaxed);
        static const double kMsPerTick = [] {
            LARGE_INTEGER f{};
            ::QueryPerformanceFrequency(&f);
            return f.QuadPart != 0 ? 1000.0 / static_cast<double>(f.QuadPart) : 0.0;
        }();
        state.capFrames = g.capFrames.load(std::memory_order_relaxed);
        if (state.capFrames != 0) {
            const auto n = static_cast<double>(state.capFrames);
            state.capPaceMs = static_cast<float>(static_cast<double>(g.capPaceTicks.load(std::memory_order_relaxed)) * kMsPerTick / n);
            state.capSleepMs = static_cast<float>(static_cast<double>(g.capSleepTicks.load(std::memory_order_relaxed)) * kMsPerTick / n);
        }
        state.freeFrames = g.freeFrames.load(std::memory_order_relaxed);
        if (state.freeFrames != 0) {
            state.freeSleepMs = static_cast<float>(static_cast<double>(g.freeSleepTicks.load(std::memory_order_relaxed)) * kMsPerTick / static_cast<double>(state.freeFrames));
        }
        state.gameSeamLive = g.seamLiveSnap.load(std::memory_order_relaxed);
        state.markers = g.snapMarkers.load(std::memory_order_relaxed);
        state.ringDepthMax = g.ringDepthMax.load(std::memory_order_relaxed);
        state.ringEmptyFinds = g.ringEmptyFinds.load(std::memory_order_relaxed);
        state.ringFullDrops = g.ringFullDrops.load(std::memory_order_relaxed);
        state.latencyMs = g.latencyMs.load(std::memory_order_relaxed);
        state.gpuFrameMs = g.gpuFrameMs.load(std::memory_order_relaxed);
        state.reportedFrames = g.reportedFrames.load(std::memory_order_relaxed);
        state.pacedByFrameGen = g.snapPaced.load(std::memory_order_relaxed);
        return state;
    }
}
