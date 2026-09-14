// SPDX-License-Identifier: GPL-3.0-or-later
// Portions adapted from High FPS Physics Fix, Copyright (c) 2025 AntoniX35, MIT License.

#include "PCH.h"

#include "Platform/HfpfCoexist.h"

#include <mutex>
#include <string>
#include <vector>

namespace
{
    struct Reading
    {
        bool loaded = false;
        std::string version;
        std::string path;
    };

    [[nodiscard]] std::string ReadFileVersion(const std::wstring& a_path) noexcept
    {
        try {
            DWORD ignored = 0;
            const DWORD size = ::GetFileVersionInfoSizeW(a_path.c_str(), &ignored);
            if (size == 0U) {
                return {};
            }
            std::vector<std::uint8_t> buffer(size);
            if (!::GetFileVersionInfoW(a_path.c_str(), 0, size, buffer.data())) {
                return {};
            }
            VS_FIXEDFILEINFO* fixed = nullptr;
            UINT fixedLen = 0;
            if (!::VerQueryValueW(buffer.data(), L"\\", reinterpret_cast<LPVOID*>(&fixed), &fixedLen) ||
                !fixed || fixedLen == 0U) {
                return {};
            }
            return std::to_string(HIWORD(fixed->dwFileVersionMS)) + "." +
                   std::to_string(LOWORD(fixed->dwFileVersionMS)) + "." +
                   std::to_string(HIWORD(fixed->dwFileVersionLS)) + "." +
                   std::to_string(LOWORD(fixed->dwFileVersionLS));
        } catch (...) {
            return {};
        }
    }
}

namespace Platform
{
    void HfpfCoexist::Observe() noexcept
    {
        static std::once_flag once;
        bool ran = false;
        std::call_once(once, [&ran]() { ran = true; });
        if (!ran) {
            return;
        }

        Reading reading{};
        try {
            const HMODULE module = ::GetModuleHandleW(L"HighFPSPhysicsFix.dll");
            if (module) {
                reading.loaded = true;
                wchar_t path[MAX_PATH]{};
                if (::GetModuleFileNameW(module, path, static_cast<DWORD>(std::size(path))) != 0U) {
                    const std::wstring wide{ path };
                    reading.path = std::filesystem::path(wide).string();
                    reading.version = ReadFileVersion(wide);
                }
            }
        } catch (...) {
        }

        if (reading.loaded) {
            logger::info(
                "[HFPF] High FPS Physics Fix IS loaded (version {}, {}). Coexistence: the PRESENT "
                "path is ours by construction (sync interval, tearing and the frame limiter are "
                "resolved inside our Present hook, so we get the last word every frame). Load-time "
                "byte patches are LAST-WRITER-WINS and it patches after us, so ITS physics fixes are "
                "the ones live where we overlap — benign, since ours are adapted from the same "
                "sites. We never overwrite a site that already holds another mod's jump; each "
                "refusal is logged. Uninstall it to run purely on ours.",
                reading.version.empty() ? "unknown" : reading.version,
                reading.path.empty() ? "path unknown" : reading.path);
        } else {
            logger::info("[HFPF] High FPS Physics Fix not loaded — our absorbed features run unopposed");
        }
    }
}
