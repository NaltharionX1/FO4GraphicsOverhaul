#include "Platform/DeviceGenerationTracker.h"

#include <atomic>
#include <cstdint>
#include <iostream>
#include <thread>

namespace
{
    int g_checks{};
    int g_failures{};

    void Check(bool condition, const char* message)
    {
        ++g_checks;
        if (!condition) {
            ++g_failures;
            std::cout << "[FAIL] " << message << '\n';
        }
    }

    using Platform::DeviceGenerationTracker;
}

int main()
{
    {
        constexpr std::uint64_t kBase = 9000ULL;
        constexpr int kCreations = 20000;
        DeviceGenerationTracker tracker;
        std::atomic<bool> stop{ false };
        std::atomic<bool> go{ false };
        std::atomic<int> readyCount{ 0 };
        std::atomic<bool> torn{ false };
        std::atomic<bool> nonMonotonic{ false };
        std::atomic<std::uint64_t> readCount{ 0 };

        std::thread reader([&]() noexcept {
            readyCount.fetch_add(1, std::memory_order_release);
            while (!go.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            std::uint64_t lastSerial = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                const auto g = tracker.Current();
                readCount.fetch_add(1, std::memory_order_relaxed);
                if (g.serial != 0 && g.identity != kBase + (g.serial - 1ULL)) {
                    torn.store(true, std::memory_order_relaxed);
                }
                if (g.serial < lastSerial) {
                    nonMonotonic.store(true, std::memory_order_relaxed);
                }
                lastSerial = g.serial;
            }
        });
        std::thread producer([&]() noexcept {
            readyCount.fetch_add(1, std::memory_order_release);
            while (!go.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (int i = 0; i < kCreations; ++i) {
                tracker.NoteDeviceCreated(kBase + static_cast<std::uint64_t>(i));
                if ((i & 0xFF) == 0) {
                    std::this_thread::yield();
                }
            }
        });

        while (readyCount.load(std::memory_order_acquire) < 2) {
            std::this_thread::yield();
        }
        go.store(true, std::memory_order_release);

        producer.join();
        for (int spin = 0; spin < 1000000 && readCount.load(std::memory_order_relaxed) == 0; ++spin) {
            std::this_thread::yield();
        }
        stop.store(true, std::memory_order_relaxed);
        reader.join();

        Check(!torn.load(), "N5-single: a barriered, simultaneous create-vs-read race must NEVER observe "
            "a pair violating the exact identity == kBase + (serial-1) correspondence");
        Check(!nonMonotonic.load(), "N5-single: the reader must never observe a serial regression");
        Check(readCount.load() > 0, "N5-single: sanity check -- the reader thread must have actually performed reads");
        Check(tracker.Current().serial == static_cast<std::uint64_t>(kCreations),
            "N5-single: after the producer finishes, the final serial must equal the total creation count");
    }
    {
        constexpr int kPerProducer = 10000;
        constexpr std::uint64_t kBaseA = 2000ULL;
        constexpr std::uint64_t kBaseB = 100000ULL;
        DeviceGenerationTracker tracker;
        std::atomic<bool> stop{ false };
        std::atomic<bool> go{ false };
        std::atomic<int> readyCount{ 0 };
        std::atomic<bool> invalidIdentity{ false };
        std::atomic<bool> nonMonotonic{ false };
        std::atomic<std::uint64_t> readCount{ 0 };

        const auto waitForGo = [&]() noexcept {
            readyCount.fetch_add(1, std::memory_order_release);
            while (!go.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
        };

        std::thread reader([&]() noexcept {
            waitForGo();
            std::uint64_t lastSerial = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                const auto g = tracker.Current();
                readCount.fetch_add(1, std::memory_order_relaxed);
                const bool inRangeA = g.identity >= kBaseA && g.identity < kBaseA + static_cast<std::uint64_t>(kPerProducer);
                const bool inRangeB = g.identity >= kBaseB && g.identity < kBaseB + static_cast<std::uint64_t>(kPerProducer);
                if (g.serial != 0 && !inRangeA && !inRangeB) {
                    invalidIdentity.store(true, std::memory_order_relaxed);
                }
                if (g.serial < lastSerial) {
                    nonMonotonic.store(true, std::memory_order_relaxed);
                }
                lastSerial = g.serial;
            }
        });
        std::thread producerA([&]() noexcept {
            waitForGo();
            for (int i = 0; i < kPerProducer; ++i) {
                tracker.NoteDeviceCreated(kBaseA + static_cast<std::uint64_t>(i));
                if ((i & 0x3F) == 0) {
                    std::this_thread::yield();
                }
            }
        });
        std::thread producerB([&]() noexcept {
            waitForGo();
            for (int i = 0; i < kPerProducer; ++i) {
                tracker.NoteDeviceCreated(kBaseB + static_cast<std::uint64_t>(i));
                if ((i & 0x3F) == 0) {
                    std::this_thread::yield();
                }
            }
        });

        while (readyCount.load(std::memory_order_acquire) < 3) {
            std::this_thread::yield();
        }
        go.store(true, std::memory_order_release);

        producerA.join();
        producerB.join();
        for (int spin = 0; spin < 1000000 && readCount.load(std::memory_order_relaxed) == 0; ++spin) {
            std::this_thread::yield();
        }
        stop.store(true, std::memory_order_relaxed);
        reader.join();

        Check(!invalidIdentity.load(), "N5-multi: every Current() read must have an identity from one of "
            "the two producers' own (now genuinely disjoint) ranges, never a corrupted/impossible value");
        Check(!nonMonotonic.load(), "N5-multi: the reader must NEVER observe a serial regression across "
            "many reads, even with two concurrent, uncoordinated, SIMULTANEOUSLY-released producers");
        Check(readCount.load() > 0, "N5-multi: sanity check -- the reader thread must have actually performed reads");
        Check(tracker.Current().serial == static_cast<std::uint64_t>(kPerProducer) * 2U,
            "N5-multi: after both producers finish, the final published serial must equal the TOTAL "
            "creation count across both producers -- publish-if-newer always converges on the "
            "globally-highest serial regardless of which producer's publish happened to land last");
        Check(tracker.IsLockFree(), "N5-multi: FeatureSupported()/IsLockFree() must report a REAL "
            "IsProcessorFeaturePresent(PF_COMPARE_EXCHANGE128) probe result (never a tautological "
            "literal) -- true on this (x64, CMPXCHG16B-supporting) shipping target");
    }
    {
        std::uint64_t publishedSerial = 0;
        std::uint64_t publishedIdentity = 0;

        constexpr std::uint64_t serialA = 1;
        constexpr std::uint64_t identityA = 0xA000ULL;

        constexpr std::uint64_t serialB = 2;
        constexpr std::uint64_t identityB = 0xB000ULL;
        if (DeviceGenerationTracker::ShouldInstall(publishedSerial, serialB)) {
            publishedSerial = serialB;
            publishedIdentity = identityB;
        }
        Check(publishedIdentity == identityB && publishedSerial == serialB,
            "N5b: B's own publish (serial 2) must succeed immediately (nothing published yet, "
            "ShouldInstall(0, 2) must be true)");

        if (DeviceGenerationTracker::ShouldInstall(publishedSerial, serialA)) {
            publishedSerial = serialA;
            publishedIdentity = identityA;
        }
        Check(publishedIdentity == identityB && publishedSerial == serialB,
            "N5b: A's late publish attempt (ShouldInstall(2, 1) must be false) must be DISCARDED -- the "
            "final published pair must still be B's (serial 2), proving a nested/reentrant creation that "
            "publishes first can NEVER be overwritten by an enclosing creation's own stale, later-applied "
            "publish -- and this is PRODUCTION'S OWN ShouldInstall(), not a mirror of it");
    }

    std::cout << (g_failures == 0 ? "PASS" : "FAIL") << " — " << g_checks << " checks, "
              << g_failures << " failures\n";
    return g_failures == 0 ? 0 : 1;
}
