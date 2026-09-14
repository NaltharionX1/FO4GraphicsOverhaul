#pragma once

#include <cstdint>

namespace Platform::AoRenderSwitch
{
    void BindFromHandler(const void* a_handler) noexcept;

    [[nodiscard]] bool Bound() noexcept;

    bool Write(bool a_render) noexcept;

}
