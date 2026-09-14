#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace Platform
{
    class EnginePatch
    {
    public:
        [[nodiscard]] static bool WriteBytes(std::uint64_t a_id, std::ptrdiff_t a_offset,
            std::span<const std::uint8_t> a_patch, std::span<const std::uint8_t> a_expected,
            const char* a_name) noexcept;

        [[nodiscard]] static bool WriteByte(std::uint64_t a_id, std::ptrdiff_t a_offset,
            std::uint8_t a_value, const char* a_name) noexcept;

        [[nodiscard]] static bool FillBytes(std::uint64_t a_id, std::ptrdiff_t a_offset,
            std::uint8_t a_value, std::size_t a_count, const char* a_name) noexcept;

        static void LogSummary(const char* a_batchName) noexcept;
    };
}
