// SPDX-License-Identifier: GPL-3.0-or-later
// Portions adapted from High FPS Physics Fix, Copyright (c) 2025 AntoniX35, MIT License.

#include "PCH.h"

#include "Platform/PresentMath.h"
#include "Platform/PresentPolicy.h"
#include "Platform/Reflex.h"

#include <dxgi1_6.h>

#include <atomic>
#include <mutex>
#include <thread>

namespace
{
    std::mutex g_mutex;
    Platform::PresentPolicy::Settings g_settings{};
    Platform::PresentPolicy::State g_state{};

    struct Pacer
    {
        long long nextDeadline{ 0 };
        std::uint32_t deadlineLimit{ 0 };
        HANDLE timer{ nullptr };
        std::once_flag timerOnce;
        const char* name{ "" };

        bool Pace(std::uint32_t a_limit) noexcept;
    };
    Pacer g_presentPacer{ 0, 0, nullptr, {}, "present" };
    Pacer g_gamePacer{ 0, 0, nullptr, {}, "game thread" };
    std::atomic<long long> g_gamePacedQpc{ 0 };
    std::atomic<bool> g_capOnGameThread{ false };
    std::atomic<std::uint32_t> g_fpsLimitAtomic{ 0 };
    std::atomic<std::uint32_t> g_loadingLimitAtomic{ 0 };
    std::atomic<bool> g_loadingActive{ false };

    [[nodiscard]] long long QpcNow() noexcept
    {
        LARGE_INTEGER v{};
        ::QueryPerformanceCounter(&v);
        return v.QuadPart;
    }

    [[nodiscard]] long long QpcFrequency() noexcept
    {
        static const long long freq = [] {
            LARGE_INTEGER f{};
            ::QueryPerformanceFrequency(&f);
            return f.QuadPart != 0 ? f.QuadPart : 10000000LL;
        }();
        return freq;
    }

    [[nodiscard]] Platform::SwapChainCaps QueryCaps() noexcept
    {
        Platform::SwapChainCaps caps{};
        IDXGIFactory* factory = nullptr;
        if (FAILED(::CreateDXGIFactory(__uuidof(IDXGIFactory),
                reinterpret_cast<void**>(&factory))) ||
            !factory) {
            return caps;
        }
        IDXGIFactory3* f3 = nullptr;
        if (SUCCEEDED(factory->QueryInterface(__uuidof(IDXGIFactory3),
                reinterpret_cast<void**>(&f3))) &&
            f3) {
            caps.flipSequential = true;
            f3->Release();
        }
        IDXGIFactory4* f4 = nullptr;
        if (SUCCEEDED(factory->QueryInterface(__uuidof(IDXGIFactory4),
                reinterpret_cast<void**>(&f4))) &&
            f4) {
            caps.flipDiscard = true;
            f4->Release();
        }
        IDXGIFactory5* f5 = nullptr;
        if (SUCCEEDED(factory->QueryInterface(__uuidof(IDXGIFactory5),
                reinterpret_cast<void**>(&f5))) &&
            f5) {
            BOOL tearing = FALSE;
            if (SUCCEEDED(f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing,
                    sizeof(tearing)))) {
                caps.tearing = tearing != FALSE;
            }
            f5->Release();
        }
        factory->Release();
        return caps;
    }

    constexpr UINT kDxgiAllowTearingFlag = 2048;
    bool Pacer::Pace(std::uint32_t a_limit) noexcept
    {
        if (a_limit == 0U) {
            nextDeadline = 0;
            deadlineLimit = 0;
            return false;
        }
        const long long freq = QpcFrequency();
        const long long period = freq / static_cast<long long>(a_limit);
        const long long now = QpcNow();

        const bool limitChanged = deadlineLimit != a_limit;
        deadlineLimit = a_limit;

        if (nextDeadline == 0 || limitChanged || now > nextDeadline + period) {
            nextDeadline = now + period;
            return false;
        }

        std::call_once(timerOnce, [this] {
            timer = ::CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
            if (timer == nullptr) {
                timer = ::CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
            }
            logger::info("[Present] frame limiter timer ({} pacer): {}", name, timer != nullptr
                ? "high-resolution waitable (sub-millisecond placement)"
                : "refused by the OS - falling back to thread sleeps, expect coarser pacing");
        });
        const long long spinThreshold = Platform::PaceSpinTicks(freq);
        bool waitedThisFrame = false;
        for (;;) {
            const long long remaining = nextDeadline - QpcNow();
            if (remaining <= 0) {
                break;
            }
            waitedThisFrame = true;
            if (remaining > spinThreshold) {
                bool waited = false;
                if (timer != nullptr) {
                    LARGE_INTEGER due{};
                    due.QuadPart = Platform::PaceTimerDue100ns(remaining, spinThreshold, freq);
                    if (due.QuadPart < 0 && ::SetWaitableTimerEx(timer, &due, 0, nullptr, nullptr, nullptr, 0) != 0) {
                        (void)::WaitForSingleObject(timer, 100);
                        waited = true;
                    }
                }
                if (!waited) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            } else {
                ::YieldProcessor();
            }
        }
        nextDeadline += period;
        return waitedThisFrame;
    }
}

namespace Platform
{
    namespace
    {
        void MirrorLimits() noexcept
        {
            g_fpsLimitAtomic.store(g_settings.fpsLimit, std::memory_order_relaxed);
            g_loadingLimitAtomic.store(g_settings.loadingScreenFpsLimit, std::memory_order_relaxed);
        }
    }

    void PresentPolicy::Configure(const Settings& a_settings) noexcept
    {
        const std::scoped_lock lock{ g_mutex };
        g_settings = a_settings;
        MirrorLimits();
        g_state.configured = true;
    }

    void PresentPolicy::SetUserOptions(bool a_vsyncEnabled, std::uint32_t a_vsyncInterval,
        std::uint32_t a_fpsLimit, std::uint32_t a_loadingScreenFpsLimit) noexcept
    {
        bool changed = false;
        {
            const std::scoped_lock lock{ g_mutex };
            changed = g_settings.enableVSync != a_vsyncEnabled ||
                      g_settings.vsyncInterval != a_vsyncInterval ||
                      g_settings.fpsLimit != a_fpsLimit ||
                      g_settings.loadingScreenFpsLimit != a_loadingScreenFpsLimit;
            g_settings.enableVSync = a_vsyncEnabled;
            g_settings.vsyncInterval = a_vsyncInterval;
            g_settings.fpsLimit = a_fpsLimit;
            g_settings.loadingScreenFpsLimit = a_loadingScreenFpsLimit;
            MirrorLimits();
        }
        if (!changed) {
            return;
        }
        logger::info("[Present] user options: vsync={} interval={} fps_limit={} loading_limit={} (the tearing flag is never requested "
                     "- the presenting chain is a borderless flip chain, so the compositor paces it)",
            a_vsyncEnabled ? "on" : "off", a_vsyncInterval,
            a_fpsLimit == 0U ? 0U : a_fpsLimit, a_loadingScreenFpsLimit);
    }

    void PresentPolicy::ApplyToDesc(DXGI_SWAP_CHAIN_DESC& a_desc) noexcept
    {
        Settings settings{};
        {
            const std::scoped_lock lock{ g_mutex };
            settings = g_settings;
        }
        const SwapChainCaps caps = QueryCaps();
        logger::info("[Present] capabilities: flip_sequential={} flip_discard={} tearing={}",
            caps.flipSequential, caps.flipDiscard, caps.tearing);

        const bool windowedBefore = a_desc.Windowed != FALSE;
        if (!windowedBefore) {
            a_desc.Windowed = TRUE;
            logger::info("[Present] the descriptor asked for exclusive fullscreen -> windowed: the presenting "
                         "chain cannot serve it (borderless fullscreen is what the player sees)");
        }
        const bool windowed = true;

        const SwapEffectChoice resolved =
            ResolveSwapEffect(SwapEffectChoice::kAuto, caps, windowed);

        const auto oldEffect = static_cast<std::uint32_t>(a_desc.SwapEffect);
        a_desc.SwapEffect = static_cast<DXGI_SWAP_EFFECT>(resolved);

        const std::uint32_t oldBuffers = a_desc.BufferCount;
        a_desc.BufferCount = ResolveBufferCount(0U, resolved, a_desc.BufferCount);

        const bool tearing = false;
        const bool tearingBefore = (a_desc.Flags & kDxgiAllowTearingFlag) != 0U;
        a_desc.Flags &= ~kDxgiAllowTearingFlag;
        if (tearingBefore) {
            logger::info("[Present] the caller's ALLOW_TEARING flag is dropped: tearing is not requested");
        }

        {
            const std::scoped_lock lock{ g_mutex };
            g_state.descApplied = true;
            g_state.chainAllowsTearing = tearing;
            g_state.resolvedEffect = resolved;
            g_state.resolvedBufferCount = a_desc.BufferCount;
        }

        logger::info("[Present] desc: effect {} -> {}, buffers {} -> {}, tearing {} -> {}, windowed {} (forced: the presenting chain is windowed)",
            oldEffect, static_cast<std::uint32_t>(resolved), oldBuffers, a_desc.BufferCount,
            tearingBefore, tearing, windowed);
    }

    PresentParams PresentPolicy::ParamsFor() noexcept
    {
        Settings settings{};
        bool tearingChain = false;
        {
            const std::scoped_lock lock{ g_mutex };
            settings = g_settings;
            tearingChain = g_state.chainAllowsTearing;
        }
        return ResolvePresentParams(settings.enableVSync, settings.vsyncInterval, tearingChain);
    }

    void PresentPolicy::SetLoadingScreenActive(bool a_active) noexcept
    {
        const std::scoped_lock lock{ g_mutex };
        g_loadingActive.store(a_active, std::memory_order_relaxed);
        if (g_state.loadingScreenActive != a_active) {
            g_state.loadingScreenActive = a_active;
            logger::info("[Present] loading screen {} — frame cap {}", a_active ? "entered" : "left",
                a_active ? "switches to the loading-screen limit" : "returns to the general limit");
        }
    }

    bool PresentPolicy::LimitAfterPresent() noexcept
    {
        const std::scoped_lock lock{ g_mutex };
        return g_settings.limitAfterPresent;
    }

    bool PresentPolicy::LoadingScreenActive() noexcept
    {
        return g_loadingActive.load(std::memory_order_relaxed);
    }

    bool PresentPolicy::CapOnGameThread() noexcept
    {
        return g_capOnGameThread.load(std::memory_order_relaxed);
    }

    void PresentPolicy::PaceFrame() noexcept
    {
        std::uint32_t limit = 0;
        {
            const std::scoped_lock lock{ g_mutex };
            limit = g_state.loadingScreenActive ? g_settings.loadingScreenFpsLimit
                                                : g_settings.fpsLimit;
            g_state.activeFpsLimit = limit;
        }
        const long long stamp = g_gamePacedQpc.load(std::memory_order_acquire);
        bool gamePaced = stamp != 0;
        if (gamePaced) {
            const long long freq = QpcFrequency();
            const long long window = limit != 0U ? (freq / static_cast<long long>(limit)) * 2 : freq / 20;
            gamePaced = (QpcNow() - stamp) < window;
        }
        g_capOnGameThread.store(gamePaced, std::memory_order_relaxed);
        if (gamePaced) {
            g_presentPacer.nextDeadline = 0;
            g_presentPacer.deadlineLimit = 0;
            return;
        }
        (void)g_presentPacer.Pace(limit);
    }

    bool PresentPolicy::PaceGameFrame() noexcept
    {
        const std::uint32_t limit = g_loadingActive.load(std::memory_order_relaxed)
                                        ? g_loadingLimitAtomic.load(std::memory_order_relaxed)
                                        : g_fpsLimitAtomic.load(std::memory_order_relaxed);
        const bool waited = g_gamePacer.Pace(limit);
        g_gamePacedQpc.store(QpcNow(), std::memory_order_release);
        return waited;
    }

    PresentPolicy::State PresentPolicy::CurrentState() noexcept
    {
        const std::scoped_lock lock{ g_mutex };
        return g_state;
    }
}
