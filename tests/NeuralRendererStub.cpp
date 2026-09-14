#include "PCH.h"

#include "Platform/NeuralRenderer.h"
#include "NeuralRendererStub.h"

#include <d3d12.h>

namespace
{
    struct StageStub
    {
        bool ready{ false };
        bool latched{ false };
        std::uint32_t consecutiveFailures{ 0 };
        std::uint32_t evaluates{ 0 };
        std::uint32_t creates{ 0 };
        std::uint32_t releases{ 0 };
        std::uint32_t abandons{ 0 };
        Platform::NeuralRenderer::FeatureKey key{};
    };
    StageStub g_stages[Platform::Neural::kMaxPasses]{};
    std::uint32_t g_failStage = Platform::NeuralRendererStub::kNoFailure;
    std::uint32_t g_refuseNative = Platform::NeuralRendererStub::kNoFailure;
    bool g_identity = false;

    void Transition(ID3D12GraphicsCommandList* a_list, ID3D12Resource* a_resource, D3D12_RESOURCE_STATES a_from,
        D3D12_RESOURCE_STATES a_to) noexcept
    {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = a_resource;
        barrier.Transition.StateBefore = a_from;
        barrier.Transition.StateAfter = a_to;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        a_list->ResourceBarrier(1, &barrier);
    }
}

namespace Platform::NeuralRendererStub
{
    void FailStage(std::uint32_t a_stage) noexcept { g_failStage = a_stage; }
    void RefuseNativeCarrier(std::uint32_t a_stage) noexcept { g_refuseNative = a_stage; }
    void SetIdentity(bool a_identity) noexcept { g_identity = a_identity; }
    std::uint32_t Evaluates(std::uint32_t a_stage) noexcept { return a_stage < Neural::kMaxPasses ? g_stages[a_stage].evaluates : 0U; }
    std::uint32_t Creates(std::uint32_t a_stage) noexcept { return a_stage < Neural::kMaxPasses ? g_stages[a_stage].creates : 0U; }
    std::uint32_t Releases(std::uint32_t a_stage) noexcept { return a_stage < Neural::kMaxPasses ? g_stages[a_stage].releases : 0U; }
    std::uint32_t Abandons(std::uint32_t a_stage) noexcept { return a_stage < Neural::kMaxPasses ? g_stages[a_stage].abandons : 0U; }
    bool FeatureAlive(std::uint32_t a_stage) noexcept { return a_stage < Neural::kMaxPasses && g_stages[a_stage].ready; }
    bool Latched(std::uint32_t a_stage) noexcept { return a_stage < Neural::kMaxPasses && g_stages[a_stage].latched; }
    void ResetCounters() noexcept
    {
        for (auto& stage : g_stages) {
            stage.evaluates = 0;
            stage.creates = 0;
        }
    }
}

namespace Platform::NeuralRenderer
{
    bool EnsureRuntime(ID3D12Device*) noexcept { return true; }
    bool RuntimeReady() noexcept { return true; }

    bool EnsureFeature(const FeatureKey& a_key, const Neural::Shape&, const Neural::Settings&, const Neural::Resources&,
        std::uint32_t a_stage) noexcept
    {
        if (a_stage >= Neural::kMaxPasses || g_stages[a_stage].latched) {
            return false;
        }
        if (a_stage == g_refuseNative && a_key.carrierFormat != DXGI_FORMAT_R16G16B16A16_FLOAT) {
            return false;
        }
        g_stages[a_stage].ready = true;
        g_stages[a_stage].key = a_key;
        ++g_stages[a_stage].creates;
        return true;
    }

    bool FeatureReady(std::uint32_t a_stage) noexcept { return a_stage < Neural::kMaxPasses && g_stages[a_stage].ready; }
    bool HasResources(std::uint32_t a_stage) noexcept { return FeatureReady(a_stage); }
    FeatureKey CurrentKey(std::uint32_t a_stage) noexcept { return a_stage < Neural::kMaxPasses ? g_stages[a_stage].key : FeatureKey{}; }

    bool Evaluate(ID3D12GraphicsCommandList* a_list, const Neural::Shape& a_shape, const Neural::Settings&,
        const Neural::Resources& a_resources, bool, std::uint32_t a_stage) noexcept
    {
        if (a_stage >= Neural::kMaxPasses || a_list == nullptr) {
            return false;
        }
        ++g_stages[a_stage].evaluates;
        if (a_stage == g_failStage) {
            if (++g_stages[a_stage].consecutiveFailures >= NeuralRendererStub::kEvaluateStrikes) {
                g_stages[a_stage].latched = true;
            }
            return false;
        }
        g_stages[a_stage].consecutiveFailures = 0;
        auto* const colour = static_cast<ID3D12Resource*>(a_resources.color);
        auto* const output = static_cast<ID3D12Resource*>(a_resources.output);
        if (colour == nullptr || output == nullptr || a_shape.outWidth < 2U || a_shape.outHeight == 0U) {
            return false;
        }
        Transition(a_list, colour, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Transition(a_list, output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = colour;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = output;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = 0;
        if (g_identity) {
            a_list->CopyResource(output, colour);
        } else {
            const D3D12_BOX shifted{ 0, 0, 0, a_shape.outWidth - 1U, a_shape.outHeight, 1 };
            a_list->CopyTextureRegion(&dst, 1, 0, 0, &src, &shifted);
            const D3D12_BOX first{ 0, 0, 0, 1, a_shape.outHeight, 1 };
            a_list->CopyTextureRegion(&dst, 0, 0, 0, &src, &first);
        }
        Transition(a_list, colour, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(a_list, output, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        return true;
    }

    void DestroyFeature(std::uint32_t a_stage) noexcept
    {
        if (a_stage < Neural::kMaxPasses) {
            if (g_stages[a_stage].ready) ++g_stages[a_stage].releases;
            g_stages[a_stage].ready = false;
        }
    }
    void ReleasePass(std::uint32_t a_stage) noexcept { DestroyFeature(a_stage); }
    void AbandonPass(std::uint32_t a_stage) noexcept
    {
        if (a_stage < Neural::kMaxPasses) {
            if (g_stages[a_stage].ready) ++g_stages[a_stage].abandons;
            g_stages[a_stage].ready = false;
        }
    }
    void ReleaseRuntime() noexcept {}
    void RearmLatch(std::uint32_t a_stageMask) noexcept
    {
        for (std::uint32_t i = 0; i < Neural::kMaxPasses; ++i) {
            if ((a_stageMask & (1U << i)) != 0U) {
                g_stages[i].latched = false;
                g_stages[i].consecutiveFailures = 0;
            }
        }
    }

    State Snapshot(std::uint32_t a_stage) noexcept
    {
        State state{};
        state.runtimeReady = true;
        state.featureReady = a_stage < Neural::kMaxPasses && g_stages[a_stage].ready;
        state.latched = a_stage < Neural::kMaxPasses && g_stages[a_stage].latched;
        if (state.latched) {
            std::snprintf(state.reason, sizeof(state.reason), "pass %u evaluate failed %u times (fixture stub)", a_stage + 1U,
                NeuralRendererStub::kEvaluateStrikes);
        }
        state.evaluations = a_stage < Neural::kMaxPasses ? g_stages[a_stage].evaluates : 0U;
        state.creates = a_stage < Neural::kMaxPasses ? g_stages[a_stage].creates : 0U;
        state.profileLabel = "fixture stub (shift-by-one transform)";
        state.profileValidated = true;
        std::snprintf(state.runtimeVersion, sizeof(state.runtimeVersion), "stub");
        return state;
    }
}
