// SPDX-License-Identifier: GPL-3.0-or-later
// Portions adapted from Community Shaders for Fallout 4 (northaxosky), GPL-3.0.

#include "PCH.h"

#include "Platform/AaMailbox.h"
#include "Platform/DlaaSettings.h"
#include "Platform/DownsampleShaderBytecode.h"
#include "Platform/Fallout4Renderer.h"
#include "Platform/Dlss12Engine.h"
#include "Platform/FrameGenEngine.h"
#include "Platform/FsrEngine.h"
#include "Platform/RcasConstants.h"
#include "Platform/ResolveUpShaderBytecode.h"
#include "Platform/SubNativeMath.h"
#include "Platform/RcasShaderBytecode.h"
#include "Platform/Streamline.h"
#include "Platform/WiringAdapters.h"

#include <d3d11.h>

#include <array>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>

namespace
{

    std::unique_ptr<Platform::AaMailbox> g_aaMailbox;

    Platform::PublishedSnapshot<Platform::AaEngineCapabilities> g_aaCapabilityObservation;
    Platform::PublishedSnapshot<Platform::AaEngineSelectionSnapshot> g_aaSelectionSnapshot;
    Platform::PublishedSnapshot<Platform::AaEngineDiagnosticSnapshot> g_aaDiagnosticSnapshot;
    Platform::AaDeviceObservationGate g_aaDeviceObservationGate;
    std::atomic<ID3D11Device*> g_lastObservedDevice{ nullptr };
    std::atomic<bool> g_capabilityPublishPending{ false };
    Platform::AaEngineCapabilities g_pendingCapabilities{};
    Platform::AaEngineCapabilities g_firstDeviceCapabilities{};
    std::uint64_t g_nextDeviceEpoch{ 1 };

    Platform::DeviceGenerationTracker g_deviceGenerationTracker;

    Platform::AaEngineCapabilities g_renderCapabilities{};
    Platform::AaDeviceGeneration g_boundDeviceGeneration{};
    Platform::AaRequest g_canonicalApplied = []() noexcept {
        Platform::AaRequest request{};
        request.dlaaEnabled = false;
        request.effectiveEngine = Platform::AaEffectiveEngine::kUnresolved;
        return request;
    }();
    Platform::AaEngineSelectionSnapshot g_renderSelection{};
    bool g_startupEngineResolved{ false };
    bool g_selectionFailClosed{ false };
    bool g_mailboxHasPumped{ false };

    Platform::PumpResult g_lastPumpResult{};

    std::atomic<float> g_exposureScale{ 1.0F };
    std::uint32_t g_previousPreset{ 0xFFFFFFFFU };
    std::uint32_t g_previousQualityMode{ 0xFFFFFFFFU };
    std::atomic<std::uint32_t> g_colorPathFormat{ 0 };
    std::atomic<bool> g_colorPathHdr{ false };
    std::uint32_t g_colorPathLoggedFormat{ 0xFFFFFFFFU };

    [[nodiscard]] int NgxPerfQualityForRatio(float ratio) noexcept
    {
        switch (Platform::EffectiveRatioBucket(ratio)) {
        case 1: return 2;
        case 2: return 1;
        case 3: return 0;
        case 4: return 3;
        default: return 5;
        }
    }

    ID3D11Texture2D* g_dlaaScratch{ nullptr };
    std::uint32_t g_dlaaScratchW{ 0 };
    std::uint32_t g_dlaaScratchH{ 0 };
    DXGI_FORMAT g_dlaaScratchFmt{ DXGI_FORMAT_UNKNOWN };
    bool g_dlaaScratchFailed{ false };

    std::atomic<float> g_sharpness{ 0.3F };
    ID3D11ComputeShader* g_rcasCS{ nullptr };
    ID3D11Buffer* g_rcasCB{ nullptr };
    ID3D11Texture2D* g_sharpScratch{ nullptr };
    ID3D11UnorderedAccessView* g_sharpScratchUAV{ nullptr };
    ID3D11ShaderResourceView* g_dlaaScratchSRV{ nullptr };
    std::uint32_t g_rcasW{ 0 };
    std::uint32_t g_rcasH{ 0 };
    DXGI_FORMAT g_rcasFmt{ DXGI_FORMAT_UNKNOWN };
    std::atomic<bool> g_rcasFailed{ false };
    ID3D11Resource* g_stampSource{ nullptr };

    ID3D11Texture2D* g_upscaleScratch{ nullptr };
    ID3D11ShaderResourceView* g_upscaleScratchSRV{ nullptr };
    ID3D11UnorderedAccessView* g_upscaleScratchUAV{ nullptr };
    ID3D11UnorderedAccessView* g_dlaaScratchUAV{ nullptr };
    std::uint32_t g_upscaleW{ 0 };
    std::uint32_t g_upscaleH{ 0 };
    DXGI_FORMAT g_upscaleFmt{ DXGI_FORMAT_UNKNOWN };
    ID3D11ComputeShader* g_downsampleCS{ nullptr };
    ID3D11ComputeShader* g_resolveUpCS{ nullptr };
    std::atomic<float> g_effectiveOutOverRender{ 0.0F };
    bool g_trsFailed{ false };
    float g_loggedScale{ -1.0F };

    [[nodiscard]] ID3D11Resource* RunRcas(
        ID3D11DeviceContext* context, std::uint32_t width, std::uint32_t height) noexcept
    {
        const float sharpness = g_sharpness.load(std::memory_order_relaxed);
        if (g_rcasFailed.load(std::memory_order_relaxed) &&
            (g_rcasW != width || g_rcasH != height || g_rcasFmt != g_dlaaScratchFmt)) {
            g_rcasFailed.store(false, std::memory_order_relaxed);
        }
        if (sharpness <= 0.0F || g_rcasFailed.load(std::memory_order_relaxed) || !g_dlaaScratch ||
            !context || width == 0 || height == 0) {
            return g_dlaaScratch;
        }
        ID3D11Device* device = nullptr;
        context->GetDevice(&device);
        if (!device) {
            return g_dlaaScratch;
        }
        const auto fail = [&](const char* what) -> ID3D11Resource* {
            if (g_sharpScratchUAV) { g_sharpScratchUAV->Release(); g_sharpScratchUAV = nullptr; }
            if (g_sharpScratch) { g_sharpScratch->Release(); g_sharpScratch = nullptr; }
            if (g_dlaaScratchSRV) { g_dlaaScratchSRV->Release(); g_dlaaScratchSRV = nullptr; }
            g_rcasFailed.store(true, std::memory_order_relaxed);
            g_rcasW = width;
            g_rcasH = height;
            g_rcasFmt = g_dlaaScratchFmt;
            device->Release();
            logger::error("[RCAS] {} — sharpening disabled (DLAA unaffected; retries on a size/format/sharpness change)", what);
            return g_dlaaScratch;
        };
        if (!g_rcasCS) {
            if (FAILED(device->CreateComputeShader(Platform::kRcasShaderBytecode, Platform::kRcasShaderBytecodeSize,
                    nullptr, &g_rcasCS)) || !g_rcasCS) {
                return fail("CreateComputeShader failed");
            }
        }
        if (!g_rcasCB) {
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = sizeof(Platform::RcasConstants);
            bd.Usage = D3D11_USAGE_DYNAMIC;
            bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            if (FAILED(device->CreateBuffer(&bd, nullptr, &g_rcasCB)) || !g_rcasCB) {
                return fail("constant-buffer creation failed");
            }
        }
        if (!g_sharpScratch || !g_dlaaScratchSRV || g_rcasW != width || g_rcasH != height || g_rcasFmt != g_dlaaScratchFmt) {
            if (g_sharpScratchUAV) { g_sharpScratchUAV->Release(); g_sharpScratchUAV = nullptr; }
            if (g_sharpScratch) { g_sharpScratch->Release(); g_sharpScratch = nullptr; }
            if (g_dlaaScratchSRV) { g_dlaaScratchSRV->Release(); g_dlaaScratchSRV = nullptr; }
            D3D11_TEXTURE2D_DESC td{};
            td.Width = width;
            td.Height = height;
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Format = g_dlaaScratchFmt;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
            if (FAILED(device->CreateTexture2D(&td, nullptr, &g_sharpScratch)) || !g_sharpScratch) {
                return fail("sharpScratch creation failed");
            }
            if (FAILED(device->CreateUnorderedAccessView(g_sharpScratch, nullptr, &g_sharpScratchUAV)) || !g_sharpScratchUAV) {
                return fail("sharpScratch UAV creation failed");
            }
            if (FAILED(device->CreateShaderResourceView(g_dlaaScratch, nullptr, &g_dlaaScratchSRV)) || !g_dlaaScratchSRV) {
                return fail("dlaaScratch SRV creation failed");
            }
            g_rcasW = width;
            g_rcasH = height;
            g_rcasFmt = g_dlaaScratchFmt;
        }
        device->Release();
        D3D11_MAPPED_SUBRESOURCE ms{};
        if (FAILED(context->Map(g_rcasCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
            return g_dlaaScratch;
        }
        const float lobe = std::exp2(2.0F * sharpness - 2.0F);
        Platform::RcasConstants cb{};
        cb.sharpness = lobe;
        std::memcpy(ms.pData, &cb, sizeof(cb));
        context->Unmap(g_rcasCB, 0);
        context->CSSetShader(g_rcasCS, nullptr, 0);
        ID3D11Buffer* cbs[] = { g_rcasCB };
        context->CSSetConstantBuffers(0, 1, cbs);
        ID3D11ShaderResourceView* srvs[] = { g_dlaaScratchSRV };
        context->CSSetShaderResources(0, 1, srvs);
        ID3D11UnorderedAccessView* uavs[] = { g_sharpScratchUAV };
        context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
        context->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
        ID3D11ShaderResourceView* nsrv[] = { nullptr };
        context->CSSetShaderResources(0, 1, nsrv);
        ID3D11UnorderedAccessView* nuav[] = { nullptr };
        context->CSSetUnorderedAccessViews(0, 1, nuav, nullptr);
        context->CSSetShader(nullptr, nullptr, 0);
        static std::atomic<bool> logged{ false };
        if (!logged.exchange(true, std::memory_order_relaxed)) {
            logger::info("[RCAS] sharpening active (sharpness={:.2f}, lobe={:.3f})", sharpness, lobe);
        }
        return g_sharpScratch;
    }

}
namespace Platform
{

    void Streamline::ObserveAaDevice(ID3D11Device* a_device) noexcept
    {
        if (!a_device) {
            return;
        }

        if (g_lastObservedDevice.load(std::memory_order_acquire) == a_device &&
            !g_capabilityPublishPending.load(std::memory_order_acquire) &&
            !g_aaDeviceObservationGate.FailClosed()) {
            return;
        }

        if (!g_aaDeviceObservationGate.TryEnter()) {
            return;
        }
        struct ObservationGateExit final {
            Platform::AaDeviceObservationGate& gate;
            ~ObservationGateExit() noexcept { gate.Leave(); }
        } gateExit{ g_aaDeviceObservationGate };

        try {
            const auto publishPending = [&]() noexcept {
                using Publisher = Platform::PublishedSnapshot<Platform::AaEngineCapabilities>;
                const auto result = g_aaCapabilityObservation.PublishIfChanged(g_pendingCapabilities);
                const bool failed = result == Publisher::PublishResult::kFailed;
                g_capabilityPublishPending.store(failed, std::memory_order_release);
                if (!failed) {
                    g_lastObservedDevice.store(a_device, std::memory_order_release);
                }
                return !failed;
            };

            const auto nextEpoch = [&]() noexcept -> std::uint64_t {
                constexpr std::uint64_t kExhausted = (std::numeric_limits<std::uint64_t>::max)();
                if (g_nextDeviceEpoch == 0U || g_nextDeviceEpoch == kExhausted) {
                    return 0U;
                }
                return g_nextDeviceEpoch++;
            };

            const auto stageIndeterminate = [&](std::uint64_t observedIdentity) noexcept {
                g_aaDeviceObservationGate.MarkFailClosed();
                if (observedIdentity == 0U) {
                    observedIdentity = static_cast<std::uint64_t>(
                        reinterpret_cast<std::uintptr_t>(a_device));
                }
                if (observedIdentity == 0U) {
                    observedIdentity = 1U;
                }

                std::uint64_t epoch = 0U;
                if (g_capabilityPublishPending.load(std::memory_order_relaxed) &&
                    g_pendingCapabilities.device.identity == observedIdentity &&
                    g_pendingCapabilities.state == Platform::AaCapabilityState::kIndeterminate) {
                    epoch = g_pendingCapabilities.device.epoch;
                }
                const auto current = g_aaCapabilityObservation.Read();
                if (epoch == 0U && current && current->device.identity == observedIdentity &&
                    current->state == Platform::AaCapabilityState::kIndeterminate) {
                    epoch = current->device.epoch;
                }
                if (epoch == 0U) {
                    epoch = nextEpoch();
                }
                if (epoch == 0U) {
                    epoch = 1U;
                }

                g_pendingCapabilities = {
                    Platform::AaDeviceGeneration{ observedIdentity, epoch },
                    Platform::AaCapabilityState::kIndeterminate,
                    Platform::AaAvailability::kUnknown,
                    Platform::AaAvailability::kUnknown,
                };
                g_capabilityPublishPending.store(true, std::memory_order_relaxed);
            };

            if (g_aaDeviceObservationGate.FailClosed()) {
                if (g_pendingCapabilities.device.identity != 0U &&
                    g_pendingCapabilities.device.epoch != 0U &&
                    g_pendingCapabilities.state == Platform::AaCapabilityState::kIndeterminate) {
                    g_capabilityPublishPending.store(true, std::memory_order_relaxed);
                    (void)publishPending();
                    return;
                }
                stageIndeterminate(0U);
                (void)publishPending();
                return;
            }

            if (g_lastObservedDevice.load(std::memory_order_relaxed) == a_device &&
                g_capabilityPublishPending.load(std::memory_order_relaxed)) {
                (void)publishPending();
                return;
            }

            std::uint64_t observedIdentity = 0U;
            {
                IUnknown* canonical = nullptr;
                if (a_device &&
                    SUCCEEDED(a_device->QueryInterface(
                        __uuidof(IUnknown), reinterpret_cast<void**>(&canonical))) &&
                    canonical) {
                    observedIdentity =
                        static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(canonical));
                    canonical->Release();
                } else {
                    observedIdentity =
                        static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(a_device));
                }
            }

            if (g_aaDeviceObservationGate.FailClosed()) {
                stageIndeterminate(observedIdentity);
                (void)publishPending();
                return;
            }

            const bool acceptedCurrent = observedIdentity != 0U &&
                (g_firstDeviceCapabilities.device.identity == 0U ||
                    g_firstDeviceCapabilities.device.identity == observedIdentity);
            const bool rejectedDifferent = !acceptedCurrent && observedIdentity != 0U;

            if (acceptedCurrent && observedIdentity != 0U &&
                (g_firstDeviceCapabilities.device.identity == 0U ||
                    g_firstDeviceCapabilities.device.identity == observedIdentity)) {
                if (g_firstDeviceCapabilities.device.identity == 0U) {
                    const std::uint64_t epoch = nextEpoch();
                    if (epoch == 0U) {
                        stageIndeterminate(observedIdentity);
                        (void)publishPending();
                        return;
                    }

                    const Platform::AaAvailability dlss =
                        Platform::Dlss12Engine::ProbeFirstDevice(a_device);
                    const Platform::AaAvailability fsr =
                        Platform::FsrEngine::ProbeFirstDevice(a_device);
                    g_firstDeviceCapabilities = {
                        Platform::AaDeviceGeneration{ observedIdentity, epoch },
                        Platform::AaCapabilityState::kResolved,
                        dlss,
                        fsr,
                    };
                    logger::info(
                        "[DLAA] first-device AA capabilities observed: identity={:#x} epoch={} dlss={} fsr={} (render resolution pending)",
                        observedIdentity, epoch, dlss == Platform::AaAvailability::kAvailable,
                        fsr == Platform::AaAvailability::kAvailable);
                    if (g_aaDeviceObservationGate.FailClosed()) {
                        stageIndeterminate(observedIdentity);
                        (void)publishPending();
                        return;
                    }
                }
                g_pendingCapabilities = g_firstDeviceCapabilities;
            } else {
                stageIndeterminate(observedIdentity);
                logger::error(
                    "[DLAA] {} D3D11 device observed (identity={:#x}, epoch={}); rebind is unproven, AA fails closed",
                    rejectedDifferent ? "later/different" : "indeterminate",
                    g_pendingCapabilities.device.identity, g_pendingCapabilities.device.epoch);
            }

            if (g_aaDeviceObservationGate.FailClosed()) {
                stageIndeterminate(observedIdentity);
            }
            g_capabilityPublishPending.store(true, std::memory_order_relaxed);
            (void)publishPending();
        } catch (...) {
            g_aaDeviceObservationGate.MarkFailClosed();
        }
    }

    void Streamline::InitMailbox(const DlaaSettings& a_settings)
    {
        Platform::AaRequest initial{};
        initial.requestedEngine = Platform::RequestedEngineOf(a_settings);
        initial.effectiveEngine = Platform::AaEffectiveEngine::kUnresolved;
        initial.dlaaEnabled = a_settings.enable;
        initial.preset = ValidateDlaaPreset(a_settings.preset);
        initial.autoExposure = a_settings.autoExposure;
        initial.resolutionScale = ClampResolutionScale(a_settings.resolutionScale);
        initial.qualityMode = ClampQualityMode(a_settings.qualityMode);
        initial.mipBias = ClampMipBias(a_settings.mipBias);
        initial.fsrMipBias = initial.mipBias;

        g_aaMailbox = std::make_unique<Platform::AaMailbox>(initial);
        g_renderCapabilities = Platform::AaEngineCapabilities{};
        g_boundDeviceGeneration = Platform::AaDeviceGeneration{};
        g_canonicalApplied = initial;
        g_canonicalApplied.dlaaEnabled = false;
        g_renderSelection = Platform::AaEngineSelectionSnapshot{};
        g_renderSelection.latestRequested = initial.requestedEngine;
        g_renderSelection.requestGeneration = initial.generation;
        g_renderSelection.appliedGeneration = initial.generation;
        g_startupEngineResolved = false;
        g_selectionFailClosed = false;
        g_mailboxHasPumped = false;
        g_lastPumpResult = {};
        (void)g_aaSelectionSnapshot.PublishIfChanged(g_renderSelection);
        (void)g_aaDiagnosticSnapshot.PublishIfChanged(
            Platform::AaEngineDiagnosticSnapshot{ g_renderSelection, g_aaMailbox->MakeDiagnosticSnapshot() });

        Platform::AaEffects effects;

        effects.freeDlss = []() noexcept -> Platform::AaTeardownResult {
            return Platform::Dlss12Engine::DestroyFeature();
        };

        effects.destroyFsrContext = []() noexcept -> bool {
            return Platform::FsrEngine::DestroyContext();
        };
        effects.rearmFsrLatch = []() noexcept { Platform::FsrEngine::RearmFailureLatch(); };

        effects.rearmExposure = []() noexcept {
            Platform::Dlss12Engine::RearmExposure();
        };

        effects.requestHistoryReset = []() noexcept {
            Platform::Fallout4Renderer::RequestHistoryReset("AA engine edge");
        };

        effects.logCommit = [](const Platform::AaCommitInfo& info) noexcept {
            if ((info.scope & Platform::kAaRecreateDlss) != 0 && (info.scope & Platform::kAaRecreateFsr) != 0) {
                if (info.effectiveEngine == Platform::AaEffectiveEngine::kFsr) {
                    logger::info("[DLAA] engine handoff COMMITTED -> FSR active; DLSS DISABLED (feature 1 released on the sidecar; zero DLSS calls while FSR is active)");
                } else if (info.effectiveEngine == Platform::AaEffectiveEngine::kDlss) {
                    logger::info("[DLAA] engine handoff COMMITTED -> DLSS active (on the D3D12 sidecar); FSR DISABLED (context destroyed; zero FSR dispatches while DLSS is active)");
                } else {
                    logger::info("[DLAA] engine handoff COMMITTED -> no AA engine (requested={}; fail-closed/unsupported)",
                        static_cast<int>(info.requestedEngine));
                }
            }
            logger::info("[DLAA] feature recreation committed (scope: dlss={} fsr={}; create-time options apply at next evaluate)",
                (info.scope & Platform::kAaRecreateDlss) != 0, (info.scope & Platform::kAaRecreateFsr) != 0);
        };

        g_aaMailbox->SetEffects(std::move(effects));
    }

    PumpResult Streamline::PumpMailbox(std::uint64_t a_frame) noexcept
    {
        if (!g_aaMailbox) {
            return {};
        }

        const auto publishSelectionThenRefreshFsr = []() noexcept {
            using SelectionPublisher =
                Platform::PublishedSnapshot<Platform::AaEngineSelectionSnapshot>;
            const auto outcome = g_aaSelectionSnapshot.PublishIfChanged(g_renderSelection);
            if (outcome != SelectionPublisher::PublishResult::kFailed) {
                (void)g_aaDiagnosticSnapshot.PublishIfChanged(
                    Platform::AaEngineDiagnosticSnapshot{
                        g_renderSelection, g_aaMailbox->MakeDiagnosticSnapshot() });
            }
        };

        const std::shared_ptr<const Platform::AaEngineCapabilities> observedNode =
            g_aaCapabilityObservation.Read();
        const Platform::AaEngineCapabilities observed = observedNode ? *observedNode :
                                                                     Platform::AaEngineCapabilities{};
        bool startupResolvedThisFrame = false;

        if (g_aaDeviceObservationGate.FailClosed()) {
            g_selectionFailClosed = true;
        }

        const Platform::AaRequest latestBeforeResolution = g_aaMailbox->PeekPublished();
        const Platform::AaCapabilityReduceResult reduced = Platform::ReduceAaCapabilityObservation(
            latestBeforeResolution.requestedEngine, g_renderCapabilities, observed);

        const bool observationDiffersFromBound =
            g_boundDeviceGeneration.identity != 0U &&
            observed.state != Platform::AaCapabilityState::kUnknown &&
            observed.device != g_boundDeviceGeneration;

        g_renderCapabilities = reduced.capabilities;
        if (g_boundDeviceGeneration.identity == 0U &&
            (reduced.outcome == Platform::AaCapabilityReduceOutcome::kAccepted ||
                reduced.outcome == Platform::AaCapabilityReduceOutcome::kNoChange) &&
            g_renderCapabilities.state == Platform::AaCapabilityState::kResolved) {
            g_boundDeviceGeneration = g_renderCapabilities.device;
        }

        const bool invalidOrConflictingReduction =
            reduced.outcome == Platform::AaCapabilityReduceOutcome::kRejectedInvalidCurrent ||
            reduced.outcome == Platform::AaCapabilityReduceOutcome::kRejectedInvalidObservation ||
            reduced.outcome == Platform::AaCapabilityReduceOutcome::kRejectedEpochExhausted ||
            reduced.outcome == Platform::AaCapabilityReduceOutcome::kFailClosedConflict;
        if (observationDiffersFromBound ||
            observed.state == Platform::AaCapabilityState::kIndeterminate ||
            invalidOrConflictingReduction) {
            g_selectionFailClosed = true;
        }

        if (!g_startupEngineResolved) {
            if (g_renderCapabilities.state == Platform::AaCapabilityState::kUnknown) {
                const Platform::AaRequest latest = g_aaMailbox->PeekPublished();
                g_canonicalApplied = g_aaMailbox->Applied();
                g_canonicalApplied.dlaaEnabled = false;
                g_canonicalApplied.effectiveEngine = Platform::AaEffectiveEngine::kUnresolved;
                g_renderSelection.latestRequested = latest.requestedEngine;
                g_renderSelection.capabilities = g_renderCapabilities;
                g_renderSelection.appliedEnabled = false;
                g_renderSelection.appliedEffective = Platform::AaEffectiveEngine::kUnresolved;
                g_renderSelection.targetEffective = Platform::AaEffectiveEngine::kUnresolved;
                g_renderSelection.targetReason = Platform::AaResolutionReason::kCapabilitiesPending;
                g_renderSelection.requestGeneration = latest.generation;
                g_renderSelection.appliedGeneration = g_canonicalApplied.generation;
                g_renderSelection.transition = Platform::AaSelectionTransition::kCapabilitiesPending;
                g_renderSelection.lastPumpOutcome = Platform::AaPumpOutcome::kIdle;
                if (g_selectionFailClosed) {
                    Platform::ApplyAaSelectionFailClosed(g_renderSelection);
                }
                publishSelectionThenRefreshFsr();
                return g_lastPumpResult;
            }

            const Platform::AaStartupResolutionResult startup =
                g_aaMailbox->InitializeEffectiveEngine(g_renderCapabilities);
            if (startup.outcome == Platform::AaStartupResolutionOutcome::kAllocationFailed) {
                const Platform::AaRequest latest = g_aaMailbox->PeekPublished();
                g_canonicalApplied = g_aaMailbox->Applied();
                g_canonicalApplied.dlaaEnabled = false;
                g_canonicalApplied.effectiveEngine = Platform::AaEffectiveEngine::kUnresolved;
                g_renderSelection.latestRequested = latest.requestedEngine;
                g_renderSelection.capabilities = g_renderCapabilities;
                g_renderSelection.appliedEnabled = false;
                g_renderSelection.appliedEffective = Platform::AaEffectiveEngine::kUnresolved;
                g_renderSelection.targetEffective = startup.resolution.effective;
                g_renderSelection.targetReason = startup.resolution.reason;
                g_renderSelection.requestGeneration = latest.generation;
                g_renderSelection.appliedGeneration = g_canonicalApplied.generation;
                g_renderSelection.transition = Platform::AaSelectionTransition::kCapabilitiesPending;
                g_renderSelection.lastPumpOutcome = Platform::AaPumpOutcome::kIdle;
                if (g_selectionFailClosed) {
                    Platform::ApplyAaSelectionFailClosed(g_renderSelection);
                }
                publishSelectionThenRefreshFsr();
                return g_lastPumpResult;
            }
            if (startup.outcome == Platform::AaStartupResolutionOutcome::kRejectedAfterPump ||
                g_mailboxHasPumped) {
                g_selectionFailClosed = true;
                g_startupEngineResolved = true;
            } else {
                g_startupEngineResolved = true;
                startupResolvedThisFrame = true;
            }
        }

        const Platform::AaEngineCapabilities capabilities = g_renderCapabilities;
        const bool failClosed = g_selectionFailClosed;
        g_aaMailbox->Publish([capabilities, failClosed](Platform::AaRequest& request) noexcept {
            const Platform::AaEngineResolution resolution =
                Platform::ResolveAaEngine(request.requestedEngine, capabilities);
            request.effectiveEngine = failClosed ? Platform::AaEffectiveEngine::kNone :
                                                   resolution.effective;
        });

        g_lastPumpResult = g_aaMailbox->Pump(a_frame);
        g_mailboxHasPumped = true;

        const Platform::AaRequest rawApplied = g_aaMailbox->Applied();
        const Platform::AaRequest latest = g_aaMailbox->PeekPublished();
        const Platform::AaEngineResolution target =
            Platform::ResolveAaEngine(latest.requestedEngine, g_renderCapabilities);

        g_canonicalApplied = rawApplied;
        if (g_selectionFailClosed) {
            g_canonicalApplied.effectiveEngine = Platform::AaEffectiveEngine::kNone;
        }
        g_canonicalApplied.dlaaEnabled = rawApplied.dlaaEnabled &&
            Platform::AaEffectiveEngineReady(g_canonicalApplied.effectiveEngine) &&
            !g_selectionFailClosed;

        g_renderSelection.latestRequested = latest.requestedEngine;
        g_renderSelection.capabilities = g_renderCapabilities;
        g_renderSelection.appliedEnabled = g_canonicalApplied.dlaaEnabled;
        g_renderSelection.appliedEffective = g_canonicalApplied.effectiveEngine;
        g_renderSelection.targetEffective = g_selectionFailClosed ?
            Platform::AaEffectiveEngine::kNone : target.effective;
        g_renderSelection.targetReason = target.reason;
        g_renderSelection.requestGeneration = latest.generation;
        g_renderSelection.appliedGeneration = rawApplied.generation;
        g_renderSelection.lastPumpOutcome = g_lastPumpResult.outcome;
        if (g_selectionFailClosed) {
            Platform::ApplyAaSelectionFailClosed(g_renderSelection);
        } else if (startupResolvedThisFrame) {
            g_renderSelection.transition = Platform::AaSelectionTransition::kStartupResolved;
        } else if (latest.generation != rawApplied.generation ||
            g_lastPumpResult.outcome == Platform::AaPumpOutcome::kDraining ||
            g_lastPumpResult.outcome == Platform::AaPumpOutcome::kSuperseded ||
            g_lastPumpResult.outcome == Platform::AaPumpOutcome::kCommitFailed) {
            g_renderSelection.transition = Platform::AaSelectionTransition::kDraining;
        } else {
            g_renderSelection.transition = Platform::AaSelectionTransition::kStable;
        }

        publishSelectionThenRefreshFsr();
        return g_lastPumpResult;
    }

    bool Streamline::DrainAffectsActiveEngine() noexcept { return g_lastPumpResult.affectsActiveEngine; }

    AaRequest Streamline::AppliedRequest() noexcept
    {
        Platform::AaRequest applied = g_canonicalApplied;
        if (g_aaDeviceObservationGate.FailClosed()) {
            applied.dlaaEnabled = false;
            applied.effectiveEngine = Platform::AaEffectiveEngine::kNone;
        }
        return applied;
    }

    std::uint32_t Streamline::AppliedDamagedScope() noexcept
    {
        return g_aaMailbox ? g_aaMailbox->DamagedScope() : Platform::kAaRecreateNone;
    }

    AaEffectiveEngine Streamline::AppliedEffectiveEngine() noexcept
    {
        return AppliedRequest().effectiveEngine;
    }

    AaEngineSelectionSnapshot Streamline::EngineSelectionSnapshot() noexcept
    {
        const std::shared_ptr<const Platform::AaEngineSelectionSnapshot> snapshot =
            g_aaSelectionSnapshot.Read();
        return snapshot ? *snapshot : Platform::AaEngineSelectionSnapshot{};
    }

    void Streamline::NoteDeviceCreated(ID3D11Device* device) noexcept
    {
        g_deviceGenerationTracker.NoteDeviceCreated(reinterpret_cast<std::uint64_t>(device));
    }

    void Streamline::NoteDeviceObserved(ID3D11Device* device) noexcept
    {
        g_deviceGenerationTracker.NoteDeviceObserved(reinterpret_cast<std::uint64_t>(device));
    }

    Platform::DeviceGeneration Streamline::RenderOwnedDeviceGeneration() noexcept
    {
        return g_deviceGenerationTracker.Current();
    }

    AaDiagnosticSnapshot Streamline::MailboxSnapshot() noexcept
    {
        return g_aaMailbox ? g_aaMailbox->MakeDiagnosticSnapshot() : Platform::AaDiagnosticSnapshot{};
    }

    void Streamline::ApplyDlaaSettings(bool a_enable, std::uint32_t a_preset, bool a_autoExposure) noexcept
    {
        if (!g_aaMailbox) {
            return;
        }
        const std::uint32_t validPreset = ValidateDlaaPreset(a_preset);
        g_aaMailbox->Publish([&](Platform::AaRequest& req) {
            req.dlaaEnabled = a_enable;
            req.preset = validPreset;
            req.autoExposure = a_autoExposure;
        });
    }

    void Streamline::ApplyDlaaSettingsLive(bool a_enable, std::uint32_t a_preset, bool a_autoExposure) noexcept
    {
        if (!g_aaMailbox) {
            return;
        }
        const std::uint32_t validPreset = ValidateDlaaPreset(a_preset);
        const Platform::AaRequest before = g_aaMailbox->PeekPublished();
        const bool presetChanged = before.preset != validPreset;
        const bool autoChanged = before.autoExposure != a_autoExposure;
        g_aaMailbox->Publish([&](Platform::AaRequest& req) {
            req.dlaaEnabled = a_enable;
            req.preset = validPreset;
            req.autoExposure = a_autoExposure;
        });
        if (autoChanged) {
            Platform::Dlss12Engine::RearmExposure();
        }
        if (presetChanged || autoChanged) {
            logger::info("[DLAA] live settings change staged a DLSS feature recreation (preset={} autoExposure={})",
                validPreset, a_autoExposure);
        }
    }

    bool Streamline::DlaaEnabled() noexcept
    {
        return AppliedRequest().dlaaEnabled;
    }

    void Streamline::LogDebugSnapshot() noexcept
    {
        const std::shared_ptr<const Platform::AaEngineDiagnosticSnapshot> node =
            g_aaDiagnosticSnapshot.Read();
        const Platform::AaEngineDiagnosticSnapshot diagnostic =
            node ? *node : Platform::AaEngineDiagnosticSnapshot{};
        const AaDiagnosticSnapshot& mailbox = diagnostic.mailbox;
        const AaEngineSelectionSnapshot& selection = diagnostic.selection;
        logger::info("[SNAPSHOT] requested={} effective={} target={} reason={} transition={} caps(state={} dlss={} fsr={} device={:#x}/epoch={}) generations(req={} applied={}) | enabled={} preset={} autoExposure={} resolutionScale={:.2f} drainCountdown={} drainScope={}",
            static_cast<int>(selection.latestRequested), static_cast<int>(selection.appliedEffective),
            static_cast<int>(selection.targetEffective), static_cast<int>(selection.targetReason),
            static_cast<int>(selection.transition), static_cast<int>(selection.capabilities.state),
            static_cast<int>(selection.capabilities.dlss), static_cast<int>(selection.capabilities.fsr),
            selection.capabilities.device.identity, selection.capabilities.device.epoch,
            selection.requestGeneration, selection.appliedGeneration,
            selection.appliedEnabled, mailbox.preset, mailbox.autoExposure, mailbox.resolutionScale,
            mailbox.drainCountdown, mailbox.drainScope);
    }

    bool Streamline::EvaluateDLAA(ID3D11DeviceContext* context, ID3D11Resource* color,
        ID3D11Resource* depth, ID3D11Resource* motionVectors, const D3D11_TEXTURE2D_DESC& colorDesc,
        std::uint32_t renderWidth, std::uint32_t renderHeight,
        float jitterPixelX, float jitterPixelY, bool reset) noexcept
    {
        if (!context || !color || !depth || !motionVectors) {
            return false;
        }
        const Platform::AaEffectiveEngine effective = AppliedEffectiveEngine();
        const bool useFsr = effective == Platform::AaEffectiveEngine::kFsr;
        const bool useDlss = effective == Platform::AaEffectiveEngine::kDlss;
        if (!useFsr && !useDlss) {
            return false;
        }

        const std::uint32_t width = colorDesc.Width;
        const std::uint32_t height = colorDesc.Height;
        if (width == 0 || height == 0) {
            return false;
        }
        if (colorDesc.MipLevels != 1 || colorDesc.ArraySize != 1 || colorDesc.SampleDesc.Count != 1) {
            static std::atomic<bool> logged{ false };
            if (!logged.exchange(true)) {
                logger::warn("[DLAA] color target is not 1 mip / 1 slice / 1 sample (mips={} slices={} samples={}); DLAA skipped (frame stays native - no AA)",
                    colorDesc.MipLevels, colorDesc.ArraySize, colorDesc.SampleDesc.Count);
            }
            return false;
        }

        const std::uint32_t renderW = (renderWidth == 0 || renderWidth > width) ? width : renderWidth;
        const std::uint32_t renderH = (renderHeight == 0 || renderHeight > height) ? height : renderHeight;

        const auto colorFormatRaw = static_cast<std::uint32_t>(colorDesc.Format);
        const bool colorIsHDR = Platform::IsHdrColorFormat(colorFormatRaw);
        g_colorPathFormat.store(colorFormatRaw, std::memory_order_relaxed);
        g_colorPathHdr.store(colorIsHDR, std::memory_order_relaxed);
        if (g_colorPathLoggedFormat != colorFormatRaw) {
            g_colorPathLoggedFormat = colorFormatRaw;
            logger::info("[DLSS] color path: {} (DXGI format {}) — colorBuffersHDR={}",
                colorIsHDR ? "pre-tonemap linear HDR" : "SDR (or unrecognized format — failing closed)",
                colorFormatRaw, colorIsHDR);
        }

        const Platform::AaRequest applied = AppliedRequest();
        const std::uint32_t preset = applied.preset;
        const bool autoExposure = applied.autoExposure;
        if (!useFsr && preset != g_previousPreset) {
            g_previousPreset = preset;
            logger::info("[DLSS] preset -> {}", preset);
        }

        const std::uint32_t qualityMode = applied.qualityMode;
        const bool subNative = qualityMode != 0U && (renderW < width || renderH < height);
        const bool qualityModeChanged = qualityMode != g_previousQualityMode;
        g_previousQualityMode = qualityMode;

        const float scale = applied.resolutionScale;
        const std::uint32_t outWFull = Platform::ResolveTargetDim(renderW, scale, width);
        const std::uint32_t outHFull = Platform::ResolveTargetDim(renderH, scale, height);
        bool superRes = scale > 1.001F && (outWFull != width || outHFull != height);
        if (!superRes && scale <= 1.001F && g_upscaleScratch) {
            if (g_upscaleScratchUAV) { g_upscaleScratchUAV->Release(); g_upscaleScratchUAV = nullptr; }
            if (g_upscaleScratchSRV) { g_upscaleScratchSRV->Release(); g_upscaleScratchSRV = nullptr; }
            g_upscaleScratch->Release();
            g_upscaleScratch = nullptr;
            g_upscaleW = 0;
            g_upscaleH = 0;
            g_upscaleFmt = DXGI_FORMAT_UNKNOWN;
            logger::info("[TRS] super-resolution off — upscale target freed");
        }

        if (!g_dlaaScratch || g_dlaaScratchW != width || g_dlaaScratchH != height ||
            g_dlaaScratchFmt != colorDesc.Format) {
            if (g_dlaaScratchW != width || g_dlaaScratchH != height || g_dlaaScratchFmt != colorDesc.Format) {
                g_dlaaScratchFailed = false;
            }
            if (g_dlaaScratchFailed) {
                return false;
            }
            if (g_dlaaScratchUAV) { g_dlaaScratchUAV->Release(); g_dlaaScratchUAV = nullptr; }
            if (g_dlaaScratchSRV) { g_dlaaScratchSRV->Release(); g_dlaaScratchSRV = nullptr; }
            if (g_dlaaScratch) {
                g_dlaaScratch->Release();
                g_dlaaScratch = nullptr;
            }
            ID3D11Device* device = nullptr;
            context->GetDevice(&device);
            if (!device) {
                return false;
            }
            D3D11_TEXTURE2D_DESC sd{};
            sd.Width = width;
            sd.Height = height;
            sd.MipLevels = 1;
            sd.ArraySize = 1;
            sd.Format = colorDesc.Format;
            sd.SampleDesc.Count = 1;
            sd.Usage = D3D11_USAGE_DEFAULT;
            sd.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
            const HRESULT hr = device->CreateTexture2D(&sd, nullptr, &g_dlaaScratch);
            device->Release();
            if (FAILED(hr) || !g_dlaaScratch) {
                g_dlaaScratch = nullptr;
                g_dlaaScratchFailed = true;
                g_dlaaScratchW = width;
                g_dlaaScratchH = height;
                g_dlaaScratchFmt = colorDesc.Format;
                logger::error("[DLAA] scratch CreateTexture2D failed ({:#x}); DLAA disabled until the resolution/format changes",
                    static_cast<std::uint32_t>(hr));
                return false;
            }
            g_dlaaScratchW = width;
            g_dlaaScratchH = height;
            g_dlaaScratchFmt = colorDesc.Format;
        }

        if (superRes) {
            ID3D11Device* device = nullptr;
            context->GetDevice(&device);
            const bool dimsChanged = (g_upscaleW != outWFull || g_upscaleH != outHFull ||
                                      g_upscaleFmt != colorDesc.Format);
            if (dimsChanged) {
                g_trsFailed = false;
            }
            if (!device || g_trsFailed) {
                if (device) { device->Release(); }
                superRes = false;
            } else {
                const auto trsFail = [&](const char* what) {
                    if (g_upscaleScratchUAV) { g_upscaleScratchUAV->Release(); g_upscaleScratchUAV = nullptr; }
                    if (g_upscaleScratchSRV) { g_upscaleScratchSRV->Release(); g_upscaleScratchSRV = nullptr; }
                    if (g_upscaleScratch) { g_upscaleScratch->Release(); g_upscaleScratch = nullptr; }
                    g_upscaleW = outWFull;
                    g_upscaleH = outHFull;
                    g_upscaleFmt = colorDesc.Format;
                    g_trsFailed = true;
                    if (g_aaMailbox) {
                        g_aaMailbox->Publish([](Platform::AaRequest& req) { ++req.forceRecreateSerial; });
                    }
                    logger::error("[TRS] {} — super-resolution disabled (native DLAA unaffected)", what);
                };
                if (!g_upscaleScratch || dimsChanged) {
                    if (g_upscaleScratchUAV) { g_upscaleScratchUAV->Release(); g_upscaleScratchUAV = nullptr; }
                    if (g_upscaleScratchSRV) { g_upscaleScratchSRV->Release(); g_upscaleScratchSRV = nullptr; }
                    if (g_upscaleScratch) { g_upscaleScratch->Release(); g_upscaleScratch = nullptr; }
                    D3D11_TEXTURE2D_DESC ud{};
                    ud.Width = outWFull;
                    ud.Height = outHFull;
                    ud.MipLevels = 1;
                    ud.ArraySize = 1;
                    ud.Format = colorDesc.Format;
                    ud.SampleDesc.Count = 1;
                    ud.Usage = D3D11_USAGE_DEFAULT;
                    ud.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
                    if (FAILED(device->CreateTexture2D(&ud, nullptr, &g_upscaleScratch)) || !g_upscaleScratch) {
                        trsFail("upscale target creation failed");
                    } else if (FAILED(device->CreateShaderResourceView(g_upscaleScratch, nullptr, &g_upscaleScratchSRV)) || !g_upscaleScratchSRV) {
                        trsFail("upscale target SRV creation failed");
                    } else if (FAILED(device->CreateUnorderedAccessView(g_upscaleScratch, nullptr, &g_upscaleScratchUAV)) || !g_upscaleScratchUAV) {
                        trsFail("upscale target UAV creation failed");
                    } else {
                        g_upscaleW = outWFull;
                        g_upscaleH = outHFull;
                        g_upscaleFmt = colorDesc.Format;
                    }
                }
                if (!g_trsFailed && !g_dlaaScratchUAV) {
                    if (FAILED(device->CreateUnorderedAccessView(g_dlaaScratch, nullptr, &g_dlaaScratchUAV)) || !g_dlaaScratchUAV) {
                        trsFail("downsample dest UAV creation failed");
                    }
                }
                if (!g_trsFailed && !g_downsampleCS) {
                    if (FAILED(device->CreateComputeShader(Platform::kDownsampleShaderBytecode,
                            Platform::kDownsampleShaderBytecodeSize, nullptr, &g_downsampleCS)) || !g_downsampleCS) {
                        trsFail("downsample CreateComputeShader failed");
                    }
                }
                if (!g_trsFailed && !g_resolveUpCS) {
                    if (FAILED(device->CreateComputeShader(Platform::kResolveUpShaderBytecode,
                            Platform::kResolveUpShaderBytecodeSize, nullptr, &g_resolveUpCS)) || !g_resolveUpCS) {
                        trsFail("resolve-up CreateComputeShader failed");
                    }
                }
                device->Release();
                if (g_trsFailed) {
                    superRes = false;
                }
            }
        }

        int perfQuality = 5;
        std::uint32_t outW = renderW;
        std::uint32_t outH = renderH;
        if (superRes) {
            outW = outWFull;
            outH = outHFull;
        } else if (subNative) {
            outW = width;
            outH = height;
        }
        if (superRes || subNative) {
            const float effRatio = outW > 0U
                ? static_cast<float>(renderW) / static_cast<float>(outW)
                : 1.0F;
            perfQuality = NgxPerfQualityForRatio(effRatio);
        }
        g_effectiveOutOverRender.store(
            renderW > 0U ? static_cast<float>(outW) / static_cast<float>(renderW) : 1.0F,
            std::memory_order_relaxed);
        if (qualityModeChanged) {
            logger::info("[DLSS] quality mode -> {} ({}); render {}x{} -> reconstruct {}x{} -> monitor {}x{}",
                qualityMode, Platform::DlaaQualityModeName(qualityMode), renderW, renderH,
                outW, outH, width, height);
        }

        context->CopyResource(g_dlaaScratch, color);

        if (useFsr) {
            if (!Platform::FsrEngine::Evaluate(context, g_dlaaScratch,
                    superRes ? g_upscaleScratch : nullptr, depth, motionVectors, renderW,
                    renderH, outW, outH, colorIsHDR,
                    g_sharpness.load(std::memory_order_relaxed), jitterPixelX, jitterPixelY,
                    reset)) {
                return false;
            }
        } else if (useDlss) {
            Platform::Dlss12Engine::Inputs in{};
            in.colorIn = g_dlaaScratch;
            in.colorOut = superRes ? g_upscaleScratch : g_dlaaScratch;
            in.depth = depth;
            in.motionVectors = motionVectors;
            in.renderW = renderW;
            in.renderH = renderH;
            in.outW = outW;
            in.outH = outH;
            in.colorIsHDR = colorIsHDR;
            in.autoExposure = autoExposure;
            in.exposureScale = g_exposureScale.load(std::memory_order_relaxed);
            in.preset = preset;
            in.perfQuality = perfQuality;
            in.jitterX = jitterPixelX;
            in.jitterY = jitterPixelY;
            in.reset = reset;
            {
                const auto cam = Platform::Fallout4Renderer::CameraSnapshot();
                in.nearPlane = cam.nearPlane;
                in.farPlane = cam.farPlane;
            }
            if (!Platform::Dlss12Engine::Evaluate(context, in)) {
                return false;
            }
            static std::atomic<bool> s_loggedDx12Active{ false };
            if (!s_loggedDx12Active.exchange(true, std::memory_order_relaxed)) {
                logger::info("[DLAA] active (DLSS on the D3D12 sidecar): preset={} at {}x{} -> {}x{} HDR={}", preset, renderW, renderH,
                    outW, outH, colorIsHDR);
            }
        }

        if (superRes) {
            const bool upsampleResolve = outW < width || outH < height;
            context->CSSetShader(upsampleResolve ? g_resolveUpCS : g_downsampleCS, nullptr, 0);
            ID3D11ShaderResourceView* dsrv[] = { g_upscaleScratchSRV };
            context->CSSetShaderResources(0, 1, dsrv);
            ID3D11UnorderedAccessView* duav[] = { g_dlaaScratchUAV };
            context->CSSetUnorderedAccessViews(0, 1, duav, nullptr);
            context->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
            ID3D11ShaderResourceView* nsrv[] = { nullptr };
            context->CSSetShaderResources(0, 1, nsrv);
            ID3D11UnorderedAccessView* nuav[] = { nullptr };
            context->CSSetUnorderedAccessViews(0, 1, nuav, nullptr);
            context->CSSetShader(nullptr, nullptr, 0);
            if (g_loggedScale != scale) {
                g_loggedScale = scale;
                logger::info("[TRS] active: scale={:.2f} in={}x{} out={}x{} mode={}", scale, renderW, renderH,
                    outW, outH, perfQuality);
            }
        }

        g_stampSource = useFsr ? static_cast<ID3D11Resource*>(g_dlaaScratch)
                               : RunRcas(context, width, height);

        return true;
    }

    bool Streamline::SharpenOnly(ID3D11DeviceContext* context, ID3D11Resource* color,
        const D3D11_TEXTURE2D_DESC& colorDesc) noexcept
    {
        try {
            if (!context || !color) {
                return false;
            }
            if (g_sharpness.load(std::memory_order_relaxed) <= 0.0F) {
                return false;
            }
            const std::uint32_t width = colorDesc.Width;
            const std::uint32_t height = colorDesc.Height;
            if (width == 0 || height == 0) {
                return false;
            }
            if (colorDesc.MipLevels != 1 || colorDesc.ArraySize != 1 ||
                colorDesc.SampleDesc.Count != 1) {
                return false;
            }
            if (!g_dlaaScratch || g_dlaaScratchW != width || g_dlaaScratchH != height ||
                g_dlaaScratchFmt != colorDesc.Format) {
                if (g_dlaaScratchW != width || g_dlaaScratchH != height ||
                    g_dlaaScratchFmt != colorDesc.Format) {
                    g_dlaaScratchFailed = false;
                }
                if (g_dlaaScratchFailed) {
                    return false;
                }
                if (g_dlaaScratchUAV) { g_dlaaScratchUAV->Release(); g_dlaaScratchUAV = nullptr; }
                if (g_dlaaScratchSRV) { g_dlaaScratchSRV->Release(); g_dlaaScratchSRV = nullptr; }
                if (g_dlaaScratch) { g_dlaaScratch->Release(); g_dlaaScratch = nullptr; }
                ID3D11Device* device = nullptr;
                context->GetDevice(&device);
                if (!device) {
                    return false;
                }
                D3D11_TEXTURE2D_DESC sd{};
                sd.Width = width;
                sd.Height = height;
                sd.MipLevels = 1;
                sd.ArraySize = 1;
                sd.Format = colorDesc.Format;
                sd.SampleDesc.Count = 1;
                sd.Usage = D3D11_USAGE_DEFAULT;
                sd.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
                const HRESULT hr = device->CreateTexture2D(&sd, nullptr, &g_dlaaScratch);
                device->Release();
                g_dlaaScratchW = width;
                g_dlaaScratchH = height;
                g_dlaaScratchFmt = colorDesc.Format;
                if (FAILED(hr) || !g_dlaaScratch) {
                    g_dlaaScratch = nullptr;
                    g_dlaaScratchFailed = true;
                    logger::error("[RCAS] standalone scratch CreateTexture2D failed ({:#x}) — sharpening off until the resolution/format changes",
                        static_cast<std::uint32_t>(hr));
                    return false;
                }
            }
            context->CopyResource(g_dlaaScratch, color);
            ID3D11Resource* const result = RunRcas(context, width, height);
            if (!result || result == g_dlaaScratch) {
                return false;
            }
            context->CopyResource(color, result);
            static std::atomic<bool> logged{ false };
            if (!logged.exchange(true)) {
                logger::info("[RCAS] standalone sharpening active (DLAA off) — {}x{}", width, height);
            }
            return true;
        } catch (...) {
            return false;
        }
    }

    void Streamline::SetSharpness(float sharpness) noexcept
    {
        const float clamped = std::clamp(sharpness, 0.0F, 1.0F);
        if (g_sharpness.exchange(clamped, std::memory_order_relaxed) != clamped) {
            g_rcasFailed.store(false, std::memory_order_relaxed);
        }
    }

    void Streamline::SetExposureScale(float scale) noexcept
    {
        g_exposureScale.store(std::clamp(scale, 0.1F, 16.0F), std::memory_order_relaxed);
    }

    void Streamline::SetMipBias(float a_bias) noexcept
    {
        if (g_aaMailbox) {
            g_aaMailbox->Publish([&](Platform::AaRequest& req) {
                req.mipBias = a_bias;
                req.fsrMipBias = a_bias;
            });
        }
    }

    void Streamline::SetRequestedEngine(Platform::AaEngineRequest a_want) noexcept
    {
        if (!g_aaMailbox) {
            return;
        }
        const Platform::AaEngineRequest want = a_want;
        g_aaMailbox->Publish([want](Platform::AaRequest& req) {
            if (req.requestedEngine == want) {
                return;
            }
            req.requestedEngine = want;
            ++req.fsrRetrySerial;
            ++req.historyResetSerial;
        });
    }

    void Streamline::SetFsrVelocity(float a_value) noexcept
    {
        Platform::FsrEngine::SetVelocityFactor(a_value);
    }

    void Streamline::SetFsrReactiveness(float a_value) noexcept
    {
        Platform::FsrEngine::SetReactiveness(a_value);
    }

    void Streamline::SetFsrShadingChange(float a_value) noexcept
    {
        Platform::FsrEngine::SetShadingChange(a_value);
    }

    void Streamline::SetFsrAccumulation(float a_value) noexcept
    {
        Platform::FsrEngine::SetAccumulation(a_value);
    }

    void Streamline::SetFsrMinDisocclusion(float a_value) noexcept
    {
        Platform::FsrEngine::SetMinDisocclusion(a_value);
    }

    void Streamline::SetFsrMasks(bool a_enabled) noexcept
    {
        Platform::FsrEngine::SetMasksEnabled(a_enabled);
    }

    void Streamline::SetFsrTransparencyScale(float a_value) noexcept
    {
        Platform::FsrEngine::SetTransparencyScale(a_value);
    }

    void Streamline::SetResolutionScale(float scale, bool a_stageRecreate) noexcept
    {
        if (!g_aaMailbox) {
            return;
        }
        const float clamped = ClampResolutionScale(scale);
        const bool changed = g_aaMailbox->PeekPublished().resolutionScale != clamped;
        g_aaMailbox->Publish([&](Platform::AaRequest& req) { req.resolutionScale = clamped; });
        if (changed && a_stageRecreate) {
            logger::info("[TRS] resolution scale -> {:.2f} (DLSS feature recreation staged; FSR re-keys via its debounce)", clamped);
        }
    }

    float Streamline::CurrentResolutionScale() noexcept
    {
        return g_canonicalApplied.resolutionScale;
    }

    void Streamline::SetQualityMode(std::uint32_t mode, bool a_stageRecreate) noexcept
    {
        if (!g_aaMailbox) {
            return;
        }
        const std::uint32_t clamped = ClampQualityMode(mode);
        const bool changed = g_aaMailbox->PeekPublished().qualityMode != clamped;
        g_aaMailbox->Publish([&](Platform::AaRequest& req) { req.qualityMode = clamped; });
        if (changed && a_stageRecreate) {
            logger::info("[DLSS] quality mode -> {} ({}) (feature recreation staged; render ratio re-derives on commit)",
                clamped, DlaaQualityModeName(clamped));
        }
    }

    std::uint32_t Streamline::CurrentQualityMode() noexcept
    {
        return g_canonicalApplied.qualityMode;
    }

    bool Streamline::ColorPathIsHdr(std::uint32_t& a_outDxgiFormat) noexcept
    {
        a_outDxgiFormat = g_colorPathFormat.load(std::memory_order_relaxed);
        return g_colorPathHdr.load(std::memory_order_relaxed);
    }

    float Streamline::EffectiveOutOverRender() noexcept
    {
        return g_effectiveOutOverRender.load(std::memory_order_relaxed);
    }

    float Streamline::QueryOptimalRatio(
        std::uint32_t qualityMode, std::uint32_t outputW, std::uint32_t outputH) noexcept
    {
        const std::uint32_t mode = Platform::ClampQualityMode(qualityMode);
        if (mode == 0U || outputW == 0U || outputH == 0U) {
            return 1.0F;
        }
        float ratio = Platform::DlaaQualityModeFallbackRatio(mode);
        const Platform::AaEffectiveEngine effectiveNow = AppliedEffectiveEngine();
        if (effectiveNow == Platform::AaEffectiveEngine::kFsr) {
            logger::info("[FSR] render ratio for {} at {}x{}: {:.3f} (published table — the "
                         "FSR quality-mode convention)",
                Platform::DlaaQualityModeName(mode), outputW, outputH, ratio);
            return ratio;
        }
        if (!(ratio > 0.05F) || ratio > 1.0F) {
            ratio = 1.0F;
        }
        logger::info("[DLSS12] render ratio for {} at {}x{}: {:.3f} (~{}x{}, published NVIDIA table)",
            Platform::DlaaQualityModeName(mode), outputW, outputH, ratio,
            static_cast<std::uint32_t>(static_cast<float>(outputW) * ratio),
            static_cast<std::uint32_t>(static_cast<float>(outputH) * ratio));
        return ratio;
    }

    void Streamline::StampDlaaResult(ID3D11DeviceContext* context, ID3D11Resource* color) noexcept
    {
        ID3D11Resource* const src = g_stampSource ? g_stampSource : static_cast<ID3D11Resource*>(g_dlaaScratch);
        if (context && color && src) {
            context->CopyResource(color, src);
        }
    }

}
