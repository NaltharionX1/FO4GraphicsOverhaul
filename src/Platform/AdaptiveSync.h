#pragma once

#include <cstdint>

namespace Platform::AdaptiveSync
{
    void Observe(void* a_hwnd) noexcept;

    void Apply(bool a_fixEnabled, std::uint32_t a_fpsCap) noexcept;

    void Restore() noexcept;

    [[nodiscard]] std::uint32_t SampleRepeats() noexcept;

    [[nodiscard]] float FloorMs() noexcept;
    [[nodiscard]] bool FloorIsMeasured() noexcept;

    struct State
    {
        bool observed{ false };
        bool available{ false };
        bool vrrCapable{ false };
        bool trueGsync{ false };
        bool vrrActiveNow{ false };
        bool adaptiveSyncDisabled{ false };
        std::uint32_t refreshHz{ 0 };
        std::uint32_t driverMaxIntervalUs{ 0 };
        std::uint32_t appliedIntervalUs{ 0 };
        std::uint32_t lastFlipRepeats{ 0 };
        bool fixRequested{ false };
        std::uint32_t capRequested{ 0 };
        char reason[120]{};
    };
    [[nodiscard]] State Snapshot() noexcept;
}
