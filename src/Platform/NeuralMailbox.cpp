#include "Platform/NeuralMailbox.h"

#include <cstdio>
#include <cstring>

namespace Platform
{
    namespace
    {
        [[nodiscard]] Neural::Settings Clamp(const Neural::Settings& a_in) noexcept
        {
            Neural::Settings out = a_in;
            const Neural::Settings defaults{};
            out.style = Neural::ClampStyle(a_in.style);
            out.preset = Neural::ClampPreset(a_in.preset);
            out.intensity = Neural::ClampStrength(a_in.intensity, defaults.intensity);
            out.localTone = Neural::ClampStrength(a_in.localTone, defaults.localTone);
            out.localStructure = Neural::ClampStrength(a_in.localStructure, defaults.localStructure);
            out.skinStructure = Neural::ClampStrength(a_in.skinStructure, defaults.skinStructure);
            return out;
        }

        [[nodiscard]] std::uint32_t DestroyMask(const NeuralRequest& a_before, const NeuralRequest& a_after) noexcept
        {
            if (a_after.retrySerial != a_before.retrySerial ||
                (a_before.settings.enabled && !a_after.settings.enabled) ||
                (a_after.settings.enabled && a_after.forceRecreateSerial != a_before.forceRecreateSerial)) {
                return Neural::kAllPasses;
            }
            std::uint32_t mask = Neural::RecreateMask(a_before.settings, a_after.settings);
            const std::uint32_t active = Neural::ActiveMask(a_after.settings);
            for (std::uint32_t i = 0; i < Neural::kMaxPasses; ++i) {
                if ((active & (1U << i)) != 0U &&
                    Neural::RequiresRecreate(a_before.settings.passes[i], a_after.settings.passes[i])) {
                    mask |= 1U << i;
                }
            }
            return mask;
        }
    }

    NeuralMailbox::NeuralMailbox(const NeuralRequest& a_initial)
    {
        NeuralRequest initial = a_initial;
        Normalize(initial);
        initial.generation = 0;
        auto node = std::make_shared<const NeuralRequest>(initial);
        published_.store(node, std::memory_order_release);
        applied_.store(node, std::memory_order_release);
    }

    void NeuralMailbox::SetEffects(NeuralEffects a_effects) noexcept
    {
        effects_ = std::move(a_effects);
    }

    void NeuralMailbox::Normalize(NeuralRequest& a_request) noexcept
    {
        a_request.settings.passCount = Neural::ClampPassCount(a_request.settings.passCount);
        a_request.settings.modelPercent = Neural::EffectiveModelPercent(a_request.settings);
        for (auto& stage : a_request.settings.passes) {
            stage = Clamp(stage);
        }
    }

    bool NeuralMailbox::RequestsEqual(const NeuralRequest& a_lhs, const NeuralRequest& a_rhs) noexcept
    {
        return a_lhs.settings == a_rhs.settings && a_lhs.retrySerial == a_rhs.retrySerial &&
               a_lhs.forceRecreateSerial == a_rhs.forceRecreateSerial;
    }

    std::uint32_t NeuralMailbox::ComputeScope(const NeuralRequest& a_applied, const NeuralRequest& a_next) noexcept
    {
        std::uint32_t scope = kNeuralScopeNone;
        const Neural::CascadeSettings& before = a_applied.settings;
        const Neural::CascadeSettings& after = a_next.settings;
        if (a_next.retrySerial != a_applied.retrySerial) {
            scope |= kNeuralScopeRetry;
        }
        if (!before.enabled && after.enabled) {
            scope |= kNeuralScopeArm;
        } else if (before.enabled && !after.enabled) {
            scope |= kNeuralScopeDisarm;
        }
        if (after.enabled) {
            bool creationKeyChanged = false;
            for (std::uint32_t i = 0; i < Neural::ClampPassCount(after.passCount); ++i) {
                creationKeyChanged = creationKeyChanged ||
                    Neural::RequiresRecreate(before.passes[i], after.passes[i]);
            }
            if (creationKeyChanged || before.passCount != after.passCount ||
                before.modelPercent != after.modelPercent ||
                a_next.forceRecreateSerial != a_applied.forceRecreateSerial) {
                scope |= kNeuralScopeRecreate;
            }
            Neural::CascadeSettings liveBefore = before;
            Neural::CascadeSettings liveAfter = after;
            liveBefore.enabled = liveAfter.enabled = true;
            liveBefore.passCount = liveAfter.passCount;
            liveBefore.modelPercent = liveAfter.modelPercent;
            liveBefore.modelBeyondPlay = liveAfter.modelBeyondPlay;
            for (std::uint32_t i = 0; i < Neural::kMaxPasses; ++i) {
                if (i < after.passCount) {
                    liveBefore.passes[i].preset = liveAfter.passes[i].preset;
                } else {
                    liveBefore.passes[i] = liveAfter.passes[i];
                }
            }
            if (!(liveBefore == liveAfter)) {
                scope |= kNeuralScopeLive;
            }
        }
        return scope;
    }

    void NeuralMailbox::ScopeName(std::uint32_t a_scope, char* a_out, std::size_t a_size) noexcept
    {
        if (a_out == nullptr || a_size == 0) {
            return;
        }
        a_out[0] = '\0';
        if (a_scope == kNeuralScopeNone) {
            std::snprintf(a_out, a_size, "%s", "none");
            return;
        }
        struct Part
        {
            std::uint32_t bit;
            const char* name;
        };
        static constexpr Part kParts[]{ { kNeuralScopeArm, "arm" }, { kNeuralScopeDisarm, "disarm" },
            { kNeuralScopeRetry, "retry" }, { kNeuralScopeRecreate, "recreate" }, { kNeuralScopeLive, "live" } };
        std::size_t used = 0;
        for (const Part& part : kParts) {
            if ((a_scope & part.bit) == 0U) {
                continue;
            }
            const int n = std::snprintf(a_out + used, a_size - used, "%s%s", used == 0 ? "" : "+", part.name);
            if (n < 0) {
                return;
            }
            used += static_cast<std::size_t>(n);
            if (used >= a_size - 1) {
                return;
            }
        }
    }

    NeuralTeardownResult NeuralMailbox::InvokeDestroy(std::uint32_t a_stageMask) noexcept
    {
        if (!effects_.destroyFeature) {
            return NeuralTeardownResult::kAlreadyAbsent;
        }
        try {
            return effects_.destroyFeature(a_stageMask);
        } catch (...) {
            return NeuralTeardownResult::kFailed;
        }
    }

    void NeuralMailbox::InvokeRearm() noexcept
    {
        if (!effects_.rearmLatch) {
            return;
        }
        try {
            effects_.rearmLatch();
        } catch (...) {
        }
    }

    void NeuralMailbox::InvokeLogCommit(const NeuralCommitInfo& a_info) noexcept
    {
        if (!effects_.logCommit) {
            return;
        }
        try {
            effects_.logCommit(a_info);
        } catch (...) {
        }
    }

    bool NeuralMailbox::SupersededBy(const std::shared_ptr<const NeuralRequest>& a_target) noexcept
    {
        const std::shared_ptr<const NeuralRequest> latest = published_.load(std::memory_order_acquire);
        if (latest->generation == a_target->generation) {
            return false;
        }
        return true;
    }

    NeuralPumpResult NeuralMailbox::Pump(std::uint64_t a_frameCounter) noexcept
    {
        NeuralPumpResult result{};
        const std::shared_ptr<const NeuralRequest> appliedNode = applied_.load(std::memory_order_acquire);
        result.appliedGeneration = appliedNode->generation;
        result.scope = pendingScope_.load(std::memory_order_relaxed);
        if (pumpedOnce_ && a_frameCounter == lastPumpFrame_) {
            return result;
        }
        pumpedOnce_ = true;
        lastPumpFrame_ = a_frameCounter;

        const std::shared_ptr<const NeuralRequest> target = published_.load(std::memory_order_acquire);
        if (target->generation == appliedNode->generation) {
            pendingScope_.store(kNeuralScopeNone, std::memory_order_relaxed);
            result.scope = kNeuralScopeNone;
            return result;
        }

        std::uint32_t scope = ComputeScope(*appliedNode, *target);
        if (damagedStages_.load(std::memory_order_relaxed) != 0U && target->settings.enabled) {
            scope |= kNeuralScopeRecreate;
        }
        pendingScope_.store(scope, std::memory_order_relaxed);
        result.scope = scope;

        const std::uint32_t needed = DestroyMask(*appliedNode, *target) &
            ~completedTeardownSteps_.load(std::memory_order_relaxed);
        bool stepFailed = false;
        if (needed != 0U) {
            const NeuralTeardownResult teardown = InvokeDestroy(needed);
            if (teardown == NeuralTeardownResult::kFailed) {
                stepFailed = true;
            } else {
                if (teardown == NeuralTeardownResult::kDestroyed) {
                    teardownSerial_.fetch_add(1, std::memory_order_relaxed);
                    damagedStages_.fetch_or(needed, std::memory_order_relaxed);
                }
                completedTeardownSteps_.fetch_or(needed, std::memory_order_relaxed);
            }
        }
        if (SupersededBy(target)) {
            result.outcome = NeuralPumpOutcome::kSuperseded;
            return result;
        }
        if (stepFailed) {
            result.outcome = NeuralPumpOutcome::kCommitFailed;
            return result;
        }

        if ((scope & kNeuralScopeRetry) != 0U && target->retrySerial != consumedRetrySerial_) {
            consumedRetrySerial_ = target->retrySerial;
            InvokeRearm();
            if (SupersededBy(target)) {
                result.outcome = NeuralPumpOutcome::kSuperseded;
                return result;
            }
        }
        if (SupersededBy(target)) {
            result.outcome = NeuralPumpOutcome::kSuperseded;
            return result;
        }

        std::uint32_t historyMask = Neural::HistoryResetMask(appliedNode->settings, target->settings);
        const std::uint32_t damage = damagedStages_.load(std::memory_order_relaxed);
        for (std::uint32_t i = 0; i < Neural::kMaxPasses; ++i) {
            if ((damage & (1U << i)) != 0U) {
                historyMask |= Neural::ActiveMask(target->settings) & ~((1U << i) - 1U);
            }
        }
        if ((scope & kNeuralScopeRetry) != 0U || target->forceRecreateSerial != appliedNode->forceRecreateSerial) {
            historyMask |= Neural::ActiveMask(target->settings);
        }
        applied_.store(target, std::memory_order_release);
        completedTeardownSteps_.store(kNeuralScopeNone, std::memory_order_relaxed);
        pendingScope_.store(kNeuralScopeNone, std::memory_order_relaxed);
        damagedStages_.store(0, std::memory_order_relaxed);

        NeuralCommitInfo info{};
        info.generation = target->generation;
        info.scope = scope;
        info.settings = target->settings;
        info.historyResetMask = historyMask;
        info.retrySerial = target->retrySerial;
        info.forceRecreateSerial = target->forceRecreateSerial;
        InvokeLogCommit(info);

        result.outcome = NeuralPumpOutcome::kCommitted;
        result.appliedGeneration = target->generation;
        return result;
    }

    NeuralRequest NeuralMailbox::Applied() const noexcept
    {
        return *applied_.load(std::memory_order_acquire);
    }

    NeuralRequest NeuralMailbox::PeekPublished() const noexcept
    {
        return *published_.load(std::memory_order_acquire);
    }

    NeuralDiagnosticSnapshot NeuralMailbox::MakeDiagnosticSnapshot() const noexcept
    {
        NeuralDiagnosticSnapshot snapshot{};
        std::shared_ptr<const NeuralRequest> published;
        std::shared_ptr<const NeuralRequest> applied;
        for (int attempt = 0; attempt < 8; ++attempt) {
            published = published_.load(std::memory_order_acquire);
            applied = applied_.load(std::memory_order_acquire);
            const std::shared_ptr<const NeuralRequest> again = published_.load(std::memory_order_acquire);
            if (again->generation == published->generation) {
                break;
            }
        }
        snapshot.latestGeneration = published->generation;
        snapshot.appliedGeneration = applied->generation;
        snapshot.requested = published->settings;
        snapshot.applied = applied->settings;
        snapshot.retrySerial = published->retrySerial;
        snapshot.forceRecreateSerial = published->forceRecreateSerial;
        snapshot.pending = published->generation != applied->generation;
        snapshot.damagedStages = damagedStages_.load(std::memory_order_relaxed);
        snapshot.damaged = snapshot.damagedStages != 0U;
        snapshot.pendingScope = snapshot.pending ? ComputeScope(*applied, *published) : kNeuralScopeNone;
        if (snapshot.pending && snapshot.damaged && published->settings.enabled) {
            snapshot.pendingScope |= kNeuralScopeRecreate;
        }
        snapshot.completedTeardownSteps = completedTeardownSteps_.load(std::memory_order_relaxed);
        snapshot.teardownSerial = teardownSerial_.load(std::memory_order_relaxed);
        return snapshot;
    }

    std::uint64_t NeuralMailbox::TeardownSerial() const noexcept
    {
        return teardownSerial_.load(std::memory_order_relaxed);
    }

    bool NeuralMailbox::Damaged() const noexcept
    {
        return damagedStages_.load(std::memory_order_relaxed) != 0U;
    }
}
