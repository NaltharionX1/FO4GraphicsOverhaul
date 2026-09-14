#pragma once

#include "RE/Bethesda/Script.h"

#include <cstdint>
#include <span>

namespace Platform::ImagespaceOverride
{
    void Bind(std::span<RE::SCRIPT_FUNCTION> a_functions) noexcept;

    [[nodiscard]] bool Owns(const char* a_command) noexcept;

}
