#pragma once

#include "Platform/NeuralContract.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

namespace Platform
{
    inline constexpr std::uint32_t kNeuralScopeNone = 0U;
    inline constexpr std::uint32_t kNeuralScopeLive = 1U << 0;
    inline constexpr std::uint32_t kNeuralScopeRecreate = 1U << 1;
    inline constexpr std::uint32_t kNeuralScopeArm = 1U << 2;
    inline constexpr std::uint32_t kNeuralScopeDisarm = 1U << 3;
    inline constexpr std::uint32_t kNeuralScopeRetry = 1U << 4;
    inline constexpr std::uint32_t kNeuralScopeTeardown = kNeuralScopeRecreate | kNeuralScopeDisarm | kNeuralScopeRetry;

    struct NeuralRequest
    {
        std::uint64_t generation{ 0 };
        Neural::CascadeSettings settings{};
        std::uint64_t retrySerial{ 0 };
        std::uint64_t forceRecreateSerial{ 0 };
    };

    enum class NeuralPumpOutcome : std::uint8_t
    {
        kIdle,
        kCommitted,
        kCommitFailed,
        kSuperseded,
    };

    struct NeuralPumpResult
    {
        NeuralPumpOutcome outcome{ NeuralPumpOutcome::kIdle };
        std::uint64_t appliedGeneration{ 0 };
        std::uint32_t scope{ kNeuralScopeNone };
    };

    enum class NeuralTeardownResult : std::uint8_t
    {
        kFailed,
        kAlreadyAbsent,
        kDestroyed,
    };

    struct NeuralCommitInfo
    {
        std::uint64_t generation{ 0 };
        std::uint32_t scope{ kNeuralScopeNone };
        Neural::CascadeSettings settings{};
        std::uint32_t historyResetMask{ 0 };
        std::uint64_t retrySerial{ 0 };
        std::uint64_t forceRecreateSerial{ 0 };
    };

    struct NeuralEffects
    {
        std::function<NeuralTeardownResult(std::uint32_t)> destroyFeature;
        std::function<void()> rearmLatch;
        std::function<void(const NeuralCommitInfo&)> logCommit;
    };

    struct NeuralDiagnosticSnapshot
    {
        std::uint64_t latestGeneration{ 0 };
        std::uint64_t appliedGeneration{ 0 };
        Neural::CascadeSettings requested{};
        Neural::CascadeSettings applied{};
        std::uint64_t retrySerial{ 0 };
        std::uint64_t forceRecreateSerial{ 0 };
        std::uint32_t pendingScope{ kNeuralScopeNone };
        std::uint32_t completedTeardownSteps{ kNeuralScopeNone };
        std::uint64_t teardownSerial{ 0 };
        bool pending{ false };
        bool damaged{ false };
        std::uint32_t damagedStages{ 0 };
    };

    class NeuralMailbox
    {
    public:
        explicit NeuralMailbox(const NeuralRequest& a_initial = NeuralRequest{});
        NeuralMailbox(const NeuralMailbox&) = delete;
        NeuralMailbox& operator=(const NeuralMailbox&) = delete;

        void SetEffects(NeuralEffects a_effects) noexcept;

        template <class Fn>
        std::uint64_t Publish(Fn&& a_builder) noexcept
        {
            std::shared_ptr<const NeuralRequest> current = published_.load(std::memory_order_acquire);
            for (;;) {
                NeuralRequest next = *current;
                try {
                    a_builder(next);
                } catch (...) {
                    return current->generation;
                }
                next.generation = current->generation;
                Normalize(next);
                if (RequestsEqual(next, *current)) {
                    return current->generation;
                }
                next.generation = current->generation + 1;
                std::shared_ptr<const NeuralRequest> desired;
                try {
                    desired = std::make_shared<const NeuralRequest>(next);
                } catch (...) {
                    return current->generation;
                }
                if (published_.compare_exchange_weak(current, desired, std::memory_order_acq_rel,
                        std::memory_order_acquire)) {
                    return next.generation;
                }
            }
        }

        [[nodiscard]] NeuralPumpResult Pump(std::uint64_t a_frameCounter) noexcept;

        [[nodiscard]] NeuralRequest Applied() const noexcept;
        [[nodiscard]] NeuralRequest PeekPublished() const noexcept;
        [[nodiscard]] NeuralDiagnosticSnapshot MakeDiagnosticSnapshot() const noexcept;
        [[nodiscard]] std::uint64_t TeardownSerial() const noexcept;
        [[nodiscard]] bool Damaged() const noexcept;

        [[nodiscard]] static std::uint32_t ComputeScope(
            const NeuralRequest& a_applied, const NeuralRequest& a_next) noexcept;
        static void ScopeName(std::uint32_t a_scope, char* a_out, std::size_t a_size) noexcept;
        static void Normalize(NeuralRequest& a_request) noexcept;
        [[nodiscard]] static bool RequestsEqual(const NeuralRequest& a_lhs, const NeuralRequest& a_rhs) noexcept;

    private:
        [[nodiscard]] NeuralTeardownResult InvokeDestroy(std::uint32_t a_stageMask) noexcept;
        void InvokeRearm() noexcept;
        void InvokeLogCommit(const NeuralCommitInfo& a_info) noexcept;
        [[nodiscard]] bool SupersededBy(const std::shared_ptr<const NeuralRequest>& a_target) noexcept;

        std::atomic<std::shared_ptr<const NeuralRequest>> published_;
        std::atomic<std::shared_ptr<const NeuralRequest>> applied_;
        NeuralEffects effects_{};

        bool pumpedOnce_{ false };
        std::uint64_t lastPumpFrame_{ 0 };
        std::uint64_t consumedRetrySerial_{ 0 };
        std::atomic<std::uint32_t> completedTeardownSteps_{ kNeuralScopeNone };
        std::atomic<std::uint32_t> pendingScope_{ kNeuralScopeNone };
        std::atomic<std::uint64_t> teardownSerial_{ 0 };
        std::atomic<std::uint32_t> damagedStages_{ 0 };
    };
}
