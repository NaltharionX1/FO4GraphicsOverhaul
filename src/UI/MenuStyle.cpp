#include "UI/MenuStyle.h"

namespace UI::MenuStyle
{
    std::uint32_t Clamp(std::uint32_t a_value) noexcept
    {
        return a_value <= static_cast<std::uint32_t>(Style::kDark) ? a_value : kDefault;
    }

    void Apply(std::uint32_t a_value)
    {
        switch (static_cast<Style>(Clamp(a_value))) {
        case Style::kClassic:
            ImGui::StyleColorsClassic();
            break;
        case Style::kLight:
            ImGui::StyleColorsLight();
            break;
        case Style::kDark:
        default:
            ImGui::StyleColorsDark();
            break;
        }
    }
}
