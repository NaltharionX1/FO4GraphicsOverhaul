#pragma once

#include <atomic>
#include <cstdint>
#include <type_traits>

#include <intrin.h>
#include <Windows.h>
#pragma intrinsic(_InterlockedCompareExchange128)

namespace Platform
{
    struct DeviceGeneration
    {
        std::uint64_t identity{ 0 };
        std::uint64_t serial{ 0 };

        friend bool operator==(const DeviceGeneration&, const DeviceGeneration&) noexcept = default;
    };

    class DeviceGenerationTracker
    {
    public:
        DeviceGenerationTracker() noexcept = default;

        [[nodiscard]] static bool FeatureSupported() noexcept
        {
            static const bool supported = ::IsProcessorFeaturePresent(PF_COMPARE_EXCHANGE128) != 0;
            return supported;
        }

        [[nodiscard]] bool IsLockFree() const noexcept { return FeatureSupported(); }

        [[nodiscard]] static constexpr bool ShouldInstall(
            std::uint64_t currentSerial, std::uint64_t candidateSerial) noexcept
        {
            return candidateSerial > currentSerial;
        }

        void NoteDeviceCreated(std::uint64_t identity) noexcept
        {
            if (!FeatureSupported()) {
                return;
            }
            const std::uint64_t serial = nextSerial_.fetch_add(1, std::memory_order_relaxed) + 1;
            __int64 comparand[2] = { 0, 0 };
            ReadStorage(comparand);
            for (;;) {
                if (!ShouldInstall(static_cast<std::uint64_t>(comparand[1]), serial)) {
                    return;
                }
                const unsigned char exchanged = _InterlockedCompareExchange128(
                    storage_,
                    static_cast<__int64>(serial),
                    static_cast<__int64>(identity),
                    comparand);
                if (exchanged) {
                    return;
                }
            }
        }

        void NoteDeviceObserved(std::uint64_t identity) noexcept
        {
            lastObservedIdentity_ = identity;
            lastObservedIdentitySeeded_ = true;
        }

        [[nodiscard]] DeviceGeneration Current() const noexcept
        {
            if (!FeatureSupported()) {
                return DeviceGeneration{};
            }
            __int64 comparand[2] = { 0, 0 };
            ReadStorage(comparand);
            DeviceGeneration result{};
            result.identity = static_cast<std::uint64_t>(comparand[0]);
            result.serial = static_cast<std::uint64_t>(comparand[1]);
            return result;
        }

        [[nodiscard]] std::uint64_t LastObservedIdentity() const noexcept { return lastObservedIdentity_; }
        [[nodiscard]] bool HasObserved() const noexcept { return lastObservedIdentitySeeded_; }

    private:
        void ReadStorage(__int64 (&comparand)[2]) const noexcept
        {
            _InterlockedCompareExchange128(storage_, comparand[1], comparand[0], comparand);
        }

        alignas(16) mutable volatile __int64 storage_[2]{ 0, 0 };
        std::atomic<std::uint64_t> nextSerial_{ 0 };
        std::uint64_t lastObservedIdentity_{ 0 };
        bool lastObservedIdentitySeeded_{ false };
    };
}
