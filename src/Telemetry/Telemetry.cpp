#include "PCH.h"

#include "Telemetry/Telemetry.h"

#include "Platform/PresentPolicy.h"
#include "Platform/Reflex.h"

#define PSAPI_VERSION 2

#include <d3d11.h>
#include <dxgi1_4.h>
#include <psapi.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <thread>

namespace
{
    Telemetry::FrameTimings g_ring;
    std::int64_t g_lastFrameTicks = 0;
    double g_qpcPeriodMs = 0.0;

    Telemetry::SnapshotStore g_store;

    std::thread g_sampler;
    bool (*g_wanted)() noexcept { nullptr };
    std::atomic<bool> g_stopRequested{ false };
    std::atomic<bool> g_running{ false };

    std::atomic<std::int64_t> g_loadStartTicks{ 0 };
    std::atomic<float> g_lastLoadSeconds{ -1.0F };

    [[nodiscard]] std::int64_t QpcNow() noexcept
    {
        LARGE_INTEGER now{};
        return ::QueryPerformanceCounter(&now) ? now.QuadPart : 0;
    }

    [[nodiscard]] double QpcPeriodMs() noexcept
    {
        static const double period = [] {
            LARGE_INTEGER freq{};
            if (!::QueryPerformanceFrequency(&freq) || freq.QuadPart == 0) {
                return 0.0;
            }
            return 1000.0 / static_cast<double>(freq.QuadPart);
        }();
        return period;
    }

    [[nodiscard]] std::uint64_t FileTimeToU64(const FILETIME& a_time) noexcept
    {
        ULARGE_INTEGER value{};
        value.LowPart = a_time.dwLowDateTime;
        value.HighPart = a_time.dwHighDateTime;
        return value.QuadPart;
    }

    struct CpuSampler
    {
        bool primed{ false };
        std::uint64_t prevProcess{ 0 };
        std::uint64_t prevIdle{ 0 };
        std::uint64_t prevKernel{ 0 };
        std::uint64_t prevUser{ 0 };
        std::uint64_t prevWall{ 0 };
        double cores{ 1.0 };

        void Init() noexcept
        {
            const DWORD count = ::GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
            cores = count > 0 ? static_cast<double>(count) : 1.0;
        }

        void Sample(Telemetry::Snapshot& a_out) noexcept
        {
            FILETIME creation{};
            FILETIME exit{};
            FILETIME procKernel{};
            FILETIME procUser{};
            FILETIME sysIdle{};
            FILETIME sysKernel{};
            FILETIME sysUser{};
            if (!::GetProcessTimes(::GetCurrentProcess(), &creation, &exit, &procKernel, &procUser) ||
                !::GetSystemTimes(&sysIdle, &sysKernel, &sysUser)) {
                return;
            }
            FILETIME wallFt{};
            ::GetSystemTimeAsFileTime(&wallFt);

            const std::uint64_t process = FileTimeToU64(procKernel) + FileTimeToU64(procUser);
            const std::uint64_t idle = FileTimeToU64(sysIdle);
            const std::uint64_t kernel = FileTimeToU64(sysKernel);
            const std::uint64_t user = FileTimeToU64(sysUser);
            const std::uint64_t wall = FileTimeToU64(wallFt);

            if (primed && wall > prevWall) {
                const double wallDelta = static_cast<double>(wall - prevWall);
                const double procDelta = static_cast<double>(process - prevProcess);
                a_out.processCpuPercent =
                    static_cast<float>(100.0 * procDelta / (wallDelta * cores));

                const double kernelDelta = static_cast<double>(kernel - prevKernel);
                const double userDelta = static_cast<double>(user - prevUser);
                const double idleDelta = static_cast<double>(idle - prevIdle);
                const double totalDelta = kernelDelta + userDelta;
                if (totalDelta > 0.0) {
                    a_out.systemCpuPercent =
                        static_cast<float>(100.0 * (totalDelta - idleDelta) / totalDelta);
                }
                if (a_out.processCpuPercent < 0.0F) {
                    a_out.processCpuPercent = 0.0F;
                }
                if (a_out.systemCpuPercent < 0.0F) {
                    a_out.systemCpuPercent = 0.0F;
                }
                a_out.cpuValid = true;
            }

            prevProcess = process;
            prevIdle = idle;
            prevKernel = kernel;
            prevUser = user;
            prevWall = wall;
            primed = true;
        }
    };

    void SampleMemory(Telemetry::Snapshot& a_out) noexcept
    {
        PROCESS_MEMORY_COUNTERS_EX counters{};
        counters.cb = sizeof(counters);
        if (::GetProcessMemoryInfo(::GetCurrentProcess(),
                reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters))) {
            a_out.processPrivateBytes = static_cast<std::uint64_t>(counters.PrivateUsage);
            a_out.memValid = true;
        }
        MEMORYSTATUSEX status{};
        status.dwLength = sizeof(status);
        if (::GlobalMemoryStatusEx(&status)) {
            a_out.systemTotalBytes = status.ullTotalPhys;
            a_out.systemAvailBytes = status.ullAvailPhys;
            a_out.memValid = true;
        }
    }

    std::mutex g_adapterMutex;
    IDXGIAdapter3* g_adapter = nullptr;

    void SampleVram(Telemetry::Snapshot& a_out) noexcept
    {
        std::scoped_lock lock(g_adapterMutex);
        if (!g_adapter) {
            return;
        }
        DXGI_QUERY_VIDEO_MEMORY_INFO info{};
        if (SUCCEEDED(g_adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) {
            a_out.vramUsedBytes = info.CurrentUsage;
            a_out.vramBudgetBytes = info.Budget;
            a_out.vramValid = true;
        }
        DXGI_ADAPTER_DESC desc{};
        if (SUCCEEDED(g_adapter->GetDesc(&desc))) {
            a_out.vramTotalBytes = static_cast<std::uint64_t>(desc.DedicatedVideoMemory);
        }
    }

    using NvmlDevice = void*;
    using NvmlReturn = int;
    constexpr NvmlReturn kNvmlSuccess = 0;
    constexpr int kNvmlTemperatureGpu = 0;

    struct NvmlUtilization
    {
        unsigned int gpu;
        unsigned int memory;
    };

    struct Nvml
    {
        HMODULE module{ nullptr };
        NvmlDevice device{ nullptr };
        bool ready{ false };
        std::atomic<const char*> status{ "not initialized" };

        NvmlReturn(*init)(){ nullptr };
        NvmlReturn(*shutdown)(){ nullptr };
        NvmlReturn(*getCount)(unsigned int*){ nullptr };
        NvmlReturn(*getHandle)(unsigned int, NvmlDevice*){ nullptr };
        NvmlReturn(*getName)(NvmlDevice, char*, unsigned int){ nullptr };
        NvmlReturn(*getDriverVersion)(char*, unsigned int){ nullptr };
        NvmlReturn(*getUtilization)(NvmlDevice, NvmlUtilization*){ nullptr };
        NvmlReturn(*getTemperature)(NvmlDevice, int, unsigned int*){ nullptr };

        template <class T>
        [[nodiscard]] bool Bind(T& a_target, const char* a_name) noexcept
        {
            a_target = reinterpret_cast<T>(
                reinterpret_cast<void*>(::GetProcAddress(module, a_name)));
            return a_target != nullptr;
        }

        void Open() noexcept
        {
            module = ::LoadLibraryW(L"nvml.dll");
            if (!module) {
                status = "nvml.dll not present (non-NVIDIA GPU or no NVIDIA driver)";
                return;
            }
            if (!Bind(init, "nvmlInit_v2") || !Bind(shutdown, "nvmlShutdown") ||
                !Bind(getCount, "nvmlDeviceGetCount_v2") ||
                !Bind(getHandle, "nvmlDeviceGetHandleByIndex_v2") ||
                !Bind(getUtilization, "nvmlDeviceGetUtilizationRates") ||
                !Bind(getTemperature, "nvmlDeviceGetTemperature")) {
                status = "nvml.dll loaded but required exports are missing";
                return;
            }
            (void)Bind(getName, "nvmlDeviceGetName");
            (void)Bind(getDriverVersion, "nvmlSystemGetDriverVersion");

            if (init() != kNvmlSuccess) {
                status = "nvmlInit_v2 failed";
                return;
            }
            unsigned int count = 0;
            if (getCount(&count) != kNvmlSuccess || count == 0) {
                status = "NVML reports no devices";
                shutdown();
                return;
            }
            if (getHandle(0, &device) != kNvmlSuccess || !device) {
                status = "nvmlDeviceGetHandleByIndex_v2 failed";
                shutdown();
                return;
            }
            char name[96]{};
            if (getName && getName(device, name, sizeof(name)) == kNvmlSuccess) {
                logger::info("[Telemetry] NVML bound to GPU 0 of {}: {}", count, name);
            } else {
                logger::info("[Telemetry] NVML bound to GPU 0 of {}", count);
            }
            char driver[80]{};
            if (getDriverVersion && getDriverVersion(driver, sizeof(driver)) == kNvmlSuccess && driver[0] != '\0') {
                logger::info("[Telemetry] NVIDIA driver {} (quote this with the BUILD when reporting an issue)", driver);
            }
            if (count > 1) {
                logger::warn("[Telemetry] {} NVIDIA GPUs present — reporting index 0, which may not "
                             "be the adapter rendering the game",
                    count);
            }
            ready = true;
            status = "NVML active";
        }

        void Close() noexcept
        {
            if (ready && shutdown) {
                shutdown();
            }
            if (module) {
                ::FreeLibrary(module);
            }
            module = nullptr;
            device = nullptr;
            ready = false;
        }

        void Sample(Telemetry::Snapshot& a_out) noexcept
        {
            if (!ready) {
                return;
            }
            NvmlUtilization util{};
            unsigned int temperature = 0;
            const bool utilOk = getUtilization(device, &util) == kNvmlSuccess;
            const bool tempOk = getTemperature(device, kNvmlTemperatureGpu, &temperature) == kNvmlSuccess;
            if (utilOk) {
                a_out.gpuUtilPercent = util.gpu;
            }
            if (tempOk) {
                a_out.gpuTempC = temperature;
            }
            a_out.gpuValid = utilOk || tempOk;
        }
    };

    Nvml g_nvml;

    void SamplerMain() noexcept
    {
        CpuSampler cpu;
        cpu.Init();
        g_nvml.Open();

        {
            Telemetry::Snapshot discard{};
            cpu.Sample(discard);
        }

        using clock = std::chrono::steady_clock;
        auto nextSample = clock::now();
        bool wasWanted = true;
        while (!g_stopRequested.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

            const bool wanted = (g_wanted == nullptr) || g_wanted();
            if (!wanted) {
                wasWanted = false;
                continue;
            }
            if (!wasWanted) {
                Telemetry::Snapshot discard{};
                cpu.Sample(discard);
                wasWanted = true;
                nextSample = clock::now() + std::chrono::seconds(1);
                continue;
            }
            if (clock::now() < nextSample) {
                continue;
            }
            nextSample = clock::now() + std::chrono::seconds(1);

            Telemetry::Snapshot snapshot{};
            cpu.Sample(snapshot);
            SampleMemory(snapshot);
            SampleVram(snapshot);
            g_nvml.Sample(snapshot);

            const float lastLoad = g_lastLoadSeconds.load(std::memory_order_acquire);
            if (lastLoad >= 0.0F) {
                snapshot.lastLoadSeconds = lastLoad;
                snapshot.loadValid = true;
            }

            g_store.Publish(snapshot);
        }

        g_nvml.Close();
    }

    class LoadingMenuSink : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
    {
    public:
        [[nodiscard]] static LoadingMenuSink& GetSingleton() noexcept
        {
            static LoadingMenuSink instance;
            return instance;
        }

        RE::BSEventNotifyControl ProcessEvent(
            const RE::MenuOpenCloseEvent& a_event,
            RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
        {
            try {
                if (a_event.menuName == "LoadingMenu") {
                    if (a_event.opening) {
                        Telemetry::NoteLoadBegin();
                    } else {
                        Telemetry::NoteLoadEnd();
                        Telemetry::ResetFrameHistory();
                    }
                    Platform::PresentPolicy::SetLoadingScreenActive(a_event.opening);
                }
            } catch (...) {
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    std::atomic<bool> g_sinkRegistered{ false };
}

namespace Telemetry
{
    void Start(bool (*a_wanted)() noexcept) noexcept
    {
        if (g_running.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        try {
            g_stopRequested.store(false, std::memory_order_relaxed);
            g_wanted = a_wanted;
            g_sampler = std::thread(SamplerMain);

            static std::once_flag s_atexitOnce;
            std::call_once(s_atexitOnce, [] { std::atexit([] { Telemetry::Stop(); }); });

            logger::info("[Telemetry] sampler thread started (1 Hz)");
        } catch (const std::exception& e) {
            g_running.store(false, std::memory_order_release);
            logger::error("[Telemetry] sampler thread failed to start: {} — metrics unavailable", e.what());
        } catch (...) {
            g_running.store(false, std::memory_order_release);
            logger::error("[Telemetry] sampler thread failed to start — metrics unavailable");
        }
    }

    void Stop() noexcept
    {
        if (!g_running.exchange(false, std::memory_order_acq_rel)) {
            return;
        }
        g_stopRequested.store(true, std::memory_order_relaxed);
        try {
            if (g_sampler.joinable()) {
                g_sampler.join();
            }
        } catch (...) {
        }
        std::scoped_lock lock(g_adapterMutex);
        if (g_adapter) {
            g_adapter->Release();
            g_adapter = nullptr;
        }
    }

    namespace
    {
        std::int64_t g_lastStatsLogTicks = 0;
        char g_boundaryText[96]{};
        bool g_boundaryArmed = false;
        char g_pendingTag[96]{};
        bool g_pendingTagValid = false;
        constexpr double kStatsLogIntervalMs = 30000.0;
        constexpr std::uint32_t kMinSamplesToReport = 256;
    }

    void MarkFrameStatsBoundary(const char* a_reason) noexcept
    {
        if (!a_reason) {
            return;
        }
        std::snprintf(g_boundaryText, sizeof(g_boundaryText), "%s", a_reason);
        g_boundaryArmed = true;
    }

    void LogFrameStatsIfDue() noexcept
    {
        const double period = QpcPeriodMs();
        const std::int64_t now = QpcNow();
        if (period == 0.0 || now == 0) {
            return;
        }
        if (g_lastStatsLogTicks == 0) {
            g_lastStatsLogTicks = now;
            return;
        }

        if (g_boundaryArmed) {
            g_boundaryArmed = false;
            ResetFrameHistory();
            std::snprintf(g_pendingTag, sizeof(g_pendingTag), "%s", g_boundaryText);
            g_pendingTagValid = true;
            g_lastStatsLogTicks = now;
            return;
        }

        if ((static_cast<double>(now - g_lastStatsLogTicks) * period) < kStatsLogIntervalMs) {
            return;
        }

        const FrameStats s = CurrentFrameStats();
        if (s.sampleCount < kMinSamplesToReport) {
            return;
        }
        g_lastStatsLogTicks = now;

        const auto reflex = Platform::Reflex::Snapshot();
        logger::info("[Perf] {:.1f} fps avg ({:.2f} ms) | 1% low {:.1f} fps | 0.1% low {:.1f} fps | regularity {:.1f}% (mean step "
                     "between consecutive frames; 0 = even) | menu open {:.0f}% of the window | {} frames{}{}{}",
            static_cast<double>(s.averageFps), static_cast<double>(s.averageMs),
            static_cast<double>(s.onePercentLowFps),
            static_cast<double>(s.pointOnePercentLowFps),
            static_cast<double>(s.intervalRegularityPercent), static_cast<double>(s.menuOpenSharePercent), s.sampleCount,
            reflex.boundDevice != 0 && !reflex.latched
                ? fmt::format(" | Reflex: driver latency {:.1f} ms (the driver's last {} frames) | game-thread frames: {} capped "
                              "(our wait {:.2f} ms + the driver's sleep {:.2f} ms), {} uncapped (the driver's sleep {:.2f} ms) "
                              "(session counts and means) | ring depth max {}, unmarked {}",
                      reflex.latencyMs, reflex.reportedFrames, reflex.capFrames, reflex.capPaceMs, reflex.capSleepMs,
                      reflex.freeFrames, reflex.freeSleepMs, reflex.ringDepthMax, reflex.ringEmptyFinds)
                : "",
            g_pendingTagValid ? " | AFTER: " : "", g_pendingTagValid ? g_pendingTag : "");
        g_pendingTagValid = false;
    }

    void NoteFrame(bool a_menuOpen) noexcept
    {
        const double period = QpcPeriodMs();
        if (period == 0.0) {
            return;
        }
        const std::int64_t now = QpcNow();
        if (now == 0) {
            return;
        }
        if (g_lastFrameTicks != 0 && now > g_lastFrameTicks) {
            g_ring.Push(static_cast<float>(static_cast<double>(now - g_lastFrameTicks) * period), a_menuOpen);
        }
        g_lastFrameTicks = now;
    }

    void ResetFrameHistory() noexcept
    {
        g_ring.Reset();
        g_lastFrameTicks = 0;
    }

    FrameStats CurrentFrameStats() noexcept
    {
        return g_ring.Compute();
    }

    Snapshot Read() noexcept
    {
        return g_store.Read();
    }

    void NoteDevice(ID3D11Device* a_device) noexcept
    {
        IDXGIAdapter3* resolved = nullptr;
        if (a_device) {
            IDXGIDevice* dxgiDevice = nullptr;
            if (SUCCEEDED(a_device->QueryInterface(IID_PPV_ARGS(&dxgiDevice))) && dxgiDevice) {
                IDXGIAdapter* adapter = nullptr;
                if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter)) && adapter) {
                    (void)adapter->QueryInterface(IID_PPV_ARGS(&resolved));
                    adapter->Release();
                }
                dxgiDevice->Release();
            }
        }

        std::scoped_lock lock(g_adapterMutex);
        if (g_adapter) {
            g_adapter->Release();
        }
        g_adapter = resolved;
        if (a_device && !resolved) {
            static std::atomic<bool> s_logged{ false };
            if (!s_logged.exchange(true, std::memory_order_acq_rel)) {
                logger::warn("[Telemetry] IDXGIAdapter3 unavailable — VRAM readout disabled");
            }
        }
    }

    void NoteLoadBegin() noexcept
    {
        g_loadStartTicks.store(QpcNow(), std::memory_order_release);
    }

    void NoteLoadEnd() noexcept
    {
        const std::int64_t start = g_loadStartTicks.exchange(0, std::memory_order_acq_rel);
        const std::int64_t end = QpcNow();
        const double period = QpcPeriodMs();
        if (start == 0 || end <= start || period == 0.0) {
            return;
        }
        const double seconds = static_cast<double>(end - start) * period / 1000.0;
        g_lastLoadSeconds.store(static_cast<float>(seconds), std::memory_order_release);
        logger::info("[Telemetry] loading screen took {:.1f}s", seconds);
    }

    bool TryRegisterLoadingMenuSink() noexcept
    {
        if (g_sinkRegistered.load(std::memory_order_acquire)) {
            return true;
        }
        try {
            auto* ui = RE::UI::GetSingleton();
            if (!ui) {
                return false;
            }
            ui->RegisterSink<RE::MenuOpenCloseEvent>(&LoadingMenuSink::GetSingleton());
            g_sinkRegistered.store(true, std::memory_order_release);
            logger::info("[Telemetry] LoadingMenu event sink registered (load-time measurement live)");
            return true;
        } catch (...) {
            logger::warn("[Telemetry] LoadingMenu sink registration threw — load time unavailable");
            return false;
        }
    }

    const char* GpuSourceStatus() noexcept
    {
        return g_nvml.status.load(std::memory_order_acquire);
    }

    const char* CpuTempSourceStatus() noexcept
    {
        return "unavailable: needs kernel-level (ring-0) hardware access this mod does not ship";
    }
}
