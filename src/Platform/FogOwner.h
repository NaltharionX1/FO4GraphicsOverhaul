#pragma once

#include "RE/Bethesda/Script.h"

#include <span>

namespace Platform::FogOwner
{
    void Bind(std::span<RE::SCRIPT_FUNCTION> a_functions) noexcept;

    [[nodiscard]] bool Owns() noexcept;

    bool Write(int a_first, int a_second) noexcept;
}
