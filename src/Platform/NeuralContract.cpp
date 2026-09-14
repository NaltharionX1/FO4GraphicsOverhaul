#include "Platform/NeuralContract.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwchar>

namespace Platform::Neural
{
    namespace
    {
        [[nodiscard]] bool Contains(const wchar_t* a_haystack, const wchar_t* a_needle) noexcept
        {
            if (a_haystack == nullptr || a_needle == nullptr) {
                return false;
            }
            return std::wcsstr(a_haystack, a_needle) != nullptr;
        }

        void Push(ParamList& a_list, const char* a_key, unsigned int a_value) noexcept
        {
            if (a_list.count < kMaxParams) {
                a_list.items[a_list.count++] = KeyValue{ a_key, ValueType::kUInt, a_value, 0, 0.0F, nullptr };
            }
        }

        void Push(ParamList& a_list, const char* a_key, int a_value) noexcept
        {
            if (a_list.count < kMaxParams) {
                a_list.items[a_list.count++] = KeyValue{ a_key, ValueType::kInt, 0U, a_value, 0.0F, nullptr };
            }
        }

        void Push(ParamList& a_list, const char* a_key, float a_value) noexcept
        {
            if (a_list.count < kMaxParams) {
                a_list.items[a_list.count++] = KeyValue{ a_key, ValueType::kFloat, 0U, 0, a_value, nullptr };
            }
        }

        void PushResource(ParamList& a_list, const char* a_key, void* a_value) noexcept
        {
            if (a_list.count < kMaxParams) {
                a_list.items[a_list.count++] = KeyValue{ a_key, ValueType::kResource, 0U, 0, 0.0F, a_value };
            }
        }

        void PushSubrect(ParamList& a_list, const char* a_baseX, const char* a_baseY,
            const char* a_width, const char* a_height, std::uint32_t a_w, std::uint32_t a_h) noexcept
        {
            Push(a_list, a_baseX, 0);
            Push(a_list, a_baseY, 0);
            Push(a_list, a_width, static_cast<int>(a_w));
            Push(a_list, a_height, static_cast<int>(a_h));
        }
    }

    GpuFamily FamilyFromAdapter(
        unsigned int a_vendorId, unsigned int a_deviceId, const wchar_t* a_description) noexcept
    {
        if (a_vendorId != kNvidiaVendorId) {
            return GpuFamily::kUnknown;
        }
        if ((a_deviceId >= 0x1E00U && a_deviceId <= 0x1FFFU) ||
            (a_deviceId >= 0x2180U && a_deviceId <= 0x21FFU)) {
            return GpuFamily::kTuring;
        }
        if (a_deviceId >= 0x2200U && a_deviceId <= 0x25FFU) {
            return GpuFamily::kAmpere;
        }
        if (a_deviceId >= 0x2600U && a_deviceId <= 0x28FFU) {
            return GpuFamily::kAda;
        }
        if (a_deviceId >= 0x2B80U && a_deviceId <= 0x2DFFU) {
            return GpuFamily::kBlackwell;
        }
        if (Contains(a_description, L"RTX 50")) {
            return GpuFamily::kBlackwell;
        }
        if (Contains(a_description, L"RTX 40")) {
            return GpuFamily::kAda;
        }
        if (Contains(a_description, L"RTX 30")) {
            return GpuFamily::kAmpere;
        }
        if (Contains(a_description, L"RTX 20") || Contains(a_description, L"GTX 16")) {
            return GpuFamily::kTuring;
        }
        return a_deviceId > 0x2DFFU ? GpuFamily::kNewer : GpuFamily::kUnknown;
    }

    const char* FamilyName(GpuFamily a_family) noexcept
    {
        switch (a_family) {
        case GpuFamily::kTuring:    return "Turing (RTX 20)";
        case GpuFamily::kAmpere:    return "Ampere (RTX 30)";
        case GpuFamily::kAda:       return "Ada (RTX 40)";
        case GpuFamily::kBlackwell: return "Blackwell (RTX 50)";
        case GpuFamily::kNewer:     return "NVIDIA (newer than Blackwell)";
        default:                    return "unknown / not NVIDIA";
        }
    }

    const char* FamilyExpectation(GpuFamily a_family) noexcept
    {
        switch (a_family) {
        case GpuFamily::kBlackwell:
            return "NVIDIA's target family for this runtime; the measured status decides";
        case GpuFamily::kNewer:
            return "newer than Blackwell: untested here; the measured status decides";
        case GpuFamily::kAda:
            return "community FP8 path; the reference caller's field figure was 130 -> 83 fps at 4K (about two thirds kept) - measure here";
        case GpuFamily::kAmpere:
        case GpuFamily::kTuring:
            return "community FP16 path; expect a large frame-rate cost, watch for GPU timeouts";
        default:
            return "no NVIDIA tensor cores detected; the runtime will most likely refuse";
        }
    }

    std::uint32_t ClampStyle(std::uint32_t a_style) noexcept
    {
        return a_style <= kStyleMax ? a_style : 0U;
    }

    std::uint32_t ClampPreset(std::uint32_t a_preset) noexcept
    {
        return a_preset <= kPresetMax ? a_preset : 0U;
    }

    float ClampStrength(float a_value, float a_default) noexcept
    {
        if (!std::isfinite(a_value) || a_value < kStrengthMin || a_value > kStrengthMax) {
            return a_default;
        }
        return a_value;
    }

    bool RequiresRecreate(const Settings& a_current, const Settings& a_next) noexcept
    {
        return a_current.preset != a_next.preset;
    }

    const RuntimeProfile* FindRuntimeProfile(const char* a_sha256) noexcept
    {
        static constexpr RuntimeProfile kProfiles[]{
            { "8270b350cd82de5ce89806872cdd6b6a9249b80836b91bbeb3573470744cc206",
              "DLSS 5 310.8 (community Ada variant)",
              "tested on an RTX 40: the 310.8 key table, strengths 0-1, standard Z, the render-extent MV scale, the native carrier",
              true },
            { "e16bcf15e16e13f527491cdf7845b2fe6521a738d8f7c9c721866a8496e1fc8e",
              "DLSS 5 310.8 (the Streamline 2.13 package)",
              "on disk, never run here: the same key names by static inspection, contract untested",
              false },
        };
        if (a_sha256 == nullptr) {
            return nullptr;
        }
        for (const auto& profile : kProfiles) {
            if (std::strcmp(profile.sha256, a_sha256) == 0) {
                return &profile;
            }
        }
        return nullptr;
    }

    float TimingWindow::Percentile(float a_fraction) const noexcept
    {
        if (count == 0U) {
            return 0.0F;
        }
        float sorted[kCapacity];
        std::memcpy(sorted, samples, sizeof(float) * count);
        std::sort(sorted, sorted + count);
        const float f = a_fraction < 0.0F ? 0.0F : (a_fraction > 1.0F ? 1.0F : a_fraction);
        std::uint32_t rank = static_cast<std::uint32_t>(std::ceil(f * static_cast<float>(count)));
        if (rank < 1U) rank = 1U;
        if (rank > count) rank = count;
        return sorted[rank - 1U];
    }

    std::uint32_t ActiveMask(const CascadeSettings& a_settings) noexcept
    {
        return a_settings.enabled ? (1U << ClampPassCount(a_settings.passCount)) - 1U : 0U;
    }

    std::uint32_t RecreateMask(const CascadeSettings& a_before, const CascadeSettings& a_after) noexcept
    {
        const std::uint32_t before = ActiveMask(a_before);
        const std::uint32_t after = ActiveMask(a_after);
        std::uint32_t mask = before & ~after;
        const bool extentChanged = EffectiveModelPercent(a_before) != EffectiveModelPercent(a_after);
        for (std::uint32_t i = 0; i < kMaxPasses; ++i) {
            const std::uint32_t bit = 1U << i;
            if ((before & after & bit) != 0U && (extentChanged || RequiresRecreate(a_before.passes[i], a_after.passes[i]))) {
                mask |= bit;
            }
        }
        return mask;
    }

    std::uint32_t HistoryResetMask(const CascadeSettings& a_before, const CascadeSettings& a_after) noexcept
    {
        const std::uint32_t before = ActiveMask(a_before);
        const std::uint32_t after = ActiveMask(a_after);
        std::uint32_t mask = after & ~before;
        if (EffectiveModelPercent(a_before) != EffectiveModelPercent(a_after)) {
            return mask | after;
        }
        for (std::uint32_t i = 0; i < kMaxPasses; ++i) {
            if ((before & after & (1U << i)) != 0U && RequiresRecreate(a_before.passes[i], a_after.passes[i])) {
                mask |= after & ~((1U << i) - 1U);
            }
        }
        return mask;
    }

    const KeyValue* ParamList::Find(const char* a_key) const noexcept
    {
        if (a_key == nullptr) {
            return nullptr;
        }
        for (std::size_t i = 0; i < count; ++i) {
            if (items[i].key != nullptr && std::strcmp(items[i].key, a_key) == 0) {
                return &items[i];
            }
        }
        return nullptr;
    }

    ParamList BuildParams(const Shape& a_shape, const Settings& a_settings,
        const Resources& a_resources, bool a_reset) noexcept
    {
        ParamList list{};

        Push(list, "DLSSNR.Width", a_shape.outWidth);
        Push(list, "DLSSNR.Height", a_shape.outHeight);
        Push(list, "DLSSNR.Enabled", 1U);
        Push(list, "DLSSNR.Reset", a_reset ? 1U : 0U);
        Push(list, "DLSSNR.Style", static_cast<int>(ClampStyle(a_settings.style)));
        Push(list, "DLSSNR.Hint.Render.Preset", static_cast<int>(ClampPreset(a_settings.preset)));
        Push(list, "DLSSNR.Intensity", a_settings.intensity);
        Push(list, "DLSSNR.LocalToneStrength", a_settings.localTone);
        Push(list, "DLSSNR.LocalStructureStrength", a_settings.localStructure);
        Push(list, "DLSSNR.SkinStructureStrength", a_settings.skinStructure);
        Push(list, "DLSSNR.UseAutoMask", a_settings.autoMask ? 1U : 0U);
        Push(list, "DLSSNR.UICorrection", 0);
        Push(list, "DLSSNR.DepthInverted", 0);
        Push(list, "DLSSNR.ScalingRatio", 1.0F);

        PushResource(list, "DLSSNR.Color", a_resources.color);
        PushResource(list, "DLSSNR.Output", a_resources.output);
        PushResource(list, "DLSSNR.Depth", a_resources.depth);
        PushResource(list, "DLSSNR.MVec", a_resources.mvec);

        PushSubrect(list, "DLSSNR.ColorSubrectBaseX", "DLSSNR.ColorSubrectBaseY",
            "DLSSNR.ColorSubrectWidth", "DLSSNR.ColorSubrectHeight", a_shape.outWidth,
            a_shape.outHeight);
        PushSubrect(list, "DLSSNR.OutputSubrectBaseX", "DLSSNR.OutputSubrectBaseY",
            "DLSSNR.OutputSubrectWidth", "DLSSNR.OutputSubrectHeight", a_shape.outWidth,
            a_shape.outHeight);
        PushSubrect(list, "DLSSNR.DepthSubrectBaseX", "DLSSNR.DepthSubrectBaseY",
            "DLSSNR.DepthSubrectWidth", "DLSSNR.DepthSubrectHeight", a_shape.renderWidth,
            a_shape.renderHeight);
        PushSubrect(list, "DLSSNR.MVecSubrectBaseX", "DLSSNR.MVecSubrectBaseY",
            "DLSSNR.MVecSubrectWidth", "DLSSNR.MVecSubrectHeight", a_shape.renderWidth,
            a_shape.renderHeight);

        Push(list, "DLSSNR.MVecScaleX", static_cast<float>(a_shape.renderWidth));
        Push(list, "DLSSNR.MVecScaleY", static_cast<float>(a_shape.renderHeight));
        return list;
    }
}
