#include "PCH.h"

#include "Platform/AdaptiveSync.h"

#pragma warning(push, 0)
#include <nvapi.h>
#pragma warning(pop)

#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <cstring>
#include <mutex>
#include <string>

namespace
{
    using namespace Platform;

    int CaptureCode(unsigned long a_code, unsigned long* a_out) noexcept
    {
        *a_out = a_code;
        return EXCEPTION_EXECUTE_HANDLER;
    }

    int SehInitialize(unsigned long* a_code) noexcept
    {
        __try {
            return static_cast<int>(NvAPI_Initialize());
        } __except (CaptureCode(GetExceptionCode(), a_code)) {
            return -1;
        }
    }

    int SehDisplayIdByName(const char* a_name, NvU32* a_id, unsigned long* a_code) noexcept
    {
        __try {
            return static_cast<int>(NvAPI_DISP_GetDisplayIdByDisplayName(a_name, a_id));
        } __except (CaptureCode(GetExceptionCode(), a_code)) {
            return -1;
        }
    }

    int SehGetCaps(NvU32 a_id, NV_MONITOR_CAPABILITIES* a_caps, unsigned long* a_code) noexcept
    {
        __try {
            return static_cast<int>(NvAPI_DISP_GetMonitorCapabilities(a_id, a_caps));
        } __except (CaptureCode(GetExceptionCode(), a_code)) {
            return -1;
        }
    }

    int SehGetAdaptive(NvU32 a_id, NV_GET_ADAPTIVE_SYNC_DATA* a_data, unsigned long* a_code) noexcept
    {
        __try {
            return static_cast<int>(NvAPI_DISP_GetAdaptiveSyncData(a_id, a_data));
        } __except (CaptureCode(GetExceptionCode(), a_code)) {
            return -1;
        }
    }

    int SehSetAdaptive(NvU32 a_id, NV_SET_ADAPTIVE_SYNC_DATA* a_data, unsigned long* a_code) noexcept
    {
        __try {
            return static_cast<int>(NvAPI_DISP_SetAdaptiveSyncData(a_id, a_data));
        } __except (CaptureCode(GetExceptionCode(), a_code)) {
            return -1;
        }
    }

    constexpr float kAssumedFloorMs = 1000.0F / 48.0F;
    constexpr std::uint32_t kAssumedMinimumUs = 4000U;

    struct Guts
    {
        std::mutex mutex;
        AdaptiveSync::State state{};
        bool nvapiTried{ false };
        bool nvapiOk{ false };
        bool faulted{ false };
        NvU32 displayId{ 0 };
        bool disableFlag{ false };
        bool frameSplitFlag{ false };
        bool everSet{ false };
        std::uint32_t panelFloorUs{ 0 };
        bool exitHooked{ false };
        bool displayResolved{ false };
        bool setFaulted{ false };
        bool clampLogged{ false };
        std::uint32_t refusedUs{ 0 };
        std::uint32_t refreshPeriodUs{ 0 };
        std::atomic<std::uint32_t> floorUs{ 0 };
        char displayName[64]{};
    };
    Guts g;

    void SetReason(const char* a_text) noexcept
    {
        std::snprintf(g.state.reason, sizeof(g.state.reason), "%s", a_text != nullptr ? a_text : "");
    }

    void Fault(const char* a_call, unsigned long a_code) noexcept
    {
        g.faulted = true;
        g.state.available = false;
        char text[120]{};
        std::snprintf(text, sizeof(text), "%s faulted inside the driver (code %#lx) — adaptive-sync control is off for the session", a_call, a_code);
        SetReason(text);
        logger::error("[VRR] {}", text);
    }

    [[nodiscard]] bool EnsureNvapi() noexcept
    {
        if (g.faulted) {
            return false;
        }
        if (g.nvapiTried) {
            return g.nvapiOk;
        }
        g.nvapiTried = true;
        unsigned long code = 0;
        const int status = SehInitialize(&code);
        if (code != 0) {
            Fault("NvAPI_Initialize", code);
            return false;
        }
        g.nvapiOk = status == static_cast<int>(NVAPI_OK);
        if (!g.nvapiOk) {
            char text[120]{};
            std::snprintf(text, sizeof(text), "NvAPI_Initialize failed (%d) — no NVIDIA driver NVAPI here; the panel is not observed and the fix has nothing to write", status);
            SetReason(text);
            logger::info("[VRR] {}", text);
        }
        return g.nvapiOk;
    }

    [[nodiscard]] bool WriteInterval(std::uint32_t a_intervalUs, const char* a_why) noexcept
    {
        NV_SET_ADAPTIVE_SYNC_DATA data{};
        data.version = NV_SET_ADAPTIVE_SYNC_DATA_VER;
        data.maxFrameInterval = a_intervalUs;
        data.maxFrameIntervalNs = static_cast<NvU64>(a_intervalUs) * 1000ULL;
        data.bDisableAdaptiveSync = g.disableFlag ? 1U : 0U;
        data.bDisableFrameSplitting = g.frameSplitFlag ? 1U : 0U;
        unsigned long code = 0;
        const int status = SehSetAdaptive(g.displayId, &data, &code);
        if (code != 0) {
            g.setFaulted = true;
            Fault("NvAPI_DISP_SetAdaptiveSyncData", code);
            return false;
        }
        if (status != static_cast<int>(NVAPI_OK)) {
            g.refusedUs = a_intervalUs;
            char text[160]{};
            if (status == static_cast<int>(NVAPI_INVALID_ARGUMENT)) {
                std::snprintf(text, sizeof(text), "the driver would not hold a maximum frame interval of %u us (status -5): the usual cause is variable refresh being OFF for this game (G-SYNC disabled in the NVIDIA Control Panel), in which case there is no flicker to fix and nothing to hold", a_intervalUs);
                SetReason(text);
                logger::info("[VRR] {} — {}", text, a_why);
            } else {
                std::snprintf(text, sizeof(text), "the driver refused the maximum frame interval %u us (NvAPI status %d)", a_intervalUs, status);
                SetReason(text);
                logger::warn("[VRR] {} — {}", text, a_why);
            }
            return false;
        }
        g.refusedUs = 0;
        g.state.appliedIntervalUs = a_intervalUs;
        g.floorUs.store(a_intervalUs != 0U ? a_intervalUs : g.panelFloorUs, std::memory_order_relaxed);
        if (a_intervalUs != 0U) {
            g.everSet = true;
        }
        logger::info("[VRR] the driver's maximum frame interval on {} is now {} — {}", g.displayName,
            a_intervalUs == 0U ? "the EDID default" : std::to_string(a_intervalUs) + " us", a_why);
        return true;
    }

    void RestoreAtExit() noexcept
    {
        AdaptiveSync::Restore();
    }

    void Reconcile(const char* a_why) noexcept
    {
        if (!g.displayResolved || g.setFaulted) {
            return;
        }
        const bool want = g.state.fixRequested && g.state.capRequested != 0U;
        std::uint32_t wantUs = 0U;
        bool doubled = false;
        if (want) {
            const std::uint32_t periodUs = 1'000'000U / g.state.capRequested;
            const std::uint32_t minimumUs = g.refreshPeriodUs != 0U ? g.refreshPeriodUs : kAssumedMinimumUs;
            wantUs = periodUs / 2U;
            doubled = true;
            if (wantUs < minimumUs) {
                doubled = false;
                wantUs = periodUs >= minimumUs ? periodUs : 0U;
            }
            if (wantUs == 0U && !g.clampLogged) {
                g.clampLogged = true;
                logger::info("[VRR] the G-Sync flicker fix has nothing to hold at a {} fps cap: the panel's mode refreshes every {} us and no floor can sit below that",
                    g.state.capRequested, minimumUs);
            }
        }
        if (want && wantUs != 0U && !g.state.available) {
            return;
        }
        if (wantUs == g.state.appliedIntervalUs) {
            return;
        }
        if (wantUs != 0U && wantUs == g.refusedUs) {
            return;
        }
        if (wantUs == 0U && !g.everSet) {
            return;
        }
        char why[200]{};
        if (wantUs != 0U) {
            std::snprintf(why, sizeof(why), "the G-Sync flicker fix is ON at a %u fps cap: %s, so the driver %s and never hops at a hitch (%s)",
                g.state.capRequested,
                doubled ? "half the cap period" : "the cap period (the cap is above half the panel's refresh, so no doubling)",
                doubled ? "shows every frame twice on a steady beat" : "repeats only a late frame", a_why);
        } else {
            std::snprintf(why, sizeof(why), "restored (%s)", a_why);
        }
        if (WriteInterval(wantUs, why) && wantUs != 0U && !g.exitHooked) {
            g.exitHooked = true;
            std::atexit(RestoreAtExit);
        }
    }
}

namespace Platform::AdaptiveSync
{
    void Observe(void* a_hwnd) noexcept
    {
        try {
            const std::scoped_lock lock{ g.mutex };
            if (g.state.observed) {
                return;
            }
            g.state.observed = true;
            if (!EnsureNvapi()) {
                return;
            }
            const HMONITOR monitor = ::MonitorFromWindow(static_cast<HWND>(a_hwnd), MONITOR_DEFAULTTONEAREST);
            MONITORINFOEXW info{};
            info.cbSize = sizeof(info);
            if (monitor == nullptr || ::GetMonitorInfoW(monitor, &info) == 0) {
                SetReason("the game window's monitor could not be resolved");
                logger::warn("[VRR] {}", g.state.reason);
                return;
            }
            const int written = ::WideCharToMultiByte(CP_ACP, 0, info.szDevice, -1, g.displayName, sizeof(g.displayName), nullptr, nullptr);
            if (written <= 0) {
                SetReason("the monitor's device name could not be narrowed");
                logger::warn("[VRR] {}", g.state.reason);
                return;
            }
            DEVMODEW mode{};
            mode.dmSize = sizeof(mode);
            if (::EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode) != 0 && mode.dmDisplayFrequency > 1U) {
                g.state.refreshHz = mode.dmDisplayFrequency;
                g.refreshPeriodUs = 1'000'000U / mode.dmDisplayFrequency;
            }
            unsigned long code = 0;
            int status = SehDisplayIdByName(g.displayName, &g.displayId, &code);
            if (code != 0) {
                Fault("NvAPI_DISP_GetDisplayIdByDisplayName", code);
                return;
            }
            if (status != static_cast<int>(NVAPI_OK)) {
                char text[120]{};
                std::snprintf(text, sizeof(text), "the driver has no display id for %s (NvAPI status %d) — not an NVIDIA-driven display", g.displayName, status);
                SetReason(text);
                logger::info("[VRR] {}", text);
                return;
            }
            g.displayResolved = true;

            NV_MONITOR_CAPABILITIES caps{};
            caps.version = NV_MONITOR_CAPABILITIES_VER;
            caps.infoType = NV_MONITOR_CAPS_TYPE_GENERIC;
            caps.size = sizeof(caps);
            status = SehGetCaps(g.displayId, &caps, &code);
            if (code != 0) {
                Fault("NvAPI_DISP_GetMonitorCapabilities", code);
                return;
            }
            const bool capsOk = status == static_cast<int>(NVAPI_OK) && caps.bIsValidInfo != 0U;
            if (capsOk) {
                g.state.vrrCapable = caps.data.caps.supportVRR != 0U;
                g.state.trueGsync = caps.data.caps.isTrueGsync != 0U;
                g.state.vrrActiveNow = caps.data.caps.currentlyCapableOfVRR != 0U;
            }

            NV_GET_ADAPTIVE_SYNC_DATA get{};
            get.version = NV_GET_ADAPTIVE_SYNC_DATA_VER;
            status = SehGetAdaptive(g.displayId, &get, &code);
            if (code != 0) {
                Fault("NvAPI_DISP_GetAdaptiveSyncData", code);
                return;
            }
            if (status != static_cast<int>(NVAPI_OK)) {
                char text[120]{};
                std::snprintf(text, sizeof(text), "the driver would not report adaptive-sync data for %s (NvAPI status %d)", g.displayName, status);
                SetReason(text);
                logger::info("[VRR] {}", text);
                return;
            }
            g.disableFlag = get.bDisableAdaptiveSync != 0U;
            g.frameSplitFlag = get.bDisableFrameSplitting != 0U;
            g.state.adaptiveSyncDisabled = g.disableFlag;
            g.state.driverMaxIntervalUs = get.maxFrameInterval;
            g.state.lastFlipRepeats = get.lastFlipRefreshCount;
            g.state.available = true;
            SetReason("");
            logger::info("[VRR] display {} (id {:#x}, the mode refreshes at {} Hz): variable refresh {} ({}), active on this mode: {}; adaptive sync {}; the driver's maximum frame interval reads {}; last flip shown {} time(s)",
                g.displayName, g.displayId, g.state.refreshHz,
                capsOk ? (g.state.vrrCapable ? "supported" : "NOT supported") : "unknown (no capability data)",
                capsOk ? (g.state.trueGsync ? "a true G-SYNC module" : "an adaptive-sync panel") : "-",
                capsOk ? (g.state.vrrActiveNow ? "yes" : "no") : "unknown",
                g.disableFlag ? "DISABLED by the user" : "enabled",
                get.maxFrameInterval == 0U ? std::string("0 (not reported on this display)")
                                           : std::to_string(get.maxFrameInterval) + " us (the panel's own floor, unless a program set it)",
                get.lastFlipRefreshCount);
            if (get.maxFrameInterval != 0U) {
                g.panelFloorUs = get.maxFrameInterval;
                g.floorUs.store(get.maxFrameInterval, std::memory_order_relaxed);
                const std::uint32_t cap = g.state.capRequested;
                const std::uint32_t ourHalf = cap != 0U ? (1'000'000U / cap) / 2U : 0U;
                const std::uint32_t ourFull = cap != 0U ? (1'000'000U / cap) : 0U;
                if (ourHalf != 0U && (get.maxFrameInterval == ourHalf || get.maxFrameInterval == ourFull)) {
                    g.state.appliedIntervalUs = get.maxFrameInterval;
                    g.everSet = true;
                    logger::warn("[VRR] the driver's maximum frame interval on {} is {} us at launch, which is exactly what this mod writes at a {} fps cap — a previous session left it there (a crash before its restore). Claimed back: kept if the fix asks for the same, restored otherwise",
                        g.displayName, get.maxFrameInterval, cap);
                } else {
                    logger::info("[VRR] the panel's own variable-refresh floor is {} us ({:.1f} Hz) — the driver reports it here, so the cadence counters compare against the real number instead of the 48 Hz assumption",
                        get.maxFrameInterval, 1'000'000.0 / static_cast<double>(get.maxFrameInterval));
                }
            }
            Reconcile("applied at the chain's build");
        } catch (...) {
            SetReason("C++ exception while observing the display");
        }
    }

    void Apply(bool a_fixEnabled, std::uint32_t a_fpsCap) noexcept
    {
        try {
            const std::scoped_lock lock{ g.mutex };
            const bool changed = g.state.fixRequested != a_fixEnabled || g.state.capRequested != a_fpsCap;
            g.state.fixRequested = a_fixEnabled;
            g.state.capRequested = a_fpsCap;
            if (changed) {
                g.clampLogged = false;
                g.refusedUs = 0;
                Reconcile("the setting changed");
            }
        } catch (...) {
        }
    }

    void Restore() noexcept
    {
        try {
            const std::scoped_lock lock{ g.mutex };
            if (g.everSet && g.state.appliedIntervalUs != 0U && g.displayResolved && !g.setFaulted) {
                (void)WriteInterval(0U, "restored to the EDID default");
            }
        } catch (...) {
        }
    }

    std::uint32_t SampleRepeats() noexcept
    {
        try {
            const std::scoped_lock lock{ g.mutex };
            if (!g.state.available) {
                return 0U;
            }
            NV_GET_ADAPTIVE_SYNC_DATA get{};
            get.version = NV_GET_ADAPTIVE_SYNC_DATA_VER;
            unsigned long code = 0;
            const int status = SehGetAdaptive(g.displayId, &get, &code);
            if (code != 0) {
                Fault("NvAPI_DISP_GetAdaptiveSyncData", code);
                return 0U;
            }
            if (status == static_cast<int>(NVAPI_OK)) {
                g.state.lastFlipRepeats = get.lastFlipRefreshCount;
                g.state.adaptiveSyncDisabled = get.bDisableAdaptiveSync != 0U;
            }
            return g.state.lastFlipRepeats;
        } catch (...) {
            return 0U;
        }
    }

    float FloorMs() noexcept
    {
        const std::uint32_t us = g.floorUs.load(std::memory_order_relaxed);
        return us != 0U ? static_cast<float>(us) / 1000.0F : kAssumedFloorMs;
    }

    bool FloorIsMeasured() noexcept
    {
        return g.floorUs.load(std::memory_order_relaxed) != 0U;
    }

    State Snapshot() noexcept
    {
        try {
            const std::scoped_lock lock{ g.mutex };
            return g.state;
        } catch (...) {
            return {};
        }
    }
}
