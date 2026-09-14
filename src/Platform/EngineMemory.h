#pragma once

#include <cstddef>
#include <cstdint>

namespace Platform::EngineMemory
{

    [[nodiscard]] bool WithinGameImage(std::uintptr_t a_address, std::size_t a_length) noexcept;

    [[nodiscard]] bool SafeRead(void* a_dst, const void* a_src, std::size_t a_count) noexcept;
    [[nodiscard]] bool SafeWrite(void* a_dst, const void* a_src, std::size_t a_count) noexcept;
}
