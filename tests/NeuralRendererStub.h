#pragma once
#include <cstdint>

namespace Platform::NeuralRendererStub
{
    inline constexpr std::uint32_t kNoFailure = 0xFFFFFFFFU;
    inline constexpr std::uint32_t kEvaluateStrikes = 3;

    void FailStage(std::uint32_t a_stage) noexcept;
    void RefuseNativeCarrier(std::uint32_t a_stage) noexcept;
    [[nodiscard]] std::uint32_t Evaluates(std::uint32_t a_stage) noexcept;
    [[nodiscard]] std::uint32_t Creates(std::uint32_t a_stage) noexcept;
    [[nodiscard]] std::uint32_t Releases(std::uint32_t a_stage) noexcept;
    [[nodiscard]] std::uint32_t Abandons(std::uint32_t a_stage) noexcept;
    void SetIdentity(bool a_identity) noexcept;
    [[nodiscard]] bool FeatureAlive(std::uint32_t a_stage) noexcept;
    [[nodiscard]] bool Latched(std::uint32_t a_stage) noexcept;
    void ResetCounters() noexcept;
}
