#include "Platform/AaMailbox.h"

#include <new>
#include <utility>

namespace
{
    using Platform::AaCommitInfo;
    using Platform::AaEffectiveEngine;
    using Platform::AaRequest;
    using Platform::AaTeardownResult;
    using Platform::kAaDrainFrames;
    using Platform::kAaRecreateDlss;
    using Platform::kAaRecreateFsr;
    using Platform::kAaRecreateNone;

    [[nodiscard]] constexpr bool IsDlssFamily(const AaEffectiveEngine engine) noexcept
    {
        return engine == AaEffectiveEngine::kDlss;
    }

    [[nodiscard]] constexpr bool IsLiveEngine(const AaEffectiveEngine engine) noexcept
    {
        return IsDlssFamily(engine) || engine == AaEffectiveEngine::kFsr;
    }

    [[nodiscard]] std::uint32_t ComputeScope(const AaRequest& from, const AaRequest& to) noexcept
    {
        const auto engineScope = [](const AaEffectiveEngine engine) noexcept {
            switch (engine) {
            case AaEffectiveEngine::kDlss:
                return kAaRecreateDlss;
            case AaEffectiveEngine::kFsr:
                return kAaRecreateFsr;
            default:
                return kAaRecreateNone;
            }
        };

        std::uint32_t scope = kAaRecreateNone;
        if (from.effectiveEngine != to.effectiveEngine) {
            const std::uint32_t fromScope = engineScope(from.effectiveEngine);
            const std::uint32_t toScope = engineScope(to.effectiveEngine);
            scope |= fromScope | toScope;
        }

        if (from.preset != to.preset || from.autoExposure != to.autoExposure ||
            from.resolutionScale != to.resolutionScale || from.qualityMode != to.qualityMode) {
            if (to.effectiveEngine == AaEffectiveEngine::kDlss ||
                to.effectiveEngine == AaEffectiveEngine::kFsr) {
                scope |= kAaRecreateDlss;
            }
        }

        if (from.forceRecreateSerial != to.forceRecreateSerial) {
            scope |= engineScope(to.effectiveEngine);
        }
        return scope;
    }

    [[nodiscard]] bool AffectsActiveEngine(const AaRequest& applied, std::uint32_t scope) noexcept
    {
        if (!applied.dlaaEnabled) {
            return false;
        }
        std::uint32_t activeBit = kAaRecreateNone;
        if (applied.effectiveEngine == AaEffectiveEngine::kDlss) {
            activeBit = kAaRecreateDlss;
        } else if (applied.effectiveEngine == AaEffectiveEngine::kFsr) {
            activeBit = kAaRecreateFsr;
        }
        return (scope & activeBit) != 0U;
    }

    [[nodiscard]] int InitialDrainCountdown(std::uint32_t scope) noexcept
    {
        return scope == kAaRecreateNone ? 1 : kAaDrainFrames;
    }

    [[nodiscard]] bool InvokeDestructiveStep(const std::function<bool()>& fn) noexcept
    {
        if (!fn) {
            return true;
        }
        try {
            return fn();
        } catch (...) {
            return false;
        }
    }

    [[nodiscard]] AaTeardownResult InvokeDlssTeardownStep(const std::function<AaTeardownResult()>& fn) noexcept
    {
        if (!fn) {
            return AaTeardownResult::kAlreadyAbsent;
        }
        try {
            return fn();
        } catch (...) {
            return AaTeardownResult::kFailed;
        }
    }

    void InvokeNoncriticalEffect(const std::function<void()>& fn) noexcept
    {
        if (!fn) {
            return;
        }
        try {
            fn();
        } catch (...) {
        }
    }

    void InvokeLogCommit(const std::function<void(const AaCommitInfo&)>& fn, const AaCommitInfo& info) noexcept
    {
        if (!fn) {
            return;
        }
        try {
            fn(info);
        } catch (...) {
        }
    }
}

namespace Platform
{
    AaMailbox::AaMailbox(const AaRequest& initial, AaMailboxAllocators allocators)
        : makeRequestNode_(std::move(allocators.makeRequestNode)),
          beforeStartupIdentityCheck_(std::move(allocators.beforeStartupIdentityCheck))
    {
        AaRequest normalizedInitial = initial;
        normalizedInitial.mipBias = ClampBias(normalizedInitial.mipBias);
        normalizedInitial.fsrMipBias = ClampBias(normalizedInitial.fsrMipBias);
        normalizedInitial.qualityMode = ClampQuality(normalizedInitial.qualityMode);

        const std::shared_ptr<const AaRequest> requestNode = makeRequestNode_
            ? makeRequestNode_(normalizedInitial)
            : std::make_shared<const AaRequest>(normalizedInitial);
        if (!requestNode) {
            throw std::bad_alloc{};
        }

        AaDiagnosticSnapshot initialDiagnostic{};
        initialDiagnostic.appliedGeneration = normalizedInitial.generation;
        initialDiagnostic.requestedEngine = normalizedInitial.requestedEngine;
        initialDiagnostic.effectiveEngine = normalizedInitial.effectiveEngine;
        initialDiagnostic.dlaaEnabled = normalizedInitial.dlaaEnabled;
        initialDiagnostic.preset = normalizedInitial.preset;
        initialDiagnostic.autoExposure = normalizedInitial.autoExposure;
        initialDiagnostic.resolutionScale = normalizedInitial.resolutionScale;
        initialDiagnostic.qualityMode = normalizedInitial.qualityMode;

        const std::shared_ptr<const AaDiagnosticSnapshot> diagnosticNode = allocators.makeDiagnosticNode
            ? allocators.makeDiagnosticNode(initialDiagnostic)
            : std::make_shared<const AaDiagnosticSnapshot>(initialDiagnostic);
        if (!diagnosticNode) {
            throw std::bad_alloc{};
        }

        published_.store(requestNode, std::memory_order_relaxed);
        diagPublished_.store(diagnosticNode, std::memory_order_relaxed);

        applied_ = normalizedInitial;
        pending_ = AaPendingDrain{};
        pending_.target = normalizedInitial;
        pending_.consumedHistoryResetSerial = normalizedInitial.historyResetSerial;
        pending_.consumedFsrRetrySerial = normalizedInitial.fsrRetrySerial;
        pending_.consumedExposureRetrySerial = normalizedInitial.exposureRetrySerial;
    }

    void AaMailbox::SetEffects(AaEffects effects) noexcept
    {
        effects_ = std::move(effects);
    }

    AaStartupResolutionResult AaMailbox::InitializeEffectiveEngine(
        const AaEngineCapabilities& capabilities) noexcept
    {
        std::shared_ptr<const AaRequest> current = published_.load(std::memory_order_acquire);
        AaEngineResolution resolution = ResolveAaEngine(current->requestedEngine, capabilities);
        if (hasConsumedFrame_) {
            return { AaStartupResolutionOutcome::kRejectedAfterPump, resolution, current->generation };
        }

        const auto rebase = [this](const AaRequest& request) noexcept {
            applied_ = request;
            pending_ = AaPendingDrain{};
            pending_.target = applied_;
            pending_.consumedHistoryResetSerial = applied_.historyResetSerial;
            pending_.consumedFsrRetrySerial = applied_.fsrRetrySerial;
            pending_.consumedExposureRetrySerial = applied_.exposureRetrySerial;
            PublishDiagnostic();
        };

        const auto allocationFailure = [this, &capabilities]() noexcept {
            const std::shared_ptr<const AaRequest> latest =
                published_.load(std::memory_order_acquire);
            const AaEngineResolution latestResolution =
                ResolveAaEngine(latest->requestedEngine, capabilities);
            const AaStartupResolutionOutcome outcome = hasConsumedFrame_
                ? AaStartupResolutionOutcome::kRejectedAfterPump
                : AaStartupResolutionOutcome::kAllocationFailed;
            return AaStartupResolutionResult{ outcome, latestResolution, latest->generation };
        };

        bool publishedResolution = false;
        for (;;) {
            resolution = ResolveAaEngine(current->requestedEngine, capabilities);
            if (current->effectiveEngine == resolution.effective) {
                if (beforeStartupIdentityCheck_) {
                    try {
                        beforeStartupIdentityCheck_();
                    } catch (...) {
                    }
                }
                if (hasConsumedFrame_) {
                    current = published_.load(std::memory_order_acquire);
                    resolution = ResolveAaEngine(current->requestedEngine, capabilities);
                    return { AaStartupResolutionOutcome::kRejectedAfterPump,
                        resolution, current->generation };
                }
                const std::shared_ptr<const AaRequest> observed =
                    published_.load(std::memory_order_acquire);
                if (observed != current) {
                    current = observed;
                    continue;
                }
                rebase(*current);
                return { publishedResolution ? AaStartupResolutionOutcome::kInitialized
                                             : AaStartupResolutionOutcome::kUnchanged,
                    resolution, current->generation };
            }

            AaRequest next = *current;
            next.effectiveEngine = resolution.effective;
            next.generation = current->generation + 1U;

            std::shared_ptr<const AaRequest> desired;
            try {
                desired = makeRequestNode_ ? makeRequestNode_(next)
                                           : std::make_shared<const AaRequest>(next);
            } catch (...) {
                return allocationFailure();
            }
            if (!desired) {
                return allocationFailure();
            }
            if (hasConsumedFrame_) {
                current = published_.load(std::memory_order_acquire);
                resolution = ResolveAaEngine(current->requestedEngine, capabilities);
                return { AaStartupResolutionOutcome::kRejectedAfterPump,
                    resolution, current->generation };
            }

            if (!published_.compare_exchange_weak(
                    current, desired, std::memory_order_acq_rel, std::memory_order_acquire)) {
                continue;
            }

            publishedResolution = true;
            rebase(*desired);
            current = published_.load(std::memory_order_acquire);
            if (current != desired) {
                continue;
            }

            return { AaStartupResolutionOutcome::kInitialized, resolution, desired->generation };
        }
    }

    AaRequest AaMailbox::ReadPublishedNow() const noexcept
    {
        return *PeekPublishedNode();
    }

    void AaMailbox::PublishDiagnostic() noexcept
    {
        AaDiagnosticSnapshot snap{};
        snap.appliedGeneration = applied_.generation;
        snap.requestedEngine = applied_.requestedEngine;
        snap.effectiveEngine = applied_.effectiveEngine;
        snap.dlaaEnabled = applied_.dlaaEnabled;
        snap.preset = applied_.preset;
        snap.autoExposure = applied_.autoExposure;
        snap.resolutionScale = applied_.resolutionScale;
        snap.qualityMode = applied_.qualityMode;
        snap.drainCountdown = pending_.countdown;
        snap.drainScope = pending_.scope;
        snap.completedTeardownSteps = pending_.completedTeardownSteps;
        snap.damagedScope = damagedScope_;

        const std::shared_ptr<const AaDiagnosticSnapshot> current =
            diagPublished_.load(std::memory_order_acquire);
        if (current && DiagnosticSnapshotsEqual(snap, *current)) {
            return;
        }

        try {
            diagPublished_.store(std::make_shared<const AaDiagnosticSnapshot>(snap), std::memory_order_release);
        } catch (...) {
        }
    }

    std::shared_ptr<const AaDiagnosticSnapshot> AaMailbox::PeekDiagnosticIdentityNode() const noexcept
    {
        return diagPublished_.load(std::memory_order_acquire);
    }

    AaDiagnosticSnapshot AaMailbox::MakeDiagnosticSnapshot() const noexcept
    {
        const std::shared_ptr<const AaDiagnosticSnapshot> node = PeekDiagnosticIdentityNode();
        AaDiagnosticSnapshot renderSide = node ? *node : AaDiagnosticSnapshot{};

        const AaRequest latest = ReadPublishedNow();
        renderSide.latestGeneration = latest.generation;
        renderSide.historyResetSerial = latest.historyResetSerial;
        renderSide.fsrRetrySerial = latest.fsrRetrySerial;
        renderSide.exposureRetrySerial = latest.exposureRetrySerial;
        renderSide.forceRecreateSerial = latest.forceRecreateSerial;
        return renderSide;
    }

    AaRequest AaMailbox::Applied() const noexcept
    {
        return applied_;
    }

    std::uint32_t AaMailbox::DamagedScope() const noexcept
    {
        return damagedScope_;
    }

    std::uint32_t AaMailbox::DlssTeardownSerial() const noexcept
    {
        return dlssTeardownSerial_;
    }

    AaRequest AaMailbox::PeekPublished() const noexcept
    {
        return ReadPublishedNow();
    }

    std::shared_ptr<const AaRequest> AaMailbox::PeekPublishedNode() const noexcept
    {
        return published_.load(std::memory_order_acquire);
    }

    bool AaMailbox::HasPublishedRequestNode() const noexcept
    {
        return published_.load(std::memory_order_acquire) != nullptr;
    }

    void AaMailbox::RetargetPendingTo(const AaRequest& newTarget) noexcept
    {
        const std::uint32_t newScope = ComputeScope(applied_, newTarget);

        const std::uint32_t carried = pending_.completedTeardownSteps & newScope;
        const std::uint32_t abandoned = pending_.completedTeardownSteps & ~newScope;
        damagedScope_ |= abandoned;

        pending_.target = newTarget;
        pending_.scope = newScope;
        pending_.completedTeardownSteps = carried;
        pending_.countdown = InitialDrainCountdown(newScope);
    }

    bool AaMailbox::FireNotificationAndCheckStale(bool wasActive, bool willBeActive,
        std::uint64_t targetSerial, std::uint64_t& consumedSerial, bool& edgeDelivered,
        const std::function<void()>& callback, PumpResult& result) noexcept
    {
        if (!willBeActive) {
            edgeDelivered = false;
        }

        const bool edgeFires = willBeActive && !wasActive && !edgeDelivered;
        const bool serialFires = targetSerial != consumedSerial;
        if (!edgeFires && !serialFires) {
            return false;
        }

        InvokeNoncriticalEffect(callback);

        consumedSerial = targetSerial;
        if (edgeFires) {
            edgeDelivered = true;
        }

        const AaRequest afterNotification = ReadPublishedNow();
        if (afterNotification.generation != pending_.target.generation) {
            RetargetPendingTo(afterNotification);
            result.outcome = AaPumpOutcome::kSuperseded;
            result.appliedGeneration = applied_.generation;
            result.affectsActiveEngine = AffectsActiveEngine(applied_, pending_.scope);
            PublishDiagnostic();
            return true;
        }
        return false;
    }

    PumpResult AaMailbox::Pump(std::uint64_t frameCounter) noexcept
    {
        PumpResult result{};

        if (hasConsumedFrame_ && frameCounter == lastConsumedFrame_) {
            result.outcome = (pending_.countdown == 0) ? AaPumpOutcome::kIdle : AaPumpOutcome::kDraining;
            result.appliedGeneration = applied_.generation;
            result.affectsActiveEngine = AffectsActiveEngine(applied_, pending_.scope);
            return result;
        }
        hasConsumedFrame_ = true;
        lastConsumedFrame_ = frameCounter;

        if (pending_.countdown == 0) {
            const AaRequest latest = ReadPublishedNow();
            if (latest.generation == applied_.generation) {
                result.outcome = AaPumpOutcome::kIdle;
                result.appliedGeneration = applied_.generation;
                result.affectsActiveEngine = false;
                PublishDiagnostic();
                return result;
            }

            pending_.target = latest;
            pending_.scope = ComputeScope(applied_, latest);
            pending_.completedTeardownSteps = kAaRecreateNone;
            pending_.countdown = InitialDrainCountdown(pending_.scope);
        }

        if (pending_.countdown > 1) {
            --pending_.countdown;
            result.outcome = AaPumpOutcome::kDraining;
            result.appliedGeneration = applied_.generation;
            result.affectsActiveEngine = AffectsActiveEngine(applied_, pending_.scope);
            PublishDiagnostic();
            return result;
        }

        const AaRequest recheck = ReadPublishedNow();
        if (recheck.generation != pending_.target.generation) {
            RetargetPendingTo(recheck);
            result.outcome = AaPumpOutcome::kSuperseded;
            result.appliedGeneration = applied_.generation;
            result.affectsActiveEngine = AffectsActiveEngine(applied_, pending_.scope);
            PublishDiagnostic();
            return result;
        }

        bool ok = true;
        if ((pending_.scope & kAaRecreateDlss) != 0U &&
            (pending_.completedTeardownSteps & kAaRecreateDlss) == 0U) {
            const AaTeardownResult stepResult = InvokeDlssTeardownStep(effects_.freeDlss);
            if (stepResult == AaTeardownResult::kFailed) {
                ok = false;
            } else {
                pending_.completedTeardownSteps |= kAaRecreateDlss;
                if (stepResult == AaTeardownResult::kDestroyed) {
                    ++dlssTeardownSerial_;
                }
            }

            const AaRequest afterDlss = ReadPublishedNow();
            if (afterDlss.generation != pending_.target.generation) {
                RetargetPendingTo(afterDlss);
                result.outcome = AaPumpOutcome::kSuperseded;
                result.appliedGeneration = applied_.generation;
                result.affectsActiveEngine = AffectsActiveEngine(applied_, pending_.scope);
                PublishDiagnostic();
                return result;
            }
        }
        if (ok && (pending_.scope & kAaRecreateFsr) != 0U &&
            (pending_.completedTeardownSteps & kAaRecreateFsr) == 0U) {
            const bool stepOk = InvokeDestructiveStep(effects_.destroyFsrContext);
            if (stepOk) {
                pending_.completedTeardownSteps |= kAaRecreateFsr;
            } else {
                ok = false;
            }

            const AaRequest afterFsr = ReadPublishedNow();
            if (afterFsr.generation != pending_.target.generation) {
                RetargetPendingTo(afterFsr);
                result.outcome = AaPumpOutcome::kSuperseded;
                result.appliedGeneration = applied_.generation;
                result.affectsActiveEngine = AffectsActiveEngine(applied_, pending_.scope);
                PublishDiagnostic();
                return result;
            }
        }

        if (!ok) {
            result.outcome = AaPumpOutcome::kCommitFailed;
            result.appliedGeneration = applied_.generation;
            result.affectsActiveEngine = AffectsActiveEngine(applied_, pending_.scope);
            PublishDiagnostic();
            return result;
        }

        const bool wasFsrActive = applied_.dlaaEnabled &&
            applied_.effectiveEngine == AaEffectiveEngine::kFsr;
        const bool targetIsFsr = pending_.target.effectiveEngine == AaEffectiveEngine::kFsr;
        const bool willBeFsrActive = pending_.target.dlaaEnabled &&
            targetIsFsr;
        if (FireNotificationAndCheckStale(wasFsrActive, willBeFsrActive,
                targetIsFsr ? pending_.target.fsrRetrySerial : pending_.consumedFsrRetrySerial,
                pending_.consumedFsrRetrySerial,
                pending_.fsrEdgeDelivered, effects_.rearmFsrLatch, result)) {
            return result;
        }

        const bool wasExposureActive =
            applied_.dlaaEnabled && applied_.autoExposure &&
            IsDlssFamily(applied_.effectiveEngine);
        const bool targetIsDlss = IsDlssFamily(pending_.target.effectiveEngine);
        const bool willBeExposureActive =
            pending_.target.dlaaEnabled && pending_.target.autoExposure &&
            targetIsDlss;
        if (FireNotificationAndCheckStale(wasExposureActive, willBeExposureActive,
                targetIsDlss ? pending_.target.exposureRetrySerial : pending_.consumedExposureRetrySerial,
                pending_.consumedExposureRetrySerial,
                pending_.exposureEdgeDelivered, effects_.rearmExposure, result)) {
            return result;
        }

        const bool wasHistoryActive = applied_.dlaaEnabled && IsLiveEngine(applied_.effectiveEngine);
        const bool willBeHistoryActive = pending_.target.dlaaEnabled &&
            IsLiveEngine(pending_.target.effectiveEngine);
        const bool targetHasEngine = targetIsDlss || targetIsFsr;
        if (FireNotificationAndCheckStale(wasHistoryActive, willBeHistoryActive,
                targetHasEngine ? pending_.target.historyResetSerial : pending_.consumedHistoryResetSerial,
                pending_.consumedHistoryResetSerial,
                pending_.historyEdgeDelivered, effects_.requestHistoryReset, result)) {
            return result;
        }

        const AaRequest preCommitRecheck = ReadPublishedNow();
        if (preCommitRecheck.generation != pending_.target.generation) {
            RetargetPendingTo(preCommitRecheck);
            result.outcome = AaPumpOutcome::kSuperseded;
            result.appliedGeneration = applied_.generation;
            result.affectsActiveEngine = AffectsActiveEngine(applied_, pending_.scope);
            PublishDiagnostic();
            return result;
        }

        const AaRequest committed = pending_.target;
        const std::uint32_t committedScope = pending_.scope;
        applied_ = committed;

        damagedScope_ &= ~pending_.completedTeardownSteps;

        {
            AaCommitInfo info{};
            info.generation = committed.generation;
            info.requestedEngine = committed.requestedEngine;
            info.effectiveEngine = committed.effectiveEngine;
            info.dlaaEnabled = committed.dlaaEnabled;
            info.preset = committed.preset;
            info.autoExposure = committed.autoExposure;
            info.resolutionScale = committed.resolutionScale;
            info.qualityMode = committed.qualityMode;
            info.scope = committedScope;
            InvokeLogCommit(effects_.logCommit, info);
        }

        const std::uint64_t consumedHistoryResetSerial = pending_.consumedHistoryResetSerial;
        const std::uint64_t consumedFsrRetrySerial = pending_.consumedFsrRetrySerial;
        const std::uint64_t consumedExposureRetrySerial = pending_.consumedExposureRetrySerial;
        pending_ = AaPendingDrain{};
        pending_.target = applied_;
        pending_.consumedHistoryResetSerial = consumedHistoryResetSerial;
        pending_.consumedFsrRetrySerial = consumedFsrRetrySerial;
        pending_.consumedExposureRetrySerial = consumedExposureRetrySerial;

        result.outcome = AaPumpOutcome::kCommitted;
        result.appliedGeneration = applied_.generation;
        result.affectsActiveEngine = false;
        PublishDiagnostic();
        return result;
    }
}
