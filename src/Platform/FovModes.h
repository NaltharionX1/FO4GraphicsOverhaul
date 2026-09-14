#pragma once

namespace Platform::FovModes
{
    void Apply() noexcept;

    void OnMenuBoundary(const char* a_menuName, bool a_opening) noexcept;

    void OnLoadBoundary() noexcept;

    void Tick() noexcept;
}
