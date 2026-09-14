// SPDX-License-Identifier: GPL-3.0-or-later
// Portions adapted from Community Shaders for Fallout 4 (northaxosky), GPL-3.0.

#pragma once

#include <cstdint>

struct IDXGISwapChain4;
struct IDXGIFactory;
struct ID3D12CommandQueue;
struct DXGI_SWAP_CHAIN_DESC1;

namespace Platform::FsrFrameGen
{
    void Probe() noexcept;

    [[nodiscard]] bool Available() noexcept;

    [[nodiscard]] bool Selected() noexcept;
    void SetSelected(bool a_selected) noexcept;

    void WaitForPresents() noexcept;

    void OnChainResized() noexcept;

    void RetirePendingRelease() noexcept;

    [[nodiscard]] bool CreateSwapChain(IDXGIFactory* a_factory, ID3D12CommandQueue* a_queue, void* a_hwnd,
        const DXGI_SWAP_CHAIN_DESC1* a_desc, IDXGISwapChain4** a_outChain) noexcept;

    [[nodiscard]] bool EnsureContext(void* a_device12, std::uint32_t a_displayW, std::uint32_t a_displayH,
        std::uint32_t a_backBufferFormat) noexcept;

    void CaptureAndPrepare(void* a_context11, void* a_depth11, void* a_motion11, std::uint32_t a_renderW,
        std::uint32_t a_renderH, float a_jitterX, float a_jitterY) noexcept;

    void ConfigureFrame(void* a_swapChain, bool a_enabled, void* a_hudless12) noexcept;

    void Release() noexcept;

    void Abandon() noexcept;

    void ReArmAfterEnable() noexcept;

    [[nodiscard]] bool ContextReady() noexcept;
    [[nodiscard]] bool Engaged() noexcept;
    [[nodiscard]] std::uint64_t Generated() noexcept;

    void SetPacingSafetyMarginMs(float a_ms) noexcept;
    void SetPacingVarianceFactor(float a_factor) noexcept;
    void SetPacingHybridSpin(bool a_enabled) noexcept;
    void SetPacingHybridSpinTime(std::uint32_t a_units) noexcept;
    void SetPacingWaitOnFence(bool a_enabled) noexcept;

    struct State
    {
        bool available{ false };
        bool selected{ false };
        bool chainCreated{ false };
        bool contextReady{ false };
        bool latched{ false };
        std::uint64_t prepared{ 0 };
        std::uint64_t configureFailures{ 0 };
        std::uint64_t dispatchFailures{ 0 };
        std::uint64_t generated{ 0 };
        char reason[128]{};
    };
    [[nodiscard]] State Snapshot() noexcept;
}
