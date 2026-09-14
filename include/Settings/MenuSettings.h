#pragma once

#include "Platform/NeuralContract.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace Settings
{
    inline constexpr float kMinWindowSize = 120.0F;
    inline constexpr float kMaxWindowExtent = 16384.0F;

    inline constexpr std::size_t kFontPathCapacity = 256;
    using FontPath = std::array<char, kFontPathCapacity>;

    enum class OsdAnchor : std::uint32_t
    {
        kTopLeft = 0,
        kTopRight = 1,
        kBottomLeft = 2,
        kBottomRight = 3,
        kCustom = 4,
    };
    inline constexpr std::uint32_t kOsdAnchorMax = 4;

    inline constexpr float kOsdFontSizeMin = 8.0F;
    inline constexpr float kOsdFontSizeMax = 48.0F;

    inline constexpr float kMinOsdSize = 40.0F;

    inline constexpr std::uint32_t kFpsLimitMin = 60;
    inline constexpr std::uint32_t kFpsLimitMax = 600;
    inline constexpr std::uint32_t kVSyncIntervalMax = 4;

    struct MenuSettings
    {
        std::uint32_t openHotkey{ 45 };
        std::uint32_t menuStyle{ 2 };
        float fontScale{ 1.0F };
        bool transparentMenu{ false };

        float windowX{ 0.0F };
        float windowY{ 0.0F };
        float windowW{ 0.0F };
        float windowH{ 0.0F };

        std::uint32_t aoMode{ 0 };
        bool aoEnabled{ false };

        std::uint32_t aoQuality{ 2 };
        std::uint32_t aoDenoisePasses{ 1 };
        float aoRadius{ 70.0F };
        float aoRadiusMultiplier{ 1.703F };
        float aoFalloffRange{ 0.601F };
        float aoSampleDistributionPower{ 2.50F };
        float aoOccluderThickness{ 8.0F };
        float aoFinalValuePower{ 2.70F };
        float aoMinScreenRadius{ 3.0F };
        bool aoDepthFadeEnabled{ true };
        float aoDepthFadeStart{ 40000.0F };
        float aoDepthFadeEnd{ 50000.0F };

        bool gradingEnabled{ false };
        bool engineGradingNeutral{ false };
        bool sceneExposureDisabled{ false };
        bool gameAutoExposureDisabled{ false };
        bool radialBlurSuppressed{ false };
        bool doubleVisionSuppressed{ false };

        Platform::Neural::CascadeSettings neural{};

        bool osdEnabled{ false };
        std::uint32_t osdHotkey{ 121 };
        std::uint32_t fingerprintToggleHotkey{ 0 };
        std::uint32_t fingerprintMarkHotkey{ 0 };
        std::uint32_t osdAnchor{ static_cast<std::uint32_t>(OsdAnchor::kTopRight) };
        float osdX{ 0.0F };
        float osdY{ 0.0F };
        float osdW{ 0.0F };
        float osdH{ 0.0F };
        float osdFontSize{ 16.0F };
        FontPath osdFontPath{};

        float osdTextR{ 1.0F };
        float osdTextG{ 1.0F };
        float osdTextB{ 1.0F };
        float osdTextA{ 1.0F };
        float osdBgR{ 0.0F };
        float osdBgG{ 0.0F };
        float osdBgB{ 0.0F };
        float osdBgA{ 0.45F };

        bool osdShowFps{ true };
        bool osdShowFrametime{ true };
        bool osdShowLows{ false };
        bool osdShowCpu{ true };
        bool osdShowRam{ true };
        bool osdShowVram{ true };
        bool osdShowGpu{ true };
        bool osdShowGpuTemp{ true };
        bool osdShowCpuTemp{ false };
        bool osdShowLoad{ false };
        bool osdShowNeural{ false };

        bool vsyncEnabled{ false };
        std::uint32_t vsyncInterval{ 1 };

        bool fpsUnlimited{ true };
        std::uint32_t fpsLimit{ 60 };
        bool loadingScreenUnlimited{ true };
        std::uint32_t loadingScreenFpsLimit{ 60 };

        bool lockCursor{ false };

        bool gsyncFlickerFix{ false };

        bool disableActorFade{ false };
        bool disablePlayerFade{ false };

        [[nodiscard]] bool HasWindowGeometry() const noexcept
        {
            return windowW >= kMinWindowSize && windowH >= kMinWindowSize;
        }

        [[nodiscard]] bool HasOsdGeometry() const noexcept
        {
            return osdW >= kMinOsdSize && osdH >= kMinOsdSize;
        }

        [[nodiscard]] bool operator==(const MenuSettings&) const noexcept = default;
    };

    [[nodiscard]] std::uint32_t ClampHotkey(std::uint32_t a_vk) noexcept;
    [[nodiscard]] std::uint32_t ClampOptionalHotkey(std::uint32_t a_vk) noexcept;
    [[nodiscard]] std::uint32_t ClampMenuStyle(std::uint32_t a_style) noexcept;
    [[nodiscard]] float ClampFontScale(float a_scale) noexcept;
    [[nodiscard]] float ClampWindowCoord(float a_coord) noexcept;
    [[nodiscard]] float ClampWindowSize(float a_size) noexcept;
    [[nodiscard]] bool ClampFlag(bool a_flag) noexcept;
    [[nodiscard]] std::uint32_t ClampOsdAnchor(std::uint32_t a_anchor) noexcept;
    [[nodiscard]] std::uint32_t ClampAoMode(std::uint32_t a_mode) noexcept;
    [[nodiscard]] std::uint32_t ClampAoQuality(std::uint32_t a_quality) noexcept;
    [[nodiscard]] std::uint32_t ClampAoDenoisePasses(std::uint32_t a_passes) noexcept;
    [[nodiscard]] float ClampAoRadius(float a_units) noexcept;
    [[nodiscard]] float ClampAoRadiusMultiplier(float a_value) noexcept;
    [[nodiscard]] float ClampAoFalloffRange(float a_value) noexcept;
    [[nodiscard]] float ClampAoSampleDistributionPower(float a_value) noexcept;
    [[nodiscard]] float ClampAoOccluderThickness(float a_value) noexcept;
    [[nodiscard]] float ClampAoFinalValuePower(float a_value) noexcept;
    [[nodiscard]] float ClampAoMinScreenRadius(float a_pixels) noexcept;
    [[nodiscard]] float ClampAoDepthFadeStart(float a_units) noexcept;
    [[nodiscard]] float ClampAoDepthFadeEnd(float a_units) noexcept;
    [[nodiscard]] std::uint32_t ClampNeuralStyle(std::uint32_t a_style) noexcept;
    [[nodiscard]] float ClampNeuralStrength(float a_value) noexcept;
    [[nodiscard]] std::uint32_t ClampNeuralModelPercent(std::uint32_t a_percent) noexcept;
    [[nodiscard]] float ClampOsdSize(float a_size) noexcept;
    [[nodiscard]] float ClampOsdFontSize(float a_size) noexcept;
    [[nodiscard]] float ClampColorChannel(float a_channel) noexcept;
    [[nodiscard]] std::uint32_t ClampVSyncInterval(std::uint32_t a_interval) noexcept;
    [[nodiscard]] std::uint32_t ClampFpsLimit(std::uint32_t a_limit) noexcept;
    [[nodiscard]] FontPath ClampFontPath(FontPath a_path) noexcept;

    template <class Visitor>
    void ForEachField(MenuSettings& a_settings, Visitor&& a_visit)
    {
        a_visit("Menu", "OpenHotkey", a_settings.openHotkey, &ClampHotkey);
        a_visit("Menu", "MenuStyle", a_settings.menuStyle, &ClampMenuStyle);
        a_visit("Menu", "FontScale", a_settings.fontScale, &ClampFontScale);
        a_visit("Menu", "TransparentMenu", a_settings.transparentMenu, &ClampFlag);
        a_visit("Menu", "WindowX", a_settings.windowX, &ClampWindowCoord);
        a_visit("Menu", "WindowY", a_settings.windowY, &ClampWindowCoord);
        a_visit("Menu", "WindowW", a_settings.windowW, &ClampWindowSize);
        a_visit("Menu", "WindowH", a_settings.windowH, &ClampWindowSize);

        a_visit("AmbientOcclusion", "Mode", a_settings.aoMode, &ClampAoMode);
        a_visit("AmbientOcclusion", "Enabled", a_settings.aoEnabled, &ClampFlag);
        a_visit("AmbientOcclusion", "Quality", a_settings.aoQuality, &ClampAoQuality);
        a_visit("AmbientOcclusion", "DenoisePasses", a_settings.aoDenoisePasses,
            &ClampAoDenoisePasses);
        a_visit("AmbientOcclusion", "Radius", a_settings.aoRadius, &ClampAoRadius);
        a_visit("AmbientOcclusion", "RadiusMultiplier", a_settings.aoRadiusMultiplier,
            &ClampAoRadiusMultiplier);
        a_visit("AmbientOcclusion", "FalloffRange", a_settings.aoFalloffRange,
            &ClampAoFalloffRange);
        a_visit("AmbientOcclusion", "SampleDistributionPower",
            a_settings.aoSampleDistributionPower, &ClampAoSampleDistributionPower);
        a_visit("AmbientOcclusion", "OccluderThickness", a_settings.aoOccluderThickness,
            &ClampAoOccluderThickness);
        a_visit("AmbientOcclusion", "FinalValuePower", a_settings.aoFinalValuePower,
            &ClampAoFinalValuePower);
        a_visit("AmbientOcclusion", "MinScreenRadius", a_settings.aoMinScreenRadius,
            &ClampAoMinScreenRadius);
        a_visit("AmbientOcclusion", "DepthFadeEnabled", a_settings.aoDepthFadeEnabled, &ClampFlag);
        a_visit("AmbientOcclusion", "DepthFadeStart", a_settings.aoDepthFadeStart,
            &ClampAoDepthFadeStart);
        a_visit("AmbientOcclusion", "DepthFadeEnd", a_settings.aoDepthFadeEnd, &ClampAoDepthFadeEnd);

        a_visit("Effects", "GradingEnabled", a_settings.gradingEnabled, &ClampFlag);
        a_visit("Effects", "EngineGradingNeutral", a_settings.engineGradingNeutral, &ClampFlag);
        a_visit("Effects", "SceneExposureDisabled", a_settings.sceneExposureDisabled, &ClampFlag);
        a_visit("Effects", "GameAutoExposureDisabled", a_settings.gameAutoExposureDisabled,
            &ClampFlag);
        a_visit("Effects", "RadialBlurSuppressed", a_settings.radialBlurSuppressed, &ClampFlag);
        a_visit("Effects", "DoubleVisionSuppressed", a_settings.doubleVisionSuppressed, &ClampFlag);

        a_visit("NeuralRendering", "Enabled", a_settings.neural.enabled, &ClampFlag);
        a_visit("NeuralRendering", "PassCount", a_settings.neural.passCount, &Platform::Neural::ClampPassCount);
        a_visit("NeuralRendering", "ModelPercent", a_settings.neural.modelPercent, &ClampNeuralModelPercent);
        a_visit("NeuralRendering", "ModelBeyondPlay", a_settings.neural.modelBeyondPlay, &ClampFlag);
        constexpr const char* neuralSections[]{ "NeuralRendering", "NeuralRendering.Pass2", "NeuralRendering.Pass3" };
        for (std::size_t i = 0; i < Platform::Neural::kMaxPasses; ++i) {
            auto& pass = a_settings.neural.passes[i];
            const char* section = neuralSections[i];
            a_visit(section, "Style", pass.style, &ClampNeuralStyle);
            a_visit(section, "Intensity", pass.intensity, &ClampNeuralStrength);
            a_visit(section, "LocalTone", pass.localTone, &ClampNeuralStrength);
            a_visit(section, "LocalStructure", pass.localStructure, &ClampNeuralStrength);
            a_visit(section, "SkinStructure", pass.skinStructure, &ClampNeuralStrength);
            a_visit(section, "AutoSkinMask", pass.autoMask, &ClampFlag);
        }

        a_visit("OSD", "Enabled", a_settings.osdEnabled, &ClampFlag);
        a_visit("OSD", "Hotkey", a_settings.osdHotkey, &ClampHotkey);
        a_visit("Diagnostics", "FingerprintToggleHotkey", a_settings.fingerprintToggleHotkey, &ClampOptionalHotkey);
        a_visit("Diagnostics", "FingerprintMarkHotkey", a_settings.fingerprintMarkHotkey, &ClampOptionalHotkey);
        a_visit("OSD", "Anchor", a_settings.osdAnchor, &ClampOsdAnchor);
        a_visit("OSD", "X", a_settings.osdX, &ClampWindowCoord);
        a_visit("OSD", "Y", a_settings.osdY, &ClampWindowCoord);
        a_visit("OSD", "W", a_settings.osdW, &ClampOsdSize);
        a_visit("OSD", "H", a_settings.osdH, &ClampOsdSize);
        a_visit("OSD", "FontSize", a_settings.osdFontSize, &ClampOsdFontSize);
        a_visit("OSD", "FontPath", a_settings.osdFontPath, &ClampFontPath);
        a_visit("OSD", "TextR", a_settings.osdTextR, &ClampColorChannel);
        a_visit("OSD", "TextG", a_settings.osdTextG, &ClampColorChannel);
        a_visit("OSD", "TextB", a_settings.osdTextB, &ClampColorChannel);
        a_visit("OSD", "TextA", a_settings.osdTextA, &ClampColorChannel);
        a_visit("OSD", "BgR", a_settings.osdBgR, &ClampColorChannel);
        a_visit("OSD", "BgG", a_settings.osdBgG, &ClampColorChannel);
        a_visit("OSD", "BgB", a_settings.osdBgB, &ClampColorChannel);
        a_visit("OSD", "BgA", a_settings.osdBgA, &ClampColorChannel);
        a_visit("OSD", "ShowFps", a_settings.osdShowFps, &ClampFlag);
        a_visit("OSD", "ShowFrametime", a_settings.osdShowFrametime, &ClampFlag);
        a_visit("OSD", "ShowLows", a_settings.osdShowLows, &ClampFlag);
        a_visit("OSD", "ShowCpu", a_settings.osdShowCpu, &ClampFlag);
        a_visit("OSD", "ShowRam", a_settings.osdShowRam, &ClampFlag);
        a_visit("OSD", "ShowVram", a_settings.osdShowVram, &ClampFlag);
        a_visit("OSD", "ShowGpu", a_settings.osdShowGpu, &ClampFlag);
        a_visit("OSD", "ShowGpuTemp", a_settings.osdShowGpuTemp, &ClampFlag);
        a_visit("OSD", "ShowCpuTemp", a_settings.osdShowCpuTemp, &ClampFlag);
        a_visit("OSD", "ShowLoad", a_settings.osdShowLoad, &ClampFlag);
        a_visit("OSD", "ShowNeural", a_settings.osdShowNeural, &ClampFlag);

        a_visit("Display", "VSync", a_settings.vsyncEnabled, &ClampFlag);
        a_visit("Display", "VSyncInterval", a_settings.vsyncInterval, &ClampVSyncInterval);
        a_visit("Display", "FpsUnlimited", a_settings.fpsUnlimited, &ClampFlag);
        a_visit("Display", "FpsLimit", a_settings.fpsLimit, &ClampFpsLimit);
        a_visit("Display", "LoadingScreenUnlimited", a_settings.loadingScreenUnlimited, &ClampFlag);
        a_visit("Display", "LoadingScreenFpsLimit", a_settings.loadingScreenFpsLimit, &ClampFpsLimit);
        a_visit("Display", "LockCursor", a_settings.lockCursor, &ClampFlag);
        a_visit("Display", "GSyncFlickerFix", a_settings.gsyncFlickerFix, &ClampFlag);
        a_visit("Display", "DisableActorFade", a_settings.disableActorFade, &ClampFlag);
        a_visit("Display", "DisablePlayerFade", a_settings.disablePlayerFade, &ClampFlag);
    }

    void ClampAll(MenuSettings& a_settings) noexcept;
}
