#include "Telemetry/FrameTimings.h"
#include "Telemetry/Snapshot.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <thread>

namespace
{
    int g_failures = 0;

    void Check(bool a_condition, const char* a_label)
    {
        if (!a_condition) {
            ++g_failures;
            std::printf("FAIL: %s\n", a_label);
        }
    }

    [[nodiscard]] bool Near(float a_lhs, float a_rhs, float a_epsilon = 0.01F)
    {
        return std::fabs(a_lhs - a_rhs) <= a_epsilon;
    }
}

int main()
{
    {
        Telemetry::FrameTimings ring;
        const auto empty = ring.Compute();
        Check(empty.sampleCount == 0, "empty ring reports no samples");
        Check(empty.averageFps == 0.0F, "empty ring reports zero fps (renders as --)");
    }
    {
        Telemetry::FrameTimings ring;
        for (int i = 0; i < 99; ++i) {
            ring.Push(10.0F);
        }
        ring.Push(100.0F);
        const auto stats = ring.Compute();
        Check(stats.sampleCount == 100, "100 samples retained");
        Check(Near(stats.lastMs, 100.0F), "lastMs is the most recent sample");
        Check(Near(stats.averageMs, 10.9F), "averageMs = (99*10 + 100)/100");
        Check(Near(stats.averageFps, 1000.0F / 10.9F, 0.1F), "averageFps from averageMs");
        Check(Near(stats.onePercentLowFps, 10.0F, 0.1F), "1% low is the mean of the worst 1%");
        Check(Near(stats.pointOnePercentLowFps, 10.0F, 0.1F), "0.1% low clamps to >= 1 sample");
        Check(stats.onePercentLowFps < stats.averageFps, "1% low is worse than the average");
    }
    {
        Telemetry::FrameTimings ring;
        for (std::size_t i = 0; i < Telemetry::FrameTimings::kCapacity; ++i) {
            ring.Push(50.0F);
        }
        for (std::size_t i = 0; i < Telemetry::FrameTimings::kCapacity; ++i) {
            ring.Push(5.0F);
        }
        const auto stats = ring.Compute();
        Check(stats.sampleCount == Telemetry::FrameTimings::kCapacity, "window caps at capacity");
        Check(Near(stats.averageMs, 5.0F), "wrapped ring evicted every old sample");
    }
    {
        Telemetry::FrameTimings ring;
        ring.Push(10.0F);
        ring.Push(std::nanf(""));
        ring.Push(-3.0F);
        ring.Push(0.0F);
        ring.Push(std::numeric_limits<float>::infinity());
        const auto stats = ring.Compute();
        Check(stats.sampleCount == 1, "non-finite and non-positive deltas are rejected");
        Check(Near(stats.averageMs, 10.0F), "rejected samples never reach the average");
    }
    {
        Telemetry::FrameTimings ring;
        ring.Push(10.0F);
        ring.Reset();
        Check(ring.Compute().sampleCount == 0, "Reset discards history");
    }
    {
        Telemetry::FrameTimings ring;
        for (int i = 0; i < 100; ++i) {
            ring.Push(10.0F);
        }
        Check(Near(ring.Compute().intervalRegularityPercent, 0.0F), "an even series is 0 % irregular");
    }
    {
        Telemetry::FrameTimings ring;
        for (int i = 0; i < 100; ++i) {
            ring.Push(i % 2 == 0 ? 10.0F : 20.0F);
        }
        const auto stats = ring.Compute();
        Check(Near(stats.intervalRegularityPercent, 100.0F * 10.0F / 15.0F, 0.1F), "a 10/20 alternation is 66.7 % irregular");
        Check(Near(stats.menuOpenSharePercent, 0.0F), "no menu-open samples: 0 %");
    }
    {
        Telemetry::FrameTimings ring;
        for (std::size_t i = 0; i < Telemetry::FrameTimings::kCapacity; ++i) {
            ring.Push(10.0F);
        }
        ring.Push(30.0F);
        ring.Push(10.0F);
        const auto stats = ring.Compute();
        const float meanMs = (1022.0F * 10.0F + 40.0F) / 1024.0F;
        Check(Near(stats.intervalRegularityPercent, 100.0F * (40.0F / 1023.0F) / meanMs, 0.005F),
            "two real steps across the window and none at the wrap seam");
    }
    {
        Telemetry::FrameTimings ring;
        ring.Push(10.0F, true);
        ring.Push(30.0F, false);
        Check(Near(ring.Compute().menuOpenSharePercent, 25.0F, 0.01F), "menu-open share is the open time over the window's time");
    }

    {
        Telemetry::SnapshotStore store;
        const auto fresh = store.Read();
        Check(!fresh.cpuValid && !fresh.gpuValid && !fresh.memValid, "a never-published store reads as all-invalid");
        Check(!fresh.cpuTempValid, "CPU temperature starts invalid (renders as N/A)");

        Telemetry::Snapshot published{};
        published.processCpuPercent = 12.5F;
        published.cpuValid = true;
        published.gpuTempC = 61;
        published.gpuValid = true;
        store.Publish(published);

        const auto read = store.Read();
        Check(read.cpuValid && Near(read.processCpuPercent, 12.5F), "published CPU value round-trips");
        Check(read.gpuValid && read.gpuTempC == 61, "published GPU value round-trips");
        Check(!read.memValid, "unpublished sources stay invalid independently");
    }
    {
        Telemetry::SnapshotStore store;
        std::atomic<bool> stop{ false };
        std::atomic<std::uint64_t> writes{ 0 };

        std::thread writer([&store, &stop, &writes]() {
            std::uint32_t n = 1;
            while (!stop.load(std::memory_order_relaxed)) {
                Telemetry::Snapshot s{};
                s.gpuUtilPercent = n;
                s.gpuTempC = n;
                s.processPrivateBytes = n;
                s.systemTotalBytes = n;
                s.vramUsedBytes = n;
                s.processCpuPercent = static_cast<float>(n);
                s.cpuValid = true;
                s.gpuValid = true;
                s.memValid = true;
                s.vramValid = true;
                store.Publish(s);
                n = (n % 1000000U) + 1U;
                writes.fetch_add(1, std::memory_order_relaxed);
            }
        });

        using clock = std::chrono::steady_clock;
        const auto deadline = clock::now() + std::chrono::seconds(10);
        while (writes.load(std::memory_order_relaxed) == 0 && clock::now() < deadline) {
            std::this_thread::yield();
        }

        int torn = 0;
        long long observed = 0;
        while (clock::now() < deadline &&
               (writes.load(std::memory_order_relaxed) < 5000 || observed < 200000)) {
            const auto s = store.Read();
            if (!s.cpuValid) {
                continue;
            }
            ++observed;
            const std::uint32_t n = s.gpuUtilPercent;
            if (s.gpuTempC != n || s.processPrivateBytes != n || s.systemTotalBytes != n ||
                s.vramUsedBytes != n || !Near(s.processCpuPercent, static_cast<float>(n), 0.0F)) {
                ++torn;
            }
        }
        stop.store(true, std::memory_order_relaxed);
        writer.join();

        Check(torn == 0, "seqlock never yields a torn snapshot under a hammering writer");
        Check(observed > 0, "the reader actually observed published snapshots");
        Check(writes.load() > 0, "the writer actually published");
    }

    if (g_failures == 0) {
        std::printf("TelemetryTests: all checks passed\n");
        return 0;
    }
    std::printf("TelemetryTests: %d failure(s)\n", g_failures);
    return 1;
}
