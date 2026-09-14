#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace Telemetry
{
    struct Snapshot
    {
        float processCpuPercent{ 0.0F };
        float systemCpuPercent{ 0.0F };
        bool cpuValid{ false };

        std::uint64_t processPrivateBytes{ 0 };
        std::uint64_t systemTotalBytes{ 0 };
        std::uint64_t systemAvailBytes{ 0 };
        bool memValid{ false };

        std::uint64_t vramUsedBytes{ 0 };
        std::uint64_t vramBudgetBytes{ 0 };
        std::uint64_t vramTotalBytes{ 0 };
        bool vramValid{ false };

        std::uint32_t gpuUtilPercent{ 0 };
        std::uint32_t gpuTempC{ 0 };
        bool gpuValid{ false };

        std::uint32_t cpuTempC{ 0 };
        bool cpuTempValid{ false };

        float lastLoadSeconds{ 0.0F };
        bool loadValid{ false };
    };

    static_assert(std::is_trivially_copyable_v<Snapshot>,
        "Snapshot is published through a seqlock and must be memcpy-able");

    class SnapshotStore
    {
    public:
        void Publish(const Snapshot& a_snapshot) noexcept
        {
            seq_.fetch_add(1, std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_release);
            std::memcpy(&data_, &a_snapshot, sizeof(Snapshot));
            std::atomic_thread_fence(std::memory_order_release);
            seq_.fetch_add(1, std::memory_order_relaxed);
        }

        [[nodiscard]] Snapshot Read() const noexcept
        {
            Snapshot out{};
            for (;;) {
                const std::uint32_t before = seq_.load(std::memory_order_acquire);
                if ((before & 1U) != 0U) {
                    continue;
                }
                std::atomic_thread_fence(std::memory_order_acquire);
                std::memcpy(&out, &data_, sizeof(Snapshot));
                std::atomic_thread_fence(std::memory_order_acquire);
                if (seq_.load(std::memory_order_relaxed) == before) {
                    return out;
                }
            }
        }

        [[nodiscard]] std::uint32_t Sequence() const noexcept
        {
            return seq_.load(std::memory_order_acquire);
        }

    private:
        alignas(64) std::atomic<std::uint32_t> seq_{ 0 };
        Snapshot data_{};
    };
}
