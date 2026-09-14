#pragma once

#include <cstdint>

namespace Platform::GradingPass
{
    void ExecuteAfterEffectRange() noexcept;

    [[nodiscard]] bool Enabled() noexcept;
    void SetEnabled(bool a_enabled) noexcept;

    void SetParam(const char* a_command, std::uint32_t a_argIndex, float a_value) noexcept;
}
