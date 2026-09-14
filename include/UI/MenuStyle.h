#pragma once

#include <cstdint>

namespace UI::MenuStyle
{
    enum class Style : std::uint32_t
    {
        kClassic = 0,
        kLight = 1,
        kDark = 2,
    };

    inline constexpr std::uint32_t kDefault = static_cast<std::uint32_t>(Style::kDark);

    inline constexpr const char* kComboItems = "Classic\0Light\0Dark\0";

    [[nodiscard]] std::uint32_t Clamp(std::uint32_t a_value) noexcept;

    void Apply(std::uint32_t a_value);
}
