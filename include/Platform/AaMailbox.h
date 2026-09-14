#pragma once

#include "Platform/AaEngineResolution.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>

namespace Platform
{
    inline constexpr std::uint32_t kAaRecreateNone = 0U;
    inline constexpr std::uint32_t kAaRecreateDlss = 1U << 0;
    inline constexpr std::uint32_t kAaRecreateFsr = 1U << 1;
    inline constexpr std::uint32_t kAaRecreateBoth = kAaRecreateDlss | kAaRecreateFsr;

    struct AaRequest
    {
        std::uint64_t generation{ 0 };
        AaEngineRequest requestedEngine{ AaEngineRequest::kDlss };
        AaEffectiveEngine effectiveEngine{ AaEffectiveEngine::kDlss };
        bool dlaaEnabled{ true };
        std::uint32_t preset{ 11 };
        bool autoExposure{ true };
        float resolutionScale{ 1.0F };
        std::uint32_t qualityMode{ 0 };
        std::uint64_t historyResetSerial{ 0 };
        std::uint64_t fsrRetrySerial{ 0 };
        std::uint64_t exposureRetrySerial{ 0 };

        std::uint64_t forceRecreateSerial{ 0 };

        float mipBias{ -1.0F };
        float fsrMipBias{ -1.0F };
    };

    enum class AaPumpOutcome : std::uint8_t
    {
        kIdle,
        kDraining,
        kCommitted,
        kSuperseded,
        kCommitFailed
    };

    struct PumpResult
    {
        AaPumpOutcome outcome{ AaPumpOutcome::kIdle };
        std::uint64_t appliedGeneration{ 0 };
        bool affectsActiveEngine{ false };
    };

    struct AaCommitInfo
    {
        std::uint64_t generation{ 0 };
        AaEngineRequest requestedEngine{ AaEngineRequest::kDlss };
        AaEffectiveEngine effectiveEngine{ AaEffectiveEngine::kUnresolved };
        bool dlaaEnabled{ false };
        std::uint32_t preset{ 0 };
        bool autoExposure{ false };
        float resolutionScale{ 1.0F };
        std::uint32_t qualityMode{ 0 };
        std::uint32_t scope{ kAaRecreateNone };
    };

    struct AaDiagnosticSnapshot
    {
        std::uint64_t latestGeneration{ 0 };
        std::uint64_t appliedGeneration{ 0 };
        AaEngineRequest requestedEngine{ AaEngineRequest::kDlss };
        AaEffectiveEngine effectiveEngine{ AaEffectiveEngine::kUnresolved };
        bool dlaaEnabled{ false };
        std::uint32_t preset{ 0 };
        bool autoExposure{ false };
        float resolutionScale{ 1.0F };
        std::uint32_t qualityMode{ 0 };
        int drainCountdown{ 0 };
        std::uint32_t drainScope{ kAaRecreateNone };
        std::uint64_t historyResetSerial{ 0 };
        std::uint64_t fsrRetrySerial{ 0 };
        std::uint64_t exposureRetrySerial{ 0 };
        std::uint64_t forceRecreateSerial{ 0 };
        std::uint32_t completedTeardownSteps{ kAaRecreateNone };
        std::uint32_t damagedScope{ kAaRecreateNone };

        friend bool operator==(const AaDiagnosticSnapshot&, const AaDiagnosticSnapshot&) noexcept = default;
    };

    enum class AaStartupResolutionOutcome : std::uint8_t
    {
        kInitialized,
        kUnchanged,
        kRejectedAfterPump,
        kAllocationFailed,
    };

    struct AaStartupResolutionResult
    {
        AaStartupResolutionOutcome outcome{ AaStartupResolutionOutcome::kUnchanged };
        AaEngineResolution resolution{};
        std::uint64_t generation{ 0 };
    };

    enum class AaTeardownResult : std::uint8_t
    {
        kFailed,
        kAlreadyAbsent,
        kDestroyed
    };

    struct AaEffects
    {
        std::function<AaTeardownResult()> freeDlss;
        std::function<bool()> destroyFsrContext;
        std::function<void()> rearmFsrLatch;
        std::function<void()> rearmExposure;
        std::function<void()> requestHistoryReset;
        std::function<void(const AaCommitInfo&)> logCommit;
    };

    struct AaPendingDrain
    {
        AaRequest target{};
        int countdown{ 0 };
        std::uint32_t scope{ kAaRecreateNone };
        std::uint32_t completedTeardownSteps{ kAaRecreateNone };

        std::uint64_t consumedHistoryResetSerial{ 0 };
        std::uint64_t consumedFsrRetrySerial{ 0 };
        std::uint64_t consumedExposureRetrySerial{ 0 };

        bool fsrEdgeDelivered{ false };
        bool exposureEdgeDelivered{ false };
        bool historyEdgeDelivered{ false };
    };

    inline constexpr int kAaDrainFrames = 4;

    inline constexpr bool kAaRequestPublicationIsAtomicSharedPtr = true;
    inline constexpr bool kAaDiagnosticPublicationIsAtomicSharedPtr = true;

    struct AaMailboxAllocators
    {
        std::function<std::shared_ptr<const AaRequest>(const AaRequest&)> makeRequestNode;
        std::function<std::shared_ptr<const AaDiagnosticSnapshot>(const AaDiagnosticSnapshot&)> makeDiagnosticNode;
        std::function<void()> beforeStartupIdentityCheck;
    };

    class AaMailbox
    {
    public:
        explicit AaMailbox(const AaRequest& initial = AaRequest{}, AaMailboxAllocators allocators = {});

        [[nodiscard]] AaStartupResolutionResult InitializeEffectiveEngine(
            const AaEngineCapabilities& capabilities) noexcept;

        template <class Fn>
        std::uint64_t Publish(Fn&& builder) noexcept
        {
            std::shared_ptr<const AaRequest> current = published_.load(std::memory_order_acquire);
            for (;;) {
                AaRequest next = *current;
                builder(next);
                next.generation = current->generation;
                next.mipBias = ClampBias(next.mipBias);
                next.fsrMipBias = ClampBias(next.fsrMipBias);
                next.qualityMode = ClampQuality(next.qualityMode);

                if (RequestsEqual(next, *current)) {
                    return current->generation;
                }

                next.generation = current->generation + 1;

                std::shared_ptr<const AaRequest> desired;
                try {
                    desired = std::make_shared<const AaRequest>(next);
                } catch (...) {
                    return current->generation;
                }

                if (published_.compare_exchange_weak(
                        current, desired, std::memory_order_acq_rel, std::memory_order_acquire)) {
                    return next.generation;
                }
            }
        }

        void SetEffects(AaEffects effects) noexcept;

        [[nodiscard]] PumpResult Pump(std::uint64_t frameCounter) noexcept;

        [[nodiscard]] AaRequest Applied() const noexcept;

        [[nodiscard]] std::uint32_t DamagedScope() const noexcept;

        [[nodiscard]] std::uint32_t DlssTeardownSerial() const noexcept;

        [[nodiscard]] AaDiagnosticSnapshot MakeDiagnosticSnapshot() const noexcept;

        [[nodiscard]] std::shared_ptr<const AaDiagnosticSnapshot> PeekDiagnosticIdentityNode() const noexcept;

        [[nodiscard]] AaRequest PeekPublished() const noexcept;

        [[nodiscard]] std::shared_ptr<const AaRequest> PeekPublishedNode() const noexcept;

        [[nodiscard]] bool HasPublishedRequestNode() const noexcept;

    private:
        [[nodiscard]] AaRequest ReadPublishedNow() const noexcept;
        void PublishDiagnostic() noexcept;

        void RetargetPendingTo(const AaRequest& newTarget) noexcept;

        [[nodiscard]] bool FireNotificationAndCheckStale(bool wasActive, bool willBeActive,
            std::uint64_t targetSerial, std::uint64_t& consumedSerial, bool& edgeDelivered,
            const std::function<void()>& callback, PumpResult& result) noexcept;

        [[nodiscard]] static float ClampBias(float bias) noexcept
        {
            if (!std::isfinite(bias)) {
                return 0.0F;
            }
            if (bias < -3.0F) {
                return -3.0F;
            }
            if (bias > 0.0F) {
                return 0.0F;
            }
            return bias;
        }

        [[nodiscard]] static std::uint32_t ClampQuality(std::uint32_t mode) noexcept
        {
            return mode < 6U ? mode : 0U;
        }

        [[nodiscard]] static bool RequestsEqual(const AaRequest& a, const AaRequest& b) noexcept
        {
            return a.generation == b.generation && a.requestedEngine == b.requestedEngine &&
                a.effectiveEngine == b.effectiveEngine &&
                a.dlaaEnabled == b.dlaaEnabled && a.preset == b.preset &&
                a.autoExposure == b.autoExposure && a.resolutionScale == b.resolutionScale &&
                a.qualityMode == b.qualityMode &&
                a.historyResetSerial == b.historyResetSerial && a.fsrRetrySerial == b.fsrRetrySerial &&
                a.exposureRetrySerial == b.exposureRetrySerial &&
                a.forceRecreateSerial == b.forceRecreateSerial &&
                a.mipBias == b.mipBias && a.fsrMipBias == b.fsrMipBias;
        }

        [[nodiscard]] static bool DiagnosticSnapshotsEqual(
            const AaDiagnosticSnapshot& a, const AaDiagnosticSnapshot& b) noexcept
        {
            return a.appliedGeneration == b.appliedGeneration &&
                a.requestedEngine == b.requestedEngine && a.effectiveEngine == b.effectiveEngine &&
                a.dlaaEnabled == b.dlaaEnabled && a.preset == b.preset &&
                a.autoExposure == b.autoExposure && a.resolutionScale == b.resolutionScale &&
                a.qualityMode == b.qualityMode &&
                a.drainCountdown == b.drainCountdown && a.drainScope == b.drainScope &&
                a.completedTeardownSteps == b.completedTeardownSteps && a.damagedScope == b.damagedScope;
        }

        std::atomic<std::shared_ptr<const AaRequest>> published_;
        std::function<std::shared_ptr<const AaRequest>(const AaRequest&)> makeRequestNode_;
        std::function<void()> beforeStartupIdentityCheck_;

        AaRequest applied_{};
        AaPendingDrain pending_{};

        std::uint32_t damagedScope_{ kAaRecreateNone };

        std::uint32_t dlssTeardownSerial_{ 0 };

        bool hasConsumedFrame_{ false };
        std::uint64_t lastConsumedFrame_{ 0 };

        AaEffects effects_{};

        std::atomic<std::shared_ptr<const AaDiagnosticSnapshot>> diagPublished_;
    };
}
