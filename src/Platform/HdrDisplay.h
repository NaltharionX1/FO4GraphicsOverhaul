#pragma once

#include <cstdint>

struct IDXGISwapChain;

namespace Platform
{
    class HdrDisplay
    {
    public:
        struct Info
        {
            bool observed = false;
            bool displayInHdrMode = false;
            bool scrgbPresentable = false;
            float maxLuminanceNits = 0.0F;
            float minLuminanceNits = 0.0F;
            float maxFullFrameNits = 0.0F;
            std::uint32_t bitsPerColor = 0;
            std::uint32_t colorSpace = 0;
        };

        static void Observe(IDXGISwapChain* a_swapChain) noexcept;

        [[nodiscard]] static Info Current() noexcept;
    };
}
