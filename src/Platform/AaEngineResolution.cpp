#include "Platform/AaEngineResolution.h"

#include <limits>

namespace Platform {
namespace {

[[nodiscard]] constexpr bool IsKnownAvailability(const AaAvailability availability) noexcept
{
    return availability == AaAvailability::kUnavailable || availability == AaAvailability::kAvailable;
}

[[nodiscard]] constexpr bool IsValidRequest(const AaEngineRequest requested) noexcept
{
    return requested == AaEngineRequest::kDlss || requested == AaEngineRequest::kFsr;
}

[[nodiscard]] constexpr AaCapabilityReduceResult ReduceResult(
    const AaEngineRequest requested,
    const AaEngineCapabilities& capabilities,
    const AaCapabilityReduceOutcome outcome,
    const bool changed) noexcept
{
    return { requested, capabilities, outcome, changed };
}

}

bool IsValidAaEngineCapabilities(const AaEngineCapabilities& capabilities) noexcept
{
    const bool hasExactGeneration = capabilities.device.identity != 0U && capabilities.device.epoch != 0U;

    switch (capabilities.state) {
    case AaCapabilityState::kUnknown:
        return capabilities.device.identity == 0U &&
               capabilities.device.epoch == 0U &&
               capabilities.dlss == AaAvailability::kUnknown &&
               capabilities.fsr == AaAvailability::kUnknown;

    case AaCapabilityState::kResolved:
        return hasExactGeneration &&
               IsKnownAvailability(capabilities.dlss) &&
               IsKnownAvailability(capabilities.fsr);

    case AaCapabilityState::kIndeterminate:
        return hasExactGeneration &&
               capabilities.dlss == AaAvailability::kUnknown &&
               capabilities.fsr == AaAvailability::kUnknown;

    default:
        return false;
    }
}

AaEngineResolution ResolveAaEngine(
    const AaEngineRequest requested,
    const AaEngineCapabilities& capabilities) noexcept
{
    if (!IsValidRequest(requested)) {
        return { requested, AaEffectiveEngine::kNone, AaResolutionReason::kInvalidRequest };
    }

    if (!IsValidAaEngineCapabilities(capabilities)) {
        return { requested, AaEffectiveEngine::kNone, AaResolutionReason::kInvalidCapabilities };
    }

    if (capabilities.state == AaCapabilityState::kUnknown) {
        return { requested, AaEffectiveEngine::kUnresolved, AaResolutionReason::kCapabilitiesPending };
    }

    if (capabilities.state == AaCapabilityState::kIndeterminate) {
        return { requested, AaEffectiveEngine::kNone, AaResolutionReason::kCapabilityIndeterminate };
    }

    if (requested == AaEngineRequest::kDlss) {
        if (capabilities.dlss == AaAvailability::kAvailable) {
            return { requested, AaEffectiveEngine::kDlss, AaResolutionReason::kRequestedDlss };
        }

        if (capabilities.fsr == AaAvailability::kAvailable) {
            return { requested, AaEffectiveEngine::kFsr, AaResolutionReason::kDlssUnavailableFallback };
        }

        return { requested, AaEffectiveEngine::kNone, AaResolutionReason::kNoUsableEngine };
    }

    if (capabilities.fsr == AaAvailability::kAvailable) {
        return { requested, AaEffectiveEngine::kFsr, AaResolutionReason::kRequestedFsr };
    }

    return { requested, AaEffectiveEngine::kNone, AaResolutionReason::kNoUsableEngine };
}

AaCapabilityReduceResult ReduceAaCapabilityObservation(
    const AaEngineRequest persistedRequest,
    const AaEngineCapabilities& current,
    const AaEngineCapabilities& observation) noexcept
{
    if (!IsValidAaEngineCapabilities(current)) {
        return ReduceResult(
            persistedRequest,
            current,
            AaCapabilityReduceOutcome::kRejectedInvalidCurrent,
            false);
    }

    if (!IsValidAaEngineCapabilities(observation)) {
        return ReduceResult(
            persistedRequest,
            current,
            AaCapabilityReduceOutcome::kRejectedInvalidObservation,
            false);
    }

    if (observation == current) {
        return ReduceResult(persistedRequest, current, AaCapabilityReduceOutcome::kNoChange, false);
    }

    if (current.state == AaCapabilityState::kUnknown) {
        return ReduceResult(persistedRequest, observation, AaCapabilityReduceOutcome::kAccepted, true);
    }

    if (observation.state == AaCapabilityState::kUnknown) {
        return ReduceResult(
            persistedRequest,
            current,
            AaCapabilityReduceOutcome::kRejectedUnknownRollback,
            false);
    }

    if (observation.device.epoch == current.device.epoch) {
        const AaEngineCapabilities failClosed{
            current.device,
            AaCapabilityState::kIndeterminate,
            AaAvailability::kUnknown,
            AaAvailability::kUnknown,
        };
        return ReduceResult(
            persistedRequest,
            failClosed,
            AaCapabilityReduceOutcome::kFailClosedConflict,
            failClosed != current);
    }

    if (observation.device.epoch > current.device.epoch) {
        return ReduceResult(persistedRequest, observation, AaCapabilityReduceOutcome::kAccepted, true);
    }

    if (current.device.epoch == std::numeric_limits<std::uint64_t>::max()) {
        return ReduceResult(
            persistedRequest,
            current,
            AaCapabilityReduceOutcome::kRejectedEpochExhausted,
            false);
    }

    return ReduceResult(
        persistedRequest,
        current,
        AaCapabilityReduceOutcome::kRejectedRollback,
        false);
}

}
