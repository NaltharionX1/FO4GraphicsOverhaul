// SPDX-License-Identifier: GPL-3.0-or-later
// Portions adapted from High FPS Physics Fix, Copyright (c) 2025 AntoniX35, MIT License.

#pragma once

#include "Platform/PresentMath.h"

#include <cstdint>

struct IDXGISwapChain;
struct DXGI_SWAP_CHAIN_DESC;

namespace Platform
{
    class PresentPolicy
    {
    public:
        struct Settings
        {
            bool enableVSync{ false };
            std::uint32_t vsyncInterval{ 1 };
            std::uint32_t fpsLimit{ 0 };
            std::uint32_t loadingScreenFpsLimit{ 60 };
            bool limitAfterPresent{ false };
        };

        static void Configure(const Settings& a_settings) noexcept;

        static void SetUserOptions(bool a_vsyncEnabled, std::uint32_t a_vsyncInterval,
            std::uint32_t a_fpsLimit, std::uint32_t a_loadingScreenFpsLimit) noexcept;

        static void ApplyToDesc(DXGI_SWAP_CHAIN_DESC& a_desc) noexcept;

        [[nodiscard]] static PresentParams ParamsFor() noexcept;

        static void PaceFrame() noexcept;

        static void SetLoadingScreenActive(bool a_active) noexcept;

        [[nodiscard]] static bool LimitAfterPresent() noexcept;

        [[nodiscard]] static bool LoadingScreenActive() noexcept;

        [[nodiscard]] static bool PaceGameFrame() noexcept;
        [[nodiscard]] static bool CapOnGameThread() noexcept;

        struct State
        {
            bool configured{ false };
            bool descApplied{ false };
            bool chainAllowsTearing{ false };
            SwapEffectChoice resolvedEffect{ SwapEffectChoice::kAuto };
            std::uint32_t resolvedBufferCount{ 0 };
            bool loadingScreenActive{ false };
            std::uint32_t activeFpsLimit{ 0 };
        };

        [[nodiscard]] static State CurrentState() noexcept;
    };
}
