#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace Telemetry
{
    struct FrameStats
    {
        float lastMs{ 0.0F };
        float averageMs{ 0.0F };
        float averageFps{ 0.0F };
        float onePercentLowFps{ 0.0F };
        float pointOnePercentLowFps{ 0.0F };
        float intervalRegularityPercent{ 0.0F };
        float menuOpenSharePercent{ 0.0F };
        std::uint32_t sampleCount{ 0 };
    };

    class FrameTimings
    {
    public:
        static constexpr std::size_t kCapacity = 1024;

        void Push(float a_milliseconds, bool a_menuOpen = false) noexcept;

        [[nodiscard]] FrameStats Compute() const noexcept;

        void Reset() noexcept;

    private:
        std::array<float, kCapacity> samples_{};
        std::array<bool, kCapacity> menuOpen_{};
        std::uint64_t count_{ 0 };
    };
}
