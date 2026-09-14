#pragma once

#include <cstdint>

namespace Platform::Reflex
{
    void SetMode(std::uint32_t a_mode) noexcept;
    [[nodiscard]] std::uint32_t Mode() noexcept;
    void RequestRetry() noexcept;

    void FrameStart() noexcept;
    void AfterPresent() noexcept;
    void NoteGameUpdate() noexcept;
    void NoteGameUpdateEnd() noexcept;
    void GameFrameBoundary() noexcept;
    void InstallInputHook() noexcept;
    void RenderSubmitEnd() noexcept;
    void PresentStart() noexcept;
    void PresentEnd() noexcept;
    void CloseFrameWithoutPresent() noexcept;
    void NotePresentStall() noexcept;
    [[nodiscard]] bool SteppedAsideAfterStall() noexcept;

    struct State
    {
        bool faulted{ false };
        std::uint32_t mode{ 0 };
        int boundDevice{ 0 };
        bool pacedByFrameGen{ false };
        bool latched{ false };
        bool idle{ false };
        char reason[160]{};
        std::uint64_t sleeps{ 0 };
        std::uint64_t tailSleeps{ 0 };
        std::uint64_t gameSleeps{ 0 };
        bool gameSeamLive{ false };
        float capPaceMs{ 0.0F };
        float capSleepMs{ 0.0F };
        std::uint64_t capFrames{ 0 };
        float freeSleepMs{ 0.0F };
        std::uint64_t freeFrames{ 0 };
        std::uint64_t markers{ 0 };
        std::uint32_t ringDepthMax{ 0 };
        std::uint64_t ringEmptyFinds{ 0 };
        std::uint64_t ringFullDrops{ 0 };
        float latencyMs{ 0.0F };
        float gpuFrameMs{ 0.0F };
        std::uint32_t reportedFrames{ 0 };
    };
    [[nodiscard]] State Snapshot() noexcept;
}
