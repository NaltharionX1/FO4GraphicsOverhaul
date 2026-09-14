#pragma once

#include <d3d12.h>
#include <dxgi.h>

#include <cstdint>

struct ID3D11Texture2D;

namespace Platform::SidecarFrame
{
    enum class GuideMotion : std::uint8_t
    {
        kRaw,
        kDilated,
    };

    struct Guides
    {
        ID3D12Resource* depth{ nullptr };
        ID3D12Resource* mv{ nullptr };
        GuideMotion motion{ GuideMotion::kRaw };
        std::uint32_t depthWidth{ 0 }, depthHeight{ 0 };
        std::uint32_t mvWidth{ 0 }, mvHeight{ 0 };
        std::uint32_t renderWidth{ 0 }, renderHeight{ 0 };
        DXGI_FORMAT depthFormat{ DXGI_FORMAT_UNKNOWN };
        DXGI_FORMAT mvFormat{ DXGI_FORMAT_UNKNOWN };
        float jitterX{ 0.0F }, jitterY{ 0.0F };
        bool reset{ false };
        ID3D12Resource* fgDepth{ nullptr };
        ID3D12Resource* fgMv{ nullptr };
    };

    struct Hudless
    {
        ID3D12Resource* colour{ nullptr };
        ID3D11Texture2D* colour11{ nullptr };
        std::uint32_t width{ 0 }, height{ 0 };
        DXGI_FORMAT format{ DXGI_FORMAT_UNKNOWN };
    };

    struct Cursor
    {
        std::uint64_t serial{ 0 };
    };

    void BeginFrame(std::uint64_t a_frame) noexcept;

    void PublishGuides(const Guides& a_guides) noexcept;
    void PublishHudless(const Hudless& a_hudless) noexcept;
    void InvalidateGuides() noexcept;
    void InvalidateHudless() noexcept;
    void Invalidate() noexcept;

    [[nodiscard]] bool TakeGuides(Cursor& a_cursor, Guides& a_out) noexcept;
    [[nodiscard]] bool TakeHudless(Cursor& a_cursor, Hudless& a_out) noexcept;

    struct State
    {
        std::uint64_t guidesSerial{ 0 };
        std::uint64_t hudlessSerial{ 0 };
        bool guidesLive{ false };
        bool hudlessLive{ false };
        std::uint64_t guidesTaken{ 0 };
        std::uint64_t hudlessTaken{ 0 };
        std::uint64_t staleRefused{ 0 };
    };
    [[nodiscard]] State Snapshot() noexcept;
}
