#pragma once

#include <cstdint>

namespace Platform::AoSlot
{
    void SetGtaoProduction(bool a_enabled) noexcept;

    void ComputeForIntegration() noexcept;

    void IntegrateAfterLighting() noexcept;

}
