#pragma once

#include <span>

namespace RE
{
    struct SCRIPT_FUNCTION;
}

namespace Platform::EngineSwitches
{
    void BindAll(std::span<RE::SCRIPT_FUNCTION> a_functions) noexcept;

    [[nodiscard]] bool Owns(const char* a_command) noexcept;

    bool Write(const char* a_command, bool a_on) noexcept;
}
