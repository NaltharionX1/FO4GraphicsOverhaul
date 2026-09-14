#pragma once

#include <cstdint>

namespace Platform::AoMailbox
{
    enum class Point : std::uint8_t
    {
        kOff = 0,
        kGtao = 2,
    };

    [[nodiscard]] const char* PointName(Point a_point) noexcept;

    void Publish(Point a_point, bool a_enabled) noexcept;

    void Republish() noexcept;

    void Pump(std::uint64_t a_frameId) noexcept;

    struct Applied
    {
        Point requested;
    };

    [[nodiscard]] Applied Snapshot() noexcept;

}
