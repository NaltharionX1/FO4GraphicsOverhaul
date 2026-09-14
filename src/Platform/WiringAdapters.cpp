#include "Platform/WiringAdapters.h"

#include <cmath>
#include <cstring>

namespace Platform
{
    namespace
    {
        [[nodiscard]] float ClampSamplerBias(float bias) noexcept
        {
            if (!std::isfinite(bias)) {
                return 0.0F;
            }
            if (bias < -3.0F) {
                return -3.0F;
            }
            if (bias > 0.0F) {
                return 0.0F;
            }
            return bias;
        }
    }

    SamplerDescriptor PackSamplerDescriptor(const D3D11SamplerDescFields& fields) noexcept
    {
        static_assert(std::is_trivially_copyable_v<D3D11SamplerDescFields>,
            "D3D11SamplerDescFields must be trivially copyable for memcpy-based packing");
        SamplerDescriptor descriptor{};
        std::memcpy(descriptor.bytes.data(), &fields, sizeof(fields));
        return descriptor;
    }

    D3D11SamplerDescFields UnpackSamplerDescriptor(const SamplerDescriptor& descriptor) noexcept
    {
        D3D11SamplerDescFields fields{};
        std::memcpy(&fields, descriptor.bytes.data(), sizeof(fields));
        return fields;
    }

    bool MakeBiasedDescriptor(
        const SamplerDescriptor& source, float bias, SamplerDescriptor& output) noexcept
    {
        const D3D11SamplerDescFields fields = UnpackSamplerDescriptor(source);
        if (fields.filter != kD3D11FilterAnisotropic || fields.mipLodBias != 0.0F) {
            return false;
        }

        D3D11SamplerDescFields biased = fields;
        biased.mipLodBias = ClampSamplerBias(bias);
        output = PackSamplerDescriptor(biased);
        return true;
    }

    AppliedSamplerIntent DeriveSamplerIntent(
        const AaRequest& appliedSnapshot, std::uint32_t damagedScope) noexcept
    {
        AppliedSamplerIntent intent{};
        intent.appliedCommit = appliedSnapshot.generation;

        std::uint32_t selectedScopeBit = kAaRecreateNone;
        switch (appliedSnapshot.effectiveEngine) {
        case AaEffectiveEngine::kDlss:
            intent.engine = AppliedSamplerEngine::kDlss;
            intent.bias = appliedSnapshot.mipBias;
            selectedScopeBit = kAaRecreateDlss;
            break;
        case AaEffectiveEngine::kFsr:
            intent.engine = AppliedSamplerEngine::kFsr;
            intent.bias = appliedSnapshot.fsrMipBias;
            selectedScopeBit = kAaRecreateFsr;
            break;
        case AaEffectiveEngine::kNone:
            intent.engine = AppliedSamplerEngine::kDlss;
            intent.bias = appliedSnapshot.mipBias;
            intent.active = appliedSnapshot.mipBias < 0.0F;
            return intent;
        default:
            intent.engine = kAppliedSamplerEngineNone;
            intent.bias = 0.0F;
            intent.active = false;
            return intent;
        }

        intent.active = (damagedScope & selectedScopeBit) == 0U;
        return intent;
    }

    FrameStamp::FrameStamp(std::uint64_t initial) noexcept : counter_(initial)
    {
    }

    std::uint64_t FrameStamp::Next() noexcept
    {
        return counter_.fetch_add(1) + 1;
    }

    std::uint64_t FrameStamp::Current() const noexcept
    {
        return counter_.load();
    }
}
