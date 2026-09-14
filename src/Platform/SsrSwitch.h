#pragma once

#include "RE/Bethesda/Script.h"

#include <span>

namespace Platform::SsrSwitch
{
    void Bind(std::span<RE::SCRIPT_FUNCTION> a_functions) noexcept;

    [[nodiscard]] bool Owns() noexcept;

    [[nodiscard]] bool IntentEnabled() noexcept;

    [[nodiscard]] bool EngineDemand() noexcept;

    bool Write(bool a_on) noexcept;
}
