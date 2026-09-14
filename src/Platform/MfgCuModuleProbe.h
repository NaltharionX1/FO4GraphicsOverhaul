#pragma once

#include <cstddef>
#include <cstdint>

struct ID3D12Device;

namespace Platform::MfgCuModuleProbe
{
    [[nodiscard]] bool Load(ID3D12Device* a_device, const void* a_blob, std::size_t a_size, const char* a_entry, char* a_why,
        std::size_t a_whySize) noexcept;
    [[nodiscard]] bool PtxSupported(ID3D12Device* a_device, char* a_why, std::size_t a_whySize) noexcept;
}
