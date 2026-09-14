#pragma once

#include <d3d11.h>
#include <dxgi.h>

#include <cstdint>

namespace Platform::PresentProxy
{
    [[nodiscard]] bool TakeOver(ID3D11Device* a_device, ID3D11DeviceContext* a_context,
        const DXGI_SWAP_CHAIN_DESC& a_policyDesc, IDXGISwapChain** a_out) noexcept;

    [[nodiscard]] bool Active() noexcept;

    struct State
    {
        bool active{ false };
        bool d3d11Fallback{ false };
        bool fullscreenIgnored{ false };
        char reason[160]{};
        std::uint32_t width{ 0 }, height{ 0 };
        std::uint32_t bufferCount{ 0 };
        bool tearing{ false };
        std::uint64_t presents{ 0 };
        std::uint32_t resizes{ 0 };
        float cpuMs{ 0.0F };
        std::uint64_t interpolated{ 0 };
        float intervalMs{ 0.0F };
        float cadenceLongestMs{ 0.0F };
        std::uint32_t cadencePresents{ 0 };
        std::uint32_t cadenceOverFloorWindow{ 0 };
        std::uint64_t cadenceOverFloorLifetime{ 0 };
        float cadenceFloorMs{ 0.0F };
        bool cadenceFloorMeasured{ false };
    };
    [[nodiscard]] State Snapshot() noexcept;
}
