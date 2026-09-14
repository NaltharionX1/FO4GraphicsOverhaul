#include "Telemetry/FrameTimings.h"

#include <algorithm>
#include <cmath>

namespace Telemetry
{
    void FrameTimings::Push(float a_milliseconds, bool a_menuOpen) noexcept
    {
        if (!std::isfinite(a_milliseconds) || a_milliseconds <= 0.0F) {
            return;
        }
        samples_[count_ & (kCapacity - 1)] = a_milliseconds;
        menuOpen_[count_ & (kCapacity - 1)] = a_menuOpen;
        ++count_;
    }

    void FrameTimings::Reset() noexcept
    {
        count_ = 0;
        samples_.fill(0.0F);
        menuOpen_.fill(false);
    }

    FrameStats FrameTimings::Compute() const noexcept
    {
        FrameStats stats{};
        const std::size_t retained =
            static_cast<std::size_t>(count_ < kCapacity ? count_ : kCapacity);
        if (retained == 0) {
            return stats;
        }

        stats.sampleCount = static_cast<std::uint32_t>(retained);
        stats.lastMs = samples_[(count_ - 1) & (kCapacity - 1)];

        std::array<float, kCapacity> work{};
        double sum = 0.0;
        for (std::size_t i = 0; i < retained; ++i) {
            work[i] = samples_[i];
            sum += static_cast<double>(samples_[i]);
        }

        stats.averageMs = static_cast<float>(sum / static_cast<double>(retained));
        stats.averageFps = stats.averageMs > 0.0F ? 1000.0F / stats.averageMs : 0.0F;

        {
            const bool wrapped = count_ >= kCapacity;
            const std::size_t start = wrapped ? static_cast<std::size_t>(count_ & (kCapacity - 1)) : 0;
            double stepSum = 0.0;
            double menuMs = 0.0;
            std::size_t steps = 0;
            for (std::size_t j = 0; j < retained; ++j) {
                const std::size_t idx = (start + j) & (kCapacity - 1);
                if (menuOpen_[idx]) {
                    menuMs += static_cast<double>(samples_[idx]);
                }
                if (j != 0) {
                    const std::size_t prev = (start + j - 1) & (kCapacity - 1);
                    stepSum += std::fabs(static_cast<double>(samples_[idx]) - static_cast<double>(samples_[prev]));
                    ++steps;
                }
            }
            stats.intervalRegularityPercent = (steps != 0 && stats.averageMs > 0.0F)
                ? static_cast<float>(100.0 * (stepSum / static_cast<double>(steps)) / static_cast<double>(stats.averageMs))
                : 0.0F;
            stats.menuOpenSharePercent = sum > 0.0 ? static_cast<float>(100.0 * menuMs / sum) : 0.0F;
        }

        const auto lowFps = [&work, retained](double a_fraction) noexcept -> float {
            auto worst = static_cast<std::size_t>(
                std::ceil(static_cast<double>(retained) * a_fraction));
            if (worst == 0) {
                worst = 1;
            }
            if (worst > retained) {
                worst = retained;
            }
            const auto first = work.begin();
            const auto split = first + static_cast<std::ptrdiff_t>(retained - worst);
            const auto last = first + static_cast<std::ptrdiff_t>(retained);
            std::nth_element(first, split, last);
            double total = 0.0;
            for (auto it = split; it != last; ++it) {
                total += static_cast<double>(*it);
            }
            const double meanMs = total / static_cast<double>(worst);
            return meanMs > 0.0 ? static_cast<float>(1000.0 / meanMs) : 0.0F;
        };

        stats.onePercentLowFps = lowFps(0.01);
        stats.pointOnePercentLowFps = lowFps(0.001);
        return stats;
    }
}
