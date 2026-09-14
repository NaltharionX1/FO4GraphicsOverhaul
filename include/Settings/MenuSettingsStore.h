#pragma once

#include "Settings/MenuSettings.h"

#include <atomic>
#include <string>

namespace Settings
{
    [[nodiscard]] inline MenuSettings& Menu() noexcept
    {
        static MenuSettings instance;
        return instance;
    }

    [[nodiscard]] inline std::string& RuntimeIniPath() noexcept
    {
        static std::string path;
        return path;
    }

    [[nodiscard]] inline std::atomic<std::uint32_t>& OpenHotkeyMirror() noexcept
    {
        static std::atomic<std::uint32_t> mirror{ MenuSettings{}.openHotkey };
        return mirror;
    }

    [[nodiscard]] inline std::atomic<std::uint32_t>& OsdHotkeyMirror() noexcept
    {
        static std::atomic<std::uint32_t> mirror{ MenuSettings{}.osdHotkey };
        return mirror;
    }

    [[nodiscard]] inline std::atomic<bool>& OsdEnabledMirror() noexcept
    {
        static std::atomic<bool> mirror{ MenuSettings{}.osdEnabled };
        return mirror;
    }

    [[nodiscard]] inline std::atomic<std::uint32_t>& FingerprintToggleHotkeyMirror() noexcept
    {
        static std::atomic<std::uint32_t> mirror{ MenuSettings{}.fingerprintToggleHotkey };
        return mirror;
    }

    [[nodiscard]] inline std::atomic<std::uint32_t>& FingerprintMarkHotkeyMirror() noexcept
    {
        static std::atomic<std::uint32_t> mirror{ MenuSettings{}.fingerprintMarkHotkey };
        return mirror;
    }

    [[nodiscard]] inline std::atomic<bool>& DirtyFlag() noexcept
    {
        static std::atomic<bool> dirty{ false };
        return dirty;
    }

    inline void MarkDirty() noexcept { DirtyFlag().store(true, std::memory_order_release); }

    [[nodiscard]] inline bool TakeDirty() noexcept
    {
        return DirtyFlag().exchange(false, std::memory_order_acq_rel);
    }

    inline void RepublishMirrors() noexcept
    {
        OpenHotkeyMirror().store(Menu().openHotkey, std::memory_order_release);
        OsdHotkeyMirror().store(Menu().osdHotkey, std::memory_order_release);
        OsdEnabledMirror().store(Menu().osdEnabled, std::memory_order_release);
        FingerprintToggleHotkeyMirror().store(Menu().fingerprintToggleHotkey, std::memory_order_release);
        FingerprintMarkHotkeyMirror().store(Menu().fingerprintMarkHotkey, std::memory_order_release);
    }
}
