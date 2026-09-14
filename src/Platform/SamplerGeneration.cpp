#include "Platform/SamplerGeneration.h"

#include <bit>
#include <cmath>
#include <exception>
#include <utility>
#include <vector>

namespace Platform
{
    namespace
    {
        class ProvisionalSourceOwnership
        {
        public:
            explicit ProvisionalSourceOwnership(
                const std::function<void(SamplerHandle)>* release) noexcept :
                release_(release)
            {}

            ~ProvisionalSourceOwnership() noexcept
            {
                if (release_ == nullptr || !*release_) {
                    return;
                }
                for (std::size_t index = 0; index < kFo4SamplerCount; ++index) {
                    if (!owned_[index] || handles_[index] == 0) {
                        continue;
                    }
                    try {
                        (*release_)(handles_[index]);
                    } catch (...) {
                    }
                }
            }

            ProvisionalSourceOwnership(const ProvisionalSourceOwnership&) = delete;
            ProvisionalSourceOwnership& operator=(const ProvisionalSourceOwnership&) = delete;

            void Record(std::size_t index, SamplerHandle handle) noexcept
            {
                handles_[index] = handle;
                owned_[index] = true;
            }

            [[nodiscard]] bool Owns(std::size_t index) const noexcept
            {
                return owned_[index];
            }

            void Relinquish(std::size_t index) noexcept
            {
                owned_[index] = false;
                handles_[index] = 0;
            }

        private:
            const std::function<void(SamplerHandle)>* release_{ nullptr };
            std::array<SamplerHandle, kFo4SamplerCount> handles_{};
            std::array<bool, kFo4SamplerCount> owned_{};
        };
    }

    struct SamplerGenerationController::TicketLifetime
    {};

    struct SamplerGenerationController::GenerationKey
    {
        struct EntryKey
        {
            SamplerHandle source{ 0 };
            SamplerDescriptor descriptor{};

            [[nodiscard]] friend bool operator==(const EntryKey&, const EntryKey&) = default;
        };

        SamplerDeviceKey device{};
        std::uint32_t biasBits{ 0 };
        std::array<EntryKey, kFo4SamplerCount> entries{};

        [[nodiscard]] friend bool operator==(const GenerationKey&, const GenerationKey&) = default;
    };

    struct SamplerGenerationController::Generation
    {
        ~Generation() noexcept
        {
            if (!release) {
                return;
            }
            for (std::size_t index = 0; index < kFo4SamplerCount; ++index) {
                if (cloneOwned[index] && outputs[index] != 0) {
                    try {
                        release(outputs[index]);
                    } catch (...) {
                    }
                }
            }
            for (std::size_t index = 0; index < kFo4SamplerCount; ++index) {
                if (sourceOwned[index] && sources[index] != 0) {
                    try {
                        release(sources[index]);
                    } catch (...) {
                    }
                }
            }
        }

        GenerationKey key{};
        std::array<SamplerHandle, kFo4SamplerCount> sources{};
        std::array<SamplerHandle, kFo4SamplerCount> outputs{};
        std::array<bool, kFo4SamplerCount> sourceOwned{};
        std::array<bool, kFo4SamplerCount> cloneOwned{};
        std::function<void(SamplerHandle)> release;
        std::uint64_t publicationId{ 0 };
    };

    struct SamplerGenerationController::ScratchStorage
    {
        ScratchStorage()
        {
            keys.emplace_back(std::make_unique<GenerationKey>());
        }

        [[nodiscard]] GenerationKey* KeyAt(std::size_t index) noexcept
        {
            try {
                while (keys.size() <= index) {
                    keys.emplace_back(std::make_unique<GenerationKey>());
                }
            } catch (...) {
                return nullptr;
            }
            return keys[index].get();
        }

        std::vector<std::unique_ptr<GenerationKey>> keys;
    };

    struct SamplerGenerationController::OwnerOperationGuard
    {
        explicit OwnerOperationGuard(SamplerGenerationController& owner) noexcept : owner_(owner)
        {
            ++owner_.callbackOperationDepth_;
        }

        ~OwnerOperationGuard() noexcept
        {
            --owner_.callbackOperationDepth_;
            if (owner_.callbackOperationDepth_ == 0) {
                (void)owner_.RecoverAbandonedScopes();
            }
        }

        OwnerOperationGuard(const OwnerOperationGuard&) = delete;
        OwnerOperationGuard& operator=(const OwnerOperationGuard&) = delete;

    private:
        SamplerGenerationController& owner_;
    };

    SamplerGenerationController::Ticket::Ticket(std::weak_ptr<const TicketLifetime> lifetime,
                                                std::uint64_t publicationId,
                                                std::uint64_t refreshSerial) noexcept :
        lifetime_(std::move(lifetime)), publicationId_(publicationId), refreshSerial_(refreshSerial)
    {}

    bool SamplerGenerationController::Ticket::Usable() const noexcept
    {
        return publicationId_ != 0 && refreshSerial_ != 0 && !lifetime_.expired();
    }

    std::uint64_t SamplerGenerationController::Ticket::PublicationId() const noexcept
    {
        return Usable() ? publicationId_ : 0;
    }

    SamplerGenerationController::Scope::Scope(SamplerGenerationController& owner,
                                              const Ticket& ticket,
                                              const SamplerDeviceKey& device,
                                              const AppliedSamplerIntent& intent) noexcept :
        owner_(&owner), token_(owner.BeginScope(ticket, device, intent))
    {
        if (token_ == 0) {
            owner_ = nullptr;
        }
    }

    SamplerGenerationController::Scope::~Scope() noexcept
    {
        if (owner_ != nullptr && token_ != 0) {
            owner_->EndScope(token_);
        }
    }

    bool SamplerGenerationController::Scope::Active() const noexcept
    {
        return owner_ != nullptr && token_ != 0;
    }

    SamplerGenerationController::SamplerGenerationController(SamplerTableCallbacks table,
                                                             SamplerBackendCallbacks backend) :
        table_(std::move(table)), backend_(std::move(backend)), ownerThread_(std::this_thread::get_id()),
        ticketLifetime_(std::make_shared<TicketLifetime>()),
        scratchStorage_(std::make_unique<ScratchStorage>())
    {}

    SamplerGenerationController::~SamplerGenerationController() noexcept
    {
        if (!IsOwnerThread()) {
            std::terminate();
        }
        if (depth_ != 0) {
            if (!RecoverAbandonedScopes() || depth_ != 0) {
                std::terminate();
            }
        }
        ticketLifetime_.reset();
        tableTransitionInProgress_ = true;
        published_.store({}, std::memory_order_release);
    }

    bool SamplerGenerationController::IsOwnerThread() const noexcept
    {
        return std::this_thread::get_id() == ownerThread_;
    }

    bool SamplerGenerationController::IsValidIntent(const SamplerDeviceKey& device,
                                                    const AppliedSamplerIntent& intent) noexcept
    {
        const auto engine = static_cast<std::uint8_t>(intent.engine);
        return device.identity != 0 && intent.active && std::isfinite(intent.bias) &&
               intent.bias >= -3.0F && intent.bias < 0.0F &&
               engine <= static_cast<std::uint8_t>(AppliedSamplerEngine::kFsr);
    }

    bool SamplerGenerationController::IntentsEqual(const AppliedSamplerIntent& left,
                                                   const AppliedSamplerIntent& right) noexcept
    {
        return left.appliedCommit == right.appliedCommit && left.engine == right.engine &&
               left.active == right.active &&
               std::bit_cast<std::uint32_t>(left.bias) == std::bit_cast<std::uint32_t>(right.bias);
    }

    bool SamplerGenerationController::ScopeStackMatches(
        std::size_t expectedDepth,
        const std::shared_ptr<const Generation>& expectedPinned,
        std::uint64_t expectedTopToken) const noexcept
    {
        if (depth_ != expectedDepth || pinnedGeneration_ != expectedPinned) {
            return false;
        }
        if (expectedDepth == 0) {
            return true;
        }
        const std::size_t topSlot = expectedDepth - 1U;
        return scopeTokens_[topSlot] == expectedTopToken &&
               abandonedScopeTokens_[topSlot].load(std::memory_order_acquire) != expectedTopToken;
    }

    bool SamplerGenerationController::CaptureTableMutationSerial(std::uint64_t& output) const noexcept
    {
        if (table_.mutationSerial == nullptr) {
            return false;
        }
        output = table_.mutationSerial->load(std::memory_order_acquire);
        return true;
    }

    bool SamplerGenerationController::LiveTableMatches(
        const std::array<SamplerHandle, kFo4SamplerCount>& expected) const noexcept
    {
        if (!table_.load) {
            return false;
        }
        std::uint64_t serialBefore{};
        if (!CaptureTableMutationSerial(serialBefore)) {
            return false;
        }
        try {
            for (std::size_t index = 0; index < kFo4SamplerCount; ++index) {
                if (table_.load(index) != expected[index]) {
                    return false;
                }
            }
        } catch (...) {
            return false;
        }
        std::uint64_t serialAfter{};
        return CaptureTableMutationSerial(serialAfter) && serialAfter == serialBefore;
    }

    bool SamplerGenerationController::LiveTableMatchesOutputs(const Generation& generation) const noexcept
    {
        return LiveTableMatches(generation.outputs);
    }

    bool SamplerGenerationController::CaptureKeyFromHandles(
        const SamplerDeviceKey& device,
        float bias,
        const std::array<SamplerHandle, kFo4SamplerCount>& handles,
        GenerationKey& output) const noexcept
    {
        if (!backend_.describe) {
            return false;
        }
        output.device = device;
        output.biasBits = std::bit_cast<std::uint32_t>(bias);
        try {
            for (std::size_t index = 0; index < kFo4SamplerCount; ++index) {
                auto& entry = output.entries[index];
                entry.source = handles[index];
                entry.descriptor = {};
                if (handles[index] != 0 &&
                    !backend_.describe(handles[index], entry.descriptor)) {
                    return false;
                }
            }
        } catch (...) {
            return false;
        }
        return true;
    }

    bool SamplerGenerationController::CaptureCurrentSourceKey(const SamplerDeviceKey& device,
                                                              float bias,
                                                              GenerationKey& output,
                                                              const std::array<SamplerHandle,
                                                                               kFo4SamplerCount>*
                                                                  protectedHandles) const noexcept
    {
        std::array<SamplerHandle, kFo4SamplerCount> handles{};
        ProvisionalSourceOwnership provisional(&backend_.release);
        std::uint64_t serialBefore{};
        if (!CaptureTableMutationSerial(serialBefore)) {
            return false;
        }
        if (depth_ != 0) {
            if (!pinnedGeneration_ || !LiveTableMatchesOutputs(*pinnedGeneration_)) {
                return false;
            }
            handles = outerSnapshot_;
        } else {
            if (!table_.load || !backend_.retain || !backend_.release) {
                return false;
            }
            try {
                for (std::size_t index = 0; index < kFo4SamplerCount; ++index) {
                    handles[index] = table_.load(index);
                    if (handles[index] != 0 &&
                        (protectedHandles == nullptr ||
                         (*protectedHandles)[index] != handles[index])) {
                        backend_.retain(handles[index]);
                        provisional.Record(index, handles[index]);
                    }
                }
            } catch (...) {
                return false;
            }
        }
        if (!CaptureKeyFromHandles(device, bias, handles, output)) {
            return false;
        }
        std::uint64_t serialAfter{};
        return CaptureTableMutationSerial(serialAfter) && serialAfter == serialBefore;
    }

    SamplerGenerationController::GenerationKey*
        SamplerGenerationController::ScratchKeyForCurrentOperation() noexcept
    {
        if (callbackOperationDepth_ == 0 || !scratchStorage_) {
            return nullptr;
        }
        return scratchStorage_->KeyAt(callbackOperationDepth_ - 1U);
    }

    SamplerGenerationController::Ticket SamplerGenerationController::Refresh(
        const SamplerDeviceKey& device,
        const AppliedSamplerIntent& intent) noexcept
    {
        if (!IsOwnerThread() || tableTransitionInProgress_ ||
            (callbackOperationDepth_ == 0 && !RecoverAbandonedScopes())) {
            return {};
        }
        OwnerOperationGuard ownerOperation(*this);

        const std::uint64_t attemptSerial = ++refreshSerial_;
        desiredDevice_ = device;
        desiredIntent_ = intent;
        desiredValid_ = IsValidIntent(device, intent);
        if (!desiredValid_ || !table_.load || !table_.store || table_.mutationSerial == nullptr ||
            !backend_.retain || !backend_.release || !backend_.describe ||
            !backend_.makeBiasedDescriptor || !backend_.create) {
            return {};
        }
        GenerationKey* const scratchKey = ScratchKeyForCurrentOperation();
        if (scratchKey == nullptr) {
            return {};
        }
        const auto* const mutationSerialSource = table_.mutationSerial;

        const std::size_t attemptDepth = depth_;
        const auto attemptPinned = pinnedGeneration_;
        const std::uint64_t attemptTopToken =
            attemptDepth == 0 ? 0 : scopeTokens_[attemptDepth - 1U];
        auto base = published_.load(std::memory_order_acquire);
        const auto attemptStillCurrent = [&]() noexcept {
            return refreshSerial_ == attemptSerial && desiredValid_ && desiredDevice_ == device &&
                   IntentsEqual(desiredIntent_, intent) &&
                   published_.load(std::memory_order_acquire) == base &&
                   ScopeStackMatches(attemptDepth, attemptPinned, attemptTopToken);
        };
        std::array<SamplerHandle, kFo4SamplerCount> sourceHandles{};
        ProvisionalSourceOwnership provisional(&backend_.release);
        std::uint64_t observationSerialBefore{};
        if (!CaptureTableMutationSerial(observationSerialBefore)) {
            return {};
        }
        if (depth_ != 0) {
            if (!pinnedGeneration_ || !LiveTableMatchesOutputs(*pinnedGeneration_)) {
                return {};
            }
            sourceHandles = outerSnapshot_;
        } else {
            try {
                for (std::size_t index = 0; index < kFo4SamplerCount; ++index) {
                    sourceHandles[index] = table_.load(index);
                    if (sourceHandles[index] != 0 &&
                        (!base || base->sources[index] != sourceHandles[index])) {
                        backend_.retain(sourceHandles[index]);
                        provisional.Record(index, sourceHandles[index]);
                    }
                }
            } catch (...) {
                return {};
            }
        }

        std::uint64_t observationSerialAfter{};
        if (!CaptureKeyFromHandles(device, intent.bias, sourceHandles, *scratchKey) ||
            !CaptureTableMutationSerial(observationSerialAfter) ||
            observationSerialAfter != observationSerialBefore ||
            !attemptStillCurrent()) {
            return {};
        }

        if (base && base->key == *scratchKey) {
            if (!CaptureCurrentSourceKey(device, intent.bias, *scratchKey, &base->sources) ||
                !(*scratchKey == base->key) ||
                !attemptStillCurrent()) {
                return {};
            }
            return Ticket(ticketLifetime_, base->publicationId, attemptSerial);
        }

        std::shared_ptr<Generation> candidate;
        try {
            candidate = std::make_shared<Generation>();
            candidate->release = backend_.release;
            candidate->key = *scratchKey;
            for (std::size_t index = 0; index < kFo4SamplerCount; ++index) {
                const SamplerHandle source = sourceHandles[index];
                candidate->sources[index] = source;
                candidate->outputs[index] = source;
                if (source == 0) {
                    continue;
                }
                if (provisional.Owns(index)) {
                    provisional.Relinquish(index);
                } else {
                    backend_.retain(source);
                }
                candidate->sourceOwned[index] = true;
            }
        } catch (...) {
            return {};
        }

        if (!CaptureCurrentSourceKey(device, intent.bias, *scratchKey, &candidate->sources) ||
            !(*scratchKey == candidate->key) || !attemptStillCurrent()) {
            return {};
        }

        for (std::size_t index = 0; index < kFo4SamplerCount; ++index) {
            const SamplerHandle source = candidate->sources[index];
            if (source == 0) {
                continue;
            }
            SamplerDescriptor biased{};
            bool needsClone = false;
            try {
                needsClone = backend_.makeBiasedDescriptor(
                    candidate->key.entries[index].descriptor, intent.bias, biased);
            } catch (...) {
                return {};
            }
            if (!needsClone) {
                continue;
            }

            SamplerHandle created = 0;
            bool succeeded = false;
            try {
                succeeded = backend_.create(device.identity, biased, created);
            } catch (...) {
                if (created != 0) {
                    try {
                        backend_.release(created);
                    } catch (...) {
                    }
                }
                return {};
            }
            if (!succeeded || created == 0) {
                if (created != 0) {
                    try {
                        backend_.release(created);
                    } catch (...) {
                    }
                }
                return {};
            }
            candidate->outputs[index] = created;
            candidate->cloneOwned[index] = true;
        }

        if (!CaptureCurrentSourceKey(device, intent.bias, *scratchKey, &candidate->sources) ||
            !(*scratchKey == candidate->key) ||
            !attemptStillCurrent()) {
            return {};
        }

        candidate->publicationId = nextPublicationId_++;
        std::shared_ptr<const Generation> desired = candidate;
        auto expected = base;
        if (!published_.compare_exchange_strong(
                expected, desired, std::memory_order_release, std::memory_order_acquire)) {
            return {};
        }

        const std::uint64_t publicationId = desired->publicationId;
        const std::uint64_t teardownSerialBefore =
            mutationSerialSource->load(std::memory_order_acquire);
        expected.reset();
        base.reset();
        const std::uint64_t teardownSerialAfter =
            mutationSerialSource->load(std::memory_order_acquire);
        if (teardownSerialAfter != teardownSerialBefore || refreshSerial_ != attemptSerial ||
            !desiredValid_ || desiredDevice_ != device || !IntentsEqual(desiredIntent_, intent) ||
            published_.load(std::memory_order_acquire) != desired ||
            !ScopeStackMatches(attemptDepth, attemptPinned, attemptTopToken)) {
            return {};
        }
        return Ticket(ticketLifetime_, publicationId, attemptSerial);
    }

    bool SamplerGenerationController::Clear() noexcept
    {
        if (!IsOwnerThread() || tableTransitionInProgress_ ||
            (callbackOperationDepth_ == 0 && !RecoverAbandonedScopes())) {
            return false;
        }
        OwnerOperationGuard ownerOperation(*this);
        ++refreshSerial_;
        desiredValid_ = false;
        published_.store({}, std::memory_order_release);
        return true;
    }

    std::uint64_t SamplerGenerationController::PublishedId() const noexcept
    {
        if (!IsOwnerThread()) {
            return 0;
        }
        const auto generation = published_.load(std::memory_order_acquire);
        return generation ? generation->publicationId : 0;
    }

    std::size_t SamplerGenerationController::Depth() const noexcept
    {
        return IsOwnerThread() ? depth_ : 0;
    }

    std::uint64_t SamplerGenerationController::BeginScope(const Ticket& ticket,
                                                          const SamplerDeviceKey& device,
                                                          const AppliedSamplerIntent& intent) noexcept
    {
        if (!IsOwnerThread() || tableTransitionInProgress_ || callbackOperationDepth_ != 0 ||
            !RecoverAbandonedScopes()) {
            return 0;
        }
        OwnerOperationGuard ownerOperation(*this);
        const std::size_t entryDepth = depth_;
        const auto entryPinned = pinnedGeneration_;
        const std::uint64_t entryTopToken =
            entryDepth == 0 ? 0 : scopeTokens_[entryDepth - 1U];
        const auto ticketLifetime = ticket.lifetime_.lock();
        if (!ticketLifetime || ticketLifetime.get() != ticketLifetime_.get() ||
            ticket.publicationId_ == 0 || ticket.refreshSerial_ == 0 ||
            ticket.refreshSerial_ != refreshSerial_ || !desiredValid_ || device != desiredDevice_ ||
            !IntentsEqual(intent, desiredIntent_) || !IsValidIntent(device, intent) ||
            entryDepth >= kMaxSamplerScopeDepth) {
            return 0;
        }

        const auto currentPublication = published_.load(std::memory_order_acquire);
        if (!currentPublication || currentPublication->publicationId != ticket.publicationId_) {
            return 0;
        }

        if (entryDepth != 0) {
            if (!pinnedGeneration_ || !LiveTableMatchesOutputs(*pinnedGeneration_)) {
                return 0;
            }
            if (!ScopeStackMatches(entryDepth, entryPinned, entryTopToken) ||
                refreshSerial_ != ticket.refreshSerial_ || !desiredValid_ ||
                published_.load(std::memory_order_acquire) != currentPublication ||
                device != desiredDevice_ || !IntentsEqual(intent, desiredIntent_)) {
                return 0;
            }
            const std::uint64_t token = AllocateScopeToken(entryDepth);
            abandonedScopeTokens_[entryDepth].store(0, std::memory_order_relaxed);
            scopeTokens_[entryDepth] = token;
            ++depth_;
            return token;
        }

        if (currentPublication->key.device != device ||
            currentPublication->key.biasBits != std::bit_cast<std::uint32_t>(intent.bias)) {
            return 0;
        }
        GenerationKey* const scratchKey = ScratchKeyForCurrentOperation();
        if (scratchKey == nullptr) {
            return 0;
        }
        if (!CaptureCurrentSourceKey(
                device, intent.bias, *scratchKey, &currentPublication->sources) ||
            !(*scratchKey == currentPublication->key) ||
            !ScopeStackMatches(entryDepth, entryPinned, entryTopToken)) {
            return 0;
        }

        std::uint64_t snapshotSerialBefore{};
        if (!CaptureTableMutationSerial(snapshotSerialBefore)) {
            return 0;
        }
        outerSnapshotOwned_.fill(false);
        try {
            for (std::size_t index = 0; index < kFo4SamplerCount; ++index) {
                const SamplerHandle handle = table_.load(index);
                outerSnapshot_[index] = handle;
                if (handle != 0) {
                    backend_.retain(handle);
                    outerSnapshotOwned_[index] = true;
                }
            }
        } catch (...) {
            ReleaseOuterSnapshot();
            return 0;
        }
        std::uint64_t snapshotSerialAfter{};
        if (!CaptureTableMutationSerial(snapshotSerialAfter) ||
            snapshotSerialAfter != snapshotSerialBefore) {
            ReleaseOuterSnapshot();
            return 0;
        }

        if (refreshSerial_ != ticket.refreshSerial_ ||
            published_.load(std::memory_order_acquire) != currentPublication ||
            !CaptureCurrentSourceKey(
                device, intent.bias, *scratchKey, &currentPublication->sources) ||
            !(*scratchKey == currentPublication->key) ||
            !ScopeStackMatches(entryDepth, entryPinned, entryTopToken)) {
            ReleaseOuterSnapshot();
            return 0;
        }

        pinnedGeneration_ = currentPublication;
        const std::uint64_t token = AllocateScopeToken(0);
        abandonedScopeTokens_[0].store(0, std::memory_order_relaxed);
        scopeTokens_[0] = token;
        depth_ = 1;

        bool entryTransitionValid = true;
        tableTransitionInProgress_ = true;
        try {
            for (std::size_t index = 0; index < kFo4SamplerCount; ++index) {
                if (currentPublication->outputs[index] != currentPublication->sources[index]) {
                    table_.store(index, currentPublication->outputs[index]);
                }
            }
        } catch (...) {
            entryTransitionValid = false;
        }

        if (entryTransitionValid && !LiveTableMatchesOutputs(*currentPublication)) {
            entryTransitionValid = false;
        }
        tableTransitionInProgress_ = false;
        if (!entryTransitionValid ||
            refreshSerial_ != ticket.refreshSerial_ || !desiredValid_ ||
            published_.load(std::memory_order_acquire) != currentPublication ||
            device != desiredDevice_ || !IntentsEqual(intent, desiredIntent_)) {
            MarkScopeAbandoned(token);
            return 0;
        }

        return token;
    }

    std::uint64_t SamplerGenerationController::AllocateScopeToken(std::size_t slot) noexcept
    {
        std::uint64_t serial = nextScopeToken_++;
        if (serial == 0) {
            serial = nextScopeToken_++;
        }
        return (serial << 4U) | static_cast<std::uint64_t>(slot + 1U);
    }

    std::size_t SamplerGenerationController::ScopeTokenSlot(std::uint64_t token) noexcept
    {
        const auto encoded = static_cast<std::size_t>(token & 0xFU);
        if (encoded == 0 || encoded > kMaxSamplerScopeDepth) {
            return kMaxSamplerScopeDepth;
        }
        return encoded - 1U;
    }

    void SamplerGenerationController::MarkScopeAbandoned(std::uint64_t token) noexcept
    {
        const std::size_t slot = ScopeTokenSlot(token);
        if (slot < kMaxSamplerScopeDepth) {
            abandonedScopeTokens_[slot].store(token, std::memory_order_release);
        }
    }

    bool SamplerGenerationController::RecoverAbandonedScopes() noexcept
    {
        if (!IsOwnerThread() || tableTransitionInProgress_ || callbackOperationDepth_ != 0) {
            return false;
        }

        tableTransitionInProgress_ = true;
        while (depth_ != 0) {
            const std::size_t slot = depth_ - 1U;
            const std::uint64_t token = scopeTokens_[slot];
            if (abandonedScopeTokens_[slot].load(std::memory_order_acquire) != token) {
                break;
            }

            if (slot == 0) {
                if (!RestoreOuterAndRelease()) {
                    tableTransitionInProgress_ = false;
                    return false;
                }
                abandonedScopeTokens_[0].store(0, std::memory_order_relaxed);
                scopeTokens_[0] = 0;
                depth_ = 0;
                pinnedGeneration_.reset();
                break;
            }

            abandonedScopeTokens_[slot].store(0, std::memory_order_relaxed);
            scopeTokens_[slot] = 0;
            --depth_;
        }
        tableTransitionInProgress_ = false;
        return true;
    }

    void SamplerGenerationController::EndScope(std::uint64_t token) noexcept
    {
        if (token == 0) {
            return;
        }
        const std::size_t slot = ScopeTokenSlot(token);
        if (!IsOwnerThread()) {
            MarkScopeAbandoned(token);
            return;
        }
        if (tableTransitionInProgress_ || callbackOperationDepth_ != 0) {
            MarkScopeAbandoned(token);
            return;
        }
        if (slot >= depth_ || scopeTokens_[slot] != token) {
            return;
        }
        if (slot != depth_ - 1U) {
            MarkScopeAbandoned(token);
            return;
        }

        if (slot == 0) {
            tableTransitionInProgress_ = true;
            const bool restored = RestoreOuterAndRelease();
            if (!restored) {
                tableTransitionInProgress_ = false;
                MarkScopeAbandoned(token);
                return;
            }
            abandonedScopeTokens_[0].store(0, std::memory_order_relaxed);
            scopeTokens_[0] = 0;
            depth_ = 0;
            pinnedGeneration_.reset();
            tableTransitionInProgress_ = false;
            return;
        }

        abandonedScopeTokens_[slot].store(0, std::memory_order_relaxed);
        scopeTokens_[slot] = 0;
        --depth_;
        (void)RecoverAbandonedScopes();
    }

    bool SamplerGenerationController::RestoreOuterAndRelease() noexcept
    {
        if (!table_.store || !table_.load) {
            return false;
        }

        bool restored = true;
        for (std::size_t index = 0; index < kFo4SamplerCount; ++index) {
            try {
                table_.store(index, outerSnapshot_[index]);
            } catch (...) {
                restored = false;
            }
        }
        if (restored && !LiveTableMatches(outerSnapshot_)) {
            restored = false;
        }
        if (!restored) {
            return false;
        }

        ReleaseOuterSnapshot();
        return true;
    }

    void SamplerGenerationController::ReleaseOuterSnapshot() noexcept
    {
        if (backend_.release) {
            for (std::size_t index = 0; index < kFo4SamplerCount; ++index) {
                if (outerSnapshotOwned_[index] && outerSnapshot_[index] != 0) {
                    try {
                        backend_.release(outerSnapshot_[index]);
                    } catch (...) {
                    }
                }
                outerSnapshotOwned_[index] = false;
                outerSnapshot_[index] = 0;
            }
        } else {
            outerSnapshotOwned_.fill(false);
            outerSnapshot_.fill(0);
        }
    }
}
