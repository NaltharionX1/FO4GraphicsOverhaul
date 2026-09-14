#pragma once

#include "Platform/FingerprintMath.h"

#include <cstdint>

struct IDXGISwapChain;

namespace Platform::FrameFingerprint
{
    using Fingerprint::Point;

    inline constexpr std::uint32_t kRingFrames = 600;
    inline constexpr std::uint32_t kPostMarkFrames = 120;

    void SetEnabled(bool a_on) noexcept;
    [[nodiscard]] bool Enabled() noexcept;

    void SampleSeam(Point a_point) noexcept;
    void SamplePresented(IDXGISwapChain* a_swapChain) noexcept;
    void SampleCanvas(IDXGISwapChain* a_swapChain) noexcept;
    void EndFrame(IDXGISwapChain* a_swapChain) noexcept;

    void MarkIncident() noexcept;
    void TripOff(const char* a_reason) noexcept;

    struct State
    {
        bool enabled{ false };
        bool allocated{ false };
        float lastDelta[Fingerprint::kPoints]{};
        bool lastValid[Fingerprint::kPoints]{};
        std::uint32_t ringFrames{ 0 };
        std::uint32_t dumps{ 0 };
        std::uint32_t droppedReads{ 0 };
        bool markArmed{ false };
        std::uint32_t markCountdown{ 0 };
        char lastVerdict[240]{};
        char lastDump[260]{};
        char reason[160]{};
    };
    [[nodiscard]] State Snapshot() noexcept;
}
