#include "Hooks/WndProcHook.h"

#include "Core/FailOpen.h"
#include "Hooks/D3DHook.h"
#include "Platform/FrameFingerprint.h"
#include "Platform/WindowPolicy.h"
#include "Settings/MenuSettingsStore.h"
#include "UI/Menu.h"
#include "UI/Osd.h"

#include <atomic>
#include <mutex>

#include <imgui_impl_win32.h>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace Hooks::WndProcHook
{
    namespace
    {
        using PFN_ClipCursor = BOOL(WINAPI*)(const RECT*);

        std::atomic<WNDPROC> g_originalWndProc{ nullptr };
        std::atomic<HWND> g_subclassedWindow{ nullptr };
        std::atomic<PFN_ClipCursor> g_realClipCursor{ nullptr };

        std::mutex g_installMutex;

        std::mutex g_clipMutex;
        RECT g_lastGameClip{};
        bool g_haveGameClip = false;

        void ReassertGameClip(HWND a_hwnd) noexcept;

        [[nodiscard]] bool ApplyCursorLock(HWND a_hwnd) noexcept
        {
            if (!Platform::WindowPolicy::CursorLockEnabled() || UI::Menu::GetSingleton().IsOpen()) {
                return false;
            }
            const PFN_ClipCursor real = g_realClipCursor.load(std::memory_order_acquire);
            if (!real || !a_hwnd) {
                return false;
            }
            RECT rect{};
            if (!::GetWindowRect(a_hwnd, &rect)) {
                return false;
            }
            return real(&rect) != FALSE;
        }

        void ReleaseCursorLock() noexcept
        {
            if (const PFN_ClipCursor real = g_realClipCursor.load(std::memory_order_acquire)) {
                real(nullptr);
            }
        }

        void ServiceCursorLock(HWND a_hwnd, UINT a_msg, WPARAM a_wParam) noexcept
        {
            if (!Platform::WindowPolicy::CursorLockEnabled()) {
                return;
            }
            switch (a_msg) {
            case WM_SETFOCUS:
            case WM_WINDOWPOSCHANGED:
            case WM_SIZE:
                (void)ApplyCursorLock(a_hwnd);
                break;
            case WM_ACTIVATE: {
                const bool minimized = HIWORD(a_wParam) != 0;
                const WORD state = LOWORD(a_wParam);
                if (state != WA_INACTIVE) {
                    if (!minimized && ::GetFocus() == a_hwnd) {
                        (void)ApplyCursorLock(a_hwnd);
                    }
                } else {
                    ReleaseCursorLock();
                }
                break;
            }
            case WM_KILLFOCUS:
            case WM_DESTROY:
                ReleaseCursorLock();
                break;
            default:
                break;
            }
        }

        LRESULT CALLBACK hk_WndProc(HWND a_hwnd, UINT a_msg, WPARAM a_wParam, LPARAM a_lParam)
        {
            auto& menu = UI::Menu::GetSingleton();

            GUARD_BEGIN
                menu.ServicePendingForcedClose();

                ServiceCursorLock(a_hwnd, a_msg, a_wParam);

                if (a_msg == WM_KEYDOWN || a_msg == WM_SYSKEYDOWN) {
                    const bool wasAlreadyDown = (a_lParam & (1LL << 30)) != 0;
                    if (!wasAlreadyDown) {
                        const auto vk = static_cast<std::uint32_t>(a_wParam);
                        if (menu.OfferRebindKey(vk)) {
                            return 1;
                        }
                        if (vk == Settings::OpenHotkeyMirror().load(std::memory_order_acquire)) {
                            menu.Toggle();
                            if (!menu.IsOpen()) {
                                ReassertGameClip(a_hwnd);
                            }
                        }
                        if (vk == Settings::OsdHotkeyMirror().load(std::memory_order_acquire)) {
                            UI::Osd::Toggle();
                        }
                        const auto fpToggle = Settings::FingerprintToggleHotkeyMirror().load(std::memory_order_acquire);
                        if (fpToggle != 0U && vk == fpToggle) {
                            Platform::FrameFingerprint::SetEnabled(!Platform::FrameFingerprint::Enabled());
                        }
                        const auto fpMark = Settings::FingerprintMarkHotkeyMirror().load(std::memory_order_acquire);
                        if (fpMark != 0U && vk == fpMark) {
                            Platform::FrameFingerprint::MarkIncident();
                        }
                    }
                }

                if (menu.IsOpen()) {
                    std::scoped_lock lock(UI::Menu::InputMutex());
                    if (menu.IsInitialized() &&
                        ImGui_ImplWin32_WndProcHandler(a_hwnd, a_msg, a_wParam, a_lParam)) {
                        return 1;
                    }
                }
            GUARD_END("wndproc.scan")

            if (const WNDPROC original = g_originalWndProc.load(std::memory_order_acquire)) {
                return ::CallWindowProcA(original, a_hwnd, a_msg, a_wParam, a_lParam);
            }
            return ::DefWindowProcA(a_hwnd, a_msg, a_wParam, a_lParam);
        }

        BOOL WINAPI hk_ClipCursor(const RECT* a_rect) noexcept
        {
            const PFN_ClipCursor real = g_realClipCursor.load(std::memory_order_acquire);
            if (!real) {
                return FALSE;
            }

            try {
                std::scoped_lock lock(g_clipMutex);
                if (a_rect) {
                    g_lastGameClip = *a_rect;
                    g_haveGameClip = true;
                } else {
                    g_haveGameClip = false;
                }
            } catch (...) {
            }

            if (UI::Menu::GetSingleton().IsOpen()) {
                return real(nullptr);
            }
            return real(a_rect);
        }

        void ReassertGameClip(HWND a_hwnd) noexcept
        {
            const PFN_ClipCursor real = g_realClipCursor.load(std::memory_order_acquire);
            if (!real) {
                return;
            }
            if (ApplyCursorLock(a_hwnd)) {
                return;
            }
            try {
                RECT rect{};
                bool have = false;
                {
                    std::scoped_lock lock(g_clipMutex);
                    rect = g_lastGameClip;
                    have = g_haveGameClip;
                }
                real(have ? &rect : nullptr);
            } catch (...) {
            }
        }
    }

    void Install(HWND a_hwnd)
    {
        std::scoped_lock installLock(g_installMutex);

        static std::atomic<bool> s_clipArmAttempted{ false };
        if (!s_clipArmAttempted.exchange(true, std::memory_order_acq_rel)) {
            if (const HMODULE user32 = ::GetModuleHandleW(L"user32.dll")) {
                g_realClipCursor.store(
                    reinterpret_cast<PFN_ClipCursor>(::GetProcAddress(user32, "ClipCursor")),
                    std::memory_order_release);
            }
            void* original =
                Hooks::PatchIAT("user32.dll", "ClipCursor", reinterpret_cast<void*>(&hk_ClipCursor));
            if (original) {
                g_realClipCursor.store(
                    reinterpret_cast<PFN_ClipCursor>(original), std::memory_order_release);
                logger::info("[WndProcHook] ClipCursor hook armed");
            } else {
                Core::FailOpen::Trip("clipcursor-hook",
                    "ClipCursor IAT patch failed — cursor may be re-clipped while the menu is open");
            }
        }

        if (!a_hwnd) {
            Core::FailOpen::Trip("wndproc-install",
                "null HWND at subclass time — menu will render but cannot receive input this session");
            return;
        }
        if (g_subclassedWindow.load(std::memory_order_acquire) == a_hwnd) {
            return;
        }

        const LONG_PTR current = ::GetWindowLongPtrA(a_hwnd, GWLP_WNDPROC);
        if (!current) {
            Core::FailOpen::Trip("wndproc-install",
                "GetWindowLongPtrA returned null — the menu cannot be opened this session");
            return;
        }
        if (reinterpret_cast<WNDPROC>(current) == &hk_WndProc) {
            g_subclassedWindow.store(a_hwnd, std::memory_order_release);
            return;
        }
        g_originalWndProc.store(reinterpret_cast<WNDPROC>(current), std::memory_order_release);

        ::SetLastError(0);
        const LONG_PTR previous =
            ::SetWindowLongPtrA(a_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&hk_WndProc));
        if (!previous && ::GetLastError() != 0) {
            g_originalWndProc.store(nullptr, std::memory_order_release);
            Core::FailOpen::Trip("wndproc-install",
                "SetWindowLongPtrA failed — the menu cannot be opened this session");
            return;
        }
        if (previous && previous != current &&
            reinterpret_cast<WNDPROC>(previous) != &hk_WndProc) {
            g_originalWndProc.store(reinterpret_cast<WNDPROC>(previous), std::memory_order_release);
        }

        g_subclassedWindow.store(a_hwnd, std::memory_order_release);
        logger::info("[WndProcHook] game window subclassed, hwnd={}", static_cast<void*>(a_hwnd));
    }
}
