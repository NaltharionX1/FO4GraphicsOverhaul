#include "Platform/SamplerGeneration.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
    using Platform::AppliedSamplerEngine;
    using Platform::AppliedSamplerIntent;
    using Platform::SamplerBackendCallbacks;
    using Platform::SamplerDescriptor;
    using Platform::SamplerDeviceKey;
    using Platform::SamplerGenerationController;
    using Platform::SamplerHandle;
    using Platform::SamplerTableCallbacks;
    using Platform::kFo4SamplerCount;
    using Platform::kSamplerDescriptorBytes;

    constexpr std::uint32_t kFilterPoint = 0;
    constexpr std::uint32_t kFilterAnisotropic = 1;

    struct CaseResult
    {
        std::string name;
        int checks{ 0 };
        int failures{ 0 };
        std::vector<std::string> messages;
    };

    CaseResult* g_active{ nullptr };

    void Check(bool condition, std::string_view message)
    {
        if (g_active == nullptr) {
            return;
        }
        ++g_active->checks;
        if (!condition) {
            ++g_active->failures;
            g_active->messages.emplace_back(message);
        }
    }

    template <class T, class U>
    void CheckEqual(const T& actual, const U& expected, std::string_view message)
    {
        Check(actual == expected, message);
    }

    void PutU32(SamplerDescriptor& descriptor, std::size_t offset, std::uint32_t value)
    {
        std::memcpy(descriptor.bytes.data() + offset, &value, sizeof(value));
    }

    [[nodiscard]] std::uint32_t GetU32(const SamplerDescriptor& descriptor, std::size_t offset)
    {
        std::uint32_t value{};
        std::memcpy(&value, descriptor.bytes.data() + offset, sizeof(value));
        return value;
    }

    void PutFloat(SamplerDescriptor& descriptor, std::size_t offset, float value)
    {
        PutU32(descriptor, offset, std::bit_cast<std::uint32_t>(value));
    }

    [[nodiscard]] float GetFloat(const SamplerDescriptor& descriptor, std::size_t offset)
    {
        return std::bit_cast<float>(GetU32(descriptor, offset));
    }

    [[nodiscard]] SamplerDescriptor MakeDescriptor(std::uint32_t filter, float mipBias, std::uint8_t tag)
    {
        SamplerDescriptor descriptor{};
        for (std::size_t i = 8; i < descriptor.bytes.size(); ++i) {
            descriptor.bytes[i] = static_cast<std::uint8_t>(tag + static_cast<std::uint8_t>(i));
        }
        PutU32(descriptor, 0, filter);
        PutFloat(descriptor, 4, mipBias);
        return descriptor;
    }

    struct FakeSampler
    {
        SamplerDescriptor descriptor{};
        int references{ 1 };
        int retainCalls{ 0 };
        int releaseCalls{ 0 };
        bool clone{ false };
    };

    enum class CreateFailure
    {
        kNone,
        kFalseNull,
        kFalseNonNull,
        kTrueNull,
        kThrowNonNull,
    };

    class FakeBackend
    {
    public:
        [[nodiscard]] SamplerHandle AddOriginal(const SamplerDescriptor& descriptor)
        {
            return AddObject(descriptor, false);
        }

        [[nodiscard]] FakeSampler& Get(SamplerHandle handle)
        {
            return *objects_.at(handle);
        }

        [[nodiscard]] const FakeSampler& Get(SamplerHandle handle) const
        {
            return *objects_.at(handle);
        }

        void FailCreateAt(int absoluteCall, CreateFailure failure)
        {
            failAtCreateCall = absoluteCall;
            createFailure = failure;
        }

        void ClearCreateFailure()
        {
            failAtCreateCall = 0;
            createFailure = CreateFailure::kNone;
        }

        void ReleaseExternalReference(SamplerHandle handle)
        {
            auto& object = Get(handle);
            if (object.references <= 0) {
                ++releaseUnderflows;
                return;
            }
            --object.references;
            ++object.releaseCalls;
        }

        [[nodiscard]] int LiveCloneCount() const
        {
            int result = 0;
            for (const auto& [handle, object] : objects_) {
                (void)handle;
                if (object->clone && object->references > 0) {
                    ++result;
                }
            }
            return result;
        }

        [[nodiscard]] SamplerBackendCallbacks Bind()
        {
            SamplerBackendCallbacks callbacks;
            callbacks.retain = [this](SamplerHandle handle) {
                auto& object = Get(handle);
                if (object.references <= 0) {
                    throw std::runtime_error("retain on dead fake sampler");
                }
                ++object.references;
                ++object.retainCalls;
                if (onRetain) {
                    onRetain(handle);
                }
            };
            callbacks.release = [this](SamplerHandle handle) {
                auto& object = Get(handle);
                if (std::this_thread::get_id() != ownerThread) {
                    ++wrongThreadReleaseCalls;
                }
                if (object.references <= 0) {
                    ++releaseUnderflows;
                    return;
                }
                --object.references;
                ++object.releaseCalls;
                if (onRelease) {
                    onRelease(handle);
                }
            };
            callbacks.describe = [this](SamplerHandle handle, SamplerDescriptor& output) {
                ++describeCalls;
                if (onDescribe) {
                    onDescribe(handle);
                }
                const auto found = objects_.find(handle);
                if (found == objects_.end() || found->second->references <= 0) {
                    ++describeDeadCalls;
                    return false;
                }
                output = found->second->descriptor;
                return true;
            };
            callbacks.makeBiasedDescriptor = [this](const SamplerDescriptor& source,
                                                     float bias,
                                                     SamplerDescriptor& output) {
                if (onMakeBiasedDescriptor) {
                    onMakeBiasedDescriptor(source, bias);
                }
                if (GetU32(source, 0) != kFilterAnisotropic || GetFloat(source, 4) != 0.0F) {
                    return false;
                }
                output = source;
                PutFloat(output, 4, bias);
                return true;
            };
            callbacks.create = [this](SamplerHandle device,
                                      const SamplerDescriptor& descriptor,
                                      SamplerHandle& output) {
                ++createCalls;
                lastCreateDevice = device;
                output = 0;
                if (onCreate) {
                    onCreate(device, descriptor);
                }
                const bool failThisCall = failAtCreateCall > 0 && createCalls == failAtCreateCall;
                if (!failThisCall) {
                    output = AddObject(descriptor, true);
                    return true;
                }
                switch (createFailure) {
                case CreateFailure::kFalseNull:
                    return false;
                case CreateFailure::kFalseNonNull:
                    output = AddObject(descriptor, true);
                    return false;
                case CreateFailure::kTrueNull:
                    return true;
                case CreateFailure::kThrowNonNull:
                    output = AddObject(descriptor, true);
                    throw std::runtime_error("injected create throw after non-null output");
                case CreateFailure::kNone:
                default:
                    output = AddObject(descriptor, true);
                    return true;
                }
            };
            return callbacks;
        }

        int createCalls{ 0 };
        int describeCalls{ 0 };
        int describeDeadCalls{ 0 };
        int releaseUnderflows{ 0 };
        int wrongThreadReleaseCalls{ 0 };
        int failAtCreateCall{ 0 };
        CreateFailure createFailure{ CreateFailure::kNone };
        SamplerHandle lastCreateDevice{ 0 };
        std::thread::id ownerThread{ std::this_thread::get_id() };
        std::function<void(SamplerHandle)> onRetain;
        std::function<void(SamplerHandle)> onRelease;
        std::function<void(SamplerHandle)> onDescribe;
        std::function<void(const SamplerDescriptor&, float)> onMakeBiasedDescriptor;
        std::function<void(SamplerHandle, const SamplerDescriptor&)> onCreate;

    private:
        [[nodiscard]] SamplerHandle AddObject(const SamplerDescriptor& descriptor, bool clone)
        {
            const SamplerHandle handle = nextHandle_++;
            auto object = std::make_unique<FakeSampler>();
            object->descriptor = descriptor;
            object->clone = clone;
            objects_.emplace(handle, std::move(object));
            return handle;
        }

        SamplerHandle nextHandle_{ 0x1000 };
        std::unordered_map<SamplerHandle, std::unique_ptr<FakeSampler>> objects_;
    };

    class FakeTable
    {
    public:
        [[nodiscard]] SamplerTableCallbacks Bind()
        {
            SamplerTableCallbacks callbacks;
            callbacks.load = [this](std::size_t index) {
                ++loadCalls;
                if (onLoad) {
                    onLoad(index);
                }
                return entries.at(index);
            };
            callbacks.store = [this](std::size_t index, SamplerHandle handle) {
                ++storeCalls;
                if (onStore) {
                    onStore(index, handle);
                }
                if (throwStoreCount > 0 && index == throwStoreIndex) {
                    --throwStoreCount;
                    throw std::runtime_error("injected fake table store failure");
                }
                if (skipStoreCount > 0 && index == skipStoreIndex) {
                    --skipStoreCount;
                    return;
                }
                Mutate(index, handle);
            };
            callbacks.mutationSerial = &mutationSerial;
            return callbacks;
        }

        void Mutate(std::size_t index, SamplerHandle handle)
        {
            entries.at(index) = handle;
            mutationSerial.fetch_add(1, std::memory_order_release);
        }

        std::array<SamplerHandle, kFo4SamplerCount> entries{};
        std::atomic<std::uint64_t> mutationSerial{ 0 };
        int loadCalls{ 0 };
        int storeCalls{ 0 };
        std::size_t throwStoreIndex{ kFo4SamplerCount };
        int throwStoreCount{ 0 };
        std::size_t skipStoreIndex{ kFo4SamplerCount };
        int skipStoreCount{ 0 };
        std::function<void(std::size_t)> onLoad;
        std::function<void(std::size_t, SamplerHandle)> onStore;
    };

    struct Fixture
    {
        Fixture() : controller(table.Bind(), backend.Bind())
        {
            FillWithPointSamplers();
        }

        void FillWithPointSamplers()
        {
            for (std::size_t index = 0; index < kFo4SamplerCount; ++index) {
                table.entries[index] = backend.AddOriginal(
                    MakeDescriptor(kFilterPoint, 0.0F, static_cast<std::uint8_t>(index)));
            }
        }

        void MakeAllVirginAnisotropic()
        {
            for (std::size_t index = 0; index < kFo4SamplerCount; ++index) {
                table.entries[index] = backend.AddOriginal(
                    MakeDescriptor(kFilterAnisotropic, 0.0F, static_cast<std::uint8_t>(index)));
            }
        }

        [[nodiscard]] AppliedSamplerIntent Intent(float bias = -1.0F,
                                                  std::uint64_t commit = 1,
                                                  AppliedSamplerEngine engine = AppliedSamplerEngine::kDlss) const
        {
            return AppliedSamplerIntent{ commit, engine, true, bias };
        }

        [[nodiscard]] SamplerDeviceKey Device(std::uint64_t epoch = 1,
                                              SamplerHandle identity = 0xD311) const
        {
            return SamplerDeviceKey{ identity, epoch };
        }

        FakeBackend backend;
        FakeTable table;
        SamplerGenerationController controller;
    };

    void Case_SameBiasChangedSourceRebuilds()
    {
        Fixture fixture;
        fixture.table.entries[0] =
            fixture.backend.AddOriginal(MakeDescriptor(kFilterAnisotropic, 0.0F, 1));
        const auto first = fixture.controller.Refresh(fixture.Device(), fixture.Intent());
        Check(first.Usable(), "initial generation should build");
        CheckEqual(fixture.backend.createCalls, 1, "initial generation should clone the one target");

        const auto priorPublication = first.PublicationId();
        const auto sameDescriptor = fixture.backend.Get(fixture.table.entries[1]).descriptor;
        fixture.table.entries[1] = fixture.backend.AddOriginal(sameDescriptor);
        const auto second = fixture.controller.Refresh(fixture.Device(), fixture.Intent());
        Check(second.Usable(), "changed source identity should build");
        Check(second.PublicationId() != priorPublication,
              "source identity is part of the structural generation key");
        CheckEqual(fixture.backend.createCalls, 2,
                   "same bias with a changed pass-through source must rebuild targets");
    }

    void Case_SameBiasChangedDescriptorRebuilds()
    {
        Fixture fixture;
        fixture.table.entries[0] =
            fixture.backend.AddOriginal(MakeDescriptor(kFilterAnisotropic, 0.0F, 2));
        const auto first = fixture.controller.Refresh(fixture.Device(), fixture.Intent());
        Check(first.Usable(), "initial generation should build");
        const auto priorPublication = first.PublicationId();

        auto& changed = fixture.backend.Get(fixture.table.entries[7]).descriptor;
        changed.bytes[kSamplerDescriptorBytes - 1] ^= 0x5A;
        const auto second = fixture.controller.Refresh(fixture.Device(), fixture.Intent());
        Check(second.Usable(), "changed descriptor should build");
        Check(second.PublicationId() != priorPublication,
              "all 52 descriptor bytes are part of the structural key");
        CheckEqual(fixture.backend.createCalls, 2,
                   "same bias with a changed descriptor must rebuild targets");
    }

    void Case_SameBiasNewDeviceRebuilds()
    {
        Fixture fixture;
        fixture.table.entries[0] =
            fixture.backend.AddOriginal(MakeDescriptor(kFilterAnisotropic, 0.0F, 3));
        const auto first = fixture.controller.Refresh(fixture.Device(1, 0xD311), fixture.Intent());
        const auto second = fixture.controller.Refresh(fixture.Device(2, 0xD311), fixture.Intent());
        const auto third = fixture.controller.Refresh(fixture.Device(2, 0xD312), fixture.Intent());
        Check(first.Usable() && second.Usable() && third.Usable(),
              "each valid device generation should build");
        Check(first.PublicationId() != second.PublicationId(), "device epoch change must rebuild");
        Check(second.PublicationId() != third.PublicationId(), "device identity change must rebuild");
        CheckEqual(fixture.backend.createCalls, 3, "each distinct device key should clone targets");
        CheckEqual(fixture.backend.lastCreateDevice, SamplerHandle{ 0xD312 },
                   "factory must receive the desired device identity");
    }

    void Case_CloneFailureDoesNotPublishPartial()
    {
        Fixture fixture;
        const auto baseline = fixture.controller.Refresh(fixture.Device(), fixture.Intent());
        Check(baseline.Usable(), "pass-through baseline should publish");
        const auto baselinePublication = baseline.PublicationId();

        fixture.MakeAllVirginAnisotropic();
        fixture.backend.FailCreateAt(320, CreateFailure::kFalseNull);
        const auto failed = fixture.controller.Refresh(fixture.Device(), fixture.Intent());
        Check(!failed.Usable(), "late clone failure must return an unusable ticket");
        CheckEqual(fixture.controller.PublishedId(), baselinePublication,
                   "late failure must leave the old publication untouched");
        CheckEqual(fixture.backend.LiveCloneCount(), 0,
                   "all successful temporary clones before the late failure must be released");
        CheckEqual(fixture.table.storeCalls, 0, "refresh must never mutate the engine table");

        SamplerGenerationController::Scope stale(
            fixture.controller, baseline, fixture.Device(), fixture.Intent());
        Check(!stale.Active(), "a failed desired refresh must invalidate an older ticket for entry");
        CheckEqual(fixture.controller.Depth(), std::size_t{ 0 }, "failed entry must not change depth");
    }

    void Case_SameKeyAfterFailureRetries()
    {
        Fixture fixture;
        fixture.table.entries[0] =
            fixture.backend.AddOriginal(MakeDescriptor(kFilterAnisotropic, 0.0F, 4));
        fixture.backend.FailCreateAt(1, CreateFailure::kFalseNull);
        const auto failed = fixture.controller.Refresh(fixture.Device(), fixture.Intent());
        Check(!failed.Usable(), "injected create failure should fail refresh");
        CheckEqual(fixture.backend.createCalls, 1, "first refresh should attempt the clone once");

        fixture.backend.ClearCreateFailure();
        const auto retry = fixture.controller.Refresh(fixture.Device(), fixture.Intent());
        Check(retry.Usable(), "same key must retry after a failed attempt");
        CheckEqual(fixture.backend.createCalls, 2, "same-key retry must call the factory again");
        Check(retry.PublicationId() != 0, "successful retry should publish a generation");
    }

    void Case_SameKeyReuseAvoidsOwnershipChurn()
    {
        Fixture fixture;
        fixture.table.entries[0] =
            fixture.backend.AddOriginal(MakeDescriptor(kFilterAnisotropic, 0.0F, 5));
        const auto first = fixture.controller.Refresh(fixture.Device(), fixture.Intent());
        Check(first.Usable(), "initial generation should build");
        const int retainsAfterBuild = fixture.backend.Get(fixture.table.entries[0]).retainCalls;
        const int releasesAfterBuild = fixture.backend.Get(fixture.table.entries[0]).releaseCalls;
        const int createsAfterBuild = fixture.backend.createCalls;

        const auto reused = fixture.controller.Refresh(fixture.Device(), fixture.Intent());
        Check(reused.Usable(), "identical full key should reuse the publication");
        CheckEqual(reused.PublicationId(), first.PublicationId(),
                   "identical full key should return the same immutable publication");
        CheckEqual(fixture.backend.Get(fixture.table.entries[0]).retainCalls, retainsAfterBuild,
                   "identical-key refresh must not retain all originals again");
        CheckEqual(fixture.backend.Get(fixture.table.entries[0]).releaseCalls, releasesAfterBuild,
                   "identical-key refresh must not release a throwaway ownership snapshot");
        CheckEqual(fixture.backend.createCalls, createsAfterBuild,
                   "identical-key refresh must not call the clone factory");
    }

    void Case_SameKeyCallbackFailedRefreshCannotReturnStaleTicket()
    {
        Fixture fixture;
        fixture.table.entries[0] =
            fixture.backend.AddOriginal(MakeDescriptor(kFilterAnisotropic, 0.0F, 6));
        const auto intent = fixture.Intent(-1.0F, 50);
        const auto first = fixture.controller.Refresh(fixture.Device(), intent);
        Check(first.Usable(), "initial generation should build");
        const auto firstPublication = first.PublicationId();
        const int triggerDescribe = fixture.backend.describeCalls +
                                    static_cast<int>(kFo4SamplerCount) + 1;
        bool callbackFired = false;
        bool failedRefreshUsable = true;
        fixture.backend.onDescribe = [&](SamplerHandle) {
            if (!callbackFired && fixture.backend.describeCalls == triggerDescribe) {
                callbackFired = true;
                auto disabled = intent;
                disabled.active = false;
                failedRefreshUsable =
                    fixture.controller.Refresh(fixture.Device(), disabled).Usable();
            }
        };

        const auto stale = fixture.controller.Refresh(fixture.Device(), intent);
        fixture.backend.onDescribe = {};
        Check(callbackFired, "same-key final capture must execute the deterministic callback");
        Check(!failedRefreshUsable, "callback-driven disabled Refresh must itself fail");
        Check(!stale.Usable(),
              "same-key branch must recheck attempt state after callback-backed key capture");
        CheckEqual(fixture.controller.PublishedId(), firstPublication,
                   "failed nested Refresh must leave the prior publication pointer unchanged");
    }

    void Case_FinalPreCasCallbackFailedRefreshCannotPublishStaleCandidate()
    {
        Fixture fixture;
        fixture.table.entries[0] =
            fixture.backend.AddOriginal(MakeDescriptor(kFilterAnisotropic, 0.0F, 7));
        const auto intentA = fixture.Intent(-1.0F, 60);
        const auto first = fixture.controller.Refresh(fixture.Device(), intentA);
        Check(first.Usable(), "initial generation A should build");
        const auto firstPublication = first.PublicationId();

        const auto intentB = fixture.Intent(-2.0F, 61);
        const int triggerDescribe = fixture.backend.describeCalls +
                                    static_cast<int>(kFo4SamplerCount * 2U) + 1;
        bool callbackFired = false;
        bool failedRefreshUsable = true;
        fixture.backend.onDescribe = [&](SamplerHandle) {
            if (!callbackFired && fixture.backend.describeCalls == triggerDescribe) {
                callbackFired = true;
                auto disabled = intentB;
                disabled.active = false;
                failedRefreshUsable =
                    fixture.controller.Refresh(fixture.Device(), disabled).Usable();
            }
        };

        const auto staleCandidate = fixture.controller.Refresh(fixture.Device(), intentB);
        fixture.backend.onDescribe = {};
        Check(callbackFired, "new-candidate final capture must execute the deterministic callback");
        Check(!failedRefreshUsable, "callback-driven disabled Refresh must itself fail");
        Check(!staleCandidate.Usable(),
              "final pre-CAS branch must recheck attempt state after callback-backed capture");
        CheckEqual(fixture.controller.PublishedId(), firstPublication,
                   "stale candidate must not replace the unchanged prior publication");
    }

    void Case_AtomicMutationSerialFinalizesReplacementAcrossPriorTeardown()
    {
        Fixture fixture;
        fixture.table.entries[0] =
            fixture.backend.AddOriginal(MakeDescriptor(kFilterAnisotropic, 0.0F, 90));
        const auto first = fixture.controller.Refresh(fixture.Device(), fixture.Intent(-1.0F, 90));
        Check(first.Usable(), "initial generation should build before replacement finalization");
        const auto priorPublication = first.PublicationId();
        bool releaseCallbackFired = false;
        fixture.backend.onRelease = [&](SamplerHandle) { releaseCallbackFired = true; };

        const auto replacement =
            fixture.controller.Refresh(fixture.Device(), fixture.Intent(-2.0F, 91));
        fixture.backend.onRelease = {};

        Check(releaseCallbackFired,
              "replacement finalization must execute prior generation teardown before returning");
        Check(replacement.Usable(),
              "direct atomic version loads must allow coherent replacement finalization");
        Check(replacement.PublicationId() != priorPublication,
              "replacement must publish a distinct immutable generation");
        CheckEqual(fixture.controller.PublishedId(), replacement.PublicationId(),
                   "successful finalization must leave the replacement current");
    }

    void Case_NewSourceIsRetainedBeforeDescriptorCapture()
    {
        Fixture fixture;
        const auto oldSource = fixture.table.entries[0];
        const auto replacement =
            fixture.backend.AddOriginal(MakeDescriptor(kFilterPoint, 0.0F, 8));
        bool sourceRetired = false;
        fixture.table.onLoad = [&](std::size_t index) {
            if (!sourceRetired && index == 1) {
                sourceRetired = true;
                fixture.table.entries[0] = replacement;
                fixture.backend.ReleaseExternalReference(oldSource);
            }
        };

        const auto ticket = fixture.controller.Refresh(fixture.Device(), fixture.Intent());
        fixture.table.onLoad = {};
        Check(sourceRetired, "deterministic table callback must retire the earlier raw source");
        Check(!ticket.Usable(), "table change during capture must reject the stale candidate");
        CheckEqual(fixture.backend.describeDeadCalls, 0,
                   "controller must provisionally retain a new raw source before describing it");
        CheckEqual(fixture.backend.Get(oldSource).references, 0,
                   "rejected provisional ownership must release after the stable-key check");
    }

    void Case_SwapBalancesOwnership()
    {
        Fixture fixture;
        fixture.table.entries.fill(0);
        const auto nonAniso = fixture.backend.AddOriginal(MakeDescriptor(kFilterPoint, 0.0F, 10));
        const auto preBiased = fixture.backend.AddOriginal(MakeDescriptor(kFilterAnisotropic, -0.5F, 11));
        const auto virgin = fixture.backend.AddOriginal(MakeDescriptor(kFilterAnisotropic, 0.0F, 12));
        fixture.table.entries[1] = nonAniso;
        fixture.table.entries[2] = preBiased;
        fixture.table.entries[3] = virgin;
        fixture.table.entries[4] = nonAniso;
        const auto exactBefore = fixture.table.entries;

        SamplerHandle firstClone = 0;
        {
            const auto ticket = fixture.controller.Refresh(fixture.Device(), fixture.Intent());
            Check(ticket.Usable(), "mixed table should build");
            CheckEqual(fixture.backend.createCalls, 1,
                       "only the virgin anisotropic entry should call the factory");
            CheckEqual(fixture.backend.Get(nonAniso).retainCalls, 2,
                       "duplicate source must be retained once for each occupied slot");
            CheckEqual(fixture.backend.Get(preBiased).retainCalls, 1,
                       "already-biased anisotropic source must be retained without cloning");
            CheckEqual(fixture.backend.Get(virgin).retainCalls, 1,
                       "target source must remain owned alongside its clone");

            SamplerGenerationController::Scope scope(
                fixture.controller, ticket, fixture.Device(), fixture.Intent());
            Check(scope.Active(), "valid generation should enter");
            CheckEqual(fixture.table.entries[0], SamplerHandle{ 0 }, "null slot must remain null");
            CheckEqual(fixture.table.entries[1], nonAniso, "non-anisotropic slot identity must pass through");
            CheckEqual(fixture.table.entries[2], preBiased,
                       "already-biased slot identity must pass through");
            Check(fixture.table.entries[3] != virgin, "virgin target should use its biased clone");
            firstClone = fixture.table.entries[3];
            CheckEqual(fixture.table.entries[4], nonAniso, "duplicate pass-through identity must remain exact");
        }

        CheckEqual(fixture.table.entries, exactBefore, "outer scope must restore the exact entry table");
        {
            fixture.table.entries[5] =
                fixture.backend.AddOriginal(MakeDescriptor(kFilterPoint, 0.0F, 13));
            const auto replacement = fixture.controller.Refresh(fixture.Device(), fixture.Intent());
            Check(replacement.Usable(), "changed source should atomically replace the publication");
            CheckEqual(fixture.backend.Get(firstClone).references, 0,
                       "publication swap must release the old generation clone");
            CheckEqual(fixture.backend.LiveCloneCount(), 1,
                       "publication swap must retain only the replacement generation clone");
            Check(fixture.controller.Clear(), "owner thread should clear the publication");
            CheckEqual(fixture.backend.LiveCloneCount(), 0,
                       "non-owning Ticket must not retain generation ownership after clear");
        }
        CheckEqual(fixture.backend.LiveCloneCount(), 0,
                   "dropping a non-owning Ticket must not perform additional clone teardown");
        CheckEqual(fixture.backend.Get(nonAniso).references, 1,
                   "duplicate original retains/releases must balance back to table ownership");
        CheckEqual(fixture.backend.Get(preBiased).references, 1,
                   "pre-biased original ownership must balance");
        CheckEqual(fixture.backend.Get(virgin).references, 1, "target original ownership must balance");
        CheckEqual(fixture.backend.releaseUnderflows, 0, "no handle may be over-released");
    }

    void Case_NestedScopesRestoreOutermostOnly()
    {
        Fixture fixture;
        fixture.table.entries[0] =
            fixture.backend.AddOriginal(MakeDescriptor(kFilterAnisotropic, 0.0F, 20));
        const auto exactBefore = fixture.table.entries;
        const auto intentA = fixture.Intent(-1.0F, 1);
        const auto ticketA = fixture.controller.Refresh(fixture.Device(), intentA);
        Check(ticketA.Usable(), "generation A should build");

        {
            SamplerGenerationController::Scope outer(
                fixture.controller, ticketA, fixture.Device(), intentA);
            Check(outer.Active(), "outer A scope should enter");
            const auto cloneA = fixture.table.entries[0];
            Check(cloneA != exactBefore[0], "outer scope should install A clone");
            CheckEqual(fixture.controller.Depth(), std::size_t{ 1 }, "outer scope should set depth one");

            const auto intentB = fixture.Intent(-2.0F, 2);
            const auto ticketB = fixture.controller.Refresh(fixture.Device(), intentB);
            Check(ticketB.Usable(), "generation B should publish while A is pinned");
            Check(ticketB.PublicationId() != ticketA.PublicationId(), "B should be a new publication");
            CheckEqual(fixture.table.entries[0], cloneA,
                       "publishing B must not rewrite an active A scope");

            {
                SamplerGenerationController::Scope nested(
                    fixture.controller, ticketB, fixture.Device(), intentB);
                Check(nested.Active(), "nested scope should join the pinned outer generation");
                CheckEqual(fixture.controller.Depth(), std::size_t{ 2 }, "nested scope should increase depth");
                CheckEqual(fixture.table.entries[0], cloneA,
                           "nested B entry must not replace pinned A table contents");
            }
            CheckEqual(fixture.controller.Depth(), std::size_t{ 1 }, "nested exit must leave outer active");
            CheckEqual(fixture.table.entries[0], cloneA, "nested exit must not restore the table early");
        }

        CheckEqual(fixture.controller.Depth(), std::size_t{ 0 }, "outer exit should return to depth zero");
        CheckEqual(fixture.table.entries, exactBefore, "outer exit should restore its exact captured table");

        const auto intentB = fixture.Intent(-2.0F, 2);
        const auto ticketB = fixture.controller.Refresh(fixture.Device(), intentB);
        SamplerGenerationController::Scope nextOuter(
            fixture.controller, ticketB, fixture.Device(), intentB);
        Check(nextOuter.Active(), "published B should be used by the next outer scope");
        Check(fixture.table.entries[0] != exactBefore[0], "next outer should install a clone");
    }

    void Case_ExceptionRestoresExactEntryTable()
    {
        Fixture fixture;
        fixture.table.entries[0] =
            fixture.backend.AddOriginal(MakeDescriptor(kFilterAnisotropic, 0.0F, 30));
        fixture.table.entries[19] =
            fixture.backend.AddOriginal(MakeDescriptor(kFilterAnisotropic, -0.25F, 31));
        const auto exactBefore = fixture.table.entries;
        const auto ticket = fixture.controller.Refresh(fixture.Device(), fixture.Intent());

        try {
            SamplerGenerationController::Scope scope(
                fixture.controller, ticket, fixture.Device(), fixture.Intent());
            Check(scope.Active(), "scope should be active before throwing work");
            throw std::runtime_error("injected engine draw exception");
        } catch (const std::runtime_error&) {
        }

        CheckEqual(fixture.controller.Depth(), std::size_t{ 0 }, "unwinding must close the scope");
        CheckEqual(fixture.table.entries, exactBefore, "unwinding must restore all 320 exact identities");
    }

    void Case_WrongThreadCannotMutateDepth()
    {
        Fixture fixture;
        fixture.table.entries[0] =
            fixture.backend.AddOriginal(MakeDescriptor(kFilterAnisotropic, 0.0F, 40));
        const auto exactBefore = fixture.table.entries;
        const auto ticket = fixture.controller.Refresh(fixture.Device(), fixture.Intent());
        const auto publication = fixture.controller.PublishedId();
        bool wrongThreadScopeActive = true;
        bool wrongThreadRefreshUsable = true;
        bool wrongThreadClear = true;
        std::size_t wrongThreadDepth = 99;
        std::uint64_t wrongThreadPublishedId = 99;

        std::thread worker([&]() {
            SamplerGenerationController::Scope scope(
                fixture.controller, ticket, fixture.Device(), fixture.Intent());
            wrongThreadScopeActive = scope.Active();
            wrongThreadRefreshUsable =
                fixture.controller.Refresh(fixture.Device(2), fixture.Intent()).Usable();
            wrongThreadClear = fixture.controller.Clear();
            wrongThreadPublishedId = fixture.controller.PublishedId();
            wrongThreadDepth = fixture.controller.Depth();
        });
        worker.join();

        Check(!wrongThreadScopeActive, "wrong-thread entry must be rejected");
        Check(!wrongThreadRefreshUsable, "wrong-thread refresh must be rejected");
        Check(!wrongThreadClear, "wrong-thread clear must be rejected");
        CheckEqual(wrongThreadPublishedId, std::uint64_t{ 0 },
                   "wrong-thread PublishedId must not load shared generation ownership");
        CheckEqual(wrongThreadDepth, std::size_t{ 0 },
                   "wrong-thread Depth must return zero without reading render-owned depth");
        CheckEqual(fixture.controller.Depth(), std::size_t{ 0 }, "wrong thread must not mutate depth");
        CheckEqual(fixture.controller.PublishedId(), publication,
                   "wrong thread must not change the atomic publication");
        CheckEqual(fixture.table.entries, exactBefore, "wrong thread must not write the table");
    }

    void Case_AppliedDisabledOrRequestedOnlyChangeCannotEnter()
    {
        Fixture fixture;
        fixture.table.entries[0] =
            fixture.backend.AddOriginal(MakeDescriptor(kFilterAnisotropic, 0.0F, 50));
        const auto exactBefore = fixture.table.entries;
        const auto applied = fixture.Intent(-1.0F, 10);
        const auto ticket = fixture.controller.Refresh(fixture.Device(), applied);
        Check(ticket.Usable(), "active applied intent should build");

        auto disabled = applied;
        disabled.active = false;
        SamplerGenerationController::Scope disabledScope(
            fixture.controller, ticket, fixture.Device(), disabled);
        Check(!disabledScope.Active(), "disabled applied state must never enter");

        auto requestedOnly = applied;
        requestedOnly.bias = -2.0F;
        SamplerGenerationController::Scope requestedScope(
            fixture.controller, ticket, fixture.Device(), requestedOnly);
        Check(!requestedScope.Active(), "requested-only bias change must not enter as applied state");

        auto requestedCommit = applied;
        ++requestedCommit.appliedCommit;
        SamplerGenerationController::Scope uncommittedScope(
            fixture.controller, ticket, fixture.Device(), requestedCommit);
        Check(!uncommittedScope.Active(), "unseen applied commit must not enter");
        CheckEqual(fixture.controller.Depth(), std::size_t{ 0 }, "rejected intents must not change depth");
        CheckEqual(fixture.table.entries, exactBefore, "rejected intents must not write the table");

        const auto disabledTicket = fixture.controller.Refresh(fixture.Device(), disabled);
        Check(!disabledTicket.Usable(), "Refresh of disabled applied state must return no usable ticket");
        SamplerGenerationController::Scope invalidatedOldTicket(
            fixture.controller, ticket, fixture.Device(), applied);
        Check(!invalidatedOldTicket.Active(),
              "disabled Refresh must invalidate an older active ticket for future entry");
    }

    void Case_FactoryFalseNonNullIsFailure()
    {
        Fixture fixture;
        fixture.table.entries[0] =
            fixture.backend.AddOriginal(MakeDescriptor(kFilterAnisotropic, 0.0F, 60));
        fixture.backend.FailCreateAt(1, CreateFailure::kFalseNonNull);
        const auto failed = fixture.controller.Refresh(fixture.Device(), fixture.Intent());
        Check(!failed.Usable(), "factory false+nonnull must fail the candidate");
        CheckEqual(fixture.backend.LiveCloneCount(), 0,
                   "factory false+nonnull output must be released immediately");
        CheckEqual(fixture.controller.PublishedId(), std::uint64_t{ 0 },
                   "factory false+nonnull must publish nothing");
    }

    void Case_FactoryTrueNullIsFailure()
    {
        Fixture fixture;
        fixture.table.entries[0] =
            fixture.backend.AddOriginal(MakeDescriptor(kFilterAnisotropic, 0.0F, 61));
        fixture.backend.FailCreateAt(1, CreateFailure::kTrueNull);
        const auto failed = fixture.controller.Refresh(fixture.Device(), fixture.Intent());
        Check(!failed.Usable(), "factory true+null must fail the candidate");
        CheckEqual(fixture.controller.PublishedId(), std::uint64_t{ 0 },
                   "factory true+null must publish nothing");
        CheckEqual(fixture.backend.releaseUnderflows, 0,
                   "factory true+null cleanup must not release a null handle");
    }

    void Case_FactoryThrowNonNullIsFailure()
    {
        Fixture fixture;
        fixture.table.entries[0] =
            fixture.backend.AddOriginal(MakeDescriptor(kFilterAnisotropic, 0.0F, 62));
        fixture.backend.FailCreateAt(1, CreateFailure::kThrowNonNull);
        const auto failed = fixture.controller.Refresh(fixture.Device(), fixture.Intent());
        Check(!failed.Usable(), "factory throw+nonnull must fail the candidate");
        CheckEqual(fixture.backend.LiveCloneCount(), 0,
                   "factory throw+nonnull output must be released from the local catch");
        CheckEqual(fixture.controller.PublishedId(), std::uint64_t{ 0 },
                   "factory throw+nonnull must publish nothing");
    }

    void Case_TicketDoesNotOwnGenerationOrReleaseOnWorker()
    {
        Fixture fixture;
        fixture.table.entries[0] =
            fixture.backend.AddOriginal(MakeDescriptor(kFilterAnisotropic, 0.0F, 63));
        auto ticket = fixture.controller.Refresh(fixture.Device(), fixture.Intent());
        Check(ticket.Usable(), "ticket should identify the published generation");
        SamplerHandle clone = 0;
        {
            SamplerGenerationController::Scope scope(
                fixture.controller, ticket, fixture.Device(), fixture.Intent());
            Check(scope.Active(), "scope should expose the generated clone identity");
            clone = fixture.table.entries[0];
        }

        Check(fixture.controller.Clear(), "owner Clear should release the published generation");
        CheckEqual(fixture.backend.Get(clone).references, 0,
                   "a live Ticket must not retain generation/backend ownership after Clear");
        const int wrongThreadReleasesBefore = fixture.backend.wrongThreadReleaseCalls;
        std::thread worker([moved = std::move(ticket)]() mutable {
            moved = SamplerGenerationController::Ticket{};
        });
        worker.join();
        CheckEqual(fixture.backend.wrongThreadReleaseCalls, wrongThreadReleasesBefore,
                   "destroying a Ticket on a worker must never invoke backend release");
    }

    void Case_TicketOutlivesControllerAsExpiredNonOwningHandle()
    {
        FakeBackend backend;
        FakeTable table;
        for (std::size_t index = 0; index < kFo4SamplerCount; ++index) {
            table.entries[index] = backend.AddOriginal(
                MakeDescriptor(kFilterPoint, 0.0F, static_cast<std::uint8_t>(index)));
        }
        table.entries[0] = backend.AddOriginal(MakeDescriptor(kFilterAnisotropic, 0.0F, 64));
        SamplerGenerationController::Ticket survivor;
        SamplerHandle clone = 0;
        {
            auto controller = std::make_unique<SamplerGenerationController>(table.Bind(), backend.Bind());
            const SamplerDeviceKey device{ 0xD311, 1 };
            const AppliedSamplerIntent intent{ 80, AppliedSamplerEngine::kDlss, true, -1.0F };
            survivor = controller->Refresh(device, intent);
            SamplerGenerationController::Scope scope(*controller, survivor, device, intent);
            Check(scope.Active(), "scope should enter before controller lifetime ends");
            clone = table.entries[0];
        }

        Check(!survivor.Usable(), "Ticket surviving its controller must become safely expired");
        CheckEqual(backend.Get(clone).references, 0,
                   "controller destruction must release generation despite a surviving Ticket");
        const int releasesBeforeTicketDrop = backend.Get(clone).releaseCalls;
        survivor = SamplerGenerationController::Ticket{};
        CheckEqual(backend.Get(clone).releaseCalls, releasesBeforeTicketDrop,
                   "dropping an expired Ticket must not call the backend");
    }

    void Case_TableChangeBeforeEntryRejectsStaleTicket()
    {
        Fixture fixture;
        fixture.table.entries[0] =
            fixture.backend.AddOriginal(MakeDescriptor(kFilterAnisotropic, 0.0F, 70));
        const auto ticket = fixture.controller.Refresh(fixture.Device(), fixture.Intent());
        Check(ticket.Usable(), "initial ticket should build");
        const auto replacement =
            fixture.backend.AddOriginal(MakeDescriptor(kFilterAnisotropic, 0.0F, 70));
        fixture.table.entries[17] = replacement;
        const auto changedTable = fixture.table.entries;

        SamplerGenerationController::Scope scope(
            fixture.controller, ticket, fixture.Device(), fixture.Intent());
        Check(!scope.Active(), "source-table change before entry must reject the stale ticket");
        CheckEqual(fixture.controller.Depth(), std::size_t{ 0 }, "stale entry must not change depth");
        CheckEqual(fixture.table.entries, changedTable, "stale entry must not write or restore the table");
    }

    void Case_TableChangeDuringEntryRetainRejectsWithoutRollback()
    {
        Fixture fixture;
        fixture.table.entries[1] =
            fixture.backend.AddOriginal(MakeDescriptor(kFilterAnisotropic, 0.0F, 71));
        const auto ticket = fixture.controller.Refresh(fixture.Device(), fixture.Intent());
        Check(ticket.Usable(), "initial ticket should build");

        const auto replacement =
            fixture.backend.AddOriginal(MakeDescriptor(kFilterPoint, 0.0F, 72));
        bool changed = false;
        fixture.backend.onRetain = [&](SamplerHandle) {
            if (!changed) {
                changed = true;
                fixture.table.entries[0] = replacement;
            }
        };
        {
            SamplerGenerationController::Scope scope(
                fixture.controller, ticket, fixture.Device(), fixture.Intent());
            Check(!scope.Active(), "entry must reject a table change that occurs during snapshot retain");
            CheckEqual(fixture.controller.Depth(), std::size_t{ 0 },
                       "rejected retain-time change must not enter a scope");
        }
        Check(changed, "the deterministic retain-time mutation hook must have fired");
        CheckEqual(fixture.table.entries[0], replacement,
                   "rejected entry must not roll back the newer engine-table value");
    }

    void Case_NoOpEntryStoreCannotPublishMixedScope()
    {
        Fixture fixture;
        fixture.table.entries[1] =
            fixture.backend.AddOriginal(MakeDescriptor(kFilterAnisotropic, 0.0F, 73));
        const auto exactBefore = fixture.table.entries;
        const auto ticket = fixture.controller.Refresh(fixture.Device(), fixture.Intent());
        Check(ticket.Usable(), "initial ticket should build");
        fixture.table.skipStoreIndex = 1;
        fixture.table.skipStoreCount = 1;
        {
            SamplerGenerationController::Scope scope(
                fixture.controller, ticket, fixture.Device(), fixture.Intent());
            Check(!scope.Active(), "entry must verify stores and reject a mixed/no-op table");
            CheckEqual(fixture.controller.Depth(), std::size_t{ 0 },
                       "failed post-store verification must not publish scope depth");
        }
        CheckEqual(fixture.table.entries, exactBefore,
                   "failed post-store verification must restore the exact pre-entry table");
    }

    void Case_RestoreFailurePinsUntilOwnerRetry()
    {
        Fixture fixture;
        fixture.table.entries[1] =
            fixture.backend.AddOriginal(MakeDescriptor(kFilterAnisotropic, 0.0F, 74));
        const auto exactBefore = fixture.table.entries;
        std::unique_ptr<SamplerGenerationController::Scope> outer;
        SamplerHandle cloneA = 0;
        {
            const auto intentA = fixture.Intent(-1.0F, 20);
            const auto ticketA = fixture.controller.Refresh(fixture.Device(), intentA);
            outer = std::make_unique<SamplerGenerationController::Scope>(
                fixture.controller, ticketA, fixture.Device(), intentA);
            Check(outer->Active(), "outer A scope should enter");
            cloneA = fixture.table.entries[1];

            const auto intentB = fixture.Intent(-2.0F, 21);
            const auto ticketB = fixture.controller.Refresh(fixture.Device(), intentB);
            Check(ticketB.Usable(), "B should publish while A is pinned");
        }

        fixture.table.throwStoreIndex = 1;
        fixture.table.throwStoreCount = 1;
        outer.reset();
        CheckEqual(fixture.controller.Depth(), std::size_t{ 1 },
                   "failed restore must remain a pending pinned outer scope");
        CheckEqual(fixture.table.entries[1], cloneA,
                   "failed restore should leave the still-owned clone installed until retry");
        Check(fixture.backend.Get(cloneA).references > 0,
              "failed restore must keep the pinned generation clone alive");

        Check(fixture.controller.Clear(), "next owner operation should retry and complete restoration");
        CheckEqual(fixture.controller.Depth(), std::size_t{ 0 },
                   "successful owner retry should close the pending scope");
        CheckEqual(fixture.table.entries, exactBefore,
                   "successful owner retry should restore all exact entry identities");
        CheckEqual(fixture.backend.Get(cloneA).references, 0,
                   "A clone should release only after verified restoration");
    }

    void Case_RestoreRecoveryRejectsCallbackReentrancy()
    {
        Fixture fixture;
        fixture.table.entries[1] =
            fixture.backend.AddOriginal(MakeDescriptor(kFilterAnisotropic, 0.0F, 79));
        const auto exactBefore = fixture.table.entries;
        std::unique_ptr<SamplerGenerationController::Scope> outer;
        {
            const auto intentA = fixture.Intent(-1.0F, 40);
            const auto ticketA = fixture.controller.Refresh(fixture.Device(), intentA);
            outer = std::make_unique<SamplerGenerationController::Scope>(
                fixture.controller, ticketA, fixture.Device(), intentA);
            const auto intentB = fixture.Intent(-2.0F, 41);
            const auto ticketB = fixture.controller.Refresh(fixture.Device(), intentB);
            Check(outer->Active() && ticketB.Usable(), "A must be pinned while B is published");
        }
        fixture.table.throwStoreIndex = 1;
        fixture.table.throwStoreCount = 1;
        outer.reset();
        CheckEqual(fixture.controller.Depth(), std::size_t{ 1 }, "restore failure should be pending");

        bool reentered = false;
        bool innerClearResult = true;
        fixture.table.onLoad = [&](std::size_t) {
            if (!reentered) {
                reentered = true;
                innerClearResult = fixture.controller.Clear();
            }
        };
        const bool outerClearResult = fixture.controller.Clear();
        fixture.table.onLoad = {};
        Check(reentered, "restore verification must exercise the deterministic reentrant callback");
        Check(!innerClearResult, "a controller action reentered during recovery must fail without mutation");
        Check(outerClearResult, "the original owner recovery should complete successfully");
        CheckEqual(fixture.controller.Depth(), std::size_t{ 0 }, "original recovery should close depth");
        CheckEqual(fixture.table.entries, exactBefore, "original recovery should restore exact identities");
    }

    void Case_NormalRestoreRejectsReentrantBeginRefreshClear()
    {
        Fixture fixture;
        fixture.table.entries[1] =
            fixture.backend.AddOriginal(MakeDescriptor(kFilterAnisotropic, 0.0F, 81));
        const auto exactBefore = fixture.table.entries;
        const auto intent = fixture.Intent(-1.0F, 70);
        const auto ticket = fixture.controller.Refresh(fixture.Device(), intent);
        Check(ticket.Usable(), "initial generation should build");
        const auto publication = ticket.PublicationId();
        bool callbackFired = false;
        bool reentrantRefreshUsable = true;
        bool reentrantClearResult = true;
        std::unique_ptr<SamplerGenerationController::Scope> reentrantNested;

        {
            SamplerGenerationController::Scope outer(
                fixture.controller, ticket, fixture.Device(), intent);
            Check(outer.Active(), "outer scope should enter before normal restoration");
            fixture.table.onStore = [&](std::size_t, SamplerHandle) {
                if (callbackFired) {
                    return;
                }
                callbackFired = true;
                reentrantNested = std::make_unique<SamplerGenerationController::Scope>(
                    fixture.controller, ticket, fixture.Device(), intent);
                reentrantRefreshUsable =
                    fixture.controller.Refresh(fixture.Device(), intent).Usable();
                reentrantClearResult = fixture.controller.Clear();
            };
        }
        fixture.table.onStore = {};

        Check(callbackFired, "normal outer restore must execute the deterministic store callback");
        Check(reentrantNested != nullptr && !reentrantNested->Active(),
              "BeginScope reentered during restore must not create a nested orphan");
        Check(!reentrantRefreshUsable, "Refresh reentered during restore must fail without mutation");
        Check(!reentrantClearResult, "Clear reentered during restore must fail without mutation");
        CheckEqual(fixture.controller.PublishedId(), publication,
                   "reentrant restore callbacks must not replace or clear publication");
        CheckEqual(fixture.controller.Depth(), std::size_t{ 0 },
                   "normal restore must finish at depth zero with no orphan token");
        CheckEqual(fixture.table.entries, exactBefore,
                   "normal restore must preserve the exact outer snapshot");
        reentrantNested.reset();
    }

    void Case_LateLoadMutationRejectsEntryAndRestoresExactTable()
    {
        Fixture fixture;
        fixture.table.entries[1] =
            fixture.backend.AddOriginal(MakeDescriptor(kFilterAnisotropic, 0.0F, 84));
        const auto exactBefore = fixture.table.entries;
        const auto intent = fixture.Intent(-1.0F, 73);
        const auto ticket = fixture.controller.Refresh(fixture.Device(), intent);
        Check(ticket.Usable(), "initial generation should build");

        bool callbackFired = false;
        SamplerHandle installedClone = 0;
        fixture.table.onLoad = [&](std::size_t index) {
            if (!callbackFired && index == kFo4SamplerCount - 1U &&
                fixture.table.entries[1] != exactBefore[1]) {
                callbackFired = true;
                installedClone = fixture.table.entries[1];
                fixture.table.Mutate(1, exactBefore[1]);
            }
        };
        {
            SamplerGenerationController::Scope scope(
                fixture.controller, ticket, fixture.Device(), intent);
            Check(callbackFired,
                  "post-store verification must execute the deterministic late-load mutation");
            Check(!scope.Active(),
                  "entry must fail when a later load callback changes an already-checked slot");
            CheckEqual(fixture.controller.Depth(), std::size_t{ 0 },
                       "rejected unstable verification must recover the provisional outer depth");
            CheckEqual(fixture.table.entries, exactBefore,
                       "rejected unstable verification must restore the exact entry table");
        }
        fixture.table.onLoad = {};
        Check(installedClone != 0 && installedClone != exactBefore[1],
              "the mutation hook must have observed the installed clone before restoring the source");
    }

    void Case_LateLoadMutationDuringRestoreKeepsOwnershipUntilRetry()
    {
        Fixture fixture;
        fixture.table.entries[1] =
            fixture.backend.AddOriginal(MakeDescriptor(kFilterAnisotropic, 0.0F, 85));
        const auto exactBefore = fixture.table.entries;
        const auto intentA = fixture.Intent(-1.0F, 74);
        const auto ticketA = fixture.controller.Refresh(fixture.Device(), intentA);
        auto outer = std::make_unique<SamplerGenerationController::Scope>(
            fixture.controller, ticketA, fixture.Device(), intentA);
        Check(outer->Active(), "outer A scope should enter");
        const SamplerHandle cloneA = fixture.table.entries[1];

        const auto intentB = fixture.Intent(-2.0F, 75);
        const auto ticketB = fixture.controller.Refresh(fixture.Device(), intentB);
        Check(ticketB.Usable(), "B should publish while A remains pinned");

        bool callbackFired = false;
        fixture.table.onLoad = [&](std::size_t index) {
            if (!callbackFired && index == kFo4SamplerCount - 1U &&
                fixture.table.entries[1] == exactBefore[1]) {
                callbackFired = true;
                fixture.table.Mutate(1, cloneA);
            }
        };
        outer.reset();
        fixture.table.onLoad = {};

        Check(callbackFired,
              "restore verification must execute the deterministic late-load mutation");
        CheckEqual(fixture.controller.Depth(), std::size_t{ 1 },
                   "unstable restore must remain an abandoned pinned outer scope");
        CheckEqual(fixture.table.entries[1], cloneA,
                   "the failed restore must leave the observed clone identity unchanged until retry");
        Check(fixture.backend.Get(cloneA).references > 0,
              "unstable restore must retain the old pinned generation while its clone remains installed");

        Check(fixture.controller.Clear(), "the next owner operation should retry the exact restore");
        CheckEqual(fixture.controller.Depth(), std::size_t{ 0 },
                   "successful owner retry should close the abandoned scope");
        CheckEqual(fixture.table.entries, exactBefore,
                   "successful owner retry must restore all exact source identities");
        CheckEqual(fixture.backend.Get(cloneA).references, 0,
                   "the old clone may release only after a stable exact restore");
        CheckEqual(fixture.backend.releaseUnderflows, 0,
                   "unstable verification and retry must not over-release ownership");
    }

    void Case_ReleaseClearReentryInvalidatesOuterTicket()
    {
        Fixture fixture;
        fixture.table.entries[0] =
            fixture.backend.AddOriginal(MakeDescriptor(kFilterAnisotropic, 0.0F, 86));
        const auto intentA = fixture.Intent(-1.0F, 76);
        const auto ticketA = fixture.controller.Refresh(fixture.Device(), intentA);
        Check(ticketA.Usable(), "initial A generation should build");

        bool callbackFired = false;
        bool clearResult = false;
        fixture.backend.onRelease = [&](SamplerHandle) {
            if (!callbackFired) {
                callbackFired = true;
                clearResult = fixture.controller.Clear();
            }
        };
        const auto outer = fixture.controller.Refresh(fixture.Device(), fixture.Intent(-2.0F, 77));
        fixture.backend.onRelease = {};

        Check(callbackFired,
              "replacing A must execute its release callback before Refresh returns to the caller");
        Check(clearResult, "release-callback Clear should remain a valid nested owner operation");
        Check(!outer.Usable(),
              "outer Refresh must not return a usable ticket after release reentry cleared publication");
        CheckEqual(fixture.controller.PublishedId(), std::uint64_t{ 0 },
                   "release-callback Clear must leave publication coherently cleared");
    }

    void Case_ReleaseRefreshReentrySupersedesOuterTicket()
    {
        Fixture fixture;
        fixture.table.entries[0] =
            fixture.backend.AddOriginal(MakeDescriptor(kFilterAnisotropic, 0.0F, 87));
        const auto ticketA = fixture.controller.Refresh(fixture.Device(), fixture.Intent(-1.0F, 78));
        Check(ticketA.Usable(), "initial A generation should build");

        bool callbackFired = false;
        SamplerGenerationController::Ticket nested;
        fixture.backend.onRelease = [&](SamplerHandle) {
            if (!callbackFired) {
                callbackFired = true;
                nested = fixture.controller.Refresh(fixture.Device(), fixture.Intent(-2.5F, 80));
            }
        };
        const auto outer = fixture.controller.Refresh(fixture.Device(), fixture.Intent(-2.0F, 79));
        fixture.backend.onRelease = {};

        Check(callbackFired,
              "replacing A must allow the deterministic release-callback Refresh reentry");
        Check(nested.Usable(), "nested release-callback Refresh should publish its newer generation");
        Check(!outer.Usable(),
              "outer Refresh must not return a usable ticket after release reentry superseded it");
        CheckEqual(fixture.controller.PublishedId(), nested.PublicationId(),
                   "the nested release-callback publication must remain the current generation");
    }

    void Case_MakeCallbackClearInvalidatesSuspendedRefresh()
    {
        Fixture fixture;
        fixture.table.entries[0] =
            fixture.backend.AddOriginal(MakeDescriptor(kFilterAnisotropic, 0.0F, 88));
        bool callbackFired = false;
        bool clearResult = false;
        fixture.backend.onMakeBiasedDescriptor = [&](const SamplerDescriptor&, float) {
            if (!callbackFired) {
                callbackFired = true;
                clearResult = fixture.controller.Clear();
            }
        };

        const auto outer = fixture.controller.Refresh(fixture.Device(), fixture.Intent(-1.0F, 81));
        fixture.backend.onMakeBiasedDescriptor = {};
        Check(callbackFired, "descriptor transformation must execute the deterministic Clear callback");
        Check(clearResult, "descriptor-callback Clear should remain a valid nested owner operation");
        Check(!outer.Usable(), "suspended Refresh must fail after descriptor-callback Clear");
        CheckEqual(fixture.controller.PublishedId(), std::uint64_t{ 0 },
                   "descriptor-callback Clear must leave no stale publication");
    }

    void Case_CreateCallbackRefreshSupersedesSuspendedRefresh()
    {
        Fixture fixture;
        fixture.table.entries[0] =
            fixture.backend.AddOriginal(MakeDescriptor(kFilterAnisotropic, 0.0F, 89));
        bool callbackFired = false;
        SamplerGenerationController::Ticket nested;
        fixture.backend.onCreate = [&](SamplerHandle, const SamplerDescriptor&) {
            if (!callbackFired) {
                callbackFired = true;
                nested = fixture.controller.Refresh(fixture.Device(), fixture.Intent(-2.0F, 83));
            }
        };

        const auto outer = fixture.controller.Refresh(fixture.Device(), fixture.Intent(-1.0F, 82));
        fixture.backend.onCreate = {};
        Check(callbackFired, "factory creation must execute the deterministic nested Refresh callback");
        Check(nested.Usable(), "factory-callback Refresh should publish its newer generation");
        Check(!outer.Usable(), "suspended outer Refresh must fail after factory-callback publication");
        CheckEqual(fixture.controller.PublishedId(), nested.PublicationId(),
                   "factory-callback publication must remain current after the outer attempt fails");
    }

    void Case_DepthSevenCallbackCannotEnterCompetingEighthScope()
    {
        Fixture fixture;
        fixture.table.entries[1] =
            fixture.backend.AddOriginal(MakeDescriptor(kFilterAnisotropic, 0.0F, 82));
        const auto exactBefore = fixture.table.entries;
        const auto intent = fixture.Intent(-1.0F, 71);
        const auto ticket = fixture.controller.Refresh(fixture.Device(), intent);
        Check(ticket.Usable(), "initial generation should build");

        std::vector<std::unique_ptr<SamplerGenerationController::Scope>> guards;
        for (std::size_t index = 0; index < Platform::kMaxSamplerScopeDepth - 1U; ++index) {
            guards.push_back(std::make_unique<SamplerGenerationController::Scope>(
                fixture.controller, ticket, fixture.Device(), intent));
            Check(guards.back()->Active(), "the first seven scope levels should enter");
        }
        CheckEqual(fixture.controller.Depth(), Platform::kMaxSamplerScopeDepth - 1U,
                   "seven guards should leave exactly one fixed slot available");

        bool callbackFired = false;
        bool emergencyClear = false;
        std::unique_ptr<SamplerGenerationController::Scope> competingEighth;
        fixture.table.onLoad = [&](std::size_t) {
            if (callbackFired) {
                return;
            }
            callbackFired = true;
            competingEighth = std::make_unique<SamplerGenerationController::Scope>(
                fixture.controller, ticket, fixture.Device(), intent);
            if (competingEighth->Active()) {
                emergencyClear = fixture.controller.Clear();
            }
        };
        auto suspendedEighth = std::make_unique<SamplerGenerationController::Scope>(
            fixture.controller, ticket, fixture.Device(), intent);
        fixture.table.onLoad = {};

        Check(callbackFired, "depth-seven validation must execute the deterministic callback");
        Check(competingEighth != nullptr && !competingEighth->Active(),
              "callback reentry must not consume the final fixed scope slot");
        Check(!emergencyClear, "owner-operation reentry must reject the emergency Clear");
        Check(suspendedEighth->Active(),
              "the original depth-seven entry should safely consume the eighth slot");
        CheckEqual(fixture.controller.Depth(), Platform::kMaxSamplerScopeDepth,
                   "reentrant validation must never create a ninth logical depth");

        competingEighth.reset();
        suspendedEighth.reset();
        while (!guards.empty()) {
            guards.pop_back();
        }
        CheckEqual(fixture.controller.Depth(), std::size_t{ 0 },
                   "safe reverse unwind should close all fixed levels");
        CheckEqual(fixture.table.entries, exactBefore,
                   "depth-boundary reentrancy must preserve the exact outer snapshot");
    }

    void Case_CallbackClosingOuterMakesSuspendedNestedEntryFailClosed()
    {
        Fixture fixture;
        fixture.table.entries[0] =
            fixture.backend.AddOriginal(MakeDescriptor(kFilterAnisotropic, 0.0F, 83));
        const auto exactBefore = fixture.table.entries;
        const auto intent = fixture.Intent(-1.0F, 72);
        const auto ticket = fixture.controller.Refresh(fixture.Device(), intent);
        auto outer = std::make_unique<SamplerGenerationController::Scope>(
            fixture.controller, ticket, fixture.Device(), intent);
        Check(outer->Active(), "outer scope should enter before nested validation");

        bool callbackFired = false;
        fixture.table.onLoad = [&](std::size_t index) {
            if (!callbackFired && index == kFo4SamplerCount - 1U) {
                callbackFired = true;
                outer.reset();
            }
        };
        auto suspendedNested = std::make_unique<SamplerGenerationController::Scope>(
            fixture.controller, ticket, fixture.Device(), intent);
        fixture.table.onLoad = {};

        Check(callbackFired, "nested validation must execute the last-slot close callback");
        Check(!suspendedNested->Active(),
              "nested entry must fail when its validated outer scope was closed by a callback");
        CheckEqual(fixture.controller.Depth(), std::size_t{ 0 },
                   "closing the validated outer must leave no orphan logical depth");
        CheckEqual(fixture.table.entries, exactBefore,
                   "fail-closed recovery must restore the exact outer snapshot");
        suspendedNested.reset();
        CheckEqual(fixture.table.entries, exactBefore,
                   "destroying the rejected nested guard must not replay a cleared snapshot");
    }

    void Case_WrongThreadScopeDestructionRecoversOnOwner()
    {
        Fixture fixture;
        fixture.table.entries[1] =
            fixture.backend.AddOriginal(MakeDescriptor(kFilterAnisotropic, 0.0F, 75));
        const auto exactBefore = fixture.table.entries;
        const auto ticket = fixture.controller.Refresh(fixture.Device(), fixture.Intent());
        auto outer = std::make_unique<SamplerGenerationController::Scope>(
            fixture.controller, ticket, fixture.Device(), fixture.Intent());
        Check(outer->Active(), "owner-thread outer scope should enter");
        const auto overridden = fixture.table.entries;

        std::thread worker([scope = std::move(outer)]() mutable { scope.reset(); });
        worker.join();
        CheckEqual(fixture.controller.Depth(), std::size_t{ 1 },
                   "wrong-thread destruction must not directly mutate render-owned depth");
        CheckEqual(fixture.table.entries, overridden,
                   "wrong-thread destruction must not directly mutate the render table");

        Check(fixture.controller.Clear(), "next owner operation should consume the abandoned guard");
        CheckEqual(fixture.controller.Depth(), std::size_t{ 0 },
                   "owner recovery should close the abandoned outer guard");
        CheckEqual(fixture.table.entries, exactBefore,
                   "owner recovery should restore the exact table");
    }

    void Case_OutOfOrderScopeDestructionCollapsesWhenTopCloses()
    {
        Fixture fixture;
        fixture.table.entries[1] =
            fixture.backend.AddOriginal(MakeDescriptor(kFilterAnisotropic, 0.0F, 76));
        const auto exactBefore = fixture.table.entries;
        const auto ticket = fixture.controller.Refresh(fixture.Device(), fixture.Intent());
        auto outer = std::make_unique<SamplerGenerationController::Scope>(
            fixture.controller, ticket, fixture.Device(), fixture.Intent());
        auto nested = std::make_unique<SamplerGenerationController::Scope>(
            fixture.controller, ticket, fixture.Device(), fixture.Intent());
        Check(outer->Active() && nested->Active(), "outer and nested guards should enter");
        outer.reset();
        CheckEqual(fixture.controller.Depth(), std::size_t{ 2 },
                   "closing a non-top guard must wait for LIFO collapse");
        nested.reset();
        CheckEqual(fixture.controller.Depth(), std::size_t{ 0 },
                   "closing the top must collapse the already-closed outer guard");
        CheckEqual(fixture.table.entries, exactBefore,
                   "collapsed abandoned guards must restore the exact outer table");
    }

    void Case_FixedDepthEightRejectsNinthAndUnwindsExactly()
    {
        Fixture fixture;
        fixture.table.entries[1] =
            fixture.backend.AddOriginal(MakeDescriptor(kFilterAnisotropic, 0.0F, 77));
        const auto exactBefore = fixture.table.entries;
        const auto ticket = fixture.controller.Refresh(fixture.Device(), fixture.Intent());
        std::vector<std::unique_ptr<SamplerGenerationController::Scope>> guards;
        for (std::size_t index = 0; index < Platform::kMaxSamplerScopeDepth; ++index) {
            guards.push_back(std::make_unique<SamplerGenerationController::Scope>(
                fixture.controller, ticket, fixture.Device(), fixture.Intent()));
            Check(guards.back()->Active(), "each of the fixed eight scope levels should enter");
        }
        CheckEqual(fixture.controller.Depth(), Platform::kMaxSamplerScopeDepth,
                   "eight guards should occupy the fixed stack");
        auto ninth = std::make_unique<SamplerGenerationController::Scope>(
            fixture.controller, ticket, fixture.Device(), fixture.Intent());
        Check(!ninth->Active(), "ninth nested guard must be rejected");
        while (!guards.empty()) {
            guards.pop_back();
        }
        CheckEqual(fixture.controller.Depth(), std::size_t{ 0 }, "reverse unwind should close all levels");
        CheckEqual(fixture.table.entries, exactBefore, "depth-eight unwind should restore the exact table");
    }

    void Case_NestedCallbackReentrancyCannotEnterAfterClear()
    {
        Fixture fixture;
        fixture.table.entries[1] =
            fixture.backend.AddOriginal(MakeDescriptor(kFilterAnisotropic, 0.0F, 78));
        const auto intentA = fixture.Intent(-1.0F, 30);
        const auto ticketA = fixture.controller.Refresh(fixture.Device(), intentA);
        SamplerGenerationController::Scope outer(
            fixture.controller, ticketA, fixture.Device(), intentA);
        Check(outer.Active(), "outer A scope should enter");
        const auto intentB = fixture.Intent(-2.0F, 31);
        const auto ticketB = fixture.controller.Refresh(fixture.Device(), intentB);
        Check(ticketB.Usable(), "B should publish while A is pinned");

        bool cleared = false;
        fixture.table.onLoad = [&](std::size_t) {
            if (!cleared) {
                cleared = true;
                (void)fixture.controller.Clear();
            }
        };
        SamplerGenerationController::Scope nested(
            fixture.controller, ticketB, fixture.Device(), intentB);
        fixture.table.onLoad = {};
        Check(cleared, "the deterministic nested-load reentrancy hook must have cleared publication");
        Check(!nested.Active(), "nested entry must recheck publication/serial after callback-backed loads");
        CheckEqual(fixture.controller.Depth(), std::size_t{ 1 },
                   "reentrant clear must not add a nested depth");
    }

    void Case_InvalidBiasNeverBuildsOrEnters()
    {
        Fixture fixture;
        fixture.table.entries[0] =
            fixture.backend.AddOriginal(MakeDescriptor(kFilterAnisotropic, 0.0F, 80));
        const std::array<float, 5> invalidBiases{
            0.0F,
            -0.0F,
            0.25F,
            -3.25F,
            std::numeric_limits<float>::infinity(),
        };
        for (std::size_t index = 0; index < invalidBiases.size(); ++index) {
            const auto intent = fixture.Intent(invalidBiases[index], index + 1);
            const auto ticket = fixture.controller.Refresh(fixture.Device(), intent);
            Check(!ticket.Usable(), "bias outside finite [-3, 0) must not build");
        }
        CheckEqual(fixture.backend.createCalls, 0, "invalid bias must never call the factory");

        const auto valid = fixture.controller.Refresh(fixture.Device(), fixture.Intent(-3.0F, 10));
        Check(valid.Usable(), "inclusive -3 lower bound should be valid");
    }

    struct TestCase
    {
        const char* name;
        void (*function)();
    };
}

int main()
{
    static_assert(kFo4SamplerCount == 320);
    static_assert(kSamplerDescriptorBytes == 52);
    static_assert(!std::is_copy_constructible_v<SamplerGenerationController::Scope>);
    static_assert(!std::is_move_constructible_v<SamplerGenerationController::Scope>);
    static_assert(std::is_same_v<decltype(SamplerTableCallbacks::mutationSerial),
                                 const std::atomic<std::uint64_t>*>);
    static_assert(noexcept(std::declval<const std::atomic<std::uint64_t>&>().load(
        std::memory_order_acquire)));

    const TestCase cases[]{
        { "SameBiasChangedSourceRebuilds", &Case_SameBiasChangedSourceRebuilds },
        { "SameBiasChangedDescriptorRebuilds", &Case_SameBiasChangedDescriptorRebuilds },
        { "SameBiasNewDeviceRebuilds", &Case_SameBiasNewDeviceRebuilds },
        { "CloneFailureDoesNotPublishPartial", &Case_CloneFailureDoesNotPublishPartial },
        { "SameKeyAfterFailureRetries", &Case_SameKeyAfterFailureRetries },
        { "SameKeyReuseAvoidsOwnershipChurn", &Case_SameKeyReuseAvoidsOwnershipChurn },
        { "SameKeyCallbackFailedRefreshCannotReturnStaleTicket",
            &Case_SameKeyCallbackFailedRefreshCannotReturnStaleTicket },
        { "FinalPreCasCallbackFailedRefreshCannotPublishStaleCandidate",
            &Case_FinalPreCasCallbackFailedRefreshCannotPublishStaleCandidate },
        { "AtomicMutationSerialFinalizesReplacementAcrossPriorTeardown",
            &Case_AtomicMutationSerialFinalizesReplacementAcrossPriorTeardown },
        { "NewSourceIsRetainedBeforeDescriptorCapture",
            &Case_NewSourceIsRetainedBeforeDescriptorCapture },
        { "SwapBalancesOwnership", &Case_SwapBalancesOwnership },
        { "NestedScopesRestoreOutermostOnly", &Case_NestedScopesRestoreOutermostOnly },
        { "ExceptionRestoresExactEntryTable", &Case_ExceptionRestoresExactEntryTable },
        { "WrongThreadCannotMutateDepth", &Case_WrongThreadCannotMutateDepth },
        { "AppliedDisabledOrRequestedOnlyChangeCannotEnter",
            &Case_AppliedDisabledOrRequestedOnlyChangeCannotEnter },
        { "FactoryFalseNonNullIsFailure", &Case_FactoryFalseNonNullIsFailure },
        { "FactoryTrueNullIsFailure", &Case_FactoryTrueNullIsFailure },
        { "FactoryThrowNonNullIsFailure", &Case_FactoryThrowNonNullIsFailure },
        { "TicketDoesNotOwnGenerationOrReleaseOnWorker",
            &Case_TicketDoesNotOwnGenerationOrReleaseOnWorker },
        { "TicketOutlivesControllerAsExpiredNonOwningHandle",
            &Case_TicketOutlivesControllerAsExpiredNonOwningHandle },
        { "TableChangeBeforeEntryRejectsStaleTicket", &Case_TableChangeBeforeEntryRejectsStaleTicket },
        { "TableChangeDuringEntryRetainRejectsWithoutRollback",
            &Case_TableChangeDuringEntryRetainRejectsWithoutRollback },
        { "NoOpEntryStoreCannotPublishMixedScope", &Case_NoOpEntryStoreCannotPublishMixedScope },
        { "RestoreFailurePinsUntilOwnerRetry", &Case_RestoreFailurePinsUntilOwnerRetry },
        { "RestoreRecoveryRejectsCallbackReentrancy",
            &Case_RestoreRecoveryRejectsCallbackReentrancy },
        { "NormalRestoreRejectsReentrantBeginRefreshClear",
            &Case_NormalRestoreRejectsReentrantBeginRefreshClear },
        { "LateLoadMutationRejectsEntryAndRestoresExactTable",
            &Case_LateLoadMutationRejectsEntryAndRestoresExactTable },
        { "LateLoadMutationDuringRestoreKeepsOwnershipUntilRetry",
            &Case_LateLoadMutationDuringRestoreKeepsOwnershipUntilRetry },
        { "ReleaseClearReentryInvalidatesOuterTicket",
            &Case_ReleaseClearReentryInvalidatesOuterTicket },
        { "ReleaseRefreshReentrySupersedesOuterTicket",
            &Case_ReleaseRefreshReentrySupersedesOuterTicket },
        { "MakeCallbackClearInvalidatesSuspendedRefresh",
            &Case_MakeCallbackClearInvalidatesSuspendedRefresh },
        { "CreateCallbackRefreshSupersedesSuspendedRefresh",
            &Case_CreateCallbackRefreshSupersedesSuspendedRefresh },
        { "DepthSevenCallbackCannotEnterCompetingEighthScope",
            &Case_DepthSevenCallbackCannotEnterCompetingEighthScope },
        { "CallbackClosingOuterMakesSuspendedNestedEntryFailClosed",
            &Case_CallbackClosingOuterMakesSuspendedNestedEntryFailClosed },
        { "WrongThreadScopeDestructionRecoversOnOwner",
            &Case_WrongThreadScopeDestructionRecoversOnOwner },
        { "OutOfOrderScopeDestructionCollapsesWhenTopCloses",
            &Case_OutOfOrderScopeDestructionCollapsesWhenTopCloses },
        { "FixedDepthEightRejectsNinthAndUnwindsExactly",
            &Case_FixedDepthEightRejectsNinthAndUnwindsExactly },
        { "NestedCallbackReentrancyCannotEnterAfterClear",
            &Case_NestedCallbackReentrancyCannotEnterAfterClear },
        { "InvalidBiasNeverBuildsOrEnters", &Case_InvalidBiasNeverBuildsOrEnters },
    };

    std::vector<CaseResult> results;
    results.reserve(std::size(cases));
    for (const auto& test : cases) {
        CaseResult result;
        result.name = test.name;
        g_active = &result;
        try {
            test.function();
        } catch (const std::exception& error) {
            ++result.failures;
            result.messages.emplace_back(std::string("unhandled exception: ") + error.what());
        } catch (...) {
            ++result.failures;
            result.messages.emplace_back("unhandled non-standard exception");
        }
        g_active = nullptr;
        results.push_back(std::move(result));
    }

    int failedCases = 0;
    for (const auto& result : results) {
        const bool passed = result.failures == 0;
        std::cout << (passed ? "[PASS] " : "[FAIL] ") << result.name << " ("
                  << (result.checks - result.failures) << "/" << result.checks << " checks)\n";
        for (const auto& message : result.messages) {
            std::cout << "    - " << message << '\n';
        }
        if (!passed) {
            ++failedCases;
        }
    }
    std::cout << failedCases << " of " << std::size(cases) << " sampler-generation cases failed\n";
    return failedCases == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
