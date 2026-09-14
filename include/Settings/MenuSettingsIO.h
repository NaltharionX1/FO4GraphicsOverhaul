#pragma once

#include "Settings/MenuSettings.h"

#include <string>

namespace Settings::IO
{
    bool Load(const char* a_path, MenuSettings& a_out);

    bool Save(const char* a_path, const MenuSettings& a_settings);

    using GraphicsLoadFn = void (*)(const char* a_key, const char* a_value);
    using GraphicsSaveFn = void (*)(std::string& a_out);
    void SetGraphicsHandlers(GraphicsLoadFn a_load, GraphicsSaveFn a_save) noexcept;
}
