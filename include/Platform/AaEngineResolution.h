#pragma once

#include <cstdint>

namespace Platform {

enum class AaEngineRequest : std::uint8_t {
    kDlss,
    kFsr,
};

enum class AaCapabilityState : std::uint8_t {
    kUnknown,
    kResolved,
    kIndeterminate,
};

enum class AaAvailability : std::uint8_t {
    kUnknown,
    kUnavailable,
    kAvailable,
};

enum class AaEffectiveEngine : std::uint8_t {
    kUnresolved,
    kNone,
    kDlss,
    kFsr,
};

enum class AaResolutionReason : std::uint8_t {
    kCapabilitiesPending,
    kRequestedDlss,
    kRequestedFsr,
    kDlssUnavailableFallback,
    kNoUsableEngine,
    kCapabilityIndeterminate,
    kInvalidCapabilities,
    kInvalidRequest,
};

enum class AaCapabilityReduceOutcome : std::uint8_t {
    kAccepted,
    kNoChange,
    kRejectedInvalidCurrent,
    kRejectedInvalidObservation,
    kRejectedRollback,
    kRejectedUnknownRollback,
    kRejectedEpochExhausted,
    kFailClosedConflict,
};

struct AaDeviceGeneration {
    std::uint64_t identity = 0;
    std::uint64_t epoch = 0;

    friend bool operator==(const AaDeviceGeneration&, const AaDeviceGeneration&) noexcept = default;
};

struct AaEngineCapabilities {
    AaDeviceGeneration device{};
    AaCapabilityState state = AaCapabilityState::kUnknown;
    AaAvailability dlss = AaAvailability::kUnknown;
    AaAvailability fsr = AaAvailability::kUnknown;

    friend bool operator==(const AaEngineCapabilities&, const AaEngineCapabilities&) noexcept = default;
};

struct AaEngineResolution {
    AaEngineRequest requested = AaEngineRequest::kDlss;
    AaEffectiveEngine effective = AaEffectiveEngine::kUnresolved;
    AaResolutionReason reason = AaResolutionReason::kCapabilitiesPending;
};

struct AaCapabilityReduceResult {
    AaEngineRequest requested = AaEngineRequest::kDlss;
    AaEngineCapabilities capabilities{};
    AaCapabilityReduceOutcome outcome = AaCapabilityReduceOutcome::kNoChange;
    bool changed = false;
};

[[nodiscard]] bool IsValidAaEngineCapabilities(const AaEngineCapabilities& capabilities) noexcept;

[[nodiscard]] AaEngineResolution ResolveAaEngine(
    AaEngineRequest requested,
    const AaEngineCapabilities& capabilities) noexcept;

[[nodiscard]] AaCapabilityReduceResult ReduceAaCapabilityObservation(
    AaEngineRequest persistedRequest,
    const AaEngineCapabilities& current,
    const AaEngineCapabilities& observation) noexcept;

}
