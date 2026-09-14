#pragma once
#include "Platform/AaEngineResolution.h"
#include "Platform/AaMailbox.h"

namespace Platform
{
    struct Streamline
    {
        [[nodiscard]] static AaRequest AppliedRequest() noexcept;
        [[nodiscard]] static AaEffectiveEngine AppliedEffectiveEngine() noexcept;
    };
}
