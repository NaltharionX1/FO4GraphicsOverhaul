#pragma once

#include <cstdint>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11RenderTargetView;
struct IDXGISwapChain;

namespace Core::DeviceObjects
{
    class ReentrancyGuard
    {
    public:
        ReentrancyGuard() noexcept;
        ~ReentrancyGuard() noexcept;

        ReentrancyGuard(const ReentrancyGuard&) = delete;
        ReentrancyGuard& operator=(const ReentrancyGuard&) = delete;
        ReentrancyGuard(ReentrancyGuard&&) = delete;
        ReentrancyGuard& operator=(ReentrancyGuard&&) = delete;

        [[nodiscard]] bool IsOutermost() const noexcept { return isOutermost_; }

    private:
        bool isOutermost_;
    };

    struct Generation
    {
        std::uint64_t id{ 0 };
        bool isOutermostCall{ false };
    };

    struct Snapshot
    {
        ID3D11Device* device{ nullptr };
        ID3D11DeviceContext* context{ nullptr };
        IDXGISwapChain* swapChain{ nullptr };
        HWND hwnd{ nullptr };
        std::uint64_t generation{ 0 };

        [[nodiscard]] bool IsValid() const noexcept
        {
            return device != nullptr && context != nullptr && swapChain != nullptr;
        }
    };

    [[nodiscard]] Generation NoteDeviceCreated(
        bool a_isOutermostCall,
        ID3D11Device* a_device,
        IDXGISwapChain* a_swapChain,
        ID3D11DeviceContext* a_context,
        HWND a_hwnd) noexcept;

    [[nodiscard]] Snapshot Current() noexcept;

    [[nodiscard]] ID3D11RenderTargetView* AcquireBackbufferRTV(IDXGISwapChain* a_swapChain) noexcept;

    void ReleaseSizedViews() noexcept;
}
