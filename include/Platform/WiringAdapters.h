#pragma once

#include "Platform/AaMailbox.h"
#include "Platform/SamplerGeneration.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <concepts>
#include <cstdint>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

namespace Platform
{

    struct D3D11SamplerDescFields
    {
        std::uint32_t filter{ 0 };
        std::uint32_t addressU{ 0 };
        std::uint32_t addressV{ 0 };
        std::uint32_t addressW{ 0 };
        float mipLodBias{ 0.0F };
        std::uint32_t maxAnisotropy{ 0 };
        std::uint32_t comparisonFunc{ 0 };
        std::array<float, 4> borderColor{ 0.0F, 0.0F, 0.0F, 0.0F };
        float minLod{ 0.0F };
        float maxLod{ 0.0F };

        [[nodiscard]] friend bool operator==(
            const D3D11SamplerDescFields&, const D3D11SamplerDescFields&) = default;
    };

    static_assert(sizeof(D3D11SamplerDescFields) == kSamplerDescriptorBytes,
        "D3D11SamplerDescFields must be byte-for-byte the same size as the real D3D11_SAMPLER_DESC "
        "(52 bytes) and Platform::SamplerDescriptor (SamplerGeneration.h)");
    static_assert(std::is_standard_layout_v<D3D11SamplerDescFields>,
        "D3D11SamplerDescFields must be standard-layout for offsetof()/reinterpret_cast parity with "
        "the real D3D11_SAMPLER_DESC to be well-defined");
    static_assert(offsetof(D3D11SamplerDescFields, filter) == 0, "Filter must be at byte offset 0");
    static_assert(offsetof(D3D11SamplerDescFields, addressU) == 4, "AddressU must be at byte offset 4");
    static_assert(offsetof(D3D11SamplerDescFields, addressV) == 8, "AddressV must be at byte offset 8");
    static_assert(offsetof(D3D11SamplerDescFields, addressW) == 12, "AddressW must be at byte offset 12");
    static_assert(offsetof(D3D11SamplerDescFields, mipLodBias) == 16, "MipLODBias must be at byte offset 16");
    static_assert(offsetof(D3D11SamplerDescFields, maxAnisotropy) == 20, "MaxAnisotropy must be at byte offset 20");
    static_assert(offsetof(D3D11SamplerDescFields, comparisonFunc) == 24, "ComparisonFunc must be at byte offset 24");
    static_assert(offsetof(D3D11SamplerDescFields, borderColor) == 28, "BorderColor must be at byte offset 28");
    static_assert(offsetof(D3D11SamplerDescFields, minLod) == 44, "MinLOD must be at byte offset 44");
    static_assert(offsetof(D3D11SamplerDescFields, maxLod) == 48, "MaxLOD must be at byte offset 48");

    inline constexpr std::uint32_t kD3D11FilterAnisotropic = 0x55U;

    [[nodiscard]] SamplerDescriptor PackSamplerDescriptor(const D3D11SamplerDescFields& fields) noexcept;
    [[nodiscard]] D3D11SamplerDescFields UnpackSamplerDescriptor(const SamplerDescriptor& descriptor) noexcept;

    [[nodiscard]] bool MakeBiasedDescriptor(
        const SamplerDescriptor& source, float bias, SamplerDescriptor& output) noexcept;

    inline constexpr AppliedSamplerEngine kAppliedSamplerEngineNone =
        static_cast<AppliedSamplerEngine>(0xFFU);

    [[nodiscard]] AppliedSamplerIntent DeriveSamplerIntent(
        const AaRequest& appliedSnapshot, std::uint32_t damagedScope) noexcept;

    class FrameStamp
    {
    public:
        FrameStamp() noexcept = default;
        explicit FrameStamp(std::uint64_t initial) noexcept;

        FrameStamp(const FrameStamp&) = delete;
        FrameStamp& operator=(const FrameStamp&) = delete;
        FrameStamp(FrameStamp&&) = delete;
        FrameStamp& operator=(FrameStamp&&) = delete;

        [[nodiscard]] std::uint64_t Next() noexcept;

        [[nodiscard]] std::uint64_t Current() const noexcept;

    private:
        std::atomic<std::uint64_t> counter_{ 0 };
    };

    template <class T>
    class PublishedSnapshot
    {
    public:
        static_assert(std::is_nothrow_default_constructible_v<T>,
            "PublishedSnapshot<T>'s default sentinel requires noexcept default construction");
        static_assert(std::is_copy_constructible_v<T>,
            "PublishedSnapshot<T> publication requires copy construction from a captured value");

        enum class PublishResult : std::uint8_t
        {
            kPublished,
            kUnchanged,
            kFailed,
        };

        using Factory = std::function<std::shared_ptr<const T>(const T&)>;

        explicit PublishedSnapshot(Factory factory = {}) noexcept
            : factory_(std::move(factory)), node_(DefaultNode())
        {
        }

        PublishedSnapshot(const PublishedSnapshot&) = delete;
        PublishedSnapshot& operator=(const PublishedSnapshot&) = delete;
        PublishedSnapshot(PublishedSnapshot&&) = delete;
        PublishedSnapshot& operator=(PublishedSnapshot&&) = delete;

        void Publish(const T& value) noexcept
        {
            try {
                std::shared_ptr<const T> fresh = factory_ ? factory_(value) : std::make_shared<const T>(value);
                if (fresh) {
                    node_.store(std::move(fresh), std::memory_order_release);
                }
            } catch (...) {
            }
        }

        [[nodiscard]] PublishResult PublishIfChanged(const T& value) noexcept
        {
            static_assert(requires(const T& lhs, const T& rhs) {
                { lhs == rhs } -> std::convertible_to<bool>;
            }, "PublishedSnapshot<T>::PublishIfChanged requires equality-comparable values");
            try {
                const std::shared_ptr<const T> current = node_.load(std::memory_order_acquire);
                if (current && *current == value) {
                    return PublishResult::kUnchanged;
                }

                std::shared_ptr<const T> fresh = factory_ ? factory_(value) : std::make_shared<const T>(value);
                if (!fresh) {
                    return PublishResult::kFailed;
                }

                node_.store(std::move(fresh), std::memory_order_release);
                return PublishResult::kPublished;
            } catch (...) {
                return PublishResult::kFailed;
            }
        }

        [[nodiscard]] std::shared_ptr<const T> Read() const noexcept
        {
            return node_.load(std::memory_order_acquire);
        }

    private:
        [[nodiscard]] static std::shared_ptr<const T> DefaultNode() noexcept
        {
            static const T defaultValue{};
            return std::shared_ptr<const T>(std::shared_ptr<void>{}, &defaultValue);
        }

        Factory factory_;
        std::atomic<std::shared_ptr<const T>> node_;
    };
}
