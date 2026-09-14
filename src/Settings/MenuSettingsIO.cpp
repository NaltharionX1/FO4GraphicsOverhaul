#include "Settings/MenuSettingsIO.h"

#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace Settings
{
    std::uint32_t ClampHotkey(std::uint32_t a_vk) noexcept
    {
        return (a_vk >= 1 && a_vk <= 254) ? a_vk : MenuSettings{}.openHotkey;
    }

    std::uint32_t ClampOptionalHotkey(std::uint32_t a_vk) noexcept
    {
        return (a_vk >= 1 && a_vk <= 254) ? a_vk : 0U;
    }

    std::uint32_t ClampMenuStyle(std::uint32_t a_style) noexcept
    {
        return a_style <= 2 ? a_style : MenuSettings{}.menuStyle;
    }

    float ClampFontScale(float a_scale) noexcept
    {
        return (std::isfinite(a_scale) && a_scale >= 0.5F && a_scale <= 2.0F)
                   ? a_scale
                   : MenuSettings{}.fontScale;
    }

    float ClampWindowCoord(float a_coord) noexcept
    {
        if (!std::isfinite(a_coord) || a_coord < -kMaxWindowExtent || a_coord > kMaxWindowExtent) {
            return 0.0F;
        }
        return a_coord;
    }

    float ClampWindowSize(float a_size) noexcept
    {
        if (!std::isfinite(a_size) || a_size < kMinWindowSize || a_size > kMaxWindowExtent) {
            return 0.0F;
        }
        return a_size;
    }

    bool ClampFlag(bool a_flag) noexcept
    {
        return a_flag;
    }

    std::uint32_t ClampOsdAnchor(std::uint32_t a_anchor) noexcept
    {
        return a_anchor <= kOsdAnchorMax ? a_anchor : MenuSettings{}.osdAnchor;
    }

    std::uint32_t ClampAoMode(std::uint32_t a_mode) noexcept
    {
        return a_mode == 2U ? 2U : 0U;
    }

    namespace
    {
        [[nodiscard]] float BoundedOrDefault(
            float a_value, float a_min, float a_max, float a_default) noexcept
        {
            if (!std::isfinite(a_value) || a_value < a_min || a_value > a_max) {
                return a_default;
            }
            return a_value;
        }
    }

    std::uint32_t ClampAoQuality(std::uint32_t a_quality) noexcept
    {
        return a_quality <= 4U ? a_quality : MenuSettings{}.aoQuality;
    }

    std::uint32_t ClampAoDenoisePasses(std::uint32_t a_passes) noexcept
    {
        return a_passes <= 3U ? a_passes : MenuSettings{}.aoDenoisePasses;
    }

    float ClampAoRadius(float a_units) noexcept
    {
        return BoundedOrDefault(a_units, 7.0F, 700.0F, MenuSettings{}.aoRadius);
    }

    float ClampAoRadiusMultiplier(float a_value) noexcept
    {
        return BoundedOrDefault(a_value, 0.3F, 3.0F, MenuSettings{}.aoRadiusMultiplier);
    }

    float ClampAoFalloffRange(float a_value) noexcept
    {
        return BoundedOrDefault(a_value, 0.0F, 1.0F, MenuSettings{}.aoFalloffRange);
    }

    float ClampAoSampleDistributionPower(float a_value) noexcept
    {
        return BoundedOrDefault(a_value, 1.0F, 3.0F, MenuSettings{}.aoSampleDistributionPower);
    }

    float ClampAoOccluderThickness(float a_value) noexcept
    {
        return BoundedOrDefault(a_value, 0.5F, 100.0F, MenuSettings{}.aoOccluderThickness);
    }

    float ClampAoFinalValuePower(float a_value) noexcept
    {
        return BoundedOrDefault(a_value, 0.5F, 5.0F, MenuSettings{}.aoFinalValuePower);
    }

    float ClampAoMinScreenRadius(float a_pixels) noexcept
    {
        return BoundedOrDefault(a_pixels, 0.0F, 16.0F, MenuSettings{}.aoMinScreenRadius);
    }

    float ClampAoDepthFadeStart(float a_units) noexcept
    {
        return BoundedOrDefault(a_units, 1000.0F, 100000.0F, MenuSettings{}.aoDepthFadeStart);
    }

    float ClampAoDepthFadeEnd(float a_units) noexcept
    {
        return BoundedOrDefault(a_units, 1000.0F, 120000.0F, MenuSettings{}.aoDepthFadeEnd);
    }

    std::uint32_t ClampNeuralStyle(std::uint32_t a_style) noexcept
    {
        return Platform::Neural::ClampStyle(a_style);
    }

    float ClampNeuralStrength(float a_value) noexcept
    {
        return Platform::Neural::ClampStrength(a_value, Platform::Neural::Settings{}.intensity);
    }

    std::uint32_t ClampNeuralModelPercent(std::uint32_t a_percent) noexcept
    {
        return Platform::Neural::ClampModelPercent(a_percent);
    }

    float ClampOsdSize(float a_size) noexcept
    {
        if (!std::isfinite(a_size) || a_size < kMinOsdSize || a_size > kMaxWindowExtent) {
            return 0.0F;
        }
        return a_size;
    }

    float ClampOsdFontSize(float a_size) noexcept
    {
        if (!std::isfinite(a_size)) {
            return MenuSettings{}.osdFontSize;
        }
        if (a_size < kOsdFontSizeMin) {
            return kOsdFontSizeMin;
        }
        return a_size > kOsdFontSizeMax ? kOsdFontSizeMax : a_size;
    }

    float ClampColorChannel(float a_channel) noexcept
    {
        if (!std::isfinite(a_channel) || a_channel < 0.0F) {
            return 0.0F;
        }
        return a_channel > 1.0F ? 1.0F : a_channel;
    }

    FontPath ClampFontPath(FontPath a_path) noexcept
    {
        a_path[kFontPathCapacity - 1] = '\0';
        for (std::size_t i = 0; i < kFontPathCapacity; ++i) {
            const unsigned char ch = static_cast<unsigned char>(a_path[i]);
            if (ch == 0) {
                break;
            }
            if (ch < 0x20 || ch == 0x7F) {
                a_path[i] = '\0';
                break;
            }
        }
        return a_path;
    }

    std::uint32_t ClampVSyncInterval(std::uint32_t a_interval) noexcept
    {
        return (a_interval >= 1 && a_interval <= kVSyncIntervalMax) ? a_interval
                                                                    : MenuSettings{}.vsyncInterval;
    }

    std::uint32_t ClampFpsLimit(std::uint32_t a_limit) noexcept
    {
        return (a_limit >= kFpsLimitMin && a_limit <= kFpsLimitMax) ? a_limit
                                                                    : MenuSettings{}.fpsLimit;
    }

    void ClampAll(MenuSettings& a_settings) noexcept
    {
        ForEachField(a_settings, [](const char*, const char*, auto& a_value, auto a_clamp) noexcept {
            a_value = a_clamp(a_value);
        });
        a_settings.neural.modelPercent = Platform::Neural::EffectiveModelPercent(a_settings.neural);
    }
}

namespace Settings::IO
{
    namespace
    {
        GraphicsLoadFn g_graphicsLoad{ nullptr };
        GraphicsSaveFn g_graphicsSave{ nullptr };
    }

    void SetGraphicsHandlers(GraphicsLoadFn a_load, GraphicsSaveFn a_save) noexcept
    {
        g_graphicsLoad = a_load;
        g_graphicsSave = a_save;
    }

    namespace
    {
        [[nodiscard]] std::string Trim(const std::string& a_text)
        {
            const auto first = a_text.find_first_not_of(" \t\r\n");
            if (first == std::string::npos) {
                return {};
            }
            const auto last = a_text.find_last_not_of(" \t\r\n");
            return a_text.substr(first, last - first + 1);
        }

        [[nodiscard]] bool ParseU32(const std::string& a_value, std::uint32_t& a_out) noexcept
        {
            char* end = nullptr;
            const unsigned long parsed = std::strtoul(a_value.c_str(), &end, 10);
            if (end == a_value.c_str() || *end != '\0') {
                return false;
            }
            a_out = static_cast<std::uint32_t>(parsed);
            return true;
        }

        [[nodiscard]] bool ParseFloat(const std::string& a_value, float& a_out) noexcept
        {
            float parsed = 0.0F;
            const char* first = a_value.data();
            const char* last = first + a_value.size();
            const auto result = std::from_chars(first, last, parsed, std::chars_format::general);
            if (result.ec != std::errc{} || result.ptr != last || !std::isfinite(parsed)) {
                return false;
            }
            a_out = parsed;
            return true;
        }

        [[nodiscard]] bool ParseBool(const std::string& a_value, bool& a_out) noexcept
        {
            if (a_value == "1") {
                a_out = true;
                return true;
            }
            if (a_value == "0") {
                a_out = false;
                return true;
            }
            return false;
        }

        void AssignParsed(
            const std::string& a_text, std::uint32_t& a_out,
            std::uint32_t (*a_clamp)(std::uint32_t) noexcept)
        {
            std::uint32_t parsed = 0;
            if (ParseU32(a_text, parsed)) {
                a_out = a_clamp(parsed);
            }
        }

        void AssignParsed(const std::string& a_text, float& a_out, float (*a_clamp)(float) noexcept)
        {
            float parsed = 0.0F;
            if (ParseFloat(a_text, parsed)) {
                a_out = a_clamp(parsed);
            }
        }

        void AssignParsed(const std::string& a_text, bool& a_out, bool (*a_clamp)(bool) noexcept)
        {
            bool parsed = false;
            if (ParseBool(a_text, parsed)) {
                a_out = a_clamp(parsed);
            }
        }

        void AssignParsed(
            const std::string& a_text, FontPath& a_out, FontPath (*a_clamp)(FontPath) noexcept)
        {
            FontPath parsed{};
            if (a_text.size() < kFontPathCapacity) {
                std::memcpy(parsed.data(), a_text.data(), a_text.size());
                parsed[a_text.size()] = '\0';
            }
            a_out = a_clamp(parsed);
        }

        [[nodiscard]] bool AppendValue(std::string& a_out, std::uint32_t a_value)
        {
            char text[16]{};
            const auto result = std::to_chars(text, text + sizeof(text) - 1, a_value);
            if (result.ec != std::errc{}) {
                return false;
            }
            a_out.append(text, static_cast<std::size_t>(result.ptr - text));
            return true;
        }

        [[nodiscard]] bool AppendValue(std::string& a_out, float a_value)
        {
            char text[40]{};
            const auto result = std::to_chars(text, text + sizeof(text) - 1, a_value);
            if (result.ec != std::errc{}) {
                return false;
            }
            a_out.append(text, static_cast<std::size_t>(result.ptr - text));
            return true;
        }

        [[nodiscard]] bool AppendValue(std::string& a_out, bool a_value)
        {
            a_out += a_value ? '1' : '0';
            return true;
        }

        [[nodiscard]] bool AppendValue(std::string& a_out, const FontPath& a_value)
        {
            const char* text = a_value.data();
            const std::size_t length = ::strnlen(text, kFontPathCapacity);
            a_out.append(text, length);
            return true;
        }
    }

    bool Load(const char* a_path, MenuSettings& a_out)
    {
        std::FILE* file = nullptr;
        if (::fopen_s(&file, a_path, "rb") != 0 || !file) {
            return false;
        }

        char line[512];
        std::string section;
        bool firstLine = true;
        while (std::fgets(line, sizeof(line), file)) {
            const char* text = line;
            if (firstLine) {
                firstLine = false;
                if (std::strncmp(text, "\xEF\xBB\xBF", 3) == 0) {
                    text += 3;
                }
            }
            const std::string trimmed = Trim(text);
            if (trimmed.empty() || trimmed.front() == ';' || trimmed.front() == '#') {
                continue;
            }
            if (trimmed.front() == '[') {
                const auto close = trimmed.find(']');
                section = (close != std::string::npos) ? trimmed.substr(1, close - 1) : std::string{};
                continue;
            }
            const auto eq = trimmed.find('=');
            if (eq == std::string::npos) {
                continue;
            }
            const std::string key = Trim(trimmed.substr(0, eq));
            const std::string value = Trim(trimmed.substr(eq + 1));

            if (section == "Graphics") {
                if (g_graphicsLoad != nullptr) {
                    g_graphicsLoad(key.c_str(), value.c_str());
                }
                continue;
            }

            ForEachField(a_out,
                [&](const char* a_section, const char* a_key, auto& a_target, auto a_clamp) {
                    if (key == a_key && section == a_section) {
                        AssignParsed(value, a_target, a_clamp);
                    }
                });
        }

        std::fclose(file);
        ClampAll(a_out);
        return true;
    }

    bool Save(const char* a_path, const MenuSettings& a_settings)
    {
        MenuSettings clamped = a_settings;
        ClampAll(clamped);

        std::string out =
            "; FO4GraphicsOverhaul menu settings — generated by the MenuSettingsIO writer. Do not\n"
            "; hand-edit for packaging: the canonical INI is emitted by the settings driver (doc 05 A.8).\n";

        bool formatted = true;
        std::string section;
        ForEachField(clamped,
            [&](const char* a_section, const char* a_key, auto& a_value, auto) {
                if (section != a_section) {
                    section = a_section;
                    out += '[';
                    out += a_section;
                    out += "]\n";
                }
                out += a_key;
                out += '=';
                if (!AppendValue(out, a_value)) {
                    formatted = false;
                }
                out += '\n';
            });
        if (!formatted) {
            return false;
        }

        if (g_graphicsSave != nullptr) {
            g_graphicsSave(out);
        }

        std::FILE* file = nullptr;
        if (::fopen_s(&file, a_path, "wb") != 0 || !file) {
            return false;
        }
        const bool written = std::fwrite(out.data(), 1, out.size(), file) == out.size();
        const bool closed = std::fclose(file) == 0;
        return written && closed;
    }
}
