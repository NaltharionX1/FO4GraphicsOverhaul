#pragma once

#include <cstdint>

namespace RE
{
    class Setting;
}

namespace Platform::SettingsRuler
{
    void Rule() noexcept;

    bool ApplyReadSitePatches() noexcept;

    [[nodiscard]] RE::Setting* ResolveSetting(const char* a_key) noexcept;
}
