#pragma once

#include "RE/Bethesda/Script.h"

#include <span>

namespace Platform::FovOwner
{
    void Bind(std::span<RE::SCRIPT_FUNCTION> a_functions) noexcept;

    [[nodiscard]] bool Owns() noexcept;

    bool Write(float a_firstPersonFov, float a_thirdPersonFov, float a_viewModelFov) noexcept;

    bool Current(float& a_firstPersonFov, float& a_thirdPersonFov) noexcept;
}
