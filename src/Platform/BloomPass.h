#pragma once

#include <cstdint>

namespace Platform::BloomPass
{
    void ExecuteAfterEffectRange() noexcept;

    void SetParam(std::uint32_t a_argIndex, float a_value) noexcept;
}
