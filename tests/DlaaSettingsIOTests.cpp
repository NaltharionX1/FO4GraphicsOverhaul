#include "Platform/DlaaSettings.h"
#include "Platform/DlaaSettingsIO.h"

#include <cstdlib>
#include <limits>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

namespace
{
    using Platform::AaEngineRequest;
    using Platform::DlaaSettings;
    using Platform::RequestedEngineOf;

    int g_checks = 0;
    int g_failures = 0;

    void Check(const bool condition, const std::string_view message)
    {
        ++g_checks;
        if (!condition) {
            ++g_failures;
            std::cout << "  [FAIL] " << message << '\n';
        }
    }

    DlaaSettings ParseText(const std::string& text)
    {
        std::istringstream in{ text };
        return Platform::DlaaSettingsIO::Parse(in, DlaaSettings{});
    }

    void CaseOldFileWithFsrStaysFsr()
    {
        const DlaaSettings s = ParseText("[DLAA]\nEnabled = true\nEngineFSR = true\nFSRVelocityFactor = 1.0\n");
        Check(s.engineFsr, "old file: EngineFSR = true must parse");
        Check(RequestedEngineOf(s) == AaEngineRequest::kFsr,
            "old file with EngineFSR = true must still request FSR (an FSR choice is never silently promoted to DLSS)");
    }

    void CaseEmptyFileIsTheDlssDefault()
    {
        const DlaaSettings s = ParseText("");
        const DlaaSettings defaults{};
        Check(s == defaults, "an empty stream must yield the compiled defaults byte-for-byte");
        Check(!s.engineFsr && s.frameGeneration,
            "defaults: DLSS (FSR off), frame generation on — all features out of the box");
        Check(RequestedEngineOf(s) == AaEngineRequest::kDlss, "defaults request DLSS (on the D3D12 sidecar)");
    }

    void CaseExplicitCombinations()
    {
        Check(RequestedEngineOf(ParseText("[DLAA]\nEngineFSR = false\n")) == AaEngineRequest::kDlss,
            "EngineFSR = false must request DLSS");
        Check(RequestedEngineOf(ParseText("[DLAA]\nEngineFSR = true\n")) == AaEngineRequest::kFsr,
            "EngineFSR = true must request FSR");
        Check(RequestedEngineOf(ParseText("[DLAA]\nEngineDX12 = false\n")) == AaEngineRequest::kDlss,
            "an old EngineDX12 = false must land on DLSS (the D3D12 engine), never on FSR");
        Check(ParseText("[DLAA]\nEngineDX12 = false\n") == DlaaSettings{},
            "the retired EngineDX12 key changes nothing (ignored like any unknown key)");
        Check(RequestedEngineOf(ParseText("[DLAA]\nEngineFSR = true\nEngineDX12 = true\n")) == AaEngineRequest::kFsr,
            "an old EngineFSR = true + EngineDX12 = true still requests FSR");
        const DlaaSettings fgOff = ParseText("[DLAA]\nFrameGeneration = false\n");
        Check(!fgOff.frameGeneration, "FrameGeneration = false must parse");
        Check(ParseText("[DLAA]\nFrameGenerationFrames = 3\n").frameGenerationFrames == 3U, "FrameGenerationFrames = 3 must parse");
        Check(ParseText("[DLAA]\nFrameGenerationFrames = 5\n").frameGenerationFrames == 5U, "FrameGenerationFrames = 5 (6x) is the ceiling");
        Check(ParseText("[DLAA]\nFrameGenerationFrames = 6\n").frameGenerationFrames == 1U, "FrameGenerationFrames = 6 is past the runtime's own ceiling -> the default");
        Check(ParseText("[DLAA]\nFrameGenerationFrames = 9\n").frameGenerationFrames == 1U, "an out-of-range FrameGenerationFrames clamps to 1 (2x)");
        Check(ParseText("[DLAA]\n").frameGenerationFrames == 1U, "default: 2x");
        Check(ParseText("[DLAA]\nFrameGenerationDynamic = true\n").frameGenerationDynamic, "FrameGenerationDynamic = true must parse");
        Check(!ParseText("[DLAA]\n").frameGenerationDynamic, "default: the fixed multiplier, not dynamic");
        Check(ParseText("[DLAA]\nFrameGenerationDynamicTargetHz = 144\n").frameGenerationDynamicTargetHz == 144U, "a dynamic target in range parses");
        Check(ParseText("[DLAA]\nFrameGenerationDynamicTargetHz = 12\n").frameGenerationDynamicTargetHz == 0U, "a target below 30 Hz falls back to 0 (the display)");
        Check(ParseText("[DLAA]\nFrameGenerationDynamicTargetHz = 5000\n").frameGenerationDynamicTargetHz == 0U, "a target above 1000 Hz falls back to 0 (the display)");
        Check(ParseText("[DLAA]\n").frameGenerationDynamicTargetHz == 0U, "default target: the display's refresh");
        Check(ParseText("[DLAA]\nMultiFrameKernels = midpoint\n") == DlaaSettings{} && ParseText("[DLAA]\nMultiFrameKernels = blackwell\n") == DlaaSettings{},
            "the retired MultiFrameKernels key (Blackwell is the only program) is ignored like any unknown key");
        Check(ParseText("[DLAA]\nFrameGenerationDepthSeparation = 20\n").frameGenerationDepthSeparation == 20.0F, "FrameGenerationDepthSeparation = 20 must parse");
        Check(ParseText("[DLAA]\nFrameGenerationDepthSeparation = 0\n").frameGenerationDepthSeparation == 40.0F, "an out-of-range depth separation falls back to the runtime's default 40");
        Check(ParseText("[DLAA]\nFrameGenerationDepthSeparation = 5000\n").frameGenerationDepthSeparation == 40.0F, "a depth separation above 1000 falls back to 40");
        Check(ParseText("[DLAA]\n").frameGenerationDepthSeparation == 40.0F, "default: 40, the runtime's own, not pushed");
        Check(ParseText("[DLAA]\nFSRFrameGeneration = true\n").fsrFrameGeneration,
            "FSRFrameGeneration = true must parse (AMD's generator is the selected backend)");
        Check(!ParseText("[DLAA]\nFSRFrameGeneration = false\n").fsrFrameGeneration,
            "FSRFrameGeneration = false must parse (DLSS-G is the backend)");
        Check(!ParseText("[DLAA]\n").fsrFrameGeneration, "default: DLSS-G, not FSR-FG");
        Check(ParseText("[DLAA]\nReflexMode = 0\n").reflexMode == 0U, "ReflexMode = 0 (off) must parse");
        Check(ParseText("[DLAA]\nReflexMode = 2\n").reflexMode == 2U, "ReflexMode = 2 (on + boost) must parse");
        Check(ParseText("[DLAA]\nReflexMode = 7\n").reflexMode == 1U,
            "an out-of-range ReflexMode clamps to 1 (On), the way every sibling numeric key clamps");
        Check(ParseText("[DLAA]\n").reflexMode == 1U, "default: Reflex On");
        Check(ParseText("[DLAA]\nFSRFrameGeneration = true\n").frameGeneration,
            "the backend key does not disturb the shared master");
        Check(ParseText("[DLAA]\nMultiFrameUnlock = false\n") == DlaaSettings{} && ParseText("[DLAA]\nMultiFrameUnlock = true\n") == DlaaSettings{},
            "the retired MultiFrameUnlock key is ignored, true or false");
        Check(ParseText("[DLAA]\nFrameGenerationUiAlpha = true\n") == DlaaSettings{},
            "the retired FrameGenerationUiAlpha key (the alpha is always on) is ignored like any unknown key");
        Check(ParseText("[DLAA]\nSharedPipeline = true\n") == DlaaSettings{},
            "the retired SharedPipeline key (the A/B toggle) is ignored like any unknown key");
        Check(ParseText("[DLAA]\nReflexProvider = 1\n") == DlaaSettings{},
            "the retired ReflexProvider key (Reflex is NVAPI-only now) is ignored like any unknown key");
        const DlaaSettings outside = ParseText("[Other]\nEngineFSR = true\n");
        Check(!outside.engineFsr, "keys outside the [DLAA] section are ignored");
    }

    void CaseWriterRoundTrip()
    {
        DlaaSettings original{};
        original.engineFsr = true;
        original.frameGeneration = false;
        original.frameGenerationFrames = 2;
        original.frameGenerationDynamic = true;
        original.frameGenerationDynamicTargetHz = 165;
        original.frameGenerationDepthSeparation = 25.0F;
        original.qualityMode = 3;
        original.preset = 12;
        std::ostringstream out;
        Platform::DlaaSettingsIO::WriteDlaaBlock(out, original);
        const std::string text = out.str();
        Check(text.find("EngineFSR = true") != std::string::npos, "the writer emits EngineFSR");
        Check(text.find("EngineDX12") == std::string::npos, "the writer no longer emits the retired EngineDX12 key");
        Check(text.find("FrameGeneration = false") != std::string::npos, "the writer emits FrameGeneration");
        Check(text.find("FSRFrameGeneration = ") != std::string::npos, "the writer emits FSRFrameGeneration");
        Check(text.find("FrameGenerationFrames = 2") != std::string::npos, "the writer emits FrameGenerationFrames");
        Check(text.find("FrameGenerationDynamic = true") != std::string::npos, "the writer emits FrameGenerationDynamic");
        Check(text.find("FrameGenerationDynamicTargetHz = 165") != std::string::npos, "the writer emits FrameGenerationDynamicTargetHz");
        Check(text.find("FrameGenerationDepthSeparation = ") != std::string::npos, "the writer emits FrameGenerationDepthSeparation");
        Check(text.find("ReflexMode = ") != std::string::npos, "the writer emits ReflexMode");
        Check(text.find("MultiFrameKernels") == std::string::npos, "the writer no longer emits the retired MultiFrameKernels key");
        Check(text.find("MultiFrameUnlock") == std::string::npos, "the writer no longer emits the retired MultiFrameUnlock key");
        const DlaaSettings back = ParseText(text);
        Check(back == original, "WriteDlaaBlock -> Parse must round-trip every field (operator== is the auto-save's drift test)");
        Check(RequestedEngineOf(back) == AaEngineRequest::kFsr, "the round-tripped request is FSR");

        DlaaSettings dx12{};
        std::ostringstream out2;
        Platform::DlaaSettingsIO::WriteDlaaBlock(out2, dx12);
        Check(ParseText(out2.str()) == dx12, "the defaults round-trip unchanged");
    }

    void CaseFsrTuningClampsAndRoundTrip()
    {
        const DlaaSettings defaults{};
        Check(defaults.fsrReactiveness == 1.0F, "fReactivenessScale defaults to AMD's 1.0");
        Check(defaults.fsrShadingChange == 1.0F, "fShadingChangeScale defaults to AMD's 1.0");
        Check(defaults.fsrAccumulation == 0.333F, "fAccumulationAddedPerFrame defaults to AMD's 0.333");
        Check(defaults.fsrMinDisocclusion == -0.333F, "fMinDisocclusionAccumulation defaults to AMD's -0.333");
        Check(defaults.fsrMasks, "the reactive/transparency masks default to on");
        Check(defaults.fsrTransparencyScale == 1.0F, "the transparency scale defaults to 1.0");

        Check(Platform::ClampFsrAccumulation(2.0F) == 1.0F, "accumulation above 1 clamps to 1, not to the default");
        Check(Platform::ClampFsrAccumulation(-1.0F) == 0.0F, "accumulation below 0 clamps to 0");
        Check(Platform::ClampFsrMinDisocclusion(5.0F) == 1.0F, "min disocclusion clamps to +1");
        Check(Platform::ClampFsrMinDisocclusion(-5.0F) == -1.0F, "min disocclusion clamps to -1");
        Check(Platform::ClampFsrReactiveness(9.0F) == 2.0F, "reactiveness clamps to the UI ceiling of 2");
        Check(Platform::ClampFsrReactiveness(-1.0F) == 0.0F, "reactiveness clamps to 0");
        Check(Platform::ClampFsrShadingChange(-1.0F) == 0.0F, "shading change clamps to 0");
        Check(Platform::ClampFsrShadingChange(9.0F) == 2.0F, "shading change clamps to the UI ceiling of 2");
        Check(Platform::ClampFsrTransparencyScale(3.0F) == 2.0F, "transparency scale clamps to 2");
        Check(Platform::ClampFsrTransparencyScale(-1.0F) == 0.0F, "transparency scale clamps to 0");

        const float nan = std::numeric_limits<float>::quiet_NaN();
        Check(Platform::ClampFsrAccumulation(nan) == 0.333F, "a non-finite accumulation lands on AMD's default");
        Check(Platform::ClampFsrMinDisocclusion(nan) == -0.333F, "a non-finite min disocclusion lands on -0.333");
        Check(Platform::ClampFsrReactiveness(nan) == 1.0F, "a non-finite reactiveness lands on 1.0");
        Check(Platform::ClampFsrShadingChange(nan) == 1.0F, "a non-finite shading change lands on 1.0");
        Check(Platform::ClampFsrTransparencyScale(nan) == 1.0F, "a non-finite transparency scale lands on 1.0");

        DlaaSettings tuned{};
        tuned.fsrReactiveness = 1.5F;
        tuned.fsrShadingChange = 0.5F;
        tuned.fsrAccumulation = 0.75F;
        tuned.fsrMinDisocclusion = 0.25F;
        tuned.fsrMasks = false;
        tuned.fsrTransparencyScale = 1.25F;
        std::ostringstream out;
        Platform::DlaaSettingsIO::WriteDlaaBlock(out, tuned);
        const DlaaSettings back = ParseText(out.str());
        Check(back.fsrReactiveness == 1.5F, "fsrReactiveness round-trips");
        Check(back.fsrShadingChange == 0.5F, "fsrShadingChange round-trips");
        Check(back.fsrAccumulation == 0.75F, "fsrAccumulation round-trips");
        Check(back.fsrMinDisocclusion == 0.25F, "fsrMinDisocclusion round-trips (a POSITIVE value, unlike the default)");
        Check(!back.fsrMasks, "fsrMasks round-trips when switched off");
        Check(back.fsrTransparencyScale == 1.25F, "fsrTransparencyScale round-trips");

        DlaaSettings ultra{};
        ultra.engineFsr = true;
        ultra.qualityMode = 1U;
        const DlaaSettings fixed = Platform::ClampDlaaSettings(ultra);
        Check(fixed.qualityMode == 2U, "FSR + Ultra Quality is corrected to Quality by the clamp itself");
        DlaaSettings ultraDlss{};
        ultraDlss.engineFsr = false;
        ultraDlss.qualityMode = 1U;
        Check(Platform::ClampDlaaSettings(ultraDlss).qualityMode == 1U,
            "...and Ultra Quality survives untouched on DLSS, which does have that rung");
        const DlaaSettings fromFile = ParseText("[DLAA]\nEngineFSR = true\nQualityMode = 1\n");
        Check(fromFile.qualityMode == 2U, "a file pairing FSR with Ultra Quality is corrected on load, not only in the menu");

        const DlaaSettings old = ParseText("[DLAA]\nEnabled = true\nEngineFSR = true\n");
        Check(old.fsrAccumulation == 0.333F, "a file with no FSR tuning keys keeps AMD's accumulation default");
        Check(old.fsrReactiveness == 1.0F, "...and its reactiveness default");
        Check(old.fsrShadingChange == 1.0F, "...and its shading-change default");
        Check(old.fsrMinDisocclusion == -0.333F, "...and its min-disocclusion default");
        Check(old.fsrTransparencyScale == 1.0F, "...and its transparency-scale default");
        Check(old.fsrMasks, "a file with no FSRMasks key keeps the masks on");
    }
}

int main()
{
    CaseOldFileWithFsrStaysFsr();
    CaseFsrTuningClampsAndRoundTrip();
    CaseEmptyFileIsTheDlssDefault();
    CaseExplicitCombinations();
    CaseWriterRoundTrip();
    std::cout << "DlaaSettingsIOTests: " << (g_failures == 0 ? "PASS" : "FAIL") << " (" << g_checks
              << " checks, " << g_failures << " failures)\n";
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
