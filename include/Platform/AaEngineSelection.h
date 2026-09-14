#pragma once

#include "Platform/AaEngineResolution.h"
#include "Platform/AaMailbox.h"

#include <atomic>
#include <cstdint>

namespace Platform {

enum class AaSelectionTransition : std::uint8_t {
    kCapabilitiesPending,
    kStartupResolved,
    kStable,
    kDraining,
    kFailClosed,
};

class AaDeviceObservationGate final {
public:
    AaDeviceObservationGate() noexcept = default;
    AaDeviceObservationGate(const AaDeviceObservationGate&) = delete;
    AaDeviceObservationGate& operator=(const AaDeviceObservationGate&) = delete;
    AaDeviceObservationGate(AaDeviceObservationGate&&) = delete;
    AaDeviceObservationGate& operator=(AaDeviceObservationGate&&) = delete;

    [[nodiscard]] bool TryEnter() noexcept
    {
        if (busy_.test_and_set(std::memory_order_acquire)) {
            failClosed_.store(true, std::memory_order_release);
            return false;
        }
        return true;
    }

    void Leave() noexcept
    {
        busy_.clear(std::memory_order_release);
    }

    void MarkFailClosed() noexcept
    {
        failClosed_.store(true, std::memory_order_release);
    }

    [[nodiscard]] bool FailClosed() const noexcept
    {
        return failClosed_.load(std::memory_order_acquire);
    }

private:
    std::atomic_flag busy_ = ATOMIC_FLAG_INIT;
    std::atomic<bool> failClosed_{ false };
};

struct AaEngineSelectionSnapshot {
    AaEngineRequest latestRequested = AaEngineRequest::kDlss;
    AaEngineCapabilities capabilities{};
    bool appliedEnabled = false;
    AaEffectiveEngine appliedEffective = AaEffectiveEngine::kUnresolved;
    AaEffectiveEngine targetEffective = AaEffectiveEngine::kUnresolved;
    AaResolutionReason targetReason = AaResolutionReason::kCapabilitiesPending;
    std::uint64_t requestGeneration = 0;
    std::uint64_t appliedGeneration = 0;
    AaSelectionTransition transition = AaSelectionTransition::kCapabilitiesPending;
    AaPumpOutcome lastPumpOutcome = AaPumpOutcome::kIdle;

    friend bool operator==(const AaEngineSelectionSnapshot&,
        const AaEngineSelectionSnapshot&) noexcept = default;
};

constexpr void ApplyAaSelectionFailClosed(AaEngineSelectionSnapshot& selection) noexcept
{
    selection.appliedEnabled = false;
    selection.appliedEffective = AaEffectiveEngine::kNone;
    selection.targetEffective = AaEffectiveEngine::kNone;
    selection.targetReason = AaResolutionReason::kCapabilityIndeterminate;
    selection.transition = AaSelectionTransition::kFailClosed;
}

struct AaEngineDiagnosticSnapshot {
    AaEngineSelectionSnapshot selection{};
    AaDiagnosticSnapshot mailbox{};

    friend bool operator==(const AaEngineDiagnosticSnapshot&,
        const AaEngineDiagnosticSnapshot&) noexcept = default;
};

[[nodiscard]] constexpr bool AaEffectiveEngineReady(const AaEffectiveEngine engine) noexcept
{
    return engine == AaEffectiveEngine::kDlss || engine == AaEffectiveEngine::kFsr;
}

[[nodiscard]] constexpr bool AaCapabilitiesResolved(const AaEngineCapabilities& capabilities) noexcept
{
    return capabilities.state == AaCapabilityState::kResolved;
}

}
