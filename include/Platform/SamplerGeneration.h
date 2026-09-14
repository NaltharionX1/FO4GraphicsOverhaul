#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <thread>

namespace Platform
{
    inline constexpr std::size_t kFo4SamplerCount = 320;
    inline constexpr std::size_t kSamplerDescriptorBytes = 52;
    inline constexpr std::size_t kMaxSamplerScopeDepth = 8;

    using SamplerHandle = std::uintptr_t;

    struct SamplerDescriptor
    {
        std::array<std::uint8_t, kSamplerDescriptorBytes> bytes{};

        [[nodiscard]] friend bool operator==(const SamplerDescriptor&, const SamplerDescriptor&) = default;
    };

    struct SamplerDeviceKey
    {
        SamplerHandle identity{ 0 };
        std::uint64_t epoch{ 0 };

        [[nodiscard]] friend bool operator==(const SamplerDeviceKey&, const SamplerDeviceKey&) = default;
    };

    enum class AppliedSamplerEngine : std::uint8_t
    {
        kDlss,
        kFsr,
    };

    struct AppliedSamplerIntent
    {
        std::uint64_t appliedCommit{ 0 };
        AppliedSamplerEngine engine{ AppliedSamplerEngine::kDlss };
        bool active{ false };
        float bias{ 0.0F };
    };

    struct SamplerTableCallbacks
    {
        std::function<SamplerHandle(std::size_t index)> load;
        std::function<void(std::size_t index, SamplerHandle handle)> store;
        const std::atomic<std::uint64_t>* mutationSerial{ nullptr };
    };

    struct SamplerBackendCallbacks
    {
        std::function<void(SamplerHandle handle)> retain;
        std::function<void(SamplerHandle handle)> release;
        std::function<bool(SamplerHandle handle, SamplerDescriptor& output)> describe;

        std::function<bool(const SamplerDescriptor& source, float bias, SamplerDescriptor& output)>
            makeBiasedDescriptor;

        std::function<bool(SamplerHandle device,
                           const SamplerDescriptor& descriptor,
                           SamplerHandle& output)>
            create;
    };

    class SamplerGenerationController
    {
    private:
        struct GenerationKey;
        struct Generation;
        struct TicketLifetime;
        struct OwnerOperationGuard;
        struct ScratchStorage;

    public:
        class Ticket
        {
        public:
            Ticket() = default;

            [[nodiscard]] bool Usable() const noexcept;
            [[nodiscard]] std::uint64_t PublicationId() const noexcept;

        private:
            friend class SamplerGenerationController;
            friend class Scope;

            Ticket(std::weak_ptr<const TicketLifetime> lifetime,
                   std::uint64_t publicationId,
                   std::uint64_t refreshSerial) noexcept;

            std::weak_ptr<const TicketLifetime> lifetime_;
            std::uint64_t publicationId_{ 0 };
            std::uint64_t refreshSerial_{ 0 };
        };

        class Scope
        {
        public:
            Scope(SamplerGenerationController& owner,
                  const Ticket& ticket,
                  const SamplerDeviceKey& device,
                  const AppliedSamplerIntent& intent) noexcept;
            ~Scope() noexcept;

            Scope(const Scope&) = delete;
            Scope& operator=(const Scope&) = delete;
            Scope(Scope&&) = delete;
            Scope& operator=(Scope&&) = delete;

            [[nodiscard]] bool Active() const noexcept;

        private:
            SamplerGenerationController* owner_{ nullptr };
            std::uint64_t token_{ 0 };
        };

        SamplerGenerationController(SamplerTableCallbacks table, SamplerBackendCallbacks backend);
        ~SamplerGenerationController() noexcept;

        SamplerGenerationController(const SamplerGenerationController&) = delete;
        SamplerGenerationController& operator=(const SamplerGenerationController&) = delete;
        SamplerGenerationController(SamplerGenerationController&&) = delete;
        SamplerGenerationController& operator=(SamplerGenerationController&&) = delete;

        [[nodiscard]] Ticket Refresh(const SamplerDeviceKey& device,
                                     const AppliedSamplerIntent& intent) noexcept;
        [[nodiscard]] bool Clear() noexcept;

        [[nodiscard]] std::uint64_t PublishedId() const noexcept;
        [[nodiscard]] std::size_t Depth() const noexcept;

    private:
        [[nodiscard]] bool IsOwnerThread() const noexcept;
        [[nodiscard]] static bool IsValidIntent(const SamplerDeviceKey& device,
                                                const AppliedSamplerIntent& intent) noexcept;
        [[nodiscard]] static bool IntentsEqual(const AppliedSamplerIntent& left,
                                               const AppliedSamplerIntent& right) noexcept;
        [[nodiscard]] bool ScopeStackMatches(
            std::size_t expectedDepth,
            const std::shared_ptr<const Generation>& expectedPinned,
            std::uint64_t expectedTopToken) const noexcept;
        [[nodiscard]] bool CaptureTableMutationSerial(std::uint64_t& output) const noexcept;
        [[nodiscard]] bool LiveTableMatches(
            const std::array<SamplerHandle, kFo4SamplerCount>& expected) const noexcept;
        [[nodiscard]] bool LiveTableMatchesOutputs(const Generation& generation) const noexcept;
        [[nodiscard]] bool CaptureKeyFromHandles(const SamplerDeviceKey& device,
                                                 float bias,
                                                 const std::array<SamplerHandle, kFo4SamplerCount>& handles,
                                                 GenerationKey& output) const noexcept;
        [[nodiscard]] bool CaptureCurrentSourceKey(const SamplerDeviceKey& device,
                                                   float bias,
                                                   GenerationKey& output,
                                                   const std::array<SamplerHandle, kFo4SamplerCount>*
                                                       protectedHandles = nullptr) const noexcept;
        [[nodiscard]] GenerationKey* ScratchKeyForCurrentOperation() noexcept;
        [[nodiscard]] std::uint64_t BeginScope(const Ticket& ticket,
                                               const SamplerDeviceKey& device,
                                               const AppliedSamplerIntent& intent) noexcept;
        void EndScope(std::uint64_t token) noexcept;
        [[nodiscard]] std::uint64_t AllocateScopeToken(std::size_t slot) noexcept;
        [[nodiscard]] static std::size_t ScopeTokenSlot(std::uint64_t token) noexcept;
        void MarkScopeAbandoned(std::uint64_t token) noexcept;
        [[nodiscard]] bool RecoverAbandonedScopes() noexcept;
        [[nodiscard]] bool RestoreOuterAndRelease() noexcept;
        void ReleaseOuterSnapshot() noexcept;

        SamplerTableCallbacks table_;
        SamplerBackendCallbacks backend_;
        const std::thread::id ownerThread_;
        std::atomic<std::shared_ptr<const Generation>> published_{};
        std::shared_ptr<const TicketLifetime> ticketLifetime_;
        std::unique_ptr<ScratchStorage> scratchStorage_;

        std::uint64_t nextPublicationId_{ 1 };
        std::uint64_t refreshSerial_{ 0 };
        std::size_t callbackOperationDepth_{ 0 };
        bool tableTransitionInProgress_{ false };
        bool desiredValid_{ false };
        SamplerDeviceKey desiredDevice_{};
        AppliedSamplerIntent desiredIntent_{};

        std::size_t depth_{ 0 };
        std::uint64_t nextScopeToken_{ 1 };
        std::array<std::uint64_t, kMaxSamplerScopeDepth> scopeTokens_{};
        std::array<std::atomic<std::uint64_t>, kMaxSamplerScopeDepth> abandonedScopeTokens_{};
        std::shared_ptr<const Generation> pinnedGeneration_;
        std::array<SamplerHandle, kFo4SamplerCount> outerSnapshot_{};
        std::array<bool, kFo4SamplerCount> outerSnapshotOwned_{};
    };
}
