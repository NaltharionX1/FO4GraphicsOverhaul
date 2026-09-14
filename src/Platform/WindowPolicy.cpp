// SPDX-License-Identifier: GPL-3.0-or-later
// Portions adapted from High FPS Physics Fix, Copyright (c) 2025 AntoniX35, MIT License.

#include "PCH.h"

#include "Platform/WindowPolicy.h"

#include "Platform/SettingsRuler.h"

#include <atomic>
#include <mutex>

namespace
{
    std::mutex g_mutex;
    Platform::WindowPolicy::Status g_status{};

    std::atomic<bool> g_cursorLock{ false };

    using PFN_DisableProcessWindowsGhosting = void(WINAPI*)();

    [[nodiscard]] bool DisableGhosting() noexcept
    {
        const HMODULE user32 = ::GetModuleHandleW(L"user32.dll");
        if (!user32) {
            logger::warn("[Window] user32.dll not loaded — DisableProcessWindowsGhosting skipped (fail-open)");
            return false;
        }
        const auto fn = reinterpret_cast<PFN_DisableProcessWindowsGhosting>(
            reinterpret_cast<void*>(::GetProcAddress(user32, "DisableProcessWindowsGhosting")));
        if (!fn) {
            logger::warn("[Window] DisableProcessWindowsGhosting not exported — skipped (fail-open)");
            return false;
        }
        fn();
        return true;
    }
}

namespace Platform
{
    void WindowPolicy::ApplyLoadTime() noexcept
    {
        const bool displayForced = SettingsRuler::ApplyReadSitePatches();
        if (displayForced) {
            logger::info("[Window] display mode FORCED to borderless windowed — the game's own bFull Screen / bBorderless are no longer read. The presenting chain cannot do exclusive fullscreen: AMD's frame-interpolation chain carries ALLOW_TEARING and forwards the mode switch untouched, which shows as a black screen. Your fullscreen setting is delivered as borderless fullscreen.");
        }
        const bool fullscreenOff = displayForced;
        const bool borderlessOn = displayForced;

        const bool ghosting = DisableGhosting();
        logger::info("[Window] DisableProcessWindowsGhosting {} — Windows will not paint a \"not responding\" ghost over a busy frame",
            ghosting ? "applied" : "NOT applied");

        {
            const std::scoped_lock lock{ g_mutex };
            g_status.ran = true;
            g_status.fullscreenForcedOff = fullscreenOff;
            g_status.borderlessForcedOn = borderlessOn;
            g_status.ghostingDisabled = ghosting;
        }
    }

    void WindowPolicy::SetCursorLockEnabled(bool a_enabled) noexcept
    {
        const bool was = g_cursorLock.exchange(a_enabled, std::memory_order_acq_rel);
        {
            const std::scoped_lock lock{ g_mutex };
            g_status.cursorLockEnabled = a_enabled;
        }
        if (was != a_enabled) {
            logger::info("[Window] cursor lock {} — the cursor {} confined to the game window while it has focus",
                a_enabled ? "enabled" : "disabled", a_enabled ? "is" : "is no longer");
        }
    }

    bool WindowPolicy::CursorLockEnabled() noexcept
    {
        return g_cursorLock.load(std::memory_order_acquire);
    }

    WindowPolicy::Status WindowPolicy::CurrentStatus() noexcept
    {
        const std::scoped_lock lock{ g_mutex };
        return g_status;
    }
}
