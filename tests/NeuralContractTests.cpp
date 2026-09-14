#include "Platform/NeuralContract.h"
#include "Platform/NgxAbi.h"

#include <cstdio>
#include <cstring>

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

    using namespace Platform::Neural;

    [[nodiscard]] bool HasUInt(const ParamList& a_list, const char* a_key, unsigned int a_value)
    {
        const KeyValue* kv = a_list.Find(a_key);
        return kv != nullptr && kv->type == ValueType::kUInt && kv->u == a_value;
    }

    [[nodiscard]] bool HasInt(const ParamList& a_list, const char* a_key, int a_value)
    {
        const KeyValue* kv = a_list.Find(a_key);
        return kv != nullptr && kv->type == ValueType::kInt && kv->i == a_value;
    }

    [[nodiscard]] bool HasFloat(const ParamList& a_list, const char* a_key, float a_value)
    {
        const KeyValue* kv = a_list.Find(a_key);
        return kv != nullptr && kv->type == ValueType::kFloat && kv->f == a_value;
    }

    [[nodiscard]] bool HasResource(const ParamList& a_list, const char* a_key, void* a_value)
    {
        const KeyValue* kv = a_list.Find(a_key);
        return kv != nullptr && kv->type == ValueType::kResource && kv->p == a_value;
    }

    void TestBuilderCoreKeys()
    {
        Shape shape{ 1920, 1080, 1280, 720 };
        Settings settings{};
        int color = 1, output = 2, depth = 3, mvec = 4;
        Resources res{ &color, &output, &depth, &mvec };

        const ParamList list = BuildParams(shape, settings, res, true);

        Check(HasUInt(list, "DLSSNR.Width", 1920U), "Width = output width");
        Check(HasUInt(list, "DLSSNR.Height", 1080U), "Height = output height");
        Check(HasUInt(list, "DLSSNR.Enabled", 1U), "Enabled = 1");
        Check(HasUInt(list, "DLSSNR.Reset", 1U), "Reset honoured");
        Check(HasInt(list, "DLSSNR.Style", 0), "Style default 0");
        Check(HasInt(list, "DLSSNR.Hint.Render.Preset", 0), "Preset default 0");
        Check(HasFloat(list, "DLSSNR.Intensity", 1.0F), "Intensity default 1.0 (the stable maximum)");
        Check(HasFloat(list, "DLSSNR.LocalToneStrength", 1.0F), "LocalTone default 1.0");
        Check(HasFloat(list, "DLSSNR.LocalStructureStrength", 1.0F), "LocalStructure default 1.0");
        Check(list.Find("DLSSNR.GlobalToneStrength") == nullptr, "GlobalTone is never pushed (inert in the runtime)");
        Check(HasFloat(list, "DLSSNR.SkinStructureStrength", 1.0F), "Skin default 1.0 inside the auto-skin mask");
        Check(HasUInt(list, "DLSSNR.UseAutoMask", 1U), "the auto skin mask is ON by default (the skin slider's gate)");
        Check(HasInt(list, "DLSSNR.UICorrection", 0), "UICorrection off (pre-HUD)");
        Check(HasInt(list, "DLSSNR.DepthInverted", 0), "FO4 standard Z");
        Check(HasFloat(list, "DLSSNR.ScalingRatio", 1.0F), "ScalingRatio 1");
        Check(list.Find("PerfQualityValue") == nullptr, "PerfQualityValue is NOT pushed (the work scale is gone: the runtime answered ratio 1.0 for every quality and the key changed the look)");
        Check(list.Find("DLSSNR.ControlMask") == nullptr, "no ControlMask key (the experiment was dropped: no gameplay use)");

        Check(HasResource(list, "DLSSNR.Color", &color), "Color resource");
        Check(HasResource(list, "DLSSNR.Output", &output), "Output resource");
        Check(list.Find("DLSSNR.Backbuffer") == nullptr, "Backbuffer is NEVER pushed (with Backbuffer = Output, intensity 0 froze the image)");
        Check(list.Find("DLSSNR.BackbufferSubrectWidth") == nullptr, "...nor its subrect");
        Check(HasResource(list, "DLSSNR.Depth", &depth), "Depth resource");
        Check(HasResource(list, "DLSSNR.MVec", &mvec), "MVec resource");

        Check(HasInt(list, "DLSSNR.ColorSubrectWidth", 1920), "Color subrect = output");
        Check(HasInt(list, "DLSSNR.OutputSubrectHeight", 1080), "Output subrect = output");
        Check(HasInt(list, "DLSSNR.DepthSubrectWidth", 1280), "Depth subrect = render");
        Check(HasInt(list, "DLSSNR.DepthSubrectHeight", 720), "Depth subrect = render");
        Check(HasInt(list, "DLSSNR.MVecSubrectWidth", 1280), "MVec subrect = render");
        Check(HasInt(list, "DLSSNR.MVecSubrectBaseX", 0), "subrect base 0");
        Check(HasFloat(list, "DLSSNR.MVecScaleX", 1280.0F), "MVecScaleX = render width");
        Check(HasFloat(list, "DLSSNR.MVecScaleY", 720.0F), "MVecScaleY = render height");
        Check(list.count < kMaxParams, "list has headroom");
    }

    void TestBuilderOptions()
    {
        Shape shape{ 3840, 2160, 3840, 2160 };
        Settings settings{};
        settings.style = 2;
        settings.preset = 3;
        settings.skinStructure = 0.5F;
        settings.autoMask = false;
        Resources res{};

        const ParamList list = BuildParams(shape, settings, res, false);
        Check(HasUInt(list, "DLSSNR.Reset", 0U), "Reset off");
        Check(HasInt(list, "DLSSNR.Style", 2), "Style cinematic");
        Check(HasInt(list, "DLSSNR.Hint.Render.Preset", 3), "Preset 3");
        Check(HasFloat(list, "DLSSNR.SkinStructureStrength", 0.5F), "Skin explicit");
        Check(HasUInt(list, "DLSSNR.UseAutoMask", 0U), "the auto skin mask can be turned off");
        Check(HasInt(list, "DLSSNR.UICorrection", 0), "UICorrection stays off: no setting can turn it on (pre-HUD pass)");
        Check(HasInt(list, "DLSSNR.DepthInverted", 0), "DepthInverted stays 0 at 4K: Fallout 4 is standard Z and no setting says otherwise");
        Check(HasFloat(list, "DLSSNR.MVecScaleX", 3840.0F), "MVecScaleX is exactly the render width, no multiplier");
        Check(HasFloat(list, "DLSSNR.MVecScaleY", 2160.0F), "MVecScaleY is exactly the render height");
        Check(HasResource(list, "DLSSNR.Color", nullptr), "null resources are still keyed");
        Check(HasFloat(list, "DLSSNR.ScalingRatio", 1.0F), "ScalingRatio is always 1.0 (the runtime scales nothing on 310.8)");
        Check(list.Find("DLSSNR.Backbuffer") == nullptr, "Backbuffer never pushed");
    }

    void TestClampsAndRecreate()
    {
        Check(ClampStyle(3) == 0U, "style out of range -> 0");
        Check(ClampStyle(2) == 2U, "style 2 passes");
        Check(ClampPreset(4) == 0U, "preset out of range -> 0");
        Check(ClampStrength(2.0F, 1.0F) == 1.0F, "strength above the 1.0 maximum -> default (an old 2.0 lands on 1.0)");
        Check(ClampStrength(-0.1F, 1.0F) == 1.0F, "strength below min -> default");
        Check(ClampStrength(0.0F, 1.0F) == 0.0F, "strength 0 passes");
        Check(ClampStrength(1.0F, 1.0F) == 1.0F, "strength 1 passes (the maximum)");

        Settings a{};
        Settings b = a;
        b.intensity = 3.0F;
        Check(!RequiresRecreate(a, b), "strength change is live");
        b = a;
        b.style = 1;
        Check(!RequiresRecreate(a, b), "style change is LIVE (NVIDIA's own plugin never re-creates on it)");
        b = a;
        b.preset = 2;
        Check(RequiresRecreate(a, b), "preset change recreates");
        b = a;
        b.autoMask = false;
        Check(!RequiresRecreate(a, b), "the skin mask is live; preset is the only create-time key");
    }

    void TestRuntimeProfileAndSrgb()
    {
        const auto* staged = FindRuntimeProfile("8270b350cd82de5ce89806872cdd6b6a9249b80836b91bbeb3573470744cc206");
        const auto* packaged = FindRuntimeProfile("e16bcf15e16e13f527491cdf7845b2fe6521a738d8f7c9c721866a8496e1fc8e");
        Check(staged != nullptr && staged->validated, "the staged community Ada variant is the validated profile");
        Check(packaged != nullptr && !packaged->validated, "the Streamline package is known but unvalidated");
        Check(staged != packaged && std::strcmp(staged->label, packaged->label) != 0, "the two entries are distinct");
        Check(FindRuntimeProfile("0000000000000000000000000000000000000000000000000000000000000000") == nullptr, "an unknown hash has no profile");
        Check(FindRuntimeProfile(nullptr) == nullptr, "null is not a hash");
        Check(IsSrgbFormat(29U) && IsSrgbFormat(91U) && IsSrgbFormat(93U), "R8G8B8A8 / B8G8R8A8 / B8G8R8X8 sRGB variants");
        Check(!IsSrgbFormat(28U) && !IsSrgbFormat(87U) && !IsSrgbFormat(24U), "R8G8B8A8_UNORM, B8G8R8A8_UNORM, R10G10B10A2 are not sRGB-typed");
        Check(NonSrgbFormat(29U) == 28U && NonSrgbFormat(91U) == 87U && NonSrgbFormat(93U) == 88U, "the concrete sRGB formats map to their UNORM twins");
        Check(NonSrgbFormat(28U) == 28U && NonSrgbFormat(24U) == 24U && NonSrgbFormat(10U) == 10U, "non-sRGB formats pass through unchanged");
        Check(!IsSrgbFormat(NonSrgbFormat(99U)), "the twin of an sRGB format is never sRGB");
    }

    void TestTimingWindow()
    {
        TimingWindow w{};
        Check(w.Percentile(0.5F) == 0.0F, "an empty window reports 0");
        w.Push(2.5F);
        Check(w.Percentile(0.5F) == 2.5F && w.Percentile(0.99F) == 2.5F, "one sample is every percentile");
        w.Clear();
        for (const float v : { 5.0F, 1.0F, 4.0F, 2.0F, 3.0F }) w.Push(v);
        Check(w.Percentile(0.5F) == 3.0F, "the median of five (nearest rank on the sorted copy)");
        Check(w.Percentile(0.99F) == 5.0F, "p99 of five is the largest");
        Check(w.Percentile(0.0F) == 1.0F && w.Percentile(2.0F) == 5.0F, "the fraction is clamped to [0, 1]");
        w.Clear();
        w.Push(4.0F);
        w.Push(2.0F);
        Check(w.Percentile(0.5F) == 2.0F, "two samples: the median is the lower one (nearest rank, ceil(0.5 * 2) = rank 1)");
        Check(w.Percentile(0.99F) == 4.0F, "two samples: p99 is the upper one (rank 2)");
        for (std::uint32_t i = 0; i < 2U * TimingWindow::kCapacity; ++i) w.Push(1.0F);
        Check(w.count == TimingWindow::kCapacity, "pushes beyond the capacity are dropped, never overrun");
        Check(TicksToMs(1500U, 1000000U) == 1.5 && TicksToMs(7U, 0U) == 0.0, "ticks to ms at a known frequency; 0 frequency -> 0");
    }

    void TestHistoryEpoch()
    {
        HistoryEpochConsumer c{};
        Check(!c.Pending(0), "no event yet: nothing pending");
        Check(c.Pending(1), "a new epoch is pending");
        Check(c.Pending(1), "...and stays pending until acknowledged (an event on a failed evaluate is re-observed)");
        c.Acknowledge(1);
        Check(!c.Pending(1), "acknowledged after an accepted output");
        Check(c.Pending(2), "a newer epoch is pending again");
        c.Acknowledge(1);
        Check(c.Pending(2), "acknowledging the older observation leaves the newer one pending");
    }

    void TestFamilyTable()
    {
        Check(FamilyFromAdapter(0x10DE, 0x2684, L"NVIDIA GeForce RTX 4090") == GpuFamily::kAda,
            "4090 = Ada");
        Check(FamilyFromAdapter(0x10DE, 0x2B85, L"NVIDIA GeForce RTX 5090") == GpuFamily::kBlackwell,
            "5090 = Blackwell");
        Check(FamilyFromAdapter(0x10DE, 0x2204, L"NVIDIA GeForce RTX 3090") == GpuFamily::kAmpere,
            "3090 = Ampere");
        Check(FamilyFromAdapter(0x10DE, 0x1E04, L"NVIDIA GeForce RTX 2080 Ti") == GpuFamily::kTuring,
            "2080 Ti = Turing");
        Check(FamilyFromAdapter(0x10DE, 0x2182, L"NVIDIA GeForce GTX 1660 Ti") == GpuFamily::kTuring,
            "1660 Ti = Turing");
        Check(FamilyFromAdapter(0x10DE, 0x3F00, L"NVIDIA GeForce RTX 5070") == GpuFamily::kBlackwell,
            "description fallback wins over an unknown id");
        Check(FamilyFromAdapter(0x10DE, 0x3F00, L"NVIDIA Something") == GpuFamily::kNewer,
            "unknown high id = newer");
        Check(FamilyFromAdapter(0x1002, 0x744C, L"AMD Radeon RX 7900 XTX") == GpuFamily::kUnknown,
            "non-NVIDIA = unknown");
        Check(std::strcmp(FamilyName(GpuFamily::kAda), "Ada (RTX 40)") == 0, "family name");
    }

    void TestNgxResultNames()
    {
        using namespace Platform::Ngx;
        Check(std::strcmp(ResultName(kSuccess), "Success") == 0, "success name");
        Check(std::strstr(ResultName(static_cast<Result>(0xBAD00002)), "PlatformError") != nullptr,
            "0xBAD00002 = PlatformError");
        Check(std::strcmp(ResultName(static_cast<Result>(0xBAD00005)), "InvalidParameter") == 0,
            "0xBAD00005 = InvalidParameter");
        Check(std::strcmp(ResultName(static_cast<Result>(0xBAD0000B)),
                  "UnableToInitializeFeature") == 0,
            "0xBAD0000B = UnableToInitializeFeature");
        Check(std::strstr(ResultName(42), "not an NGX result") != nullptr, "non-NGX value named");
        Check(!Succeeded(static_cast<Result>(0xBAD00000)), "fail base not success");
    }
}

template <class Cascade>
void TestCascadeTransitions()
{
    Cascade before{};
    before.enabled = true;
    before.passCount = 3;
    if constexpr (requires { RecreateMask(before, before); HistoryResetMask(before, before); }) {
        auto after = before;
        after.passes[1].style = 2;
        Check(RecreateMask(before, after) == 0U, "pass 2 style is live, no feature is destroyed");
        Check(HistoryResetMask(before, after) == 0U,
            "pass 2 style is live: no history restarts (a commit lands every frame of a slider drag)");
        after = before;
        after.passes[1].preset = 2;
        Check(RecreateMask(before, after) == 2U, "pass 2 preset recreates only pass 2");
        Check(HistoryResetMask(before, after) == 6U, "pass 2 new input resets downstream history");
        after = before;
        after.passCount = 1;
        Check(RecreateMask(before, after) == 6U, "reducing to one releases only stages 2 and 3");
        Check(HistoryResetMask(before, after) == 0U, "reducing count preserves the surviving prefix history");
        before.passCount = 1;
        after = before;
        after.passCount = 3;
        Check(RecreateMask(before, after) == 0U, "adding stages preserves the existing pass 1 feature");
        Check(HistoryResetMask(before, after) == 6U, "new stages start with clean histories");
        after = before;
        after.passes[2].preset = 2;
        Check(RecreateMask(before, after) == 0U && HistoryResetMask(before, after) == 0U,
            "editing a dormant stage does not disturb active rendering");
        before.passCount = 3;
        after = before;
        after.enabled = false;
        Check(RecreateMask(before, after) == 7U, "disarming releases all active stages");
        Check(HistoryResetMask(before, after) == 0U, "disabled graph requests no history work");
        before.enabled = false;
        after = before;
        after.enabled = true;
        Check(RecreateMask(before, after) == 0U && HistoryResetMask(before, after) == 7U,
            "arming a three-stage graph creates fresh histories without destroying absent stages");
        before.enabled = true;
        before.passCount = 3;
        after = before;
        after.modelPercent = 75;
        Check(RecreateMask(before, after) == 7U, "a model-extent change re-creates all three active stages together");
        Check(HistoryResetMask(before, after) == 7U, "a model-extent change restarts every active history");
        before.passCount = 1;
        after = before;
        after.modelPercent = 125;
        Check(RecreateMask(before, after) == 1U && HistoryResetMask(before, after) == 1U,
            "with one active stage only that stage re-creates on a model-extent change");
        after = before;
        after.modelPercent = 7;
        Check(RecreateMask(before, after) == 0U && HistoryResetMask(before, after) == 0U,
            "an out-of-range percent is 100 for the masks too: no change, nothing re-creates");
        after = before;
        after.modelPercent = 300;
        Check(RecreateMask(before, after) == 1U && HistoryResetMask(before, after) == 1U,
            "a gated 300 is 150 in effect: a change from 100, the stage re-creates");
        before = after;
        after.modelBeyondPlay = true;
        Check(RecreateMask(before, after) == 1U && HistoryResetMask(before, after) == 1U,
            "opening the gate at 300 changes the extent in effect: the stage re-creates");
        before.modelPercent = after.modelPercent = 100;
        before.modelBeyondPlay = false;
        Check(RecreateMask(before, after) == 0U && HistoryResetMask(before, after) == 0U,
            "the gate alone at 100 changes nothing: no mask");
    } else {
        Check(false, "cascade recreation and downstream-history contracts are not implemented");
    }
}

template <class Cascade>
void TestCascadeExecution()
{
    Cascade request{};
    request.enabled = true;
    request.passCount = 3;
    unsigned events[6]{};
    unsigned used = 0;
    bool publishedNative = false;
    const auto before = [&](std::uint32_t i) noexcept {
        events[used++] = i * 2U;
        publishedNative = false;
    };
    const auto execute = [&](std::uint32_t i) noexcept {
        events[used++] = i * 2U + 1U;
        if (i == 0) publishedNative = true;
        return i < 2U;
    };
    if constexpr (requires { RunCascade(request, before, execute); }) {
        Check(RunCascade(request, before, execute) == 2U && used == 6U,
            "cascade retains the two-stage prefix when pass 3 fails");
        Check(events[0] == 0 && events[1] == 1 && events[2] == 2 && events[3] == 3 &&
                  events[4] == 4 && events[5] == 5,
            "each stage invalidates an earlier publication before executing, in order");
        Check(!publishedNative, "later FP16/failure cannot leave the earlier native HUD-less record visible");
        used = 0;
        request.enabled = false;
        Check(RunCascade(request, before, execute) == 0U && used == 0U, "disabled cascade invokes no stage work");
        used = 0;
        request.enabled = true;
        request.passCount = 99;
        Check(RunCascade(request, before, execute) == 1U && used == 2U,
            "invalid pass count executes only the safe Standard stage");
    } else {
        Check(false, "cascade execution policy is not implemented");
    }
}

template <class Cascade>
void TestStageOutputProof()
{
    using namespace Platform::Neural;
    if constexpr (requires(Cascade c) { CanUseStageOutput(c.enabled, true, std::uint64_t{7}, true); }) {
        Cascade fixture{};
        fixture.enabled = true;
        Check(CanUseStageOutput(fixture.enabled, true, 7, true), "a successful retired handoff permits copyback");
        fixture.enabled = false;
        Check(!CanUseStageOutput(fixture.enabled, true, 7, true), "failed evaluation cannot publish an output");
        fixture.enabled = true;
        Check(!CanUseStageOutput(fixture.enabled, false, 7, true), "a dropped command list cannot publish stale output");
        Check(!CanUseStageOutput(fixture.enabled, true, 0, true), "zero token from failed Signal is not output completion");
        Check(!CanUseStageOutput(fixture.enabled, true, 7, false), "a failed context wait cannot permit copyback");
    } else {
        Check(false, "neural output proof is not implemented");
    }
}

template <class Index>
void TestRuntimeHistoryGaps()
{
    using namespace Platform::Neural;
    if constexpr (requires(Index first) { HistoryTailMask(first); }) {
        Check(HistoryTailMask(Index{0}) == 7U,
            "a wholly skipped frame invalidates every stage history");
        Check(HistoryTailMask(Index{1}) == 6U,
            "pass 2 failure preserves pass 1 history and resets the skipped suffix");
        Check(HistoryTailMask(Index{2}) == 4U,
            "a rebuilt pass 2 invalidates its downstream pass 3 history");
        Check(HistoryTailMask(Index{3}) == 0U,
            "three delivered passes preserve all continuing histories");
        Check(HistoryTailMask(Index{99}) == 0U,
            "out-of-range stage indices never shift past the mask width");
    } else {
        Check(false, "runtime history-gap propagation is not implemented");
    }
}

void TestModelExtent()
{
    using Platform::Neural::ComputeModelExtent;
    using Platform::Neural::ModelExtent;
    using Platform::Neural::ClampModelPercent;
    Check(ComputeModelExtent(1920, 1080, 100) == ModelExtent{ 1920, 1080 }, "(1920,1080,100) -> (1920,1080): the exact path");
    Check(ComputeModelExtent(1920, 1080, 75) == ModelExtent{ 1440, 810 }, "(1920,1080,75) -> (1440,810)");
    Check(ComputeModelExtent(1920, 1080, 50) == ModelExtent{ 960, 540 }, "(1920,1080,50) -> (960,540)");
    Check(ComputeModelExtent(2558, 1439, 75) == ModelExtent{ 1919, 1079 }, "(2558,1439,75) -> (1919,1079): nearest-integer rounding");
    Check(ComputeModelExtent(1920, 1080, 125) == ModelExtent{ 2400, 1350 }, "(1920,1080,125) -> (2400,1350)");
    Check(ComputeModelExtent(1920, 1080, 150) == ModelExtent{ 2880, 1620 }, "(1920,1080,150) -> (2880,1620)");
    Check(ComputeModelExtent(1920, 1080, 200) == ModelExtent{ 3840, 2160 }, "(1920,1080,200) -> (3840,2160)");
    Check(ComputeModelExtent(1920, 1080, 300) == ModelExtent{ 5760, 3240 }, "(1920,1080,300) -> (5760,3240)");
    Check(!ComputeModelExtent(0, 1080, 75).Valid() && !ComputeModelExtent(1920, 0, 75).Valid(), "a zero output dimension is the unavailable extent (0,0), never floored to 1");
    Check(ComputeModelExtent(1920, 1080, 7) == ModelExtent{ 1920, 1080 } && ComputeModelExtent(1920, 1080, 301) == ModelExtent{ 1920, 1080 },
        "an out-of-range percentage is normalised to 100 without overflow");
    Check(ComputeModelExtent(0xFFFFFFFFU, 0xFFFFFFFFU, 150) == ModelExtent{}, "an extent that would overflow 32 bits is unavailable, not wrapped");
    Check(ComputeModelExtent(60, 60, 50) == ModelExtent{} && ComputeModelExtent(64, 64, 50) == ModelExtent{ 32, 32 },
        "below the 32 px shape floor the extent is unavailable; exactly 32 is the smallest model");
    Check(ClampModelPercent(50) == 50 && ClampModelPercent(300) == 300 && ClampModelPercent(49) == 100 && ClampModelPercent(301) == 100 && ClampModelPercent(0) == 100,
        "the clamp keeps [50, 300] and lands everything else on 100");
    Check(Platform::Neural::CascadeSettings{}.modelPercent == 100U, "legacy settings without a model percent are 100: the identical image path");
    {
        Platform::Neural::CascadeSettings gated{};
        Check(!gated.modelBeyondPlay && kModelPercentPlayMax == 150U, "the gate is closed by default; the play ceiling is 150");
        gated.modelPercent = 300;
        Check(EffectiveModelPercent(gated) == 150U, "300 with the gate closed is the ceiling in effect (150), not 100: the value was in range, only gated");
        gated.modelBeyondPlay = true;
        Check(EffectiveModelPercent(gated) == 300U, "with the gate open 300 stands");
        gated.modelPercent = 400;
        Check(EffectiveModelPercent(gated) == 100U, "out of range is 100 before the gate is asked");
        gated.modelBeyondPlay = false;
        gated.modelPercent = 150;
        Check(EffectiveModelPercent(gated) == 150U, "the ceiling itself passes a closed gate");
        gated.modelPercent = 50;
        Check(EffectiveModelPercent(gated) == 50U, "below the ceiling the gate is not asked");
    }
}

int main()
{
    TestModelExtent();
    TestBuilderCoreKeys();
    TestBuilderOptions();
    TestClampsAndRecreate();
    TestHistoryEpoch();
    TestTimingWindow();
    TestRuntimeProfileAndSrgb();
    TestFamilyTable();
    TestNgxResultNames();
    TestCascadeTransitions<Platform::Neural::CascadeSettings>();
    TestCascadeExecution<Platform::Neural::CascadeSettings>();
    TestStageOutputProof<Platform::Neural::CascadeSettings>();
    TestRuntimeHistoryGaps<std::uint32_t>();
    if (g_failures == 0) {
        std::printf("NeuralContractTests: all passed\n");
        return 0;
    }
    std::printf("NeuralContractTests: %d failure(s)\n", g_failures);
    return 1;
}
