#pragma once

#include <d3d12.h>
#include <dxgiformat.h>

#include <cstdint>

namespace Platform::SidecarCompute
{
    enum class Filter : std::uint32_t
    {
        kArea = 0,
        kCatmullRom = 1,
    };

    [[nodiscard]] constexpr Filter FilterFor(std::uint32_t a_fromWidth, std::uint32_t a_toWidth) noexcept
    {
        return a_toWidth < a_fromWidth ? Filter::kArea : Filter::kCatmullRom;
    }

    [[nodiscard]] bool Ensure(ID3D12Device* a_device) noexcept;
    [[nodiscard]] const char* Reason() noexcept;
    void Release() noexcept;

    [[nodiscard]] bool CreateTexture(ID3D12Device* a_device, std::uint32_t a_width, std::uint32_t a_height,
        DXGI_FORMAT a_format, const wchar_t* a_name, ID3D12Resource*& a_out) noexcept;

    [[nodiscard]] bool Resample(ID3D12GraphicsCommandList* a_list, ID3D12Resource* a_source, ID3D12Resource* a_dest,
        DXGI_FORMAT a_viewFormat, Filter a_filter) noexcept;
    [[nodiscard]] bool Compose(ID3D12GraphicsCommandList* a_list, ID3D12Resource* a_base, ID3D12Resource* a_modelIn,
        ID3D12Resource* a_modelOut, ID3D12Resource* a_dest, DXGI_FORMAT a_viewFormat, Filter a_filter) noexcept;
}
