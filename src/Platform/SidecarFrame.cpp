#include "Platform/SidecarFrame.h"

#include <atomic>

namespace
{
    struct Record
    {
        Platform::SidecarFrame::Guides guides{};
        Platform::SidecarFrame::Hudless hudless{};
        std::uint64_t guidesSerial{ 0 };
        std::uint64_t hudlessSerial{ 0 };
        bool guidesLive{ false };
        bool hudlessLive{ false };
        std::uint64_t frame{ 0 };
        std::uint64_t guidesFrame{ 0 };
        std::uint64_t hudlessFrame{ 0 };
        std::atomic<std::uint64_t> staleRefused{ 0 };
        std::atomic<std::uint64_t> snapGuidesSerial{ 0 };
        std::atomic<std::uint64_t> snapHudlessSerial{ 0 };
        std::atomic<bool> snapGuidesLive{ false };
        std::atomic<bool> snapHudlessLive{ false };
        std::atomic<std::uint64_t> guidesTaken{ 0 };
        std::atomic<std::uint64_t> hudlessTaken{ 0 };
    };
    Record g;

    [[nodiscard]] bool ValidGuides(const Platform::SidecarFrame::Guides& a_guides) noexcept
    {
        return a_guides.depth != nullptr && a_guides.mv != nullptr && a_guides.depthWidth != 0 &&
               a_guides.depthHeight != 0 && a_guides.mvWidth != 0 && a_guides.mvHeight != 0 &&
               a_guides.renderWidth != 0 && a_guides.renderHeight != 0;
    }

    [[nodiscard]] bool ValidHudless(const Platform::SidecarFrame::Hudless& a_hudless) noexcept
    {
        return a_hudless.colour != nullptr && a_hudless.width != 0 && a_hudless.height != 0 &&
               a_hudless.format != DXGI_FORMAT_UNKNOWN;
    }
}

namespace Platform::SidecarFrame
{
    void BeginFrame(std::uint64_t a_frame) noexcept
    {
        g.frame = a_frame;
    }

    void PublishGuides(const Guides& a_guides) noexcept
    {
        if (!ValidGuides(a_guides)) {
            InvalidateGuides();
            return;
        }
        g.guides = a_guides;
        g.guidesFrame = g.frame;
        g.guidesLive = true;
        ++g.guidesSerial;
        g.snapGuidesSerial.store(g.guidesSerial, std::memory_order_relaxed);
        g.snapGuidesLive.store(true, std::memory_order_relaxed);
    }

    void PublishHudless(const Hudless& a_hudless) noexcept
    {
        if (!ValidHudless(a_hudless)) {
            InvalidateHudless();
            return;
        }
        g.hudless = a_hudless;
        g.hudlessFrame = g.frame;
        g.hudlessLive = true;
        ++g.hudlessSerial;
        g.snapHudlessSerial.store(g.hudlessSerial, std::memory_order_relaxed);
        g.snapHudlessLive.store(true, std::memory_order_relaxed);
    }

    void InvalidateGuides() noexcept
    {
        g.guides = Guides{};
        g.guidesLive = false;
        g.snapGuidesLive.store(false, std::memory_order_relaxed);
    }

    void InvalidateHudless() noexcept
    {
        g.hudless = Hudless{};
        g.hudlessLive = false;
        g.snapHudlessLive.store(false, std::memory_order_relaxed);
    }

    void Invalidate() noexcept
    {
        InvalidateGuides();
        InvalidateHudless();
    }

    bool TakeGuides(Cursor& a_cursor, Guides& a_out) noexcept
    {
        if (!g.guidesLive || g.guidesSerial == a_cursor.serial) {
            a_out = Guides{};
            return false;
        }
        if (g.guidesFrame != g.frame) {
            a_out = Guides{};
            g.staleRefused.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        a_cursor.serial = g.guidesSerial;
        a_out = g.guides;
        g.guidesTaken.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    bool TakeHudless(Cursor& a_cursor, Hudless& a_out) noexcept
    {
        if (!g.hudlessLive || g.hudlessSerial == a_cursor.serial) {
            a_out = Hudless{};
            return false;
        }
        if (g.hudlessFrame != g.frame) {
            a_out = Hudless{};
            g.staleRefused.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        a_cursor.serial = g.hudlessSerial;
        a_out = g.hudless;
        g.hudlessTaken.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    State Snapshot() noexcept
    {
        State state{};
        state.guidesSerial = g.snapGuidesSerial.load(std::memory_order_relaxed);
        state.hudlessSerial = g.snapHudlessSerial.load(std::memory_order_relaxed);
        state.guidesLive = g.snapGuidesLive.load(std::memory_order_relaxed);
        state.hudlessLive = g.snapHudlessLive.load(std::memory_order_relaxed);
        state.guidesTaken = g.guidesTaken.load(std::memory_order_relaxed);
        state.staleRefused = g.staleRefused.load(std::memory_order_relaxed);
        state.hudlessTaken = g.hudlessTaken.load(std::memory_order_relaxed);
        return state;
    }
}
