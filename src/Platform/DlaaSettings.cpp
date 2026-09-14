#include "PCH.h"

#include "Platform/DlaaSettings.h"
#include "Platform/DlaaSettingsIO.h"

#include <filesystem>

namespace Platform
{
    namespace
    {
        DlaaSettings g_current{};

        [[nodiscard]] std::filesystem::path IniPath() noexcept
        {
            wchar_t buffer[MAX_PATH]{};
            HMODULE mod{ nullptr };
            if (!::GetModuleHandleExW(
                    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    reinterpret_cast<LPCWSTR>(&IniPath), &mod)) {
                return {};
            }
            const DWORD len = ::GetModuleFileNameW(mod, buffer, static_cast<DWORD>(std::size(buffer)));
            if (len == 0 || len >= static_cast<DWORD>(std::size(buffer))) {
                return {};
            }
            std::filesystem::path path{ buffer };
            if (!path.is_absolute()) {
                return {};
            }
            return path.parent_path() / L"FO4GraphicsOverhaul.DLAA.ini";
        }
    }

    const DlaaSettings& CurrentDlaaSettings() noexcept { return g_current; }

    void SaveDlaaSettings(const DlaaSettings& a_settings) noexcept
    {
        const auto save = DlaaSettingsIO::Save(IniPath(), a_settings);
        g_current = save.liveSettings;
        if (save.persisted) {
            logger::info("[DLAA] settings saved: enable={} preset={} mipBias={:.2f} autoExposure={} sharpness={:.2f}",
                g_current.enable, g_current.preset, g_current.mipBias, g_current.autoExposure,
                g_current.sharpness);
        } else {
            logger::error("[DLAA] settings persistence failed; clamped live values remain active; disk file unchanged");
        }
    }

    DlaaSettings LoadDlaaSettings() noexcept
    {
        DlaaSettings raw{};
        try {
            const auto path = IniPath();
            if (!path.empty()) {
                std::error_code ec;
                const bool present = std::filesystem::exists(path, ec);
                if (!present && !ec) {
                    (void)DlaaSettingsIO::Save(path, raw);
                }
                raw = DlaaSettingsIO::Load(path, raw);
            }
        } catch (...) {
        }

        g_current = ClampDlaaSettings(raw);
        logger::info("[DLAA] settings: enable={} preset={} mipBias={:.2f} autoExposure={} sharpness={:.2f}",
            g_current.enable, g_current.preset, g_current.mipBias, g_current.autoExposure, g_current.sharpness);
        return g_current;
    }
}
