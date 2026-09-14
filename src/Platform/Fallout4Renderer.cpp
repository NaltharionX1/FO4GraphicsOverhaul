// SPDX-License-Identifier: GPL-3.0-or-later
// Portions ported from Motion Vector Fixes (fo4test) by doodlum, GPL-3.0-or-later with its modding exception.

#include "PCH.h"

#include "Platform/AoGate.h"
#include "Platform/FrameGenEngine.h"
#include "Platform/Reflex.h"
#include "Platform/AoMailbox.h"
#include "Platform/AoRenderSwitch.h"
#include "Platform/AoSlot.h"
#include "Platform/EngineMemory.h"
#include "Platform/FovModes.h"
#include "Platform/BloomPass.h"
#include "Platform/EngineImod.h"
#include "Platform/GradingPass.h"
#include "Platform/GtaoPass.h"
#include "Platform/NeuralPass.h"
#include "Platform/DlaaSettings.h"
#include "Platform/OwnedSettings.h"
#include "Platform/Fallout4Renderer.h"
#include "Platform/RenderTargetProxy.h"
#include "Platform/FrameFingerprint.h"
#include "Platform/SidecarFrame.h"
#include "Platform/Streamline.h"
#include "Platform/FsrFrameGen.h"
#include "Platform/WiringAdapters.h"

#include "RE/Bethesda/BSGraphics.h"
#include "RE/Bethesda/BSShader.h"

#include <d3d11.h>
#include <intrin.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#ifdef near
#    undef near
#endif
#ifdef far
#    undef far
#endif

namespace
{
    constexpr std::size_t kMainDepth = 2;

    constexpr REL::ID kRendererDataPreNG{ 1235449 };
    constexpr REL::ID kStatePreNG{ 600795 };
    constexpr REL::ID kRenderTargetManagerPreNG{ 1508457 };

    bool historyResetPending{ true };
    std::atomic<bool> g_historyResetRequested{ false };
    std::atomic<std::uint64_t> g_historyEpoch{ 0 };
    std::atomic<const char*> g_historyEpochReason{ "" };

    std::atomic<std::uint32_t> g_dlaaSuccessFrame{ 0xFFFFFFFFu };
    std::atomic<std::uint32_t> g_dlaaAttemptFrame{ 0xFFFFFFFFu };
    std::atomic<std::uint32_t> g_dlaaFirstAttemptFrame{ 0xFFFFFFFFu };

    std::atomic<float> g_dynResRatio{ 1.0F };
    std::atomic<bool> g_dynResBlocked{ false };
    std::atomic<bool> g_dynResBlockingMenuOpen{ false };
    std::atomic<bool> g_dynResLoadingActive{ false };
    std::atomic<bool> g_dynResSinkRegistered{ false };
    std::atomic<int> g_dynResBlockingMenuCount{ 0 };
    std::uint32_t g_ratioCachedMode{ 0xFFFFFFFFu };
    std::uint32_t g_ratioCachedOutW{ 0 };
    std::uint32_t g_ratioCachedOutH{ 0 };
    Platform::AaEffectiveEngine g_ratioCachedEngine{ Platform::AaEffectiveEngine::kUnresolved };
    float g_ratioCached{ 1.0F };

    [[nodiscard]] inline bool AaEngineOn() noexcept
    {
        const Platform::AaEffectiveEngine effective =
            Platform::Streamline::AppliedEffectiveEngine();
        return Platform::Streamline::DlaaEnabled() && Platform::AaEffectiveEngineReady(effective);
    }

    [[nodiscard]] RE::BSGraphics::RendererData* RendererData() noexcept
    {
        REL::Relocation<RE::BSGraphics::RendererData**> singleton{ kRendererDataPreNG };
        return *singleton;
    }

    [[nodiscard]] RE::BSGraphics::State* RendererState() noexcept
    {
        REL::Relocation<RE::BSGraphics::State*> singleton{ kStatePreNG };
        return singleton.get();
    }

    [[nodiscard]] RE::BSGraphics::RenderTargetManager* RenderTargetManager() noexcept
    {
        REL::Relocation<RE::BSGraphics::RenderTargetManager*> singleton{ kRenderTargetManagerPreNG };
        return singleton.get();
    }

    [[nodiscard]] ID3D11SamplerState** SamplerStateArray() noexcept
    {
        return reinterpret_cast<ID3D11SamplerState**>(REL::ID(44312).address());
    }

    std::atomic<std::uint64_t> g_samplerMutationSerial{ 0 };

    [[nodiscard]] Platform::SamplerHandle LoadSamplerSlot(std::size_t index) noexcept
    {
        ID3D11SamplerState** const table = SamplerStateArray();
        return table ? reinterpret_cast<Platform::SamplerHandle>(table[index]) : 0;
    }

    void StoreSamplerSlot(std::size_t index, Platform::SamplerHandle handle) noexcept
    {
        if (ID3D11SamplerState** const table = SamplerStateArray()) {
            table[index] = reinterpret_cast<ID3D11SamplerState*>(handle);
            g_samplerMutationSerial.fetch_add(1, std::memory_order_release);
        }
    }

    void RetainSamplerHandle(Platform::SamplerHandle handle) noexcept
    {
        if (handle != 0) {
            reinterpret_cast<ID3D11SamplerState*>(handle)->AddRef();
        }
    }

    void ReleaseSamplerHandle(Platform::SamplerHandle handle) noexcept
    {
        if (handle != 0) {
            reinterpret_cast<ID3D11SamplerState*>(handle)->Release();
        }
    }

    [[nodiscard]] bool DescribeSamplerHandle(
        Platform::SamplerHandle handle, Platform::SamplerDescriptor& output) noexcept
    {
        if (handle == 0) {
            return false;
        }
        D3D11_SAMPLER_DESC desc{};
        reinterpret_cast<ID3D11SamplerState*>(handle)->GetDesc(&desc);
        Platform::D3D11SamplerDescFields fields{};
        static_assert(sizeof(fields) == sizeof(desc),
            "D3D11SamplerDescFields must stay byte-for-byte the same size as the real D3D11_SAMPLER_DESC");
        std::memcpy(&fields, &desc, sizeof(fields));
        output = Platform::PackSamplerDescriptor(fields);
        return true;
    }

    [[nodiscard]] bool CreateSamplerFromDescriptor(Platform::SamplerHandle device,
        const Platform::SamplerDescriptor& descriptor, Platform::SamplerHandle& output) noexcept
    {
        auto* const dev = reinterpret_cast<ID3D11Device*>(device);
        if (!dev) {
            return false;
        }
        const Platform::D3D11SamplerDescFields fields = Platform::UnpackSamplerDescriptor(descriptor);
        D3D11_SAMPLER_DESC desc{};
        std::memcpy(&desc, &fields, sizeof(desc));
        ID3D11SamplerState* created = nullptr;
        if (FAILED(dev->CreateSamplerState(&desc, &created)) || !created) {
            return false;
        }
        output = reinterpret_cast<Platform::SamplerHandle>(created);
        return true;
    }

    Platform::SamplerGenerationController* g_samplerController{ nullptr };

    [[nodiscard]] Platform::SamplerGenerationController& SamplerController()
    {
        if (!g_samplerController) {
            Platform::SamplerTableCallbacks table;
            table.load = &LoadSamplerSlot;
            table.store = &StoreSamplerSlot;
            table.mutationSerial = &g_samplerMutationSerial;
            Platform::SamplerBackendCallbacks backend;
            backend.retain = &RetainSamplerHandle;
            backend.release = &ReleaseSamplerHandle;
            backend.describe = &DescribeSamplerHandle;
            backend.makeBiasedDescriptor = &Platform::MakeBiasedDescriptor;
            backend.create = &CreateSamplerFromDescriptor;
            g_samplerController =
                new Platform::SamplerGenerationController(std::move(table), std::move(backend));
        }
        return *g_samplerController;
    }

    void* g_samplerLastDevice{ nullptr };
    std::uint64_t g_samplerDeviceEpoch{ 0 };

    [[nodiscard]] Platform::SamplerDeviceKey CurrentSamplerDeviceKey() noexcept
    {
        void* device = nullptr;
        if (const auto renderer = RendererData()) {
            device = renderer->device;
        }
        if (device != g_samplerLastDevice) {
            ++g_samplerDeviceEpoch;
            g_samplerLastDevice = device;
        }
        return Platform::SamplerDeviceKey{
            reinterpret_cast<Platform::SamplerHandle>(device), g_samplerDeviceEpoch
        };
    }

    Platform::SamplerGenerationController::Ticket g_samplerTicket;
    Platform::SamplerDeviceKey g_samplerDeviceKey;
    Platform::AppliedSamplerIntent g_samplerIntent;

    void RefreshSamplerIntent() noexcept
    {
        try {
            g_samplerDeviceKey = CurrentSamplerDeviceKey();
            Platform::AaRequest applied = Platform::Streamline::AppliedRequest();
            const float dynRatio = g_dynResRatio.load(std::memory_order_relaxed);
            const float trsScale = Platform::Streamline::CurrentResolutionScale();
            const float effective = Platform::Streamline::EffectiveOutOverRender();
            const float renderToOutput = effective > 0.0F
                ? 1.0F / effective
                : (trsScale > 1.001F ? 1.0F / trsScale
                                     : (dynRatio > 0.0F ? dynRatio : 1.0F));
            if (renderToOutput > 0.0F && renderToOutput < 0.999F) {
                const float delta = std::log2(renderToOutput);
                applied.mipBias = Platform::ClampMipBias(applied.mipBias + delta);
                applied.fsrMipBias = Platform::ClampMipBias(applied.fsrMipBias + delta);
            }
            g_samplerIntent = Platform::DeriveSamplerIntent(
                applied, Platform::Streamline::AppliedDamagedScope());
            g_samplerTicket = SamplerController().Refresh(g_samplerDeviceKey, g_samplerIntent);
        } catch (...) {
            Guard::catchTotal.fetch_add(1, std::memory_order_relaxed);
        }
    }

    std::atomic<std::uint32_t> g_orderStep{ 0 };

    void NoteFrameOrder(const char* a_tag) noexcept
    {
        constexpr std::uint32_t kStepsToLog = 3;
        if (g_orderStep.load(std::memory_order_relaxed) >= kStepsToLog) {
            return;
        }
        const std::uint32_t step = g_orderStep.fetch_add(1, std::memory_order_relaxed);
        if (step < kStepsToLog) {
            logger::info("[Order] {}: {}", step, a_tag);
            if (step + 1U == kStepsToLog) {
                logger::info("[Order] ---- observed frame order above; GTAO compute MUST follow the "
                             "geometry pass or it reads the previous frame's depth and normals ----");
            }
        }
    }

    struct SamplerPassDeferred
    {
        static void thunk(void* self)
        {
            {
                Platform::SamplerGenerationController* const controller = g_samplerController;
                if (controller == nullptr) {
                    func(self);
                } else {
                    Platform::SamplerGenerationController::Scope scope(
                        *controller, g_samplerTicket, g_samplerDeviceKey, g_samplerIntent);
                    func(self);
                }
            }

            NoteFrameOrder("geometry pass done -> GTAO compute");
            Platform::AoSlot::ComputeForIntegration();
        }
        static inline REL::Relocation<decltype(thunk)> func;
    };
    struct SamplerPassForward
    {
        static void thunk(void* self)
        {
            Platform::SamplerGenerationController* const controller = g_samplerController;
            if (!controller) {
                func(self);
                return;
            }
            Platform::SamplerGenerationController::Scope scope(
                *controller, g_samplerTicket, g_samplerDeviceKey, g_samplerIntent);
            func(self);
        }
        static inline REL::Relocation<decltype(thunk)> func;
    };

    void InstallSamplerHooks() noexcept
    {
        try {
            auto& tramp = F4SE::GetTrampoline();
            REL::Relocation<std::uintptr_t> deferred{ REL::ID(984743), 0x17F };
            REL::Relocation<std::uintptr_t> forward{ REL::ID(984743), 0x1C9 };
            SamplerPassDeferred::func = tramp.write_call<5>(deferred.address(), SamplerPassDeferred::thunk);
            SamplerPassForward::func = tramp.write_call<5>(forward.address(), SamplerPassForward::thunk);
            logger::info("[Renderer] mip-bias sampler hooks installed (@984743+0x17F, +0x1C9)");
        } catch (const std::exception& e) {
            logger::error("[Renderer] sampler hooks failed: {}", e.what());
        } catch (...) {
            logger::error("[Renderer] sampler hooks failed with an unknown C++ exception");
        }
    }

    [[nodiscard]] float Halton(std::uint32_t index, std::uint32_t base) noexcept
    {
        float f = 1.0F;
        float r = 0.0F;
        while (index > 0) {
            f /= static_cast<float>(base);
            r += f * static_cast<float>(index % base);
            index /= base;
        }
        return r;
    }

    float g_jitterPixelX{ 0.0F };
    float g_jitterPixelY{ 0.0F };
    std::uint32_t g_jitterFrame{ 0 };
    std::atomic<float> g_jitterScale{ 1.0F };

    Platform::FrameStamp g_frameStamp;

    [[nodiscard]] bool DlaaEvalStarved(std::uint32_t now) noexcept
    {
        constexpr std::uint32_t kEvalFailWindow = 120;
        const std::uint32_t attempt = g_dlaaAttemptFrame.load(std::memory_order_acquire);
        const std::uint32_t first = g_dlaaFirstAttemptFrame.load(std::memory_order_acquire);
        const std::uint32_t success = g_dlaaSuccessFrame.load(std::memory_order_acquire);
        const bool attemptingNow = attempt != 0xFFFFFFFFu && (now - attempt) <= 2U;
        const bool pastGrace = first != 0xFFFFFFFFu && (now - first) > kEvalFailWindow;
        const bool noRecentSuccess = (now - success) > kEvalFailWindow;
        return attemptingNow && pastGrace && noRecentSuccess;
    }

    void UpdateDynamicResolutionDrive(RE::BSGraphics::State* a_state) noexcept
    {
        float ratio = 1.0F;
        bool blocked = false;
        try {
            const std::uint32_t mode = Platform::Streamline::CurrentQualityMode();
            if (mode != 0U && AaEngineOn()) {
                const bool loading = g_dynResLoadingActive.load(std::memory_order_relaxed);
                const bool menuOpen = g_dynResBlockingMenuOpen.load(std::memory_order_relaxed);
                constexpr std::uint32_t kStarvedProbeInterval = 300U;
                constexpr std::uint32_t kStarvedProbeBurst = 3U;
                static bool s_starvedSticky = false;
                static std::uint32_t s_probeAnchor = 0U;
                const std::uint32_t successNow = g_dlaaSuccessFrame.load(std::memory_order_acquire);
                if (a_state && successNow != 0xFFFFFFFFu &&
                    (a_state->frameCount - successNow) <= 2U) {
                    s_starvedSticky = false;
                }
                if (a_state != nullptr && DlaaEvalStarved(a_state->frameCount)) {
                    if (!s_starvedSticky) {
                        s_probeAnchor = a_state->frameCount;
                    }
                    s_starvedSticky = true;
                }
                bool starved = s_starvedSticky;
                if (s_starvedSticky && a_state) {
                    const std::uint32_t sinceAnchor = a_state->frameCount - s_probeAnchor;
                    if (sinceAnchor >= kStarvedProbeInterval) {
                        s_probeAnchor = a_state->frameCount;
                        starved = false;
                    } else if (sinceAnchor < kStarvedProbeBurst) {
                        starved = false;
                    }
                }
                if (loading || menuOpen || starved) {
                    blocked = true;
                } else if (a_state && a_state->screenWidth > 0 && a_state->screenHeight > 0) {
                    const auto engineNow = Platform::Streamline::AppliedEffectiveEngine();
                    if (g_ratioCachedMode != mode || g_ratioCachedOutW != a_state->screenWidth ||
                        g_ratioCachedOutH != a_state->screenHeight ||
                        g_ratioCachedEngine != engineNow) {
                        g_ratioCached = Platform::Streamline::QueryOptimalRatio(
                            mode, a_state->screenWidth, a_state->screenHeight);
                        g_ratioCachedMode = mode;
                        g_ratioCachedOutW = a_state->screenWidth;
                        g_ratioCachedOutH = a_state->screenHeight;
                        g_ratioCachedEngine = engineNow;
                    }
                    ratio = g_ratioCached;
                }
            } else if (mode != 0U) {
                blocked = true;
            }
            if (auto* const targets = RenderTargetManager()) {
                targets->dynamicWidthRatio = ratio;
                targets->dynamicHeightRatio = ratio;
                targets->isDynamicResolutionCurrentlyActivated = ratio != 1.0F;
            } else {
                ratio = 1.0F;
                blocked = blocked || Platform::Streamline::CurrentQualityMode() != 0U;
            }
        } catch (...) {
            ratio = 1.0F;
            blocked = true;
        }
        g_dynResRatio.store(ratio, std::memory_order_relaxed);
        g_dynResBlocked.store(blocked, std::memory_order_relaxed);
        {
            static std::uint32_t s_lastMode = 0xFFFFFFFFu;
            static float s_lastRatio = -1.0F;
            static float s_lastTrs = -1.0F;
            static bool s_lastBlocked = false;
            const std::uint32_t modeNow = Platform::Streamline::CurrentQualityMode();
            const float trsNow = Platform::Streamline::CurrentResolutionScale();
            if (modeNow != s_lastMode || ratio != s_lastRatio || trsNow != s_lastTrs ||
                blocked != s_lastBlocked) {
                s_lastMode = modeNow;
                s_lastRatio = ratio;
                s_lastTrs = trsNow;
                s_lastBlocked = blocked;
                logger::info("[DynRes] mode={} trs={:.2f} -> ratio={:.3f} blocked={}",
                    modeNow, trsNow, ratio, blocked);
            }
        }
        Platform::RenderTargetProxy::UpdateForRatio(ratio);
    }

    class DynResMenuSink final : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
    {
    public:
        [[nodiscard]] static DynResMenuSink& GetSingleton() noexcept
        {
            static DynResMenuSink instance;
            return instance;
        }

        RE::BSEventNotifyControl ProcessEvent(
            const RE::MenuOpenCloseEvent& a_event,
            RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
        {
            try {
                if (a_event.menuName == "LoadingMenu") {
                    g_dynResLoadingActive.store(a_event.opening, std::memory_order_relaxed);
                } else if (a_event.menuName == "PipboyMenu" || a_event.menuName == "ExamineMenu" ||
                           a_event.menuName == "TerminalMenu") {
                    Platform::FovModes::OnMenuBoundary(a_event.menuName.c_str(), a_event.opening);
                    if (a_event.opening) {
                        const int now =
                            g_dynResBlockingMenuCount.fetch_add(1, std::memory_order_relaxed) + 1;
                        g_dynResBlockingMenuOpen.store(now > 0, std::memory_order_relaxed);
                    } else {
                        int now =
                            g_dynResBlockingMenuCount.fetch_sub(1, std::memory_order_relaxed) - 1;
                        if (now < 0) {
                            g_dynResBlockingMenuCount.store(0, std::memory_order_relaxed);
                            now = 0;
                        }
                        g_dynResBlockingMenuOpen.store(now > 0, std::memory_order_relaxed);
                    }
                }
            } catch (...) {
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    struct JitterHook
    {
        static void thunk(RE::BSGraphics::RenderTargetManager* self, void* a2, void* a3, void* a4, void* a5)
        {
            if (const auto renderer = RendererData()) {
                Platform::Streamline::ObserveAaDevice(
                    reinterpret_cast<ID3D11Device*>(renderer->device));
                Platform::Streamline::NoteDeviceObserved(
                    reinterpret_cast<ID3D11Device*>(renderer->device));
            }
            const std::uint64_t frameId = g_frameStamp.Next();
            Platform::SidecarFrame::BeginFrame(frameId);
            NoteFrameOrder("frame begin (JitterHook, PRE-geometry)");

            Platform::AoMailbox::Pump(frameId);

            constexpr std::uint64_t kOwnedBindFrame = 60U;
            if (frameId == kOwnedBindFrame) {
                Platform::OwnedSettings::Bind();
                Platform::AoGate::Bind();
                Platform::AoGate::SetSessionHold(true);
            } else if (frameId > kOwnedBindFrame) {
                Platform::OwnedSettings::EnforceFrame();
                Platform::FovModes::Tick();
                Platform::AoGate::EnforceFrame();
                Platform::EngineImod::FrameTick();
            }
            Platform::Streamline::PumpMailbox(frameId);
            func(self, a2, a3, a4, a5);
            UpdateDynamicResolutionDrive(RendererState());
            RefreshSamplerIntent();
            try {
                const auto state = RendererState();
                if (state) {
                    Platform::Reflex::FrameStart();
                }
                if (!AaEngineOn() || g_dynResBlocked.load(std::memory_order_relaxed) ||
                    Platform::Streamline::DrainAffectsActiveEngine()) {
                    g_jitterPixelX = 0.0F;
                    g_jitterPixelY = 0.0F;
                    return;
                }
                if (!state || state->screenWidth == 0 || state->screenHeight == 0) {
                    return;
                }
                const float dynRatio = g_dynResRatio.load(std::memory_order_relaxed);
                const float trsScale = Platform::Streamline::CurrentResolutionScale();
                const float effective = Platform::Streamline::EffectiveOutOverRender();
                const float r = effective > 0.0F
                    ? effective
                    : (trsScale > 1.001F
                          ? trsScale
                          : (dynRatio > 0.0F && dynRatio < 0.999F ? 1.0F / dynRatio : 1.0F));
                std::uint32_t phaseCount = static_cast<std::uint32_t>(std::ceil(8.0F * r * r));
                phaseCount = phaseCount < 8U ? 8U : (phaseCount > 64U ? 64U : phaseCount);
                const std::uint32_t idx = (g_jitterFrame % phaseCount) + 1;
                ++g_jitterFrame;
                const float scale = Platform::ClampJitterScale(g_jitterScale.load(std::memory_order_relaxed));
                const float jx = (Halton(idx, 2) - 0.5F) * scale;
                const float jy = (Halton(idx, 3) - 0.5F) * scale;
                state->offsetX = 2.0F * -jx / static_cast<float>(state->screenWidth);
                state->offsetY = 2.0F * jy / static_cast<float>(state->screenHeight);
                g_jitterPixelX = jx;
                g_jitterPixelY = jy;
                static std::atomic<bool> logged{ false };
                if (!logged.exchange(true, std::memory_order_relaxed)) {
                    logger::info("[DLAA] jitter injection active (Halton, phaseCount={})", phaseCount);
                }
            } catch (...) {
            }
        }
        static inline REL::Relocation<decltype(thunk)> func;
    };

    void InstallJitterHook() noexcept
    {
        try {
            REL::Relocation<std::uintptr_t> target{ REL::ID(984743), 0x14B };
            JitterHook::func = F4SE::GetTrampoline().write_call<5>(target.address(), JitterHook::thunk);
            logger::info("[Renderer] jitter-injection hook installed at REL(984743)+0x14B (Halton, doodlum model)");
        } catch (const std::exception& e) {
            logger::error("[Renderer] jitter hook failed: {}", e.what());
        } catch (...) {
            logger::error("[Renderer] jitter hook failed with an unknown C++ exception");
        }
    }

    inline constexpr std::uint16_t kFxaaEffectIndex = 17;
    inline constexpr std::uint16_t kTaaEffectIndex = 18;

    std::atomic<bool> g_fxaaSettled{ false };

    bool (*g_fxaaIsActiveOriginal)(void*){ nullptr };

    bool FxaaIsActiveThunk(void* self)
    {
        (void)self;
        static std::atomic<bool> loggedVeto{ false };
        if (!loggedVeto.load(std::memory_order_relaxed) && !loggedVeto.exchange(true)) {
            logger::info("[DLAA] FXAA vetoed at the root (IsActive->false, unconditional) — it can "
                         "no longer be enabled by the console, the INI, or the game's own options "
                         "menu; the same absolute status the TAA resolve has had since v7");
        }
        return false;
    }

    bool ShouldLogBlock(std::atomic<std::uint64_t>& a_counter, std::uint64_t& a_count) noexcept
    {
        a_count = a_counter.fetch_add(1, std::memory_order_relaxed) + 1;
        return a_count <= 3 || (a_count % 1000) == 0;
    }

    void EnsureDlaaGameSettings() noexcept
    {
        try {
            static std::atomic<bool> loggedArmed{ false };
            if (!loggedArmed.exchange(true, std::memory_order_relaxed)) {
                logger::info("[DLAA] AA enforcement armed (DLSS-or-nothing: TAA resolve + FXAA vetoed at the root, one-time; the TAA pipeline bool is a held switch — one compare per eval frame, written only on transition)");
            }
            bool wantTaaPipeline = AaEngineOn() && !g_dynResBlocked.load(std::memory_order_relaxed);

            if (!wantTaaPipeline) {
                g_dlaaFirstAttemptFrame.store(0xFFFFFFFFu, std::memory_order_release);
            } else {
                if (const auto st = RendererState()) {
                    if (DlaaEvalStarved(st->frameCount)) {
                        wantTaaPipeline = false;
                        static std::atomic<std::uint64_t> starved{ 0 };
                        std::uint64_t count = 0;
                        if (ShouldLogBlock(starved, count)) {
                            logger::warn("[DLAA] eval attempts keep failing (no success for a full window) — jitter/MV pipeline OFF so the image stays clean native no-AA (never TAA); recovers on the next successful eval (block #{})",
                                count);
                        }
                    }
                }
            }
            static std::atomic<std::uintptr_t> pipelineBool{ 0 };
            static std::atomic<bool> pipelineFailed{ false };
            std::uintptr_t address = pipelineBool.load(std::memory_order_acquire);
            if (address == 0 && !pipelineFailed.load(std::memory_order_relaxed)) {
                const std::uintptr_t resolved = REL::ID(460417).address();
                if (!Platform::EngineMemory::WithinGameImage(resolved, sizeof(bool))) {
                    pipelineFailed.store(true, std::memory_order_relaxed);
                    logger::warn("[DLAA] the TAA-pipeline bool resolved outside the game image "
                                 "({:#x}) — wrong build; pipeline enforcement disabled for the "
                                 "session rather than writing into whatever lives there",
                        resolved);
                } else {
                    pipelineBool.store(resolved, std::memory_order_release);
                    address = resolved;
                }
            }
            if (address != 0) {
                bool current = false;
                if (!Platform::EngineMemory::SafeRead(&current,
                        reinterpret_cast<const void*>(address), sizeof(current))) {
                    pipelineFailed.store(true, std::memory_order_relaxed);
                    pipelineBool.store(0, std::memory_order_relaxed);
                    logger::warn("[DLAA] TAA-pipeline bool read failed — enforcement disabled for "
                                 "the session");
                } else if (current != wantTaaPipeline) {
                    static std::atomic<std::uint64_t> blocked{ 0 };
                    std::uint64_t count = 0;
                    if (ShouldLogBlock(blocked, count)) {
                        logger::info("[DLAA] engine TAA pipeline was {} — forced {} (tracks the DLAA toggle; TAA itself is never a fallback; block #{})",
                            current ? "ON" : "OFF", wantTaaPipeline ? "ON (feeds DLSS jitter+MVs)" : "OFF (native, no AA)", count);
                    }
                    if (!Platform::EngineMemory::SafeWrite(reinterpret_cast<void*>(address),
                            &wantTaaPipeline, sizeof(wantTaaPipeline))) {
                        pipelineFailed.store(true, std::memory_order_relaxed);
                        pipelineBool.store(0, std::memory_order_relaxed);
                        logger::warn("[DLAA] TAA-pipeline bool write failed — enforcement disabled "
                                     "for the session");
                    }
                }
            }
        } catch (...) {
            static std::atomic<bool> warned{ false };
            if (!warned.exchange(true, std::memory_order_relaxed)) {
                logger::warn("[DLAA] could not enforce the engine TAA pipeline state (REL 460417 unresolved)");
            }
        }
        try {
            if (!g_fxaaSettled.load(std::memory_order_relaxed)) {
                auto* const manager = RE::ImageSpaceManager::GetSingleton();
                if (manager && manager->effectList.capacity() > kTaaEffectIndex) {
                    auto* const taa = manager->effectList[kTaaEffectIndex];
                    auto* const fxaa = manager->effectList[kFxaaEffectIndex];
                    std::uintptr_t taaVtable = 0;
                    const bool anchored = taa != nullptr &&
                        Platform::EngineMemory::SafeRead(&taaVtable, taa, sizeof(taaVtable)) &&
                        taaVtable == RE::VTABLE::ImageSpaceEffectTemporalAA[0].address();
                    if (anchored && fxaa != nullptr) {
                        const bool wasActive = fxaa->isActive;
                        if (wasActive) {
                            fxaa->isActive = false;
                        }

                        if (g_fxaaIsActiveOriginal == nullptr) {
                            std::uintptr_t fxaaVtable = 0;
                            if (Platform::EngineMemory::SafeRead(&fxaaVtable, fxaa,
                                    sizeof(fxaaVtable)) &&
                                fxaaVtable != 0) {
                                try {
                                    g_fxaaIsActiveOriginal =
                                        reinterpret_cast<bool (*)(void*)>(
                                            REL::Relocation<std::uintptr_t>{ fxaaVtable }
                                                .write_vfunc(0x8, FxaaIsActiveThunk));
                                    logger::info("[DLAA] FXAA veto installed: effectList[{}]'s own "
                                                 "vtable @{:#x}, IsActive (vfunc 0x8) -> false "
                                                 "UNCONDITIONALLY. Derived from the live object "
                                                 "behind the [{}]=TemporalAA anchor, not a guessed "
                                                 "ID.",
                                        kFxaaEffectIndex, fxaaVtable, kTaaEffectIndex);
                                } catch (...) {
                                    logger::warn("[DLAA] FXAA veto could not be installed — falling "
                                                 "back to the effect-list write alone, which the "
                                                 "options menu can still undo");
                                }
                            }
                        }
                        g_fxaaSettled.store(true, std::memory_order_relaxed);
                        logger::info("[DLAA] effectList[{}] (FXAA, anchored by [{}]=TemporalAA) was "
                                     "{} — SETTLED {}. Runs once per session; from here the "
                                     "IsActive veto owns FXAA absolutely (the watched-command "
                                     "re-arm died with the refini finding).",
                            kFxaaEffectIndex, kTaaEffectIndex, wasActive ? "ACTIVE" : "already off",
                            wasActive ? "by forcing it OFF" : "with no write needed");
                    }
                }
            }
        } catch (...) {
            static std::atomic<bool> warned{ false };
            if (!warned.exchange(true, std::memory_order_relaxed)) {
                logger::warn("[DLAA] could not reach ImageSpaceManager effectList — FXAA root-off enforcement unavailable this session");
            }
        }
    }

    bool g_loggedMainTempEval{ false };

    bool EvalDlaaOnMainTemp() noexcept
    {
        try {
            const Platform::AaEffectiveEngine effective =
                Platform::Streamline::AppliedEffectiveEngine();
            if (!Platform::Streamline::DlaaEnabled() || !Platform::AaEffectiveEngineReady(effective)) {
                if (const auto renderer = RendererData()) {
                    auto* context =
                        reinterpret_cast<ID3D11DeviceContext*>(Platform::Fallout4Renderer::Context());
                    auto* colorTex =
                        renderer->renderTargets[Platform::Fallout4RenderTargetIndex::kMainTemp].texture;
                    if (context && colorTex) {
                        D3D11_TEXTURE2D_DESC colorDesc{};
                        colorTex->GetDesc(&colorDesc);
                        context->OMSetRenderTargets(0, nullptr, nullptr);
                        (void)Platform::Streamline::SharpenOnly(
                            context, reinterpret_cast<ID3D11Resource*>(colorTex), colorDesc);
                    }
                }
                return false;
            }
            if (g_dynResBlocked.load(std::memory_order_relaxed)) {
                g_dlaaFirstAttemptFrame.store(0xFFFFFFFFu, std::memory_order_release);
                return false;
            }
            if (const auto st = RendererState()) {
                const std::uint32_t now = st->frameCount;
                g_dlaaAttemptFrame.store(now, std::memory_order_release);
                std::uint32_t expectedFirst = 0xFFFFFFFFu;
                g_dlaaFirstAttemptFrame.compare_exchange_strong(
                    expectedFirst, now, std::memory_order_acq_rel);
            }
            if (Platform::Streamline::DrainAffectsActiveEngine()) {
                return false;
            }
            const auto renderer = RendererData();
            auto* context = reinterpret_cast<ID3D11DeviceContext*>(Platform::Fallout4Renderer::Context());
            if (!renderer || !context) {
                return false;
            }
            auto* colorTex = renderer->renderTargets[Platform::Fallout4RenderTargetIndex::kMainTemp].texture;
            if (!colorTex) {
                return false;
            }
            auto* color = reinterpret_cast<ID3D11Resource*>(colorTex);
            const auto frame = Platform::Fallout4Renderer::Snapshot();
            if (!frame.depth || !frame.motionVectors) {
                static std::atomic<bool> l{ false };
                if (!l.exchange(true)) {
                    logger::warn("[DLAA-RT4] null depth({}) or motion({})",
                        static_cast<void*>(frame.depth), static_cast<void*>(frame.motionVectors));
                }
                return false;
            }
            D3D11_TEXTURE2D_DESC colorDesc{};
            colorTex->GetDesc(&colorDesc);
            const bool requestedReset = g_historyResetRequested.exchange(false, std::memory_order_acq_rel);
            const bool reset = historyResetPending || requestedReset;
            if (effective == Platform::AaEffectiveEngine::kDlss) {
                Platform::FrameGenEngine::NoteCamera(Platform::Fallout4Renderer::CameraSnapshot());
            }
            const float dynRatio = g_dynResRatio.load(std::memory_order_relaxed);
            const std::uint32_t renderW = Platform::ScaledSubRectDim(colorDesc.Width, dynRatio);
            const std::uint32_t renderH = Platform::ScaledSubRectDim(colorDesc.Height, dynRatio);
            context->OMSetRenderTargets(0, nullptr, nullptr);
            const bool ran = Platform::Streamline::EvaluateDLAA(
                context, color,
                reinterpret_cast<ID3D11Resource*>(frame.depth),
                reinterpret_cast<ID3D11Resource*>(frame.motionVectors),
                colorDesc, renderW, renderH,
                g_jitterPixelX, g_jitterPixelY, reset);
            if (!ran) {
                if (requestedReset) {
                    g_historyResetRequested.store(true, std::memory_order_release);
                }
                static std::atomic<bool> l{ false };
                if (!l.exchange(true)) {
                    logger::warn("[DLAA-RT4] EvaluateDLAA returned false (see [DLSS]/[DLAA] lines)");
                }
                return false;
            }
            if (Platform::FrameGenEngine::Enabled()) {
                Platform::FsrFrameGen::CaptureAndPrepare(context,
                    reinterpret_cast<ID3D11Resource*>(frame.depth),
                    reinterpret_cast<ID3D11Resource*>(frame.motionVectors), renderW, renderH,
                    g_jitterPixelX, g_jitterPixelY);
            }
            historyResetPending = false;
            Platform::Streamline::StampDlaaResult(context, color);
            if (const auto st = RendererState()) {
                g_dlaaSuccessFrame.store(st->frameCount, std::memory_order_release);
            }
            if (!g_loggedMainTempEval) {
                g_loggedMainTempEval = true;
                logger::info("[DLAA-RT4] first eval OK: kMainTemp(4) {}x{} fmt={} jitterPix=({:.4f},{:.4f})",
                    colorDesc.Width, colorDesc.Height, static_cast<std::uint32_t>(colorDesc.Format),
                    g_jitterPixelX, g_jitterPixelY);
            }
            return true;
        } catch (...) {
            Guard::catchTotal.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
    }

    using SetDynResViewportDefaultFn = void (*)(RE::BSGraphics::RenderTargetManager*, bool);
    SetDynResViewportDefaultFn g_setDynResViewportDefaultFn{ nullptr };

    struct RenderEffectRangeHook
    {
        static void thunk(RE::BSGraphics::RenderTargetManager* self, std::uint32_t a2, std::uint32_t a3,
            std::uint32_t a4, std::uint32_t a5)
        {
            try {
                EnsureDlaaGameSettings();
                if (const auto renderer = RendererData()) {
                    Platform::Streamline::ObserveAaDevice(
                        reinterpret_cast<ID3D11Device*>(renderer->device));
                    Platform::Streamline::NoteDeviceObserved(
                        reinterpret_cast<ID3D11Device*>(renderer->device));
                }
                static std::atomic<bool> loggedArgs{ false };
                if (!loggedArgs.exchange(true)) {
                    logger::info("[DLAA-RT4] RenderEffectRange hook args: start={} end={} flag={} (expect 15/21/1 at +0xD3)", a2, a3, a4);
                }
                EvalDlaaOnMainTemp();
            } catch (...) {
            }
            float savedW = 1.0F;
            float savedH = 1.0F;
            RE::BSGraphics::RenderTargetManager* juggled = nullptr;
            try {
                if (auto* const targets = RenderTargetManager()) {
                    if (targets->dynamicWidthRatio != 1.0F || targets->dynamicHeightRatio != 1.0F) {
                        juggled = targets;
                        savedW = targets->dynamicWidthRatio;
                        savedH = targets->dynamicHeightRatio;
                        if (g_setDynResViewportDefaultFn) {
                            g_setDynResViewportDefaultFn(targets, false);
                        }
                        targets->dynamicWidthRatio = 1.0F;
                        targets->dynamicHeightRatio = 1.0F;
                    }
                }
            } catch (...) {
                juggled = nullptr;
            }
            func(self, a2, a3, a4, a5);
            if (juggled) {
                juggled->dynamicWidthRatio = savedW;
                juggled->dynamicHeightRatio = savedH;
            }
            Platform::BloomPass::ExecuteAfterEffectRange();
            Platform::GradingPass::ExecuteAfterEffectRange();
            Platform::FrameFingerprint::SampleSeam(Platform::FrameFingerprint::Point::kNeuralInput);
            Platform::NeuralPass::ExecuteAfterEffectRange();
            Platform::FrameFingerprint::SampleSeam(Platform::FrameFingerprint::Point::kNeuralOutput);
            Platform::FrameGenEngine::CaptureHudless();
        }
        static inline REL::Relocation<decltype(thunk)> func;
    };

    void InstallRenderEffectRangeHook() noexcept
    {
        try {
            REL::Relocation<std::uintptr_t> target{ REL::ID(587723), 0xD3 };
            const std::uint8_t opcode = *reinterpret_cast<const std::uint8_t*>(target.address());
            if (opcode != 0xE8) {
                logger::error("[Renderer] RenderEffectRange hook: expected an E8 call at REL(587723)+0xD3, found {:#04x}; DLAA eval NOT installed", opcode);
                return;
            }
            RenderEffectRangeHook::func =
                F4SE::GetTrampoline().write_call<5>(target.address(), RenderEffectRangeHook::thunk);
            logger::info("[Renderer] DLAA eval hook installed at REL(587723)+0xD3 (kMainTemp/4, AA-stage / pre-TAA range 15-21)");

            try {
                REL::Relocation<std::uintptr_t> viewportSite{ REL::ID(587723), 0xE1 };
                const auto* const bytes = reinterpret_cast<const std::uint8_t*>(viewportSite.address());
                if (bytes[0] == 0xE8) {
                    const auto rel = *reinterpret_cast<const std::int32_t*>(viewportSite.address() + 1);
                    g_setDynResViewportDefaultFn = reinterpret_cast<SetDynResViewportDefaultFn>(
                        static_cast<std::intptr_t>(viewportSite.address()) + 5 + rel);
                    logger::info("[Renderer] dyn-res viewport-default setter resolved (REL(587723)+0xE1)");
                } else {
                    logger::warn("[Renderer] expected an E8 call at REL(587723)+0xE1, found {:#04x} — viewport-default setter unavailable (fail-open)", bytes[0]);
                }
            } catch (...) {
                logger::warn("[Renderer] dyn-res viewport-default resolution threw — setter unavailable (fail-open)");
            }
        } catch (const std::exception& e) {
            logger::error("[Renderer] RenderEffectRange hook failed: {}", e.what());
        } catch (...) {
            logger::error("[Renderer] RenderEffectRange hook failed with an unknown C++ exception");
        }
    }

    using ImageSpaceIsActiveFn = bool (*)(void*);
    ImageSpaceIsActiveFn g_taaIsActiveOriginal{ nullptr };

    bool TaaIsActiveThunk(void* self)
    {
        (void)self;
        static std::atomic<bool> loggedVeto{ false };
        if (!loggedVeto.load(std::memory_order_relaxed) && !loggedVeto.exchange(true)) {
            logger::info("[DLAA] vanilla TAA resolve vetoed at the root (IsActive->false, unconditional) — TAA is never a fallback; DLSS or nothing");
        }
        return false;
    }

    using DeferredLightsFn = void (*)(void*, void*);
    DeferredLightsFn g_deferredLightsOriginal{ nullptr };

    void DeferredLightsThunk(void* a_arg0, void* a_arg1)
    {
        if (g_deferredLightsOriginal != nullptr) {
            g_deferredLightsOriginal(a_arg0, a_arg1);
        }

        NoteFrameOrder("deferred lighting done -> AO write");
        Platform::AoSlot::IntegrateAfterLighting();
    }

    void InstallDeferredLightsHook() noexcept
    {
        try {
            REL::Relocation<std::uintptr_t> target{ REL::ID(984743), 0x1BF };
            g_deferredLightsOriginal = reinterpret_cast<DeferredLightsFn>(
                F4SE::GetTrampoline().write_call<5>(target.address(), DeferredLightsThunk));
            if (g_deferredLightsOriginal == nullptr) {
                logger::error("[GTAO] post-deferred-lights hook returned a null original");
            } else {
                logger::info("[GTAO] post-deferred-lights hook installed at REL(984743)+0x1BF - "
                             "the AO integration point (inert until XeGTAO is selected)");
            }
        } catch (const std::exception& e) {
            logger::error("[GTAO] post-deferred-lights hook failed: {}", e.what());
        } catch (...) {
            logger::error("[GTAO] post-deferred-lights hook failed (unknown exception)");
        }
    }

    void InstallTaaVetoHook() noexcept
    {
        try {
            g_taaIsActiveOriginal = reinterpret_cast<ImageSpaceIsActiveFn>(
                REL::Relocation<std::uintptr_t>{ RE::VTABLE::ImageSpaceEffectTemporalAA[0] }.write_vfunc(0x8, TaaIsActiveThunk));
            if (!g_taaIsActiveOriginal) {
                logger::error("[Renderer] ImageSpaceEffectTemporalAA::IsActive vfunc hook returned a null original");
            } else {
                logger::info("[Renderer] TAA veto installed: ImageSpaceEffectTemporalAA::IsActive (vfunc 0x8) -> false UNCONDITIONALLY (TAA is never a fallback; DLSS or nothing)");
            }
        } catch (const std::exception& e) {
            logger::error("[Renderer] TAA veto hook failed: {}", e.what());
        } catch (...) {
            logger::error("[Renderer] TAA veto hook failed with an unknown C++ exception");
        }
    }
}

namespace Platform
{
    bool Fallout4Renderer::Install() noexcept
    {
        try {
            InstallJitterHook();
            InstallSamplerHooks();
            InstallRenderEffectRangeHook();
            InstallTaaVetoHook();
            InstallDeferredLightsHook();

            const auto frame = Snapshot();
            const auto validation = ValidateFrameInputs(frame);
            logger::info("[Renderer] FO4 DLAA hooks armed (jitter + mip-bias + eval + TAA veto); initial inputs={}",
                static_cast<std::uint32_t>(validation));
            return true;
        } catch (const std::exception& e) {
            logger::error("[Renderer] install failed: {}", e.what());
        } catch (...) {
            logger::error("[Renderer] install failed with an unknown C++ exception");
        }
        return false;
    }

    FrameInputs Fallout4Renderer::Snapshot() noexcept
    {
        FrameInputs result{};
        try {
            const auto renderer = RendererData();
            const auto state = RendererState();
            const auto targets = RenderTargetManager();
            if (!renderer || !state || !targets) {
                return result;
            }

            result.color = renderer->renderTargets[Fallout4RenderTargetIndex::kMain].texture;
            result.depth = renderer->depthStencilTargets[kMainDepth].texture;
            result.motionVectors = renderer->renderTargets[Fallout4RenderTargetIndex::kMotionVectors].texture;
            result.displayWidth = state->screenWidth;
            result.displayHeight = state->screenHeight;

            if (std::isfinite(targets->dynamicWidthRatio) && targets->dynamicWidthRatio > 0.0F) {
                result.renderWidth =
                    Platform::ScaledSubRectDim(state->screenWidth, targets->dynamicWidthRatio);
            }
            if (std::isfinite(targets->dynamicHeightRatio) && targets->dynamicHeightRatio > 0.0F) {
                result.renderHeight =
                    Platform::ScaledSubRectDim(state->screenHeight, targets->dynamicHeightRatio);
            }

            result.jitterX = state->offsetX;
            result.jitterY = state->offsetY;
        } catch (...) {
            Guard::catchTotal.fetch_add(1, std::memory_order_relaxed);
        }
        return result;
    }

    void Fallout4Renderer::LogSnapshot() noexcept
    {
        try {
            const FrameInputs frame = Snapshot();
            const FrameValidation validation = ValidateFrameInputs(frame);
            logger::info("[Renderer] snapshot: validation={} display={}x{} render={}x{} guardCatches={}",
                static_cast<std::uint32_t>(validation),
                frame.displayWidth, frame.displayHeight, frame.renderWidth, frame.renderHeight,
                Guard::catchTotal.load(std::memory_order_relaxed));
        } catch (...) {
            Guard::catchTotal.fetch_add(1, std::memory_order_relaxed);
        }
    }

    RenderDevice Fallout4Renderer::DeviceSnapshot() noexcept
    {
        RenderDevice result{};
        try {
            if (const auto renderer = RendererData()) {
                result.device = renderer->device;
                result.context = renderer->context;
            }
        } catch (...) {
            Guard::catchTotal.fetch_add(1, std::memory_order_relaxed);
        }
        return result;
    }

    void* Fallout4Renderer::Context() noexcept { return DeviceSnapshot().context; }

    void Fallout4Renderer::RequestHistoryReset(const char* a_reason) noexcept
    {
        g_historyResetRequested.store(true, std::memory_order_release);
        g_historyEpochReason.store(a_reason != nullptr ? a_reason : "unspecified", std::memory_order_relaxed);
        g_historyEpoch.fetch_add(1, std::memory_order_release);
    }

    std::uint64_t Fallout4Renderer::HistoryEpoch() noexcept
    {
        return g_historyEpoch.load(std::memory_order_acquire);
    }

    std::uint64_t Fallout4Renderer::FrameStampNow() noexcept
    {
        return g_frameStamp.Current();
    }

    const char* Fallout4Renderer::HistoryEpochReason() noexcept
    {
        return g_historyEpochReason.load(std::memory_order_relaxed);
    }

    void Fallout4Renderer::SetMipBias(float a_bias) noexcept
    {
        Platform::Streamline::SetMipBias(a_bias);
    }

    void Fallout4Renderer::CurrentJitterPixels(float& a_outX, float& a_outY) noexcept
    {
        a_outX = g_jitterPixelX;
        a_outY = g_jitterPixelY;
    }

    float Fallout4Renderer::CurrentDynamicResolutionRatio() noexcept
    {
        return g_dynResRatio.load(std::memory_order_relaxed);
    }

    bool Fallout4Renderer::TryRegisterDynResMenuSink() noexcept
    {
        if (g_dynResSinkRegistered.load(std::memory_order_acquire)) {
            return true;
        }
        try {
            auto* const ui = RE::UI::GetSingleton();
            if (!ui) {
                return false;
            }
            ui->RegisterSink<RE::MenuOpenCloseEvent>(&DynResMenuSink::GetSingleton());
            g_dynResSinkRegistered.store(true, std::memory_order_release);
            logger::info("[Renderer] dyn-res menu sink registered (loading + Pip-Boy/Examine/Terminal render native)");
            return true;
        } catch (...) {
            logger::warn("[Renderer] dyn-res menu sink registration threw — sub-native will NOT pause for loading/menus until a later F4SE message retry succeeds");
            return false;
        }
    }

    void Fallout4Renderer::SetJitterScale(float a_scale) noexcept
    {
        const float clamped = Platform::ClampJitterScale(a_scale);
        if (g_jitterScale.exchange(clamped, std::memory_order_relaxed) != clamped) {
            logger::info("[Jitter] amplitude scale -> {:.2f} (1.00 = standard +/-0.5px Halton; 0 = no jitter; >1 = wider sub-pixel spread)", clamped);
        }
    }

    float Fallout4Renderer::JitterScale() noexcept
    {
        return g_jitterScale.load(std::memory_order_relaxed);
    }

    CameraConstants Fallout4Renderer::CameraSnapshot() noexcept
    {
        CameraConstants result{};
        try {
            const auto state = RendererState();
            if (state) {
                if (state->screenHeight != 0) {
                    result.aspectRatio =
                        static_cast<float>(state->screenWidth) / static_cast<float>(state->screenHeight);
                }
            }

            enum : std::uint8_t { kCamNull = 0, kCamDegenerate = 1, kCamValid = 2, kCamUnset = 255 };
            static std::atomic<std::uint8_t> lastCamProbe{ kCamUnset };
            struct TrackedCameraKey
            {
                const void* cam{ nullptr };
                bool useJitter{ false };
                std::uint64_t vpHash{ 0 };
                bool hashSeeded{ false };
                bool everChanged{ false };
                std::uint64_t lastSeen{ 0 };
            };
            static TrackedCameraKey s_trackedKeys[8]{};
            static const void* s_stickyCam{ nullptr };
            static bool s_stickyUseJitter{ false };
            static bool s_stickyValid{ false };
            static std::uint64_t s_snapshotCall{ 0 };
            ++s_snapshotCall;
            const auto hashVp = [](const void* a_matrix) noexcept {
                const auto* bytes = static_cast<const unsigned char*>(a_matrix);
                std::uint64_t h = 14695981039346656037ULL;
                for (std::size_t i = 0; i < sizeof(float) * 16; ++i) {
                    h ^= bytes[i];
                    h *= 1099511628211ULL;
                }
                return h;
            };
            const auto matrixStats = [](const void* a_matrix, float& a_sum, bool& a_finite) noexcept {
                const auto* f16 = static_cast<const float*>(a_matrix);
                a_sum = 0.0F;
                a_finite = true;
                for (int i = 0; i < 16; ++i) {
                    a_sum += std::fabs(f16[i]);
                    if (!std::isfinite(f16[i])) {
                        a_finite = false;
                    }
                }
            };
            const auto viewportsMatch = [](const RE::NiRect<float>& a_a, const RE::NiRect<float>& a_b) noexcept {
                const float widthA = std::fabs(a_a.right - a_a.left);
                const float widthB = std::fabs(a_b.right - a_b.left);
                const float w = widthA > widthB ? widthA : widthB;
                float eps = w * 0.001F;
                eps = eps < 1.0e-4F ? 1.0e-4F : (eps > 0.5F ? 0.5F : eps);
                return std::fabs(a_a.left - a_b.left) <= eps && std::fabs(a_a.right - a_b.right) <= eps &&
                       std::fabs(a_a.top - a_b.top) <= eps && std::fabs(a_a.bottom - a_b.bottom) <= eps;
            };
            const RE::BSGraphics::CameraStateData* cameraData = nullptr;
            std::int32_t chosenIndex = -1;
            if (state) {
                struct CameraCandidate
                {
                    const RE::BSGraphics::CameraStateData* entry{ nullptr };
                    std::int32_t index{ -1 };
                    bool viewportMatch{ false };
                    bool isSticky{ false };
                    float nearPlane{ 0.0F };
                    float farPlane{ 0.0F };
                };
                CameraCandidate candidates[8]{};
                std::size_t candidateCount = 0;
                std::int32_t index = 0;
                for (const auto& entry : state->cameraDataCache) {
                    TrackedCameraKey* slot = nullptr;
                    for (auto& key : s_trackedKeys) {
                        if (key.cam == entry.referenceCamera && key.useJitter == entry.useJitter) {
                            slot = &key;
                            break;
                        }
                    }
                    if (!slot) {
                        for (auto& key : s_trackedKeys) {
                            if (!key.cam) {
                                slot = &key;
                                break;
                            }
                        }
                        if (!slot) {
                            TrackedCameraKey* lru = nullptr;
                            for (auto& key : s_trackedKeys) {
                                if (s_stickyValid && key.cam == s_stickyCam && key.useJitter == s_stickyUseJitter) {
                                    continue;
                                }
                                if (!lru || key.lastSeen < lru->lastSeen) {
                                    lru = &key;
                                }
                            }
                            if (lru) {
                                *lru = {};
                                slot = lru;
                            } else {
                                static std::atomic<bool> overflowLogged{ false };
                                if (!overflowLogged.exchange(true, std::memory_order_relaxed)) {
                                    logger::warn("[Camera] key-tracking overflow (>8 keys incl. sticky) — entry left untracked (fail-open)");
                                }
                            }
                        }
                        if (slot) {
                            slot->cam = entry.referenceCamera;
                            slot->useJitter = entry.useJitter;
                        }
                    }
                    if (slot) {
                        slot->lastSeen = s_snapshotCall;
                        const std::uint64_t h = hashVp(&entry.camViewData.currentViewProjUnjittered);
                        if (slot->hashSeeded && h != slot->vpHash) {
                            slot->everChanged = true;
                        }
                        slot->vpHash = h;
                        slot->hashSeeded = true;
                    }

                    if (slot && entry.useJitter && slot->everChanged && candidateCount < 8) {
                        if (const auto cam = entry.referenceCamera) {
                            const auto& fr = cam->viewFrustum;
                            if (!fr.ortho && fr.far != fr.near && fr.right != fr.left && fr.top != fr.bottom) {
                                float viewSum{};
                                float curSum{};
                                float prevSum{};
                                bool viewFin{};
                                bool curFin{};
                                bool prevFin{};
                                matrixStats(&entry.camViewData.viewMat, viewSum, viewFin);
                                matrixStats(&entry.camViewData.currentViewProjUnjittered, curSum, curFin);
                                matrixStats(&entry.camViewData.previousViewProjUnjittered, prevSum, prevFin);
                                if (viewFin && curFin && prevFin &&
                                    viewSum > 1.0e-6F && curSum > 1.0e-6F && prevSum > 1.0e-6F) {
                                    auto& c = candidates[candidateCount++];
                                    c.entry = &entry;
                                    c.index = index;
                                    c.viewportMatch = viewportsMatch(entry.camViewData.viewPort, state->frameBufferViewport);
                                    c.isSticky = s_stickyValid && cam == s_stickyCam &&
                                                 entry.useJitter == s_stickyUseJitter;
                                    c.nearPlane = fr.near;
                                    c.farPlane = fr.far;
                                }
                            }
                        }
                    }
                    ++index;
                }
                for (auto& key : s_trackedKeys) {
                    if (key.cam && key.lastSeen != s_snapshotCall) {
                        key = {};
                    }
                }
                if (s_stickyValid) {
                    bool stickyQualifiedThisScan = false;
                    for (std::size_t i = 0; i < candidateCount; ++i) {
                        if (candidates[i].isSticky) {
                            stickyQualifiedThisScan = true;
                            break;
                        }
                    }
                    if (!stickyQualifiedThisScan) {
                        logger::info("[Camera] sticky key lost qualification — cleared (its return re-acquires with a history reset)");
                        s_stickyValid = false;
                        s_stickyCam = nullptr;
                        s_stickyUseJitter = false;
                    }
                }
                const RE::BSGraphics::CameraStateData* pick = nullptr;
                std::int32_t pickIdx = -1;
                if (candidateCount > 0) {
                    float maxFar = 0.0F;
                    for (std::size_t i = 0; i < candidateCount; ++i) {
                        maxFar = candidates[i].farPlane > maxFar ? candidates[i].farPlane : maxFar;
                    }
                    const auto scoreOf = [maxFar](const CameraCandidate& a_c) noexcept -> std::uint32_t {
                        std::uint32_t s = 0;
                        if (a_c.viewportMatch) {
                            s += 4;
                        }
                        if (a_c.farPlane >= maxFar * 0.99F) {
                            s += 2;
                        }
                        return s;
                    };
                    const CameraCandidate* best = &candidates[0];
                    for (std::size_t i = 1; i < candidateCount; ++i) {
                        const auto& c = candidates[i];
                        const std::uint32_t sc = scoreOf(c);
                        const std::uint32_t sb = scoreOf(*best);
                        if (sc > sb ||
                            (sc == sb && c.nearPlane > best->nearPlane) ||
                            (sc == sb && c.nearPlane == best->nearPlane && c.isSticky && !best->isSticky)) {
                            best = &c;
                        }
                    }
                    pick = best->entry;
                    pickIdx = best->index;
                }
                static const void* s_pendingCam{ nullptr };
                static bool s_pendingUseJitter{ false };
                static std::uint32_t s_pendingScans{ 0 };
                static std::uint32_t s_suppressFrame{ 0 };
                static bool s_suppressFrameSet{ false };
                constexpr std::uint32_t kAdoptScans = 4;
                const std::uint32_t engineFrame = static_cast<std::uint32_t>(state->frameCount);
                bool suppressThisFrame = s_suppressFrameSet && engineFrame == s_suppressFrame;
                if (pick) {
                    const auto pickCam = static_cast<const void*>(pick->referenceCamera);
                    if (s_stickyValid && pickCam == s_stickyCam && pick->useJitter == s_stickyUseJitter) {
                        s_pendingCam = nullptr;
                        s_pendingScans = 0;
                    } else {
                        if (pickCam == s_pendingCam && pick->useJitter == s_pendingUseJitter) {
                            ++s_pendingScans;
                        } else {
                            s_pendingCam = pickCam;
                            s_pendingUseJitter = pick->useJitter;
                            s_pendingScans = 1;
                        }
                        if (s_pendingScans >= kAdoptScans) {
                            const bool hadPrior = s_stickyValid;
                            s_stickyCam = pickCam;
                            s_stickyUseJitter = pick->useJitter;
                            s_stickyValid = true;
                            s_pendingCam = nullptr;
                            s_pendingScans = 0;
                            logger::info("[Camera] selection key {}: cache[{}] refCam={} useJitter={} (debounced over {} scans; exact path engages next frame; history reset requested)",
                                hadPrior ? "CHANGED" : "acquired", pickIdx, pickCam, pick->useJitter, kAdoptScans);
                            RequestHistoryReset(hadPrior ? "camera changed" : "camera acquired");
                            s_suppressFrame = engineFrame;
                            s_suppressFrameSet = true;
                            suppressThisFrame = true;
                        }
                        pick = nullptr;
                        pickIdx = -1;
                    }
                } else {
                    s_pendingCam = nullptr;
                    s_pendingScans = 0;
                }
                if (suppressThisFrame) {
                    cameraData = nullptr;
                    chosenIndex = -1;
                } else if (pick) {
                    cameraData = pick;
                    chosenIndex = pickIdx;
                }
            }
            if (const auto camera = cameraData ? cameraData->referenceCamera : nullptr) {
                const auto& f = camera->viewFrustum;
                if (!f.ortho && f.far != f.near && f.right != f.left && f.top != f.bottom) {
                    if (lastCamProbe.exchange(kCamValid, std::memory_order_relaxed) != kCamValid) {
                        logger::info("[Camera] probe -> VALID camera+frustum (camera block executing; source={})",
                            chosenIndex < 0 ? "legacy cameraState slot" : "cameraDataCache entry");
                    }
                    const float l = f.left, r = f.right, t = f.top, b = f.bottom, n = f.near, fa = f.far;
                    result.nearPlane = n;
                    result.farPlane = fa;
                    result.fovVertical = std::atan2(t, 1.0F) - std::atan2(b, 1.0F);

                    float* p = result.viewToClip;
                    p[0] = 2.0F / (r - l);
                    p[5] = 2.0F / (t - b);
                    p[8] = (r + l) / (r - l);
                    p[9] = (t + b) / (t - b);
                    p[10] = fa / (n - fa);
                    p[11] = -1.0F;
                    p[14] = n * fa / (n - fa);

                    const auto& vd = cameraData->camViewData;
                    const auto* right = reinterpret_cast<const float*>(&vd.viewRight);
                    const auto* up = reinterpret_cast<const float*>(&vd.viewUp);
                    const auto* dir = reinterpret_cast<const float*>(&vd.viewDir);
                    result.cameraRight[0] = right[0]; result.cameraRight[1] = right[1]; result.cameraRight[2] = right[2];
                    result.cameraUp[0] = up[0]; result.cameraUp[1] = up[1]; result.cameraUp[2] = up[2];
                    result.cameraForward[0] = -dir[0]; result.cameraForward[1] = -dir[1]; result.cameraForward[2] = -dir[2];
                    const auto& tr = camera->world.translate;
                    result.eyePosition[0] = tr.x;
                    result.eyePosition[1] = tr.y;
                    result.eyePosition[2] = tr.z;

                    std::memcpy(result.viewMatrix, &vd.viewMat, sizeof(float) * 16);
                    std::memcpy(result.curViewProj, &vd.currentViewProjUnjittered, sizeof(float) * 16);
                    std::memcpy(result.prevViewProj, &vd.previousViewProjUnjittered, sizeof(float) * 16);
                    float vpMag = 0.0F;
                    bool vpFinite = true;
                    for (int i = 0; i < 16; ++i) {
                        vpMag += std::fabs(result.curViewProj[i]);
                        if (!std::isfinite(result.curViewProj[i]) || !std::isfinite(result.viewMatrix[i]) ||
                            !std::isfinite(result.prevViewProj[i])) {
                            vpFinite = false;
                        }
                    }
                    result.hasReprojection = vpFinite && vpMag > 1.0e-6F;

                    static std::atomic<bool> logged{ false };
                    if (!logged.exchange(true)) {
                        logger::info("[Camera] frustum l={:.4f} r={:.4f} t={:.4f} b={:.4f} n={:.2f} f={:.1f} -> fovV={:.4f} aspect={:.4f}",
                            l, r, t, b, n, fa, result.fovVertical, result.aspectRatio);
                        const auto& pa = cameraData->posAdjust;
                        logger::info("[Camera] right=({:.3f},{:.3f},{:.3f}) up=({:.3f},{:.3f},{:.3f}) fwd=({:.3f},{:.3f},{:.3f}) eye=({:.1f},{:.1f},{:.1f}) posAdjust=({:.1f},{:.1f},{:.1f})",
                            result.cameraRight[0], result.cameraRight[1], result.cameraRight[2],
                            result.cameraUp[0], result.cameraUp[1], result.cameraUp[2],
                            result.cameraForward[0], result.cameraForward[1], result.cameraForward[2],
                            result.eyePosition[0], result.eyePosition[1], result.eyePosition[2],
                            pa.x, pa.y, pa.z);
                        float viewMag = 0.0F;
                        float prevMag = 0.0F;
                        for (int i = 0; i < 16; ++i) {
                            viewMag += std::fabs(result.viewMatrix[i]);
                            prevMag += std::fabs(result.prevViewProj[i]);
                        }
                        logger::info("[Camera] ViewData matrices: sum|viewMat|={:.4f} sum|curVP|={:.4f} sum|prevVP|={:.4f} finite={} hasReprojection={} (all-zero sums => engine never fills camViewData)",
                            viewMag, vpMag, prevMag, vpFinite, result.hasReprojection);
                    }
                } else {
                    if (lastCamProbe.exchange(kCamDegenerate, std::memory_order_relaxed) != kCamDegenerate) {
                        logger::warn("[Camera] probe -> viewFrustum DEGENERATE (ortho={} l={:.4f} r={:.4f} t={:.4f} b={:.4f} n={:.4f} f={:.4f}) — camera block skipped; snapshot ships ZERO basis/projection",
                            f.ortho, f.left, f.right, f.top, f.bottom, f.near, f.far);
                    }
                }
            } else {
                const std::uint8_t before = lastCamProbe.exchange(kCamNull, std::memory_order_relaxed);
                if (before == kCamUnset) {
                    logger::info("[Camera] probe -> no camera yet (startup: the cache holds no qualified key) — camera block skipped until one is acquired");
                } else if (before != kCamNull) {
                    logger::warn("[Camera] probe -> NO trusted camera (no stable qualified cache key this scan) — camera block skipped; snapshot ships ZERO basis/projection (sentinel path)");
                }
            }

        } catch (...) {
            Guard::catchTotal.fetch_add(1, std::memory_order_relaxed);
        }
        return result;
    }
}
