#pragma once

#include <cstdint>

namespace Platform::EngineImod
{
    void FrameTick() noexcept;

    void SetLightShapeSpecialTest(bool a_on) noexcept;
    [[nodiscard]] bool LightShapeSpecialTestRequested() noexcept;

    void SetAdaptationOff(bool a_off) noexcept;
    [[nodiscard]] bool AdaptationOffRequested() noexcept;
    void SetEngineGradingNeutral(bool a_neutral) noexcept;
    [[nodiscard]] bool EngineGradingNeutralRequested() noexcept;

    void SetHdrParam(std::uint32_t a_argIndex, float a_value, bool a_set) noexcept;

    void SetEffectParam(std::uint32_t a_argIndex, float a_value, bool a_set) noexcept;

    void SetRadialBlurOff(bool a_off) noexcept;
    [[nodiscard]] bool RadialBlurOffRequested() noexcept;
    void SetDoubleVisionOff(bool a_off) noexcept;
    [[nodiscard]] bool DoubleVisionOffRequested() noexcept;

    void RequestRearm() noexcept;

    enum class Phase : std::uint8_t
    {
        kIdle,
        kReady,
        kApplied,
        kUnavailable,
    };
    struct State
    {
        Phase phase;
        bool sismeOn;
        std::uint32_t armedSlots;
    };
    [[nodiscard]] State Snapshot() noexcept;
}
