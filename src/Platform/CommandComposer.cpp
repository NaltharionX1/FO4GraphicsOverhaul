#include "Platform/CommandComposer.h"

#include <charconv>
#include <cmath>
#include <cstring>
#include <system_error>

namespace
{
    void SetLiteral(char* a_dst, std::size_t a_capacity, const char* a_text) noexcept
    {
        if (a_dst == nullptr || a_capacity == 0U) {
            return;
        }
        const std::size_t length = std::strlen(a_text);
        if (length + 1U > a_capacity) {
            a_dst[0] = '\0';
            return;
        }
        std::memcpy(a_dst, a_text, length + 1U);
    }

    [[nodiscard]] bool Append(
        char* a_out, std::size_t a_capacity, std::size_t& a_used, const char* a_text) noexcept
    {
        const std::size_t length = std::strlen(a_text);
        if (a_used + length + 1U > a_capacity) {
            return false;
        }
        std::memcpy(a_out + a_used, a_text, length);
        a_used += length;
        a_out[a_used] = '\0';
        return true;
    }
}

namespace Platform::CommandComposer
{
    FogPair ResolveFogPair(ValueView a_values) noexcept
    {
        const double rawMode = a_values.At(GlobalIndexForId("Fog.Mode"), 0.0);
        const double distance = a_values.At(GlobalIndexForId("Fog.Distance"), 1.0);
        const auto mode = static_cast<FogMode>(static_cast<int>(std::llround(rawMode)));

        switch (mode) {
        case FogMode::kOff:
            return { 1, 1 };

        case FogMode::kCustom: {
            const long long rounded = std::llround(distance);
            const int pinned = rounded < 1 ? 1 : static_cast<int>(rounded);
            return { 0, pinned };
        }

        case FogMode::kDynamic:
        default:
            return { 0, 0 };
        }
    }

    bool FormatValue(char* a_out, std::size_t a_capacity, ValueType a_type, double a_value) noexcept
    {
        if (a_out == nullptr || a_capacity == 0U) {
            return false;
        }
        a_out[0] = '\0';

        if (!std::isfinite(a_value)) {
            return false;
        }

        char scratch[64]{};

        if (a_type == ValueType::kInt || a_type == ValueType::kBool) {
            const long long rounded = std::llround(a_value);
            const auto result = std::to_chars(scratch, scratch + sizeof(scratch), rounded);
            if (result.ec != std::errc{}) {
                return false;
            }
            *result.ptr = '\0';
        } else {
            const auto result = std::to_chars(
                scratch, scratch + sizeof(scratch), a_value, std::chars_format::fixed, 6);
            if (result.ec != std::errc{}) {
                return false;
            }
            *result.ptr = '\0';

            if (std::strchr(scratch, '.') != nullptr) {
                std::size_t end = std::strlen(scratch);
                while (end > 0U && scratch[end - 1U] == '0') {
                    --end;
                }
                if (end > 0U && scratch[end - 1U] == '.') {
                    --end;
                }
                scratch[end] = '\0';
            }
            if (std::strcmp(scratch, "-0") == 0) {
                SetLiteral(scratch, sizeof(scratch), "0");
            }
        }

        const std::size_t length = std::strlen(scratch);
        if (length + 1U > a_capacity) {
            return false;
        }
        std::memcpy(a_out, scratch, length + 1U);
        return true;
    }

    bool Compose(
        const CatalogueRow& a_row,
        ValueView a_values,
        char* a_out,
        std::size_t a_capacity) noexcept
    {
        if (a_out == nullptr || a_capacity == 0U) {
            return false;
        }
        a_out[0] = '\0';

        if (a_row.command == nullptr || a_row.command[0] == '\0') {
            return false;
        }

        std::size_t used = 0U;

        if (std::strcmp(a_row.command, "setfog") == 0) {
            const FogPair pair = ResolveFogPair(a_values);

            char first[64]{};
            char second[64]{};
            if (!FormatValue(first, sizeof(first), ValueType::kInt,
                    static_cast<double>(pair.first)) ||
                !FormatValue(second, sizeof(second), ValueType::kInt,
                    static_cast<double>(pair.second))) {
                return false;
            }

            return Append(a_out, a_capacity, used, a_row.command) &&
                   Append(a_out, a_capacity, used, " ") &&
                   Append(a_out, a_capacity, used, first) &&
                   Append(a_out, a_capacity, used, " ") &&
                   Append(a_out, a_capacity, used, second);
        }

        if (!Append(a_out, a_capacity, used, a_row.command)) {
            return false;
        }

        switch (a_row.tier) {
        case Tier::kConsoleToggle:
            return true;

        case Tier::kConsoleSwitch: {
            const auto index = GlobalIndexForId(a_row.id);
            const double value = a_values.At(index, a_row.vanillaDefault);
            const bool on = std::llround(value) != 0;
            return Append(a_out, a_capacity, used, " ") &&
                   Append(a_out, a_capacity, used, on ? "on" : "off");
        }

        case Tier::kConsoleValue: {
            const auto index = GlobalIndexForId(a_row.id);
            const double value = a_values.At(index, a_row.vanillaDefault);
            char formatted[64]{};
            if (!FormatValue(formatted, sizeof(formatted), a_row.type, value)) {
                return false;
            }
            return Append(a_out, a_capacity, used, " ") &&
                   Append(a_out, a_capacity, used, formatted);
        }

        case Tier::kConsoleComposite: {
            constexpr std::size_t kMaxArgs = 16U;
            if (a_row.argCount == 0U || a_row.argCount > kMaxArgs) {
                return false;
            }

            double slotValues[kMaxArgs]{};
            bool slotFilled[kMaxArgs]{};
            ValueType slotTypes[kMaxArgs]{};

            VisitCatalogue([&](const CatalogueRow& a_other, std::size_t a_index) {
                if (a_other.tier != Tier::kConsoleComposite ||
                    std::strcmp(a_other.command, a_row.command) != 0 ||
                    a_other.argIndex >= a_row.argCount) {
                    return;
                }
                slotValues[a_other.argIndex] = a_values.At(a_index, a_other.vanillaDefault);
                slotTypes[a_other.argIndex] = a_other.type;
                slotFilled[a_other.argIndex] = true;
            });

            for (std::size_t slot = 0U; slot < a_row.argCount; ++slot) {
                if (!slotFilled[slot]) {
                    a_out[0] = '\0';
                    return false;
                }
                char formatted[64]{};
                if (!FormatValue(formatted, sizeof(formatted), slotTypes[slot], slotValues[slot])) {
                    a_out[0] = '\0';
                    return false;
                }
                if (!Append(a_out, a_capacity, used, " ") ||
                    !Append(a_out, a_capacity, used, formatted)) {
                    a_out[0] = '\0';
                    return false;
                }
            }
            return true;
        }

        case Tier::kOwnedAddress:
        case Tier::kOwnedSetting:
        case Tier::kOwnedModule:
        default:
            a_out[0] = '\0';
            return false;
        }
    }
}
