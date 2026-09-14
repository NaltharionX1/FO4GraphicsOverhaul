#pragma once

namespace Hooks
{
    [[nodiscard]] void* PatchIAT(const char* a_dllName, const char* a_funcName, void* a_detour) noexcept;
}

namespace Hooks::D3DHook
{
    bool Install();
}
