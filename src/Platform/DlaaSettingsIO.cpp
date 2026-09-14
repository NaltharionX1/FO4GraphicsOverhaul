#include "Platform/DlaaSettingsIO.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <atomic>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <ostream>
#include <string>
#include <string_view>
#include <system_error>

namespace Platform::DlaaSettingsIO
{
    namespace
    {
        std::atomic_uint64_t g_temporarySequence{ 0 };

        [[nodiscard]] constexpr bool IsSpace(char value) noexcept
        {
            return value == ' ' || value == '\t' || value == '\r' || value == '\n' ||
                   value == '\f' || value == '\v';
        }

        [[nodiscard]] std::string_view Trim(std::string_view value) noexcept
        {
            while (!value.empty() && IsSpace(value.front())) {
                value.remove_prefix(1);
            }
            while (!value.empty() && IsSpace(value.back())) {
                value.remove_suffix(1);
            }
            return value;
        }

        [[nodiscard]] constexpr char AsciiLower(char value) noexcept
        {
            return value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A')) : value;
        }

        [[nodiscard]] bool EqualsInsensitive(std::string_view lhs, std::string_view rhs) noexcept
        {
            if (lhs.size() != rhs.size()) {
                return false;
            }
            for (std::size_t i = 0; i < lhs.size(); ++i) {
                if (AsciiLower(lhs[i]) != AsciiLower(rhs[i])) {
                    return false;
                }
            }
            return true;
        }

        [[nodiscard]] bool TryParseBool(std::string_view text, bool& value) noexcept
        {
            text = Trim(text);
            if (EqualsInsensitive(text, "true") || text == "1" || EqualsInsensitive(text, "yes") ||
                EqualsInsensitive(text, "on")) {
                value = true;
                return true;
            }
            if (EqualsInsensitive(text, "false") || text == "0" || EqualsInsensitive(text, "no") ||
                EqualsInsensitive(text, "off")) {
                value = false;
                return true;
            }
            return false;
        }

        [[nodiscard]] bool TryParseUInt(std::string_view text, std::uint32_t& value) noexcept
        {
            text = Trim(text);
            if (!text.empty() && text.front() == '+') {
                text.remove_prefix(1);
            }
            if (text.empty()) {
                return false;
            }
            std::uint32_t parsed{};
            const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed, 10);
            if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
                return false;
            }
            value = parsed;
            return true;
        }

        [[nodiscard]] bool TryParseFloat(std::string_view text, float& value) noexcept
        {
            text = Trim(text);
            if (!text.empty() && text.front() == '+') {
                text.remove_prefix(1);
            }
            if (text.empty()) {
                return false;
            }
            float parsed{};
            const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed,
                std::chars_format::general);
            if (result.ec != std::errc{} || result.ptr != text.data() + text.size() ||
                !std::isfinite(parsed)) {
                return false;
            }
            value = parsed;
            return true;
        }

        [[nodiscard]] std::string FormatFloat(float value)
        {
            char buffer[64]{};
            const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value,
                std::chars_format::general);
            if (result.ec != std::errc{}) {
                return "0";
            }
            return { buffer, result.ptr };
        }

        [[nodiscard]] std::filesystem::path TemporaryPath(
            const std::filesystem::path& destination)
        {
            std::filesystem::path temporary = destination;
            temporary += L".tmp-" + std::to_wstring(::GetCurrentProcessId()) + L"-" +
                         std::to_wstring(g_temporarySequence.fetch_add(1, std::memory_order_relaxed));
            return temporary;
        }

        [[nodiscard]] bool ReplaceDestination(const std::filesystem::path& temporary,
            const std::filesystem::path& destination) noexcept
        {
            return ::MoveFileExW(temporary.c_str(), destination.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
        }

        void RemoveTemporary(const std::filesystem::path& temporary) noexcept
        {
            if (temporary.empty()) {
                return;
            }
            std::error_code ec;
            (void)std::filesystem::remove(temporary, ec);
        }
    }

    bool WriteAtomically(const std::filesystem::path& destination, const StreamWriter& writer,
        ReplaceOperation replace) noexcept
    {
        std::filesystem::path temporary;
        try {
            if (destination.empty() || !writer) {
                return false;
            }

            temporary = TemporaryPath(destination);
            std::ofstream out(temporary, std::ios::binary | std::ios::out | std::ios::trunc);
            if (!out) {
                RemoveTemporary(temporary);
                return false;
            }

            writer(out);
            const bool writeSucceeded = out.good();
            out.flush();
            const bool flushSucceeded = out.good();
            out.close();
            const bool closeSucceeded = !out.fail();
            if (!writeSucceeded || !flushSucceeded || !closeSucceeded) {
                RemoveTemporary(temporary);
                return false;
            }

            const ReplaceOperation commit = replace ? replace : ReplaceDestination;
            if (!commit(temporary, destination)) {
                RemoveTemporary(temporary);
                return false;
            }
            return true;
        } catch (...) {
            RemoveTemporary(temporary);
            return false;
        }
    }

    SaveResult Save(const std::filesystem::path& destination, DlaaSettings requested,
        ReplaceOperation replace) noexcept
    {
        SaveResult result{};
        result.liveSettings = ClampDlaaSettings(requested);
        try {
            const StreamWriter writer = [&](std::ostream& out) {
                WriteDlaaBlock(out, result.liveSettings);
            };
            result.persisted = WriteAtomically(destination, writer, replace);
        } catch (...) {
            result.persisted = false;
        }
        return result;
    }

    DlaaSettings Parse(std::istream& in, DlaaSettings defaults) noexcept
    {
        try {
            DlaaSettings result = defaults;
            bool inDlaa = false;
            bool firstLine = true;
            std::string line;
            while (std::getline(in, line)) {
                std::string_view text{ line };
                if (firstLine && text.starts_with("\xEF\xBB\xBF")) {
                    text.remove_prefix(3);
                }
                firstLine = false;
                text = Trim(text);
                if (text.empty() || text.front() == '#' || text.front() == ';') {
                    continue;
                }
                if (text.front() == '[' && text.back() == ']') {
                    const std::string_view section = Trim(text.substr(1, text.size() - 2));
                    inDlaa = EqualsInsensitive(section, "DLAA");
                    continue;
                }
                if (!inDlaa) {
                    continue;
                }

                const auto equals = text.find('=');
                if (equals == std::string_view::npos) {
                    continue;
                }
                const std::string_view key = Trim(text.substr(0, equals));
                const std::string_view value = Trim(text.substr(equals + 1));
                if (EqualsInsensitive(key, "Enabled")) {
                    (void)TryParseBool(value, result.enable);
                } else if (EqualsInsensitive(key, "Preset")) {
                    (void)TryParseUInt(value, result.preset);
                } else if (EqualsInsensitive(key, "MipLODBias")) {
                    (void)TryParseFloat(value, result.mipBias);
                } else if (EqualsInsensitive(key, "AutoExposure")) {
                    (void)TryParseBool(value, result.autoExposure);
                } else if (EqualsInsensitive(key, "Sharpness")) {
                    (void)TryParseFloat(value, result.sharpness);
                } else if (EqualsInsensitive(key, "ExposureScale")) {
                    (void)TryParseFloat(value, result.exposureScale);
                } else if (EqualsInsensitive(key, "ResolutionScale")) {
                    (void)TryParseFloat(value, result.resolutionScale);
                } else if (EqualsInsensitive(key, "ReflexMode")) {
                    (void)TryParseUInt(value, result.reflexMode);
                } else if (EqualsInsensitive(key, "JitterScale")) {
                    (void)TryParseFloat(value, result.jitterScale);
                } else if (EqualsInsensitive(key, "QualityMode")) {
                    (void)TryParseUInt(value, result.qualityMode);
                } else if (EqualsInsensitive(key, "EngineFSR")) {
                    (void)TryParseBool(value, result.engineFsr);
                } else if (EqualsInsensitive(key, "FrameGeneration")) {
                    (void)TryParseBool(value, result.frameGeneration);
                } else if (EqualsInsensitive(key, "FrameGenerationFrames")) {
                    (void)TryParseUInt(value, result.frameGenerationFrames);
                } else if (EqualsInsensitive(key, "FrameGenerationDynamic")) {
                    (void)TryParseBool(value, result.frameGenerationDynamic);
                } else if (EqualsInsensitive(key, "FrameGenerationDynamicTargetHz")) {
                    (void)TryParseUInt(value, result.frameGenerationDynamicTargetHz);
                } else if (EqualsInsensitive(key, "FrameGenerationDepthSeparation")) {
                    (void)TryParseFloat(value, result.frameGenerationDepthSeparation);
                } else if (EqualsInsensitive(key, "FSRVelocityFactor")) {
                    (void)TryParseFloat(value, result.fsrVelocity);
                } else if (EqualsInsensitive(key, "FSRReactivenessScale")) {
                    (void)TryParseFloat(value, result.fsrReactiveness);
                } else if (EqualsInsensitive(key, "FSRShadingChangeScale")) {
                    (void)TryParseFloat(value, result.fsrShadingChange);
                } else if (EqualsInsensitive(key, "FSRAccumulationAddedPerFrame")) {
                    (void)TryParseFloat(value, result.fsrAccumulation);
                } else if (EqualsInsensitive(key, "FSRMinDisocclusionAccumulation")) {
                    (void)TryParseFloat(value, result.fsrMinDisocclusion);
                } else if (EqualsInsensitive(key, "FSRFrameGeneration")) {
                    (void)TryParseBool(value, result.fsrFrameGeneration);
                } else if (EqualsInsensitive(key, "FSRMasks")) {
                    (void)TryParseBool(value, result.fsrMasks);
                } else if (EqualsInsensitive(key, "FSRTransparencyScale")) {
                    (void)TryParseFloat(value, result.fsrTransparencyScale);
                }
            }
            if (in.bad() || (in.fail() && !in.eof())) {
                return ClampDlaaSettings(defaults);
            }
            return ClampDlaaSettings(result);
        } catch (...) {
            return ClampDlaaSettings(defaults);
        }
    }

    DlaaSettings Load(const std::filesystem::path& path, DlaaSettings defaults) noexcept
    {
        try {
            std::ifstream in(path, std::ios::binary);
            if (!in) {
                return ClampDlaaSettings(defaults);
            }
            return Parse(in, defaults);
        } catch (...) {
            return ClampDlaaSettings(defaults);
        }
    }

    void WriteDlaaBlock(std::ostream& out, const DlaaSettings& settings)
    {
        const DlaaSettings clamped = ClampDlaaSettings(settings);
        out << "[DLAA]\n"
            << "# MASTER enable for DLSS DLAA anti-aliasing. false = native resolution with NO anti-aliasing (the game's TAA and FXAA stay disabled at the root while this plugin is loaded - they are never a fallback).\n"
            << "# Default: true\n"
            << "Enabled = " << (clamped.enable ? "true" : "false") << "\n\n"
            << "# DLAA model preset (DLSS 4 / 310.x). Only the transformer models are offered, because they\n"
            << "# are the only ones that do anything: 10=J, 11=K (DLAA default), 12=L (sharper), 13=M (not\n"
            << "# recommended). F/G/H/I (6-9) and N/O (14-15) are dead in current DLSS - F is deprecated\n"
            << "# legacy, the rest revert to default - so any other value here is migrated to 11 (K) on load\n"
            << "# rather than silently doing nothing. Switching recreates the feature. Default: 11 (K).\n"
            << "Preset = " << clamped.preset << "\n\n"
            << "# Negative mip-LOD bias = more texture detail. Works with or without DLAA. 0 = off, -1..-3 typical.\n"
            << "# Applied to anisotropic samplers around the geometry passes. Range [-3, 0]. Default: -1\n"
            << "MipLODBias = " << FormatFloat(clamped.mipBias) << "\n\n"
            << "# DLSS auto-exposure. true = NGX analyzes the image. false = fixed 1.0 exposure (try if bright\n"
            << "# objects look dimmed). Default: true\n"
            << "AutoExposure = " << (clamped.autoExposure ? "true" : "false") << "\n\n"
            << "# RCAS post-sharpening strength (works with or without DLAA). 0 = off, 0.3 typical, 1 = max. Range [0, 1].\n"
            << "# Default: 0.3\n"
            << "Sharpness = " << FormatFloat(clamped.sharpness) << "\n\n"
            << "# DLSS exposure multiplier. 1.0 = neutral. Range [0.1, 16]. Default: 1.0\n"
            << "ExposureScale = " << FormatFloat(clamped.exposureScale) << "\n\n"
            << "# TRS super-resolution: reconstruct ABOVE native then Catmull-Rom downsample back.\n"
            << "# 1.0 = off (native). Range [1, 2]. Default: 1.0. Only applies while QualityMode = 0\n"
            << "# (DLAA) - the sub-native quality modes own the render resolution and this is inert.\n"
            << "ResolutionScale = " << FormatFloat(clamped.resolutionScale) << "\n\n"
            << "# NVIDIA Reflex low-latency through NVAPI (every RTX generation), bound to the DirectX 12 presenter\n"
            << "# while the proxy presents on D3D12, else to the game's DirectX 11 device. It keeps running while\n"
            << "# frame generation interpolates. 0 = Off, 1 = On, 2 = On + Boost. Default: 1\n"
            << "ReflexMode = " << clamped.reflexMode << "\n\n"
            << "# Halton sub-pixel jitter amplitude. 1.0 = standard +/-0.5px; higher spreads the sample\n"
            << "# pattern wider (can resolve residual pixelation); 0 = no jitter (stable but aliased).\n"
            << "# Range [0, 3]. Default: 1.0\n"
            << "JitterScale = " << FormatFloat(clamped.jitterScale) << "\n\n"
            << "# DLSS resolution mode. The scene renders at the DLSS-optimal lower resolution (via the\n"
            << "# game's own dynamic resolution - the window never changes) and DLSS upscales to the window.\n"
            << "# 0 = DLAA (native, no upscaling), 1 = Ultra Quality, 2 = Quality, 3 = Balanced,\n"
            << "# 4 = Performance, 5 = Ultra Performance. Default: 0\n"
            << "QualityMode = " << clamped.qualityMode << "\n\n"
            << "# The AA engine. false = DLSS (NVIDIA, on a private DirectX 12 device beside the game's\n"
            << "# renderer - needs an RTX adapter with DirectX 12), true = FSR 3.1\n"
            << "# (AMD FidelityFX, vendor-neutral - works on any DX11 GPU with typed-UAV support).\n"
            << "# One requested engine, resolved against what the adapter can actually do: a DLSS\n"
            << "# request on a non-RTX adapter falls forward to FSR automatically; an FSR request\n"
            << "# never silently reverts to DLSS. Default: false\n"
            << "EngineFSR = " << (clamped.engineFsr ? "true" : "false") << "\n\n"
            << "# Master switch for frame generation. FSRFrameGeneration below selects DLSS-G or FSR-FG\n"
            << "# independently of the upscaler; both present through the DirectX 12 proxy. Real frames only\n"
            << "# while the menu or a loading screen is up. Default: true\n"
            << "FrameGeneration = " << (clamped.frameGeneration ? "true" : "false") << "\n\n"
            << "# DLSS-G generated frames per real frame: 1 = 2x, 2 = 3x, 3 = 4x, 4 = 5x, 5 = 6x. On RTX 40 the mod opens\n"
            << "# everything above 2x itself (always) and corrects the runtime's interpolation timing in memory; a\n"
            << "# runtime refusal falls back to 2x for the session. Default: 1\n"
            << "FrameGenerationFrames = " << clamped.frameGenerationFrames << "\n\n"
            << "# Dynamic multiplier: this mod's own controller varies the generated-frame count per real\n"
            << "# frame to hold a target output rate, the way NVIDIA's Dynamic Multi Frame Generation does, on the\n"
            << "# DLSS-G path (the feature is created at the maximum; each real-frame pair asks for what the interval\n"
            << "# needs). The fixed FrameGenerationFrames above is ignored while this is on. Default: false\n"
            << "FrameGenerationDynamic = " << (clamped.frameGenerationDynamic ? "true" : "false") << "\n\n"
            << "# The dynamic target in Hz: 0 = the display's current refresh rate (queried live from the monitor the\n"
            << "# window is on); 30..1000 otherwise. Default: 0\n"
            << "FrameGenerationDynamicTargetHz = " << clamped.frameGenerationDynamicTargetHz << "\n\n"
            << "# DLSS-G's minimum relative linear-depth separation between two objects (the runtime's disocclusion\n"
            << "# heuristic). 40 is the runtime's documented default: never pushed on a fresh session; another value\n"
            << "# is pushed when it changes, and 40 is pushed once more to restore the default afterwards. Lower\n"
            << "# values may improve disocclusion around nearby objects (hands, weapon) in some scenes.\n"
            << "# Range [1, 1000]. Default: 40\n"
            << "FrameGenerationDepthSeparation = " << FormatFloat(clamped.frameGenerationDepthSeparation) << "\n\n"
            << "# FSR velocity factor (AMD's fVelocityFactor tuning constant). 1.0 = AMD default;\n"
            << "# 0.0 can improve temporal stability of bright pixels. Range [0, 1]. Default: 1.0\n"
            << "FSRVelocityFactor = " << FormatFloat(clamped.fsrVelocity) << "\n\n"
            << "# Reactiveness scale (AMD's fReactivenessScale): scales the reactive mask's values, reducing\n"
            << "# ghosting on fast-changing surfaces. 1.0 = AMD default. Range [0, 2]. Default: 1.0\n"
            << "FSRReactivenessScale = " << FormatFloat(clamped.fsrReactiveness) << "\n\n"
            << "# Shading-change scale (AMD's fShadingChangeScale): raising it makes FSR treat a shading change\n"
            << "# as more reactive. 1.0 = AMD default. Range [0, 2]. Default: 1.0\n"
            << "FSRShadingChangeScale = " << FormatFloat(clamped.fsrShadingChange) << "\n\n"
            << "# Accumulation added per frame (AMD's fAccumulationAddedPerFrame) at disoccluded pixels.\n"
            << "# LOWERING it reduces ghosting but makes thin features flicker more. Range [0, 1]. Default: 0.333\n"
            << "FSRAccumulationAddedPerFrame = " << FormatFloat(clamped.fsrAccumulation) << "\n\n"
            << "# Minimum disocclusion accumulation (AMD's fMinDisocclusionAccumulation). RAISING it reduces\n"
            << "# white-pixel flicker around swaying thin objects (grass, wires); too high ghosts. Range [-1, 1].\n"
            << "# Default: -0.333\n"
            << "FSRMinDisocclusionAccumulation = " << FormatFloat(clamped.fsrMinDisocclusion) << "\n\n"
            << "# Frame-generation backend, switched LIVE. false = DLSS Frame Generation (the default). true = AMD's\n"
            << "# FSR frame generation. AMD's chain presents whenever its runtime is found and passes real frames\n"
            << "# through unless this is on. Needs FO4GraphicsOverhaul/FidelityFX/ beside the plugin. Default: false\n"
            << "FSRFrameGeneration = " << (clamped.fsrFrameGeneration ? "true" : "false") << "\n\n"
            << "# Reactive + transparency masks: FSR's own reactive mask (generated from an opaque-only copy of\n"
            << "# the frame) and our transparency/composition mask, submitted with every upscale. They reduce\n"
            << "# ghost trails behind particles, foliage alpha, glass and effects. Default: true\n"
            << "FSRMasks = " << (clamped.fsrMasks ? "true" : "false") << "\n\n"
            << "# Transparency mask strength. 1.0 = the reference default; higher marks more of the frame as\n"
            << "# transparent/composited. Range [0, 2]. Default: 1.0\n"
            << "FSRTransparencyScale = " << FormatFloat(clamped.fsrTransparencyScale) << "\n\n";
    }
}
