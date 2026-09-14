#include "Platform/WiringAdapters.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace
{
    using Platform::AaEffectiveEngine;
    using Platform::AaRequest;
    using Platform::AppliedSamplerEngine;
    using Platform::AppliedSamplerIntent;
    using Platform::D3D11SamplerDescFields;
    using Platform::DeriveSamplerIntent;
    using Platform::FrameStamp;
    using Platform::MakeBiasedDescriptor;
    using Platform::PackSamplerDescriptor;
    using Platform::PublishedSnapshot;
    using Platform::SamplerDescriptor;
    using Platform::UnpackSamplerDescriptor;
    using Platform::kAaRecreateBoth;
    using Platform::kAaRecreateDlss;
    using Platform::kAaRecreateFsr;
    using Platform::kAaRecreateNone;
    using Platform::kD3D11FilterAnisotropic;
    using Platform::kAppliedSamplerEngineNone;

    struct CaseResult
    {
        std::string name;
        int checks{ 0 };
        int failures{ 0 };
        std::vector<std::string> messages;
    };

    CaseResult* g_active{ nullptr };

    void Check(bool condition, std::string_view message)
    {
        if (g_active == nullptr) {
            return;
        }
        ++g_active->checks;
        if (!condition) {
            ++g_active->failures;
            g_active->messages.emplace_back(message);
        }
    }

    template <class T, class U>
    void CheckEqual(const T& actual, const U& expected, std::string_view message)
    {
        Check(actual == expected, message);
    }

    constexpr std::size_t kOffFilter = 0;
    constexpr std::size_t kOffAddressU = 4;
    constexpr std::size_t kOffAddressV = 8;
    constexpr std::size_t kOffAddressW = 12;
    constexpr std::size_t kOffMipLodBias = 16;
    constexpr std::size_t kOffMaxAnisotropy = 20;
    constexpr std::size_t kOffComparisonFunc = 24;
    constexpr std::size_t kOffBorderColor = 28;
    constexpr std::size_t kOffMinLod = 44;
    constexpr std::size_t kOffMaxLod = 48;

    void PutU32Raw(SamplerDescriptor& descriptor, std::size_t offset, std::uint32_t value)
    {
        std::memcpy(descriptor.bytes.data() + offset, &value, sizeof(value));
    }

    [[nodiscard]] std::uint32_t GetU32Raw(const SamplerDescriptor& descriptor, std::size_t offset)
    {
        std::uint32_t value{};
        std::memcpy(&value, descriptor.bytes.data() + offset, sizeof(value));
        return value;
    }

    void PutFloatRaw(SamplerDescriptor& descriptor, std::size_t offset, float value)
    {
        std::memcpy(descriptor.bytes.data() + offset, &value, sizeof(value));
    }

    [[nodiscard]] float GetFloatRaw(const SamplerDescriptor& descriptor, std::size_t offset)
    {
        float value{};
        std::memcpy(&value, descriptor.bytes.data() + offset, sizeof(value));
        return value;
    }

    [[nodiscard]] SamplerDescriptor MakeRawDescriptor(const D3D11SamplerDescFields& fields)
    {
        SamplerDescriptor descriptor{};
        PutU32Raw(descriptor, kOffFilter, fields.filter);
        PutU32Raw(descriptor, kOffAddressU, fields.addressU);
        PutU32Raw(descriptor, kOffAddressV, fields.addressV);
        PutU32Raw(descriptor, kOffAddressW, fields.addressW);
        PutFloatRaw(descriptor, kOffMipLodBias, fields.mipLodBias);
        PutU32Raw(descriptor, kOffMaxAnisotropy, fields.maxAnisotropy);
        PutU32Raw(descriptor, kOffComparisonFunc, fields.comparisonFunc);
        PutFloatRaw(descriptor, kOffBorderColor + 0, fields.borderColor[0]);
        PutFloatRaw(descriptor, kOffBorderColor + 4, fields.borderColor[1]);
        PutFloatRaw(descriptor, kOffBorderColor + 8, fields.borderColor[2]);
        PutFloatRaw(descriptor, kOffBorderColor + 12, fields.borderColor[3]);
        PutFloatRaw(descriptor, kOffMinLod, fields.minLod);
        PutFloatRaw(descriptor, kOffMaxLod, fields.maxLod);
        return descriptor;
    }

    [[nodiscard]] D3D11SamplerDescFields MakeSampleFields()
    {
        D3D11SamplerDescFields fields;
        fields.filter = kD3D11FilterAnisotropic;
        fields.addressU = 1;
        fields.addressV = 2;
        fields.addressW = 3;
        fields.mipLodBias = -1.5F;
        fields.maxAnisotropy = 16;
        fields.comparisonFunc = 4;
        fields.borderColor = { 0.1F, 0.2F, 0.3F, 0.4F };
        fields.minLod = 0.25F;
        fields.maxLod = 12.5F;
        return fields;
    }

    void FillSentinel(SamplerDescriptor& descriptor)
    {
        descriptor.bytes.fill(std::uint8_t{ 0xAB });
    }

    void CaseB1_PackWritesEveryFieldAtItsRealD3D11ByteOffset()
    {
        const auto fields = MakeSampleFields();
        const SamplerDescriptor packed = PackSamplerDescriptor(fields);

        CheckEqual(GetU32Raw(packed, kOffFilter), fields.filter, "Pack must write Filter at byte offset 0");
        CheckEqual(GetU32Raw(packed, kOffAddressU), fields.addressU, "Pack must write AddressU at byte offset 4");
        CheckEqual(GetU32Raw(packed, kOffAddressV), fields.addressV, "Pack must write AddressV at byte offset 8");
        CheckEqual(GetU32Raw(packed, kOffAddressW), fields.addressW, "Pack must write AddressW at byte offset 12");
        CheckEqual(GetFloatRaw(packed, kOffMipLodBias), fields.mipLodBias,
            "Pack must write MipLODBias at byte offset 16");
        CheckEqual(GetU32Raw(packed, kOffMaxAnisotropy), fields.maxAnisotropy,
            "Pack must write MaxAnisotropy at byte offset 20");
        CheckEqual(GetU32Raw(packed, kOffComparisonFunc), fields.comparisonFunc,
            "Pack must write ComparisonFunc at byte offset 24");
        CheckEqual(GetFloatRaw(packed, kOffBorderColor + 0), fields.borderColor[0],
            "Pack must write BorderColor[0] at byte offset 28");
        CheckEqual(GetFloatRaw(packed, kOffBorderColor + 4), fields.borderColor[1],
            "Pack must write BorderColor[1] at byte offset 32");
        CheckEqual(GetFloatRaw(packed, kOffBorderColor + 8), fields.borderColor[2],
            "Pack must write BorderColor[2] at byte offset 36");
        CheckEqual(GetFloatRaw(packed, kOffBorderColor + 12), fields.borderColor[3],
            "Pack must write BorderColor[3] at byte offset 40");
        CheckEqual(GetFloatRaw(packed, kOffMinLod), fields.minLod, "Pack must write MinLOD at byte offset 44");
        CheckEqual(GetFloatRaw(packed, kOffMaxLod), fields.maxLod, "Pack must write MaxLOD at byte offset 48");
    }

    void CaseB2_UnpackReadsEveryFieldFromItsRealD3D11ByteOffset()
    {
        const auto fields = MakeSampleFields();
        const SamplerDescriptor raw = MakeRawDescriptor(fields);
        const D3D11SamplerDescFields unpacked = UnpackSamplerDescriptor(raw);

        Check(unpacked == fields,
            "Unpack(raw bytes at the real D3D11_SAMPLER_DESC offsets) must reproduce every field exactly");
        CheckEqual(unpacked.filter, fields.filter, "Unpack must read Filter from byte offset 0");
        CheckEqual(unpacked.mipLodBias, fields.mipLodBias, "Unpack must read MipLODBias from byte offset 16");
        CheckEqual(unpacked.maxAnisotropy, fields.maxAnisotropy, "Unpack must read MaxAnisotropy from byte offset 20");
        CheckEqual(unpacked.minLod, fields.minLod, "Unpack must read MinLOD from byte offset 44");
        CheckEqual(unpacked.maxLod, fields.maxLod, "Unpack must read MaxLOD from byte offset 48");
    }

    void CaseB3_PackThenUnpackRoundTripsToAnIdenticalDescriptor()
    {
        const auto original = MakeSampleFields();
        const D3D11SamplerDescFields roundTripped = UnpackSamplerDescriptor(PackSamplerDescriptor(original));
        Check(roundTripped == original,
            "Unpack(Pack(fields)) must reproduce the original fields exactly (round-trip identity)");
    }

    void CaseC1_EligibilityGateCoversVirginAnisoPreBiasedAndNonAnisoCombinations()
    {
        D3D11SamplerDescFields virginAniso;
        virginAniso.filter = kD3D11FilterAnisotropic;
        virginAniso.mipLodBias = 0.0F;
        virginAniso.maxAnisotropy = 16;
        const SamplerDescriptor virginAnisoRaw = MakeRawDescriptor(virginAniso);
        SamplerDescriptor virginOutput{};
        Check(MakeBiasedDescriptor(virginAnisoRaw, -1.0F, virginOutput),
            "a virgin anisotropic sampler (Filter==0x55, MipLODBias==0.0) must report true (needs a biased clone)");

        D3D11SamplerDescFields preBiasedAniso = virginAniso;
        preBiasedAniso.mipLodBias = -2.0F;
        const SamplerDescriptor preBiasedRaw = MakeRawDescriptor(preBiasedAniso);
        SamplerDescriptor preBiasedOutput{};
        FillSentinel(preBiasedOutput);
        const SamplerDescriptor preBiasedSentinel = preBiasedOutput;
        Check(!MakeBiasedDescriptor(preBiasedRaw, -1.0F, preBiasedOutput),
            "an anisotropic sampler that already carries a non-zero MipLODBias is not virgin -- must be "
            "an exact pass-through (false)");
        Check(preBiasedOutput == preBiasedSentinel,
            "false means EXACT pass-through -- `output` must be left completely untouched, not zeroed "
            "or partially written");

        D3D11SamplerDescFields nonAnisoZeroBias = virginAniso;
        nonAnisoZeroBias.filter = 0;
        const SamplerDescriptor nonAnisoZeroRaw = MakeRawDescriptor(nonAnisoZeroBias);
        SamplerDescriptor nonAnisoZeroOutput{};
        FillSentinel(nonAnisoZeroOutput);
        const SamplerDescriptor nonAnisoZeroSentinel = nonAnisoZeroOutput;
        Check(!MakeBiasedDescriptor(nonAnisoZeroRaw, -1.0F, nonAnisoZeroOutput),
            "a non-anisotropic filter must be an exact pass-through (false) even with MipLODBias == 0.0");
        Check(nonAnisoZeroOutput == nonAnisoZeroSentinel,
            "false means EXACT pass-through -- `output` must be left completely untouched");

        D3D11SamplerDescFields nonAnisoNonZeroBias = nonAnisoZeroBias;
        nonAnisoNonZeroBias.mipLodBias = -1.0F;
        const SamplerDescriptor nonAnisoNonZeroRaw = MakeRawDescriptor(nonAnisoNonZeroBias);
        SamplerDescriptor nonAnisoNonZeroOutput{};
        Check(!MakeBiasedDescriptor(nonAnisoNonZeroRaw, -1.0F, nonAnisoNonZeroOutput),
            "a non-anisotropic filter with a non-zero MipLODBias must also be an exact pass-through (false)");
    }

    void CaseC2_TrueBranchClampsBiasExactlyLikeClampMipBias()
    {
        D3D11SamplerDescFields virginAniso;
        virginAniso.filter = kD3D11FilterAnisotropic;
        virginAniso.mipLodBias = 0.0F;
        const SamplerDescriptor source = MakeRawDescriptor(virginAniso);

        struct BiasCase
        {
            float input;
            float expectedClamped;
            const char* label;
        };
        const BiasCase cases[] = {
            { -1.5F, -1.5F, "an in-range bias must pass through unchanged" },
            { -50.0F, -3.0F, "a too-negative finite bias must clamp to -3.0 (DlaaSettings::ClampMipBias range)" },
            { 10.0F, 0.0F, "a too-positive finite bias must clamp to 0.0" },
            { std::numeric_limits<float>::quiet_NaN(), 0.0F,
                "a NaN bias must clamp to 0.0 (non-finite -> 0, mirrors ClampMipBias)" },
            { std::numeric_limits<float>::infinity(), 0.0F, "a +Infinity bias must clamp to 0.0" },
            { -std::numeric_limits<float>::infinity(), 0.0F,
                "a -Infinity bias must clamp to 0.0, NOT -3.0 -- ClampMipBias checks isfinite() first, "
                "sign-agnostic, before ever comparing against the [-3, 0] range" },
        };

        for (const auto& c : cases) {
            SamplerDescriptor output{};
            const bool result = MakeBiasedDescriptor(source, c.input, output);
            Check(result, std::string("a virgin anisotropic sampler must report true regardless of bias "
                                      "value -- ") + c.label);
            CheckEqual(GetFloatRaw(output, kOffMipLodBias), c.expectedClamped,
                std::string("clamped MipLODBias mismatch -- ") + c.label);
        }
    }

    void CaseC3_TrueBranchPreservesEveryOtherFieldExactlyExceptMipLodBias()
    {
        D3D11SamplerDescFields fields;
        fields.filter = kD3D11FilterAnisotropic;
        fields.mipLodBias = 0.0F;
        fields.addressU = 5;
        fields.addressV = 6;
        fields.addressW = 7;
        fields.maxAnisotropy = 16;
        fields.comparisonFunc = 8;
        fields.borderColor = { 0.9F, 0.8F, 0.7F, 0.6F };
        fields.minLod = 1.25F;
        fields.maxLod = 9.5F;
        const SamplerDescriptor source = MakeRawDescriptor(fields);

        SamplerDescriptor output{};
        const bool result = MakeBiasedDescriptor(source, -2.0F, output);

        Check(result, "a virgin anisotropic sampler must report true so the preserved-field assertions "
                      "below are meaningful");
        CheckEqual(GetU32Raw(output, kOffFilter), fields.filter, "Filter must be preserved unchanged on the biased clone");
        CheckEqual(GetU32Raw(output, kOffAddressU), fields.addressU, "AddressU must be preserved unchanged");
        CheckEqual(GetU32Raw(output, kOffAddressV), fields.addressV, "AddressV must be preserved unchanged");
        CheckEqual(GetU32Raw(output, kOffAddressW), fields.addressW, "AddressW must be preserved unchanged");
        CheckEqual(GetU32Raw(output, kOffMaxAnisotropy), fields.maxAnisotropy,
            "MaxAnisotropy must be preserved unchanged (the ONLY field this function changes is MipLODBias)");
        CheckEqual(GetU32Raw(output, kOffComparisonFunc), fields.comparisonFunc, "ComparisonFunc must be preserved unchanged");
        CheckEqual(GetFloatRaw(output, kOffBorderColor + 0), fields.borderColor[0], "BorderColor[0] must be preserved unchanged");
        CheckEqual(GetFloatRaw(output, kOffBorderColor + 12), fields.borderColor[3], "BorderColor[3] must be preserved unchanged");
        CheckEqual(GetFloatRaw(output, kOffMinLod), fields.minLod, "MinLOD must be preserved unchanged");
        CheckEqual(GetFloatRaw(output, kOffMaxLod), fields.maxLod, "MaxLOD must be preserved unchanged");
        CheckEqual(GetFloatRaw(output, kOffMipLodBias), -2.0F, "MipLODBias must be replaced with the (clamped) requested bias");
    }

    void CaseD1_DlssEngineSelectedAndActiveMapsMipBiasAndCommit()
    {
        AaRequest applied;
        applied.dlaaEnabled = true;
        applied.effectiveEngine = AaEffectiveEngine::kDlss;
        applied.mipBias = -1.5F;
        applied.fsrMipBias = -2.7F;
        applied.generation = 42;

        const AppliedSamplerIntent intent = DeriveSamplerIntent(applied, kAaRecreateNone);

        Check(intent.engine == AppliedSamplerEngine::kDlss, "effective DLSS must select AppliedSamplerEngine::kDlss");
        CheckEqual(intent.bias, applied.mipBias, "DLSS selected: bias must come from mipBias, never fsrMipBias");
        Check(intent.active, "dlaaEnabled == true and the DLSS scope bit is not damaged -- intent must be active");
        CheckEqual(intent.appliedCommit, applied.generation, "appliedCommit must mirror Applied().generation exactly");
    }

    void CaseD2_FsrEngineSelectedAndActiveMapsFsrMipBiasAndCommit()
    {
        AaRequest applied;
        applied.dlaaEnabled = true;
        applied.effectiveEngine = AaEffectiveEngine::kFsr;
        applied.mipBias = -1.9F;
        applied.fsrMipBias = -2.5F;
        applied.generation = 7;

        const AppliedSamplerIntent intent = DeriveSamplerIntent(applied, kAaRecreateNone);

        Check(intent.engine == AppliedSamplerEngine::kFsr, "effective FSR must select AppliedSamplerEngine::kFsr");
        CheckEqual(intent.bias, applied.fsrMipBias, "FSR selected: bias must come from fsrMipBias, never mipBias");
        Check(intent.active, "dlaaEnabled == true and the FSR scope bit is not damaged -- intent must be active");
        CheckEqual(intent.appliedCommit, applied.generation, "appliedCommit must mirror Applied().generation exactly");
    }

    void CaseD3_MasterDisabledStillAppliesBias()
    {
        AaRequest applied;
        applied.dlaaEnabled = false;
        applied.effectiveEngine = AaEffectiveEngine::kFsr;
        applied.fsrMipBias = -2.0F;
        applied.generation = 99;

        const AppliedSamplerIntent intent = DeriveSamplerIntent(applied, kAaRecreateNone);

        Check(intent.active, "dlaaEnabled == false must NOT deactivate the bias");
        Check(intent.engine == AppliedSamplerEngine::kFsr, "engine selection still reflects effectiveEngine");
        CheckEqual(intent.bias, applied.fsrMipBias, "bias must map correctly");
        CheckEqual(intent.appliedCommit, applied.generation, "appliedCommit must mirror Applied().generation");
    }

    void CaseD4_SelectedEngineDamagedMakesInactiveFailClosed()
    {
        {
            AaRequest applied;
            applied.dlaaEnabled = true;
            applied.effectiveEngine = AaEffectiveEngine::kDlss;
            applied.mipBias = -1.0F;
            applied.generation = 5;
            const AppliedSamplerIntent intent = DeriveSamplerIntent(applied, kAaRecreateDlss);
            Check(!intent.active, "the SELECTED engine's (DLSS) own damaged bit must make the intent inactive (fail-closed)");
            CheckEqual(intent.bias, applied.mipBias, "bias mapping must be unaffected by the active/damage gate");
            CheckEqual(intent.appliedCommit, applied.generation, "appliedCommit must be unaffected by the active/damage gate");
        }
        {
            AaRequest applied;
            applied.dlaaEnabled = true;
            applied.effectiveEngine = AaEffectiveEngine::kFsr;
            applied.fsrMipBias = -1.2F;
            applied.generation = 6;
            const AppliedSamplerIntent intent = DeriveSamplerIntent(applied, kAaRecreateFsr);
            Check(!intent.active, "the SELECTED engine's (FSR) own damaged bit must make the intent inactive (fail-closed)");
            CheckEqual(intent.bias, applied.fsrMipBias, "bias mapping must be unaffected by the active/damage gate");
            CheckEqual(intent.appliedCommit, applied.generation, "appliedCommit must be unaffected by the active/damage gate");
        }
        {
            AaRequest applied;
            applied.dlaaEnabled = true;
            applied.effectiveEngine = AaEffectiveEngine::kDlss;
            applied.mipBias = -0.5F;
            applied.generation = 8;
            const AppliedSamplerIntent intent = DeriveSamplerIntent(applied, kAaRecreateBoth);
            Check(!intent.active, "a both-engine damaged scope must still be inactive for whichever engine is selected");
            CheckEqual(intent.bias, applied.mipBias, "bias mapping must be unaffected by the active/damage gate");
            CheckEqual(intent.appliedCommit, applied.generation, "appliedCommit must be unaffected by the active/damage gate");
        }
    }

    void CaseD5_OtherEngineDamagedDoesNotAffectTheSelectedEngine()
    {
        {
            AaRequest applied;
            applied.dlaaEnabled = true;
            applied.effectiveEngine = AaEffectiveEngine::kDlss;
            applied.mipBias = -1.1F;
            applied.generation = 11;
            const AppliedSamplerIntent intent = DeriveSamplerIntent(applied, kAaRecreateFsr);
            Check(intent.active, "DLSS selected: the FSR damage bit must not affect DLSS's active status");
            Check(intent.engine == AppliedSamplerEngine::kDlss, "engine selection sanity");
            CheckEqual(intent.bias, applied.mipBias, "bias mapping sanity");
            CheckEqual(intent.appliedCommit, applied.generation, "appliedCommit mapping sanity");
        }
        {
            AaRequest applied;
            applied.dlaaEnabled = true;
            applied.effectiveEngine = AaEffectiveEngine::kFsr;
            applied.fsrMipBias = -1.3F;
            applied.generation = 12;
            const AppliedSamplerIntent intent = DeriveSamplerIntent(applied, kAaRecreateDlss);
            Check(intent.active, "FSR selected: the DLSS damage bit must not affect FSR's active status");
            Check(intent.engine == AppliedSamplerEngine::kFsr, "engine selection sanity");
            CheckEqual(intent.bias, applied.fsrMipBias, "bias mapping sanity");
            CheckEqual(intent.appliedCommit, applied.generation, "appliedCommit mapping sanity");
        }
    }

    void CaseD6_ZeroBiasIntentIsStillValidAndActive()
    {
        AaRequest applied;
        applied.dlaaEnabled = true;
        applied.effectiveEngine = AaEffectiveEngine::kDlss;
        applied.mipBias = 0.0F;
        applied.generation = 3;
        const AppliedSamplerIntent intent = DeriveSamplerIntent(applied, kAaRecreateNone);

        Check(intent.active, "a ZERO bias must still be a fully valid, ACTIVE intent -- the controller "
                              "handles bias == 0 as \"no clones needed\", not as \"AA inactive\"");
        CheckEqual(intent.bias, 0.0F, "bias must be exactly 0.0, passed through faithfully");
        CheckEqual(intent.appliedCommit, applied.generation, "appliedCommit mapping sanity");
    }

    void CaseD7_UnresolvedFailsClosedButNoneAppliesBias()
    {
        {
            AaRequest applied;
            applied.dlaaEnabled = true;
            applied.effectiveEngine = AaEffectiveEngine::kUnresolved;
            applied.mipBias = -3.0F;
            applied.fsrMipBias = -3.0F;
            applied.generation = 77;
            const AppliedSamplerIntent intent = DeriveSamplerIntent(applied, kAaRecreateNone);
            Check(!intent.active, "unresolved capabilities must still fail closed");
            Check(intent.engine == kAppliedSamplerEngineNone, "unresolved carries the no-engine sentinel");
            CheckEqual(intent.bias, 0.0F, "unresolved must not leak either engine's bias");
            CheckEqual(intent.appliedCommit, applied.generation, "commit identity preserved");
        }
        {
            AaRequest applied;
            applied.dlaaEnabled = false;
            applied.effectiveEngine = AaEffectiveEngine::kNone;
            applied.mipBias = -1.5F;
            applied.fsrMipBias = -3.0F;
            applied.generation = 78;
            const AppliedSamplerIntent intent = DeriveSamplerIntent(applied, kAaRecreateNone);
            Check(intent.active, "kNone with a nonzero mipBias must be ACTIVE (bias works without DLAA)");
            Check(intent.engine == AppliedSamplerEngine::kDlss, "kNone carries the kDlss bias-domain tag");
            CheckEqual(intent.bias, applied.mipBias, "kNone selects the DLSS-side (generic) bias field");
            CheckEqual(intent.appliedCommit, applied.generation, "commit identity preserved");
        }
        {
            AaRequest applied;
            applied.dlaaEnabled = false;
            applied.effectiveEngine = AaEffectiveEngine::kNone;
            applied.mipBias = 0.0F;
            applied.generation = 79;
            const AppliedSamplerIntent intent = DeriveSamplerIntent(applied, kAaRecreateNone);
            Check(!intent.active, "kNone with zero bias derives inactive (no-op, not a fake intent)");
        }
    }

    void CaseE1_DefaultConstructedFirstNextReturnsOne()
    {
        FrameStamp stamp;
        CheckEqual(stamp.Current(), std::uint64_t{ 0 },
            "a freshly default-constructed FrameStamp must report Current() == 0 (no id minted yet)");
        CheckEqual(stamp.Next(), std::uint64_t{ 1 }, "the first Next() call after default construction must return 1");
        CheckEqual(stamp.Current(), std::uint64_t{ 1 },
            "Current() must reflect the id just minted by Next(), without minting a new one");
        CheckEqual(stamp.Current(), std::uint64_t{ 1 }, "a second Current() call must not itself advance anything");
    }

    void CaseE2_SequentialCallsAreStrictlyMonotonicWithNoGapsOrRepeats()
    {
        FrameStamp stamp;
        constexpr int kIterations = 10000;
        std::uint64_t previous = stamp.Current();
        bool allStrictlyOneGreater = true;
        for (int i = 0; i < kIterations; ++i) {
            const std::uint64_t next = stamp.Next();
            if (next != previous + 1) {
                allStrictlyOneGreater = false;
            }
            previous = next;
        }
        Check(allStrictlyOneGreater,
            "every sequential Next() call must return exactly one more than the previously returned id "
            "-- no gaps, no repeats, no regressions");
        CheckEqual(stamp.Current(), previous, "Current() after the loop must equal the very last id Next() minted");
    }

    void CaseE3_ExplicitInitialValueSeedsTheCounter()
    {
        FrameStamp stamp{ 100 };
        CheckEqual(stamp.Current(), std::uint64_t{ 100 },
            "constructing with an explicit initial value must make Current() report that value before any Next() call");
        CheckEqual(stamp.Next(), std::uint64_t{ 101 }, "the first Next() after FrameStamp(100) must return 101");
        CheckEqual(stamp.Next(), std::uint64_t{ 102 }, "the second Next() after FrameStamp(100) must return 102");
    }

    void CaseE4_WraparoundFromUint64MaxIsWellDefinedAndKeepsAdvancing()
    {
        FrameStamp stamp{ std::numeric_limits<std::uint64_t>::max() };
        CheckEqual(stamp.Next(), std::uint64_t{ 0 },
            "one past UINT64_MAX must wrap to 0 -- well-defined modular arithmetic, never UB/a crash");
        CheckEqual(stamp.Next(), std::uint64_t{ 1 }, "the id after the wraparound must keep advancing normally");
        CheckEqual(stamp.Next(), std::uint64_t{ 2 }, "monotonic advance must continue past the wraparound point");
    }

    void CaseE5_ConcurrentCallersEachReceiveAUniqueContiguousId()
    {
        FrameStamp stamp;
        constexpr int kThreadCount = 8;
        constexpr int kCallsPerThread = 500;
        std::vector<std::vector<std::uint64_t>> perThreadResults(kThreadCount);
        {
            std::vector<std::thread> threads;
            threads.reserve(kThreadCount);
            for (int t = 0; t < kThreadCount; ++t) {
                threads.emplace_back([&stamp, &perThreadResults, t]() {
                    auto& results = perThreadResults[static_cast<std::size_t>(t)];
                    results.reserve(kCallsPerThread);
                    for (int i = 0; i < kCallsPerThread; ++i) {
                        results.push_back(stamp.Next());
                    }
                });
            }
            for (auto& thread : threads) {
                thread.join();
            }
        }

        std::vector<std::uint64_t> all;
        all.reserve(static_cast<std::size_t>(kThreadCount) * static_cast<std::size_t>(kCallsPerThread));
        for (auto& perThread : perThreadResults) {
            all.insert(all.end(), perThread.begin(), perThread.end());
        }
        std::sort(all.begin(), all.end());
        all.erase(std::unique(all.begin(), all.end()), all.end());

        CheckEqual(all.size(), static_cast<std::size_t>(kThreadCount) * static_cast<std::size_t>(kCallsPerThread),
            "every concurrent Next() call across all threads must return a UNIQUE id -- no two callers "
            "may ever receive the same frame id");
        if (!all.empty()) {
            const bool contiguous = (all.back() - all.front() + 1) == static_cast<std::uint64_t>(all.size());
            Check(contiguous,
                "the full set of concurrently minted ids must be an exact contiguous range with no gaps "
                "(no lost updates)");
        }
    }

    struct SampleSnapshot
    {
        std::uint64_t generation{ 0 };
        std::uint64_t doubled{ 0 };
        int tag{ -1 };

        [[nodiscard]] friend bool operator==(const SampleSnapshot&, const SampleSnapshot&) = default;
    };

    void CaseF1_DefaultConstructedNeverReturnsNullAndHoldsADefaultValue()
    {
        PublishedSnapshot<SampleSnapshot> snapshot;
        const std::shared_ptr<const SampleSnapshot> node = snapshot.Read();
        Check(node != nullptr, "a freshly default-constructed PublishedSnapshot's Read() must never return "
                               "null, even before any Publish() call");
        if (node) {
            CheckEqual(node->generation, std::uint64_t{ 0 }, "the initial node must hold a default-constructed T (generation == 0)");
            CheckEqual(node->tag, -1, "the initial node must hold a default-constructed T (tag == -1)");
        }
    }

    void CaseF2_PublishThenReadReturnsExactlyThePublishedValue()
    {
        PublishedSnapshot<SampleSnapshot> snapshot;
        SampleSnapshot value{};
        value.generation = 7;
        value.doubled = 14;
        value.tag = 99;
        snapshot.Publish(value);

        const std::shared_ptr<const SampleSnapshot> node = snapshot.Read();
        Check(node != nullptr, "Read() must never return null after a successful Publish()");
        if (node) {
            CheckEqual(node->generation, std::uint64_t{ 7 }, "Read() must reflect the just-published generation field");
            CheckEqual(node->doubled, std::uint64_t{ 14 }, "Read() must reflect the just-published doubled field");
            CheckEqual(node->tag, 99, "Read() must reflect the just-published tag field");
        }
    }

    void CaseF3_ReaderHoldingAnOldNodeIsUnaffectedByLaterPublishes()
    {
        PublishedSnapshot<SampleSnapshot> snapshot;
        SampleSnapshot first{};
        first.generation = 1;
        first.doubled = 2;
        first.tag = 111;
        snapshot.Publish(first);

        const std::shared_ptr<const SampleSnapshot> oldNode = snapshot.Read();
        Check(oldNode != nullptr, "sanity: Read() right after the first Publish() must not be null");

        SampleSnapshot second{};
        second.generation = 2;
        second.doubled = 4;
        second.tag = 222;
        snapshot.Publish(second);

        SampleSnapshot third{};
        third.generation = 3;
        third.doubled = 6;
        third.tag = 333;
        snapshot.Publish(third);

        if (oldNode) {
            CheckEqual(oldNode->generation, std::uint64_t{ 1 }, "a held old node must keep its original generation forever, regardless of later publishes");
            CheckEqual(oldNode->doubled, std::uint64_t{ 2 }, "a held old node must keep its original doubled field forever");
            CheckEqual(oldNode->tag, 111, "a held old node must keep its original tag forever");
        }

        const std::shared_ptr<const SampleSnapshot> freshNode = snapshot.Read();
        Check(freshNode != nullptr, "sanity: a fresh Read() after later publishes must not be null");
        if (freshNode) {
            CheckEqual(freshNode->generation, std::uint64_t{ 3 }, "a fresh Read() after later publishes must see the MOST RECENT published value");
            CheckEqual(freshNode->tag, 333, "a fresh Read() after later publishes must see the most recent tag");
        }
        if (oldNode && freshNode) {
            Check(oldNode.get() != freshNode.get(), "the old and fresh nodes must be genuinely distinct objects -- a "
                                                     "new immutable node per Publish(), never an in-place mutation");
        }
    }

    void CaseF4_WriterReaderHammerNeverObservesATornCrossFieldMix()
    {
        PublishedSnapshot<SampleSnapshot> snapshot;
        SampleSnapshot seed{};
        seed.tag = 0;
        snapshot.Publish(seed);
        constexpr std::uint64_t kWriterIterations = 20000;
        constexpr int kReaderThreads = 4;
        constexpr int kReaderSpins = 200000;
        std::atomic<bool> tornMixObserved{ false };
        std::atomic<bool> nullObserved{ false };

        std::thread writer([&snapshot]() {
            for (std::uint64_t g = 1; g <= kWriterIterations; ++g) {
                SampleSnapshot value{};
                value.generation = g;
                value.doubled = g * 2;
                value.tag = static_cast<int>(g % 1000);
                snapshot.Publish(value);
            }
        });

        std::vector<std::thread> readers;
        readers.reserve(kReaderThreads);
        for (int r = 0; r < kReaderThreads; ++r) {
            readers.emplace_back([&snapshot, &tornMixObserved, &nullObserved]() {
                for (int i = 0; i < kReaderSpins; ++i) {
                    const std::shared_ptr<const SampleSnapshot> node = snapshot.Read();
                    if (!node) {
                        nullObserved.store(true, std::memory_order_relaxed);
                        continue;
                    }
                    if (node->doubled != node->generation * 2 ||
                        node->tag != static_cast<int>(node->generation % 1000)) {
                        tornMixObserved.store(true, std::memory_order_relaxed);
                    }
                }
            });
        }
        for (auto& reader : readers) {
            reader.join();
        }
        writer.join();

        Check(!nullObserved.load(std::memory_order_relaxed),
            "Read() must never return null, even while a writer is concurrently publishing");
        Check(!tornMixObserved.load(std::memory_order_relaxed),
            "concurrent readers must NEVER observe a cross-field-torn snapshot -- doubled/tag must always "
            "match the SAME node's generation (proof of one atomically-swapped immutable node per publish)");

        const std::shared_ptr<const SampleSnapshot> finalNode = snapshot.Read();
        Check(finalNode != nullptr, "sanity: Read() after every thread has joined must not be null");
        if (finalNode) {
            CheckEqual(finalNode->generation, kWriterIterations, "after the writer finishes, the final Read() must reflect the LAST published generation");
            CheckEqual(finalNode->doubled, kWriterIterations * 2, "after the writer finishes, the final Read() must reflect the LAST published doubled field");
        }
    }

    void CaseF5_InjectedAllocationFailureRetainsTheLastPublishedNode()
    {
        int callCount = 0;
        PublishedSnapshot<SampleSnapshot>::Factory factory =
            [&callCount](const SampleSnapshot& value) -> std::shared_ptr<const SampleSnapshot> {
                ++callCount;
                if (callCount == 2) {
                    return nullptr;
                }
                return std::make_shared<const SampleSnapshot>(value);
            };
        PublishedSnapshot<SampleSnapshot> snapshot(factory);

        SampleSnapshot successful{};
        successful.generation = 1;
        successful.doubled = 2;
        successful.tag = 10;
        snapshot.Publish(successful);
        {
            const std::shared_ptr<const SampleSnapshot> node = snapshot.Read();
            Check(node != nullptr, "sanity: Read() after a successful Publish() must not be null");
            if (node) {
                CheckEqual(node->generation, std::uint64_t{ 1 }, "sanity: the first successful publish must be visible before the rigged failure");
            }
        }

        SampleSnapshot rigged{};
        rigged.generation = 999;
        rigged.doubled = 999999;
        rigged.tag = -777;
        snapshot.Publish(rigged);

        const std::shared_ptr<const SampleSnapshot> afterFailure = snapshot.Read();
        Check(afterFailure != nullptr, "Read() must never return null even after an allocation failure inside Publish()");
        if (afterFailure) {
            CheckEqual(afterFailure->generation, std::uint64_t{ 1 },
                "an allocation failure inside Publish() must retain the LAST successfully published node "
                "untouched -- never the failed value, never a null/default node");
            CheckEqual(afterFailure->doubled, std::uint64_t{ 2 },
                "the retained node's other fields must also be the last GOOD publish's, not the failed one's");
        }

        SampleSnapshot recovered{};
        recovered.generation = 2;
        recovered.doubled = 4;
        recovered.tag = 20;
        snapshot.Publish(recovered);
        const std::shared_ptr<const SampleSnapshot> afterRecovery = snapshot.Read();
        Check(afterRecovery != nullptr, "sanity: Read() after a subsequent successful Publish() must not be null");
        if (afterRecovery) {
            CheckEqual(afterRecovery->generation, std::uint64_t{ 2 },
                "a later successful Publish() after a transient allocation failure must be visible normally "
                "(the gate is not permanently stuck)");
        }
    }

    void CaseF6_ChangeOnlyPublishRetriesTheSameValueAfterAllocationFailure()
    {
        int callCount = 0;
        PublishedSnapshot<SampleSnapshot>::Factory factory =
            [&callCount](const SampleSnapshot& value) -> std::shared_ptr<const SampleSnapshot> {
                ++callCount;
                if (callCount == 2) {
                    return nullptr;
                }
                return std::make_shared<const SampleSnapshot>(value);
            };
        PublishedSnapshot<SampleSnapshot> snapshot(factory);

        SampleSnapshot first{};
        first.generation = 1;
        first.doubled = 2;
        first.tag = 10;
        CheckEqual(snapshot.PublishIfChanged(first), PublishedSnapshot<SampleSnapshot>::PublishResult::kPublished,
            "the first changed value must report a successful publication");

        SampleSnapshot changed{};
        changed.generation = 2;
        changed.doubled = 4;
        changed.tag = 20;
        CheckEqual(snapshot.PublishIfChanged(changed), PublishedSnapshot<SampleSnapshot>::PublishResult::kFailed,
            "an injected allocation failure must be reported instead of being mistaken for publication");
        CheckEqual(snapshot.PublishIfChanged(changed), PublishedSnapshot<SampleSnapshot>::PublishResult::kPublished,
            "the identical changed value must be retried after failure and publish successfully");
        CheckEqual(callCount, 3,
            "a failed changed value must not poison change-only caching or suppress its identical retry");

        const std::shared_ptr<const SampleSnapshot> recovered = snapshot.Read();
        Check(recovered != nullptr, "Read() must remain non-null after a failed-then-retried publication");
        if (recovered) {
            CheckEqual(recovered->generation, std::uint64_t{ 2 },
                "the identical retry after transient allocation failure must become visible");
        }

        CheckEqual(snapshot.PublishIfChanged(changed), PublishedSnapshot<SampleSnapshot>::PublishResult::kUnchanged,
            "an already-published identical value must be skipped without allocating");
        CheckEqual(callCount, 3, "an unchanged value must not invoke the allocation factory");
    }

    void CaseF7_ChangeOnlyPublishRetriesTheSameValueAfterFactoryThrows()
    {
        int callCount = 0;
        PublishedSnapshot<SampleSnapshot>::Factory factory =
            [&callCount](const SampleSnapshot& value) -> std::shared_ptr<const SampleSnapshot> {
                ++callCount;
                if (callCount == 1) {
                    throw std::bad_alloc{};
                }
                return std::make_shared<const SampleSnapshot>(value);
            };
        PublishedSnapshot<SampleSnapshot> snapshot(factory);

        SampleSnapshot changed{};
        changed.generation = 9;
        changed.doubled = 18;
        changed.tag = 90;
        CheckEqual(snapshot.PublishIfChanged(changed), PublishedSnapshot<SampleSnapshot>::PublishResult::kFailed,
            "a throwing factory must be converted to a truthful failed outcome without escaping");
        CheckEqual(snapshot.PublishIfChanged(changed), PublishedSnapshot<SampleSnapshot>::PublishResult::kPublished,
            "the identical changed value must retry successfully after a throwing factory");
        CheckEqual(callCount, 2, "the identical retry after an exception must invoke the factory again");

        const std::shared_ptr<const SampleSnapshot> recovered = snapshot.Read();
        Check(recovered != nullptr, "Read() must remain non-null after a throwing-then-retried factory");
        if (recovered) {
            CheckEqual(recovered->generation, std::uint64_t{ 9 },
                "the identical retry after a factory exception must become visible");
        }
    }
}

int main()
{
    struct NamedCase
    {
        const char* name;
        void (*fn)();
    };

    const NamedCase cases[] = {

        { "B1-pack-writes-every-field-at-its-real-d3d11-byte-offset",
            &CaseB1_PackWritesEveryFieldAtItsRealD3D11ByteOffset },
        { "B2-unpack-reads-every-field-from-its-real-d3d11-byte-offset",
            &CaseB2_UnpackReadsEveryFieldFromItsRealD3D11ByteOffset },
        { "B3-pack-then-unpack-round-trips-to-an-identical-descriptor",
            &CaseB3_PackThenUnpackRoundTripsToAnIdenticalDescriptor },

        { "C1-eligibility-gate-covers-virgin-aniso-pre-biased-and-non-aniso-combinations",
            &CaseC1_EligibilityGateCoversVirginAnisoPreBiasedAndNonAnisoCombinations },
        { "C2-true-branch-clamps-bias-exactly-like-clamp-mip-bias",
            &CaseC2_TrueBranchClampsBiasExactlyLikeClampMipBias },
        { "C3-true-branch-preserves-every-other-field-exactly-except-mip-lod-bias",
            &CaseC3_TrueBranchPreservesEveryOtherFieldExactlyExceptMipLodBias },

        { "D1-dlss-engine-selected-and-active-maps-mip-bias-and-commit",
            &CaseD1_DlssEngineSelectedAndActiveMapsMipBiasAndCommit },
        { "D2-fsr-engine-selected-and-active-maps-fsr-mip-bias-and-commit",
            &CaseD2_FsrEngineSelectedAndActiveMapsFsrMipBiasAndCommit },
        { "D3-master-disabled-still-applies-bias",
            &CaseD3_MasterDisabledStillAppliesBias },
        { "D4-selected-engine-damaged-makes-inactive-fail-closed",
            &CaseD4_SelectedEngineDamagedMakesInactiveFailClosed },
        { "D5-other-engine-damaged-does-not-affect-the-selected-engine",
            &CaseD5_OtherEngineDamagedDoesNotAffectTheSelectedEngine },
        { "D6-zero-bias-intent-is-still-valid-and-active",
            &CaseD6_ZeroBiasIntentIsStillValidAndActive },
        { "D7-unresolved-fails-closed-but-none-applies-bias",
            &CaseD7_UnresolvedFailsClosedButNoneAppliesBias },

        { "E1-default-constructed-first-next-returns-one",
            &CaseE1_DefaultConstructedFirstNextReturnsOne },
        { "E2-sequential-calls-are-strictly-monotonic-with-no-gaps-or-repeats",
            &CaseE2_SequentialCallsAreStrictlyMonotonicWithNoGapsOrRepeats },
        { "E3-explicit-initial-value-seeds-the-counter",
            &CaseE3_ExplicitInitialValueSeedsTheCounter },
        { "E4-wraparound-from-uint64-max-is-well-defined-and-keeps-advancing",
            &CaseE4_WraparoundFromUint64MaxIsWellDefinedAndKeepsAdvancing },
        { "E5-concurrent-callers-each-receive-a-unique-contiguous-id",
            &CaseE5_ConcurrentCallersEachReceiveAUniqueContiguousId },

        { "F1-default-constructed-never-returns-null-and-holds-a-default-value",
            &CaseF1_DefaultConstructedNeverReturnsNullAndHoldsADefaultValue },
        { "F2-publish-then-read-returns-exactly-the-published-value",
            &CaseF2_PublishThenReadReturnsExactlyThePublishedValue },
        { "F3-reader-holding-an-old-node-is-unaffected-by-later-publishes",
            &CaseF3_ReaderHoldingAnOldNodeIsUnaffectedByLaterPublishes },
        { "F4-writer-reader-hammer-never-observes-a-torn-cross-field-mix",
            &CaseF4_WriterReaderHammerNeverObservesATornCrossFieldMix },
        { "F5-injected-allocation-failure-retains-the-last-published-node",
            &CaseF5_InjectedAllocationFailureRetainsTheLastPublishedNode },
        { "F6-change-only-publish-retries-the-same-value-after-allocation-failure",
            &CaseF6_ChangeOnlyPublishRetriesTheSameValueAfterAllocationFailure },
        { "F7-change-only-publish-retries-the-same-value-after-factory-throws",
            &CaseF7_ChangeOnlyPublishRetriesTheSameValueAfterFactoryThrows },
    };

    std::vector<CaseResult> results;
    results.reserve(std::size(cases));

    for (const auto& c : cases) {
        CaseResult result;
        result.name = c.name;
        g_active = &result;
        try {
            c.fn();
        } catch (const std::exception& ex) {
            ++result.failures;
            result.messages.emplace_back(std::string("unhandled exception: ") + ex.what());
        } catch (...) {
            ++result.failures;
            result.messages.emplace_back("unhandled non-standard exception");
        }
        g_active = nullptr;
        results.push_back(std::move(result));
    }

    int failedCases = 0;
    for (const auto& result : results) {
        const bool pass = result.failures == 0;
        std::cout << (pass ? "[PASS] " : "[FAIL] ") << result.name << " (" << (result.checks - result.failures)
                   << "/" << result.checks << " checks)\n";
        for (const auto& message : result.messages) {
            std::cout << "    - " << message << '\n';
        }
        if (!pass) {
            ++failedCases;
        }
    }

    std::cout << failedCases << " of " << results.size() << " WiringAdapter cases failed\n";

    return failedCases > 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
