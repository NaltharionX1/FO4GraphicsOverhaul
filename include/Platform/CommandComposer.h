#pragma once

#include "Platform/CommandCatalogue.h"

#include <cstddef>

namespace Platform::CommandComposer
{
    struct ValueView
    {
        const double* values;
        std::size_t count;

        [[nodiscard]] double At(std::size_t a_index, double a_fallback) const noexcept
        {
            return (values != nullptr && a_index < count) ? values[a_index] : a_fallback;
        }
    };

    enum class FogMode : int
    {
        kDynamic = 0,
        kOff = 1,
        kCustom = 2,
    };

    struct FogPair
    {
        int first;
        int second;
    };

    [[nodiscard]] FogPair ResolveFogPair(ValueView a_values) noexcept;

    [[nodiscard]] bool FormatValue(
        char* a_out, std::size_t a_capacity, ValueType a_type, double a_value) noexcept;

    [[nodiscard]] bool Compose(
        const CatalogueRow& a_row,
        ValueView a_values,
        char* a_out,
        std::size_t a_capacity) noexcept;
}
