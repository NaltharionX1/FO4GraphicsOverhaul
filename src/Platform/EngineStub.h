#pragma once

#include <cstddef>
#include <cstdint>

namespace Platform
{

    class EngineStub
    {
    public:
        [[nodiscard]] static bool InstallBranch(std::uint64_t a_id, std::ptrdiff_t a_offset,
            const std::uint8_t* a_code, std::size_t a_size, const char* a_name) noexcept;

        [[nodiscard]] static std::uintptr_t ResolveSite(std::uint64_t a_id, std::ptrdiff_t a_offset,
            const char* a_name) noexcept;
    };
}
