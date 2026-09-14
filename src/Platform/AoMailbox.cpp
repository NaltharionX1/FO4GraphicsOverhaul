#include "PCH.h"

#include "Platform/AoMailbox.h"

#include "Platform/AoGate.h"
#include "Platform/AoRenderSwitch.h"
#include "Platform/AoSlot.h"

#include <atomic>

namespace
{
    using Platform::AoMailbox::Point;

    struct Intent
    {
        Point point{ Point::kOff };
        bool enabled{ false };
        std::uint64_t serial{ 0 };
    };

    [[nodiscard]] constexpr std::uint64_t EncodeIntent(
        Point a_point, bool a_enabled, std::uint64_t a_serial) noexcept
    {
        return (a_serial << 8U) | (static_cast<std::uint64_t>(a_point) << 1U) |
               (a_enabled ? 1ULL : 0ULL);
    }

    [[nodiscard]] constexpr Intent DecodeIntent(std::uint64_t a_packed) noexcept
    {
        Intent intent{};
        intent.enabled = (a_packed & 1ULL) != 0ULL;
        intent.point = static_cast<Point>((a_packed >> 1U) & 0x3ULL);
        intent.serial = a_packed >> 8U;
        return intent;
    }

    std::atomic<std::uint64_t> g_intent{ 0 };
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
        "the intent snapshot must publish without a lock — the UI publishes from the render thread");

    std::atomic<std::uint64_t> g_appliedSerial{ 0 };

}

namespace Platform::AoMailbox
{
    const char* PointName(Point a_point) noexcept
    {
        switch (a_point) {
        case Point::kGtao:
            return "GTAO";
        case Point::kOff:
        default:
            return "Off";
        }
    }

    void Publish(Point a_point, bool a_enabled) noexcept
    {
        const std::uint64_t previous = g_intent.load(std::memory_order_relaxed);
        const std::uint64_t serial = DecodeIntent(previous).serial + 1U;
        g_intent.store(EncodeIntent(a_point, a_enabled, serial), std::memory_order_release);
    }

    void Republish() noexcept
    {
        const Intent intent = DecodeIntent(g_intent.load(std::memory_order_relaxed));
        g_intent.store(EncodeIntent(intent.point, intent.enabled, intent.serial + 1U),
            std::memory_order_release);
        logger::info("[AO] intent re-published (scene reload) — switch #2 is re-asserted on the "
                     "next frame, whatever the engine's load path did to its object.");
    }

    void Pump(std::uint64_t a_frameId) noexcept
    {
        (void)a_frameId;

        const Intent intent = DecodeIntent(g_intent.load(std::memory_order_acquire));
        if (intent.serial == g_appliedSerial.load(std::memory_order_relaxed)) {
            return;
        }

        if (!Platform::AoRenderSwitch::Bound()) {
            return;
        }

        const bool wantGtao = intent.enabled && intent.point == Point::kGtao;
        const bool wantAo = intent.enabled && intent.point != Point::kOff;

        Platform::AoSlot::SetGtaoProduction(wantGtao);

        if (!Platform::AoRenderSwitch::Write(wantAo)) {
            return;
        }
        g_appliedSerial.store(intent.serial, std::memory_order_relaxed);
        logger::info("[AO] transition -> {} (switch #2 written directly — no console)",
            PointName(wantAo ? intent.point : Point::kOff));

        Platform::AoGate::RetireEngineProducers();
    }

    Applied Snapshot() noexcept
    {
        const Intent intent = DecodeIntent(g_intent.load(std::memory_order_acquire));

        Applied applied{};
        applied.requested = intent.point;
        return applied;
    }
}
