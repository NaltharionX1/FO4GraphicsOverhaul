#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace Platform::Neural
{
    enum class GpuFamily : std::uint32_t
    {
        kUnknown = 0,
        kTuring = 1,
        kAmpere = 2,
        kAda = 3,
        kBlackwell = 4,
        kNewer = 5,
    };

    inline constexpr unsigned int kNvidiaVendorId = 0x10DEU;

    [[nodiscard]] GpuFamily FamilyFromAdapter(
        unsigned int a_vendorId, unsigned int a_deviceId, const wchar_t* a_description) noexcept;
    [[nodiscard]] const char* FamilyName(GpuFamily a_family) noexcept;
    [[nodiscard]] const char* FamilyExpectation(GpuFamily a_family) noexcept;

    inline constexpr std::uint32_t kStyleMax = 2;
    inline constexpr std::uint32_t kPresetMax = 3;
    inline constexpr float kStrengthMin = 0.0F;
    inline constexpr float kStrengthMax = 1.0F;
    inline constexpr std::uint32_t kCarrierNative = 1;
    inline constexpr std::uint32_t kCarrierFp16 = 2;

    struct Settings
    {
        std::uint32_t style{ 0 };
        std::uint32_t preset{ 0 };
        float intensity{ 1.0F };
        float localTone{ 1.0F };
        float localStructure{ 1.0F };
        float skinStructure{ 1.0F };
        bool autoMask{ true };

        [[nodiscard]] bool operator==(const Settings&) const noexcept = default;
    };

    struct RuntimeProfile
    {
        const char* sha256;
        const char* label;
        const char* tested;
        bool validated;
    };
    [[nodiscard]] const RuntimeProfile* FindRuntimeProfile(const char* a_sha256) noexcept;

    [[nodiscard]] constexpr bool IsSrgbFormat(std::uint32_t a_dxgiFormat) noexcept
    {
        switch (a_dxgiFormat) {
        case 29U:
        case 72U:
        case 75U:
        case 78U:
        case 91U:
        case 93U:
        case 99U:
            return true;
        default:
            return false;
        }
    }

    [[nodiscard]] constexpr std::uint32_t NonSrgbFormat(std::uint32_t a_dxgiFormat) noexcept
    {
        switch (a_dxgiFormat) {
        case 29U: return 28U;
        case 91U: return 87U;
        case 93U: return 88U;
        case 72U: return 71U;
        case 75U: return 74U;
        case 78U: return 77U;
        case 99U: return 98U;
        default: return a_dxgiFormat;
        }
    }

    struct TimingWindow
    {
        static constexpr std::uint32_t kCapacity = 600;
        float samples[kCapacity]{};
        std::uint32_t count{ 0 };

        void Push(float a_ms) noexcept
        {
            if (count < kCapacity) samples[count++] = a_ms;
        }
        void Clear() noexcept { count = 0; }
        [[nodiscard]] float Percentile(float a_fraction) const noexcept;
    };

    [[nodiscard]] inline double TicksToMs(std::uint64_t a_ticks, std::uint64_t a_frequency) noexcept
    {
        return a_frequency != 0U ? 1000.0 * static_cast<double>(a_ticks) / static_cast<double>(a_frequency) : 0.0;
    }

    struct HistoryEpochConsumer
    {
        std::uint64_t acknowledged{ 0 };
        [[nodiscard]] bool Pending(std::uint64_t a_epoch) const noexcept { return a_epoch != acknowledged; }
        void Acknowledge(std::uint64_t a_epoch) noexcept { acknowledged = a_epoch; }
    };

    inline constexpr std::uint32_t kMaxPasses = 3;
    inline constexpr std::uint32_t kAllPasses = (1U << kMaxPasses) - 1U;
    struct CascadeSettings
    {
        bool enabled{ false };
        std::uint32_t passCount{ 1 };
        std::array<Settings, kMaxPasses> passes{};
        std::uint32_t modelPercent{ 100 };
        bool modelBeyondPlay{ false };

        [[nodiscard]] bool operator==(const CascadeSettings&) const noexcept = default;
    };

    inline constexpr std::uint32_t kModelPercentMin = 50;
    inline constexpr std::uint32_t kModelPercentMax = 300;
    inline constexpr std::uint32_t kModelPercentDefault = 100;
    inline constexpr std::uint32_t kModelPercentPlayMax = 150;
    inline constexpr std::uint32_t kModelExtentFloor = 32;

    [[nodiscard]] inline std::uint32_t ClampModelPercent(std::uint32_t a_percent) noexcept
    {
        return a_percent >= kModelPercentMin && a_percent <= kModelPercentMax ? a_percent : kModelPercentDefault;
    }

    [[nodiscard]] inline std::uint32_t EffectiveModelPercent(const CascadeSettings& a_settings) noexcept
    {
        const std::uint32_t percent = ClampModelPercent(a_settings.modelPercent);
        return !a_settings.modelBeyondPlay && percent > kModelPercentPlayMax ? kModelPercentPlayMax : percent;
    }

    struct ModelExtent
    {
        std::uint32_t width{ 0 };
        std::uint32_t height{ 0 };
        [[nodiscard]] bool Valid() const noexcept { return width != 0U && height != 0U; }
        [[nodiscard]] bool operator==(const ModelExtent&) const noexcept = default;
    };
    [[nodiscard]] inline ModelExtent ComputeModelExtent(std::uint32_t a_outputWidth, std::uint32_t a_outputHeight,
        std::uint32_t a_modelPercent) noexcept
    {
        if (a_outputWidth == 0U || a_outputHeight == 0U) {
            return {};
        }
        const std::uint64_t percent = ClampModelPercent(a_modelPercent);
        const std::uint64_t w = (static_cast<std::uint64_t>(a_outputWidth) * percent + 50ULL) / 100ULL;
        const std::uint64_t h = (static_cast<std::uint64_t>(a_outputHeight) * percent + 50ULL) / 100ULL;
        if (w < kModelExtentFloor || h < kModelExtentFloor || w > 0xFFFFFFFFULL || h > 0xFFFFFFFFULL) {
            return {};
        }
        return { static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h) };
    }

    [[nodiscard]] inline std::uint32_t ClampPassCount(std::uint32_t a_count) noexcept
    {
        return a_count >= 1U && a_count <= kMaxPasses ? a_count : 1U;
    }

    [[nodiscard]] std::uint32_t ActiveMask(const CascadeSettings& a_settings) noexcept;
    [[nodiscard]] std::uint32_t RecreateMask(const CascadeSettings& a_before,
        const CascadeSettings& a_after) noexcept;
    [[nodiscard]] std::uint32_t HistoryResetMask(const CascadeSettings& a_before,
        const CascadeSettings& a_after) noexcept;

    [[nodiscard]] inline std::uint32_t HistoryTailMask(std::uint32_t a_first) noexcept
    {
        return a_first < kMaxPasses ? kAllPasses & ~((1U << a_first) - 1U) : 0U;
    }

    [[nodiscard]] inline bool CanUseStageOutput(bool a_evaluated, bool a_listAccepted,
        std::uint64_t a_fence, bool a_outputOrdered) noexcept
    {
        return a_evaluated && a_listAccepted && a_fence != 0U && a_outputOrdered;
    }

    template <class Before, class Execute>
    [[nodiscard]] std::uint32_t RunCascade(const CascadeSettings& a_settings, std::uint32_t a_limit,
        Before&& a_before, Execute&& a_execute) noexcept
    {
        static_assert(noexcept(a_before(std::uint32_t{})) && noexcept(a_execute(std::uint32_t{})));
        if (!a_settings.enabled) return 0;
        std::uint32_t completed = 0;
        const std::uint32_t count = ClampPassCount(a_settings.passCount) < a_limit ? ClampPassCount(a_settings.passCount) : a_limit;
        for (std::uint32_t i = 0; i < count; ++i) {
            a_before(i);
            if (!a_execute(i)) break;
            ++completed;
        }
        return completed;
    }

    template <class Before, class Execute>
    [[nodiscard]] std::uint32_t RunCascade(const CascadeSettings& a_settings,
        Before&& a_before, Execute&& a_execute) noexcept
    {
        return RunCascade(a_settings, kMaxPasses, static_cast<Before&&>(a_before), static_cast<Execute&&>(a_execute));
    }

    [[nodiscard]] std::uint32_t ClampStyle(std::uint32_t a_style) noexcept;
    [[nodiscard]] std::uint32_t ClampPreset(std::uint32_t a_preset) noexcept;
    [[nodiscard]] float ClampStrength(float a_value, float a_default) noexcept;
    [[nodiscard]] bool RequiresRecreate(const Settings& a_current, const Settings& a_next) noexcept;

    struct Shape
    {
        std::uint32_t outWidth{ 0 };
        std::uint32_t outHeight{ 0 };
        std::uint32_t renderWidth{ 0 };
        std::uint32_t renderHeight{ 0 };

        [[nodiscard]] bool operator==(const Shape&) const noexcept = default;
    };

    struct Resources
    {
        void* color{ nullptr };
        void* output{ nullptr };
        void* depth{ nullptr };
        void* mvec{ nullptr };
    };

    enum class ValueType : std::uint8_t
    {
        kUInt,
        kInt,
        kFloat,
        kResource,
    };

    struct KeyValue
    {
        const char* key{ nullptr };
        ValueType type{ ValueType::kUInt };
        unsigned int u{ 0 };
        int i{ 0 };
        float f{ 0.0F };
        void* p{ nullptr };
    };

    inline constexpr std::size_t kMaxParams = 64;

    struct ParamList
    {
        KeyValue items[kMaxParams]{};
        std::size_t count{ 0 };

        [[nodiscard]] const KeyValue* Find(const char* a_key) const noexcept;
    };

    [[nodiscard]] ParamList BuildParams(const Shape& a_shape, const Settings& a_settings,
        const Resources& a_resources, bool a_reset) noexcept;
}
