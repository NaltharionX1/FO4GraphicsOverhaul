#pragma once

#include "Platform/NeuralContract.h"
#include "Settings/MenuSettings.h"

#include <cstdint>

namespace Platform::NeuralPass
{
    void ExecuteAfterEffectRange() noexcept;

    void ApplyMenuSettings(const Settings::MenuSettings& a_settings) noexcept;
    [[nodiscard]] Neural::CascadeSettings CurrentSettings() noexcept;

    void RequestRetry() noexcept;

    [[nodiscard]] bool LastResetFlag() noexcept;

    struct StageState
    {
        bool active{ false };
        bool ready{ false };
        bool latched{ false };
        float cpuMs{ 0.0F };
        float gpuMsMedian{ 0.0F };
        float gpuMsP99{ 0.0F };
        std::uint64_t evaluations{ 0 };
        std::uint32_t creates{ 0 };
        std::uintptr_t handleAddress{ 0 };
        const char* carrier{ "" };
        char reason[160]{};
    };

    struct State
    {
        bool enabled{ false };
        bool active{ false };
        bool latched{ false };
        bool pending{ false };
        std::uint32_t requestedPasses{ 1 };
        std::uint32_t activePasses{ 0 };
        std::array<StageState, Neural::kMaxPasses> passes{};
        char pendingScope[64]{};
        std::uint64_t appliedGeneration{ 0 };
        std::uint64_t latestGeneration{ 0 };
        char status[160]{};
        const char* family{ "" };
        const char* expectation{ "" };
        char runtimeVersion[32]{};
        char runtimeSignature[32]{};
        char runtimeSha[17]{};
        const char* profileLabel{ "" };
        bool profileValidated{ false };
        float cpuMs{ 0.0F };
        float gpuMsMedianSum{ 0.0F };
        bool gpuTimed{ false };
        std::uint64_t frames{ 0 };
        std::uint64_t evaluations{ 0 };
        std::uint32_t rebuilds{ 0 };
        std::uint32_t lastResult{ 0 };
        const char* lastResultName{ "" };
        const char* carrier{ "" };
        int guidesSource{ -1 };
        std::uint32_t outWidth{ 0 };
        std::uint32_t outHeight{ 0 };
        std::uint32_t renderWidth{ 0 };
        std::uint32_t renderHeight{ 0 };
        std::uint32_t modelPercent{ 100 };
        std::uint32_t modelWidth{ 0 };
        std::uint32_t modelHeight{ 0 };
        char modelReason[160]{};
        std::uint32_t frameStagesDelivered{ 0 };
        std::uint32_t frameColourCopies{ 0 };
        std::uint32_t frameCrossApiTrips{ 0 };
        std::uint64_t frameCopyBytes{ 0 };
        bool d3d11Timed{ false };
        float d3d11PrepMsMedian{ 0.0F };
        float d3d11ReturnMsMedian{ 0.0F };
        std::uint64_t d3d11PrepBrackets{ 0 };
        std::uint64_t d3d11ReturnBrackets{ 0 };
        std::uint32_t d3d11Windows{ 0 };
    };
    [[nodiscard]] State Snapshot() noexcept;
}
