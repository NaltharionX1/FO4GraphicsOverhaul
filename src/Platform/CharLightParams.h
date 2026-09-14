#pragma once

#include "RE/Bethesda/Script.h"

#include <span>

namespace Platform::CharLightParams
{
    void Bind(std::span<RE::SCRIPT_FUNCTION> a_functions) noexcept;

    [[nodiscard]] bool Owns(const char* a_command) noexcept;

    bool Write(const char* a_command, float a_value) noexcept;
}
