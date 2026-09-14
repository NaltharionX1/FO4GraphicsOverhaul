#include "Platform/AaEngineResolution.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <type_traits>

namespace {

std::uint64_t g_allocationCount = 0;

struct TestHarness {
    int checks = 0;
    int failures = 0;

    void Check(const bool condition, const char* const expression, const int line) noexcept
    {
        ++checks;
        if (!condition) {
            ++failures;
            std::printf("FAIL line %d: %s\n", line, expression);
        }
    }
};

#define CHECK(harness, expression) (harness).Check((expression), #expression, __LINE__)

using Platform::AaAvailability;
using Platform::AaCapabilityReduceOutcome;
using Platform::AaCapabilityState;
using Platform::AaDeviceGeneration;
using Platform::AaEffectiveEngine;
using Platform::AaEngineCapabilities;
using Platform::AaEngineRequest;
using Platform::AaResolutionReason;

constexpr AaEngineCapabilities UnknownCapabilities() noexcept
{
    return {};
}

constexpr AaEngineCapabilities Capabilities(
    const std::uint64_t identity,
    const std::uint64_t epoch,
    const AaCapabilityState state,
    const AaAvailability dlss,
    const AaAvailability fsr) noexcept
{
    return { AaDeviceGeneration{ identity, epoch }, state, dlss, fsr };
}

constexpr AaEngineCapabilities Resolved(
    const std::uint64_t identity,
    const std::uint64_t epoch,
    const AaAvailability dlss,
    const AaAvailability fsr) noexcept
{
    return Capabilities(identity, epoch, AaCapabilityState::kResolved, dlss, fsr);
}

constexpr AaEngineCapabilities Indeterminate(const std::uint64_t identity, const std::uint64_t epoch) noexcept
{
    return Capabilities(
        identity,
        epoch,
        AaCapabilityState::kIndeterminate,
        AaAvailability::kUnknown,
        AaAvailability::kUnknown);
}

struct TruthRow {
    AaEngineCapabilities capabilities;
    AaEngineRequest requested;
    AaEffectiveEngine effective;
    AaResolutionReason reason;
};

void Case_FullTruthTable(TestHarness& harness) noexcept
{
    constexpr auto unknown = UnknownCapabilities();
    constexpr auto indeterminate = Indeterminate(0xA11CEU, 7U);
    constexpr auto available = AaAvailability::kAvailable;
    constexpr auto unavailable = AaAvailability::kUnavailable;

    constexpr TruthRow rows[]{
        { unknown, AaEngineRequest::kDlss, AaEffectiveEngine::kUnresolved, AaResolutionReason::kCapabilitiesPending },
        { unknown, AaEngineRequest::kFsr, AaEffectiveEngine::kUnresolved, AaResolutionReason::kCapabilitiesPending },
        { indeterminate, AaEngineRequest::kDlss, AaEffectiveEngine::kNone, AaResolutionReason::kCapabilityIndeterminate },
        { indeterminate, AaEngineRequest::kFsr, AaEffectiveEngine::kNone, AaResolutionReason::kCapabilityIndeterminate },

        { Resolved(1U, 1U, unavailable, unavailable), AaEngineRequest::kDlss, AaEffectiveEngine::kNone, AaResolutionReason::kNoUsableEngine },
        { Resolved(1U, 1U, unavailable, unavailable), AaEngineRequest::kFsr, AaEffectiveEngine::kNone, AaResolutionReason::kNoUsableEngine },
        { Resolved(1U, 1U, unavailable, available), AaEngineRequest::kDlss, AaEffectiveEngine::kFsr, AaResolutionReason::kDlssUnavailableFallback },
        { Resolved(1U, 1U, unavailable, available), AaEngineRequest::kFsr, AaEffectiveEngine::kFsr, AaResolutionReason::kRequestedFsr },
        { Resolved(1U, 1U, available, unavailable), AaEngineRequest::kDlss, AaEffectiveEngine::kDlss, AaResolutionReason::kRequestedDlss },
        { Resolved(1U, 1U, available, unavailable), AaEngineRequest::kFsr, AaEffectiveEngine::kNone, AaResolutionReason::kNoUsableEngine },
        { Resolved(1U, 1U, available, available), AaEngineRequest::kDlss, AaEffectiveEngine::kDlss, AaResolutionReason::kRequestedDlss },
        { Resolved(1U, 1U, available, available), AaEngineRequest::kFsr, AaEffectiveEngine::kFsr, AaResolutionReason::kRequestedFsr },

    };

    for (const auto& row : rows) {
        const auto before = row.capabilities;
        const auto actual = Platform::ResolveAaEngine(row.requested, row.capabilities);
        CHECK(harness, actual.requested == row.requested);
        CHECK(harness, actual.effective == row.effective);
        CHECK(harness, actual.reason == row.reason);
        CHECK(harness, row.capabilities == before);
    }
}

void Case_AutomaticFallbackPreservesRequest(TestHarness& harness) noexcept
{
    constexpr auto capabilities = Resolved(
        0xD311CEU,
        41U,
        AaAvailability::kUnavailable,
        AaAvailability::kAvailable);

    const auto result = Platform::ResolveAaEngine(AaEngineRequest::kDlss, capabilities);
    CHECK(harness, result.requested == AaEngineRequest::kDlss);
    CHECK(harness, result.effective == AaEffectiveEngine::kFsr);
    CHECK(harness, result.reason == AaResolutionReason::kDlssUnavailableFallback);

    constexpr auto noReverseFallback = Resolved(
        0xD311CEU,
        42U,
        AaAvailability::kAvailable,
        AaAvailability::kUnavailable);
    const auto fsrResult = Platform::ResolveAaEngine(AaEngineRequest::kFsr, noReverseFallback);
    CHECK(harness, fsrResult.requested == AaEngineRequest::kFsr);
    CHECK(harness, fsrResult.effective == AaEffectiveEngine::kNone);
    CHECK(harness, fsrResult.reason == AaResolutionReason::kNoUsableEngine);
}

void Case_InvalidTuplesFailClosed(TestHarness& harness) noexcept
{
    constexpr auto unknown = AaAvailability::kUnknown;
    constexpr auto available = AaAvailability::kAvailable;
    constexpr auto unavailable = AaAvailability::kUnavailable;
    constexpr AaEngineCapabilities invalid[]{
        Capabilities(0U, 1U, AaCapabilityState::kResolved, available, available),
        Capabilities(1U, 0U, AaCapabilityState::kResolved, available, available),
        Capabilities(1U, 1U, AaCapabilityState::kResolved, unknown, available),
        Capabilities(1U, 1U, AaCapabilityState::kUnknown, unknown, unknown),
        Capabilities(0U, 0U, AaCapabilityState::kUnknown, unavailable, unknown),
        Capabilities(1U, 1U, AaCapabilityState::kIndeterminate, available, unknown),
        Capabilities(1U, 1U, static_cast<AaCapabilityState>(0xFFU), unknown, unknown),
        Capabilities(1U, 1U, AaCapabilityState::kResolved, static_cast<AaAvailability>(0xFFU), available),
    };

    CHECK(harness, Platform::IsValidAaEngineCapabilities(UnknownCapabilities()));
    CHECK(harness, Platform::IsValidAaEngineCapabilities(Indeterminate(1U, 1U)));
    CHECK(harness, Platform::IsValidAaEngineCapabilities(Resolved(1U, 1U, available, unavailable)));

    for (const auto& capabilities : invalid) {
        CHECK(harness, !Platform::IsValidAaEngineCapabilities(capabilities));
        const auto result = Platform::ResolveAaEngine(AaEngineRequest::kDlss, capabilities);
        CHECK(harness, result.requested == AaEngineRequest::kDlss);
        CHECK(harness, result.effective == AaEffectiveEngine::kNone);
        CHECK(harness, result.reason == AaResolutionReason::kInvalidCapabilities);
    }

    const auto invalidRequest = static_cast<AaEngineRequest>(0xFFU);
    const auto invalidRequestResult = Platform::ResolveAaEngine(
        invalidRequest,
        Resolved(1U, 1U, available, available));
    CHECK(harness, invalidRequestResult.requested == invalidRequest);
    CHECK(harness, invalidRequestResult.effective == AaEffectiveEngine::kNone);
    CHECK(harness, invalidRequestResult.reason == AaResolutionReason::kInvalidRequest);
}

void Case_ReducerAcceptsNewerExactGenerations(TestHarness& harness) noexcept
{
    constexpr auto available = AaAvailability::kAvailable;
    constexpr auto unavailable = AaAvailability::kUnavailable;
    constexpr auto initial = Resolved(0x100U, 1U, available, unavailable);

    const auto first = Platform::ReduceAaCapabilityObservation(
        AaEngineRequest::kDlss,
        UnknownCapabilities(),
        initial);
    CHECK(harness, first.requested == AaEngineRequest::kDlss);
    CHECK(harness, first.capabilities == initial);
    CHECK(harness, first.outcome == AaCapabilityReduceOutcome::kAccepted);
    CHECK(harness, first.changed);

    constexpr auto newerSameIdentity = Resolved(0x100U, 2U, unavailable, available);
    const auto second = Platform::ReduceAaCapabilityObservation(
        AaEngineRequest::kDlss,
        first.capabilities,
        newerSameIdentity);
    CHECK(harness, second.requested == AaEngineRequest::kDlss);
    CHECK(harness, second.capabilities == newerSameIdentity);
    CHECK(harness, second.outcome == AaCapabilityReduceOutcome::kAccepted);
    CHECK(harness, second.changed);

    constexpr auto newerDifferentIdentity = Indeterminate(0x200U, 3U);
    const auto third = Platform::ReduceAaCapabilityObservation(
        AaEngineRequest::kFsr,
        second.capabilities,
        newerDifferentIdentity);
    CHECK(harness, third.requested == AaEngineRequest::kFsr);
    CHECK(harness, third.capabilities == newerDifferentIdentity);
    CHECK(harness, third.outcome == AaCapabilityReduceOutcome::kAccepted);
    CHECK(harness, third.changed);

    constexpr auto recovery = Resolved(0x200U, 4U, available, available);
    const auto fourth = Platform::ReduceAaCapabilityObservation(
        AaEngineRequest::kFsr,
        third.capabilities,
        recovery);
    CHECK(harness, fourth.requested == AaEngineRequest::kFsr);
    CHECK(harness, fourth.capabilities == recovery);
    CHECK(harness, fourth.outcome == AaCapabilityReduceOutcome::kAccepted);
    CHECK(harness, fourth.changed);
}

void Case_ReducerNoopsIdenticalObservations(TestHarness& harness) noexcept
{
    constexpr auto current = Resolved(
        0x999U,
        88U,
        AaAvailability::kAvailable,
        AaAvailability::kUnavailable);
    const auto result = Platform::ReduceAaCapabilityObservation(
        AaEngineRequest::kDlss,
        current,
        current);
    CHECK(harness, result.requested == AaEngineRequest::kDlss);
    CHECK(harness, result.capabilities == current);
    CHECK(harness, result.outcome == AaCapabilityReduceOutcome::kNoChange);
    CHECK(harness, !result.changed);
}

void Case_ReducerFailClosesSameEpochConflicts(TestHarness& harness) noexcept
{
    constexpr auto available = AaAvailability::kAvailable;
    constexpr auto unavailable = AaAvailability::kUnavailable;
    constexpr auto current = Resolved(0x111U, 12U, available, unavailable);

    constexpr auto identityCollision = Resolved(0x222U, 12U, available, unavailable);
    const auto collision = Platform::ReduceAaCapabilityObservation(
        AaEngineRequest::kDlss,
        current,
        identityCollision);
    CHECK(harness, collision.requested == AaEngineRequest::kDlss);
    CHECK(harness, collision.capabilities == Indeterminate(0x111U, 12U));
    CHECK(harness, collision.outcome == AaCapabilityReduceOutcome::kFailClosedConflict);
    CHECK(harness, collision.changed);

    constexpr auto capabilityConflict = Resolved(0x111U, 12U, unavailable, available);
    const auto conflict = Platform::ReduceAaCapabilityObservation(
        AaEngineRequest::kFsr,
        current,
        capabilityConflict);
    CHECK(harness, conflict.requested == AaEngineRequest::kFsr);
    CHECK(harness, conflict.capabilities == Indeterminate(0x111U, 12U));
    CHECK(harness, conflict.outcome == AaCapabilityReduceOutcome::kFailClosedConflict);
    CHECK(harness, conflict.changed);

    const auto repeatedConflict = Platform::ReduceAaCapabilityObservation(
        AaEngineRequest::kFsr,
        conflict.capabilities,
        capabilityConflict);
    CHECK(harness, repeatedConflict.requested == AaEngineRequest::kFsr);
    CHECK(harness, repeatedConflict.capabilities == conflict.capabilities);
    CHECK(harness, repeatedConflict.outcome == AaCapabilityReduceOutcome::kFailClosedConflict);
    CHECK(harness, !repeatedConflict.changed);

    const auto failClosedResolution = Platform::ResolveAaEngine(
        conflict.requested,
        conflict.capabilities);
    CHECK(harness, failClosedResolution.requested == AaEngineRequest::kFsr);
    CHECK(harness, failClosedResolution.effective == AaEffectiveEngine::kNone);
    CHECK(harness, failClosedResolution.reason == AaResolutionReason::kCapabilityIndeterminate);
}

void Case_ReducerRejectsRollbackUnknownAndExhaustion(TestHarness& harness) noexcept
{
    constexpr auto available = AaAvailability::kAvailable;
    constexpr auto unavailable = AaAvailability::kUnavailable;
    constexpr auto current = Resolved(0xABCDEU, 20U, available, unavailable);
    constexpr auto rollback = Resolved(0xABCDEU, 19U, unavailable, available);

    const auto rollbackResult = Platform::ReduceAaCapabilityObservation(
        AaEngineRequest::kDlss,
        current,
        rollback);
    CHECK(harness, rollbackResult.requested == AaEngineRequest::kDlss);
    CHECK(harness, rollbackResult.capabilities == current);
    CHECK(harness, rollbackResult.outcome == AaCapabilityReduceOutcome::kRejectedRollback);
    CHECK(harness, !rollbackResult.changed);

    const auto unknownRollback = Platform::ReduceAaCapabilityObservation(
        AaEngineRequest::kFsr,
        current,
        UnknownCapabilities());
    CHECK(harness, unknownRollback.requested == AaEngineRequest::kFsr);
    CHECK(harness, unknownRollback.capabilities == current);
    CHECK(harness, unknownRollback.outcome == AaCapabilityReduceOutcome::kRejectedUnknownRollback);
    CHECK(harness, !unknownRollback.changed);

    constexpr auto maximumEpoch = std::numeric_limits<std::uint64_t>::max();
    constexpr auto exhausted = Resolved(0xABCDEU, maximumEpoch, available, unavailable);
    constexpr auto wrapped = Resolved(0xABCDEU, 1U, unavailable, available);
    const auto exhaustion = Platform::ReduceAaCapabilityObservation(
        AaEngineRequest::kDlss,
        exhausted,
        wrapped);
    CHECK(harness, exhaustion.requested == AaEngineRequest::kDlss);
    CHECK(harness, exhaustion.capabilities == exhausted);
    CHECK(harness, exhaustion.outcome == AaCapabilityReduceOutcome::kRejectedEpochExhausted);
    CHECK(harness, !exhaustion.changed);

    const auto maximumNoop = Platform::ReduceAaCapabilityObservation(
        AaEngineRequest::kDlss,
        exhausted,
        exhausted);
    CHECK(harness, maximumNoop.outcome == AaCapabilityReduceOutcome::kNoChange);
    CHECK(harness, !maximumNoop.changed);

    constexpr auto maximumCollision = Resolved(0xBCDEFU, maximumEpoch, available, unavailable);
    const auto maximumConflict = Platform::ReduceAaCapabilityObservation(
        AaEngineRequest::kDlss,
        exhausted,
        maximumCollision);
    CHECK(harness, maximumConflict.requested == AaEngineRequest::kDlss);
    CHECK(harness, maximumConflict.capabilities == Indeterminate(0xABCDEU, maximumEpoch));
    CHECK(harness, maximumConflict.outcome == AaCapabilityReduceOutcome::kFailClosedConflict);
    CHECK(harness, maximumConflict.changed);
}

void Case_ReducerRejectsInvalidState(TestHarness& harness) noexcept
{
    constexpr auto valid = Resolved(
        1U,
        2U,
        AaAvailability::kAvailable,
        AaAvailability::kUnavailable);
    constexpr auto invalidCurrent = Capabilities(
        0U,
        2U,
        AaCapabilityState::kResolved,
        AaAvailability::kAvailable,
        AaAvailability::kUnavailable);
    const auto badCurrent = Platform::ReduceAaCapabilityObservation(
        AaEngineRequest::kDlss,
        invalidCurrent,
        valid);
    CHECK(harness, badCurrent.requested == AaEngineRequest::kDlss);
    CHECK(harness, badCurrent.capabilities == invalidCurrent);
    CHECK(harness, badCurrent.outcome == AaCapabilityReduceOutcome::kRejectedInvalidCurrent);
    CHECK(harness, !badCurrent.changed);

    constexpr auto invalidObservation = Capabilities(
        3U,
        0U,
        AaCapabilityState::kResolved,
        AaAvailability::kAvailable,
        AaAvailability::kAvailable);
    const auto badObservation = Platform::ReduceAaCapabilityObservation(
        AaEngineRequest::kFsr,
        valid,
        invalidObservation);
    CHECK(harness, badObservation.requested == AaEngineRequest::kFsr);
    CHECK(harness, badObservation.capabilities == valid);
    CHECK(harness, badObservation.outcome == AaCapabilityReduceOutcome::kRejectedInvalidObservation);
    CHECK(harness, !badObservation.changed);
}

void Case_NoexceptNoAllocationAndStress(TestHarness& harness) noexcept
{
    static_assert(noexcept(Platform::IsValidAaEngineCapabilities(UnknownCapabilities())));
    static_assert(noexcept(Platform::ResolveAaEngine(AaEngineRequest::kDlss, UnknownCapabilities())));
    static_assert(noexcept(Platform::ReduceAaCapabilityObservation(
        AaEngineRequest::kDlss,
        UnknownCapabilities(),
        UnknownCapabilities())));
    static_assert(std::is_trivially_copyable_v<AaDeviceGeneration>);
    static_assert(std::is_trivially_copyable_v<AaEngineCapabilities>);
    static_assert(std::is_trivially_copyable_v<Platform::AaEngineResolution>);
    static_assert(std::is_trivially_copyable_v<Platform::AaCapabilityReduceResult>);

    const auto allocationsBefore = g_allocationCount;
    auto current = UnknownCapabilities();
    std::uint64_t sink = 0;
    constexpr std::uint64_t iterations = 100000U;
    for (std::uint64_t epoch = 1U; epoch <= iterations; ++epoch) {
        const auto observation = Resolved(
            (epoch % 17U) + 1U,
            epoch,
            (epoch % 3U) == 0U ? AaAvailability::kUnavailable : AaAvailability::kAvailable,
            (epoch % 5U) == 0U ? AaAvailability::kUnavailable : AaAvailability::kAvailable);
        const auto reduced = Platform::ReduceAaCapabilityObservation(
            (epoch % 2U) == 0U ? AaEngineRequest::kDlss : AaEngineRequest::kFsr,
            current,
            observation);
        CHECK(harness, reduced.outcome == AaCapabilityReduceOutcome::kAccepted);
        CHECK(harness, reduced.changed);
        current = reduced.capabilities;

        const auto resolution = Platform::ResolveAaEngine(reduced.requested, current);
        CHECK(harness, resolution.requested == reduced.requested);
        CHECK(harness, resolution.effective != AaEffectiveEngine::kUnresolved);
        sink += static_cast<std::uint64_t>(resolution.effective);
    }
    CHECK(harness, current.device.epoch == iterations);
    CHECK(harness, sink != 0U);
    CHECK(harness, g_allocationCount == allocationsBefore);
}

}

_Ret_notnull_ _Post_writable_byte_size_(size) _VCRT_ALLOCATOR
void* __CRTDECL operator new(const std::size_t size)
{
    ++g_allocationCount;
    if (void* const allocation = std::malloc(size)) {
        return allocation;
    }
    throw std::bad_alloc{};
}

_Ret_notnull_ _Post_writable_byte_size_(size) _VCRT_ALLOCATOR
void* __CRTDECL operator new[](const std::size_t size)
{
    return ::operator new(size);
}

void operator delete(void* const allocation) noexcept
{
    std::free(allocation);
}

void operator delete[](void* const allocation) noexcept
{
    ::operator delete(allocation);
}

void operator delete(void* const allocation, const std::size_t) noexcept
{
    std::free(allocation);
}

void operator delete[](void* const allocation, const std::size_t) noexcept
{
    ::operator delete(allocation);
}

int main()
{
    TestHarness harness{};
    Case_FullTruthTable(harness);
    Case_AutomaticFallbackPreservesRequest(harness);
    Case_InvalidTuplesFailClosed(harness);
    Case_ReducerAcceptsNewerExactGenerations(harness);
    Case_ReducerNoopsIdenticalObservations(harness);
    Case_ReducerFailClosesSameEpochConflicts(harness);
    Case_ReducerRejectsRollbackUnknownAndExhaustion(harness);
    Case_ReducerRejectsInvalidState(harness);
    Case_NoexceptNoAllocationAndStress(harness);

    if (harness.failures != 0) {
        std::printf("AaEngineResolutionTests: FAIL (%d/%d checks failed)\n", harness.failures, harness.checks);
        return 1;
    }

    std::printf("AaEngineResolutionTests: PASS (%d checks)\n", harness.checks);
    return 0;
}
