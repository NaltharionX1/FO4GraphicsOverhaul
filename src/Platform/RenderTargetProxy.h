#pragma once

#include <cstdint>

namespace Platform
{
    class RenderTargetProxy final
    {
    public:
        static void InstallHooks() noexcept;

        static void UpdateForRatio(float ratio) noexcept;

        [[nodiscard]] static bool Available() noexcept;
    };
}
