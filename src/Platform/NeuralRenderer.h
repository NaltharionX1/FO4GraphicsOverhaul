#pragma once

#include "Platform/NeuralContract.h"
#include "Platform/NgxAbi.h"

#include <d3d12.h>
#include <dxgiformat.h>

#include <cstdint>

namespace Platform::NeuralRenderer
{
    struct FeatureKey
    {
        std::uint32_t outWidth{ 0 };
        std::uint32_t outHeight{ 0 };
        DXGI_FORMAT carrierFormat{ DXGI_FORMAT_UNKNOWN };
        std::uint32_t preset{ 0 };

        [[nodiscard]] bool operator==(const FeatureKey&) const noexcept = default;
    };

    [[nodiscard]] bool EnsureRuntime(ID3D12Device* a_device) noexcept;
    [[nodiscard]] bool RuntimeReady() noexcept;

    [[nodiscard]] bool EnsureFeature(const FeatureKey& a_key, const Neural::Shape& a_shape,
        const Neural::Settings& a_settings, const Neural::Resources& a_resources, std::uint32_t a_stage = 0) noexcept;
    [[nodiscard]] bool FeatureReady(std::uint32_t a_stage = 0) noexcept;
    [[nodiscard]] bool HasResources(std::uint32_t a_stage) noexcept;
    [[nodiscard]] FeatureKey CurrentKey(std::uint32_t a_stage = 0) noexcept;

    [[nodiscard]] bool Evaluate(ID3D12GraphicsCommandList* a_list, const Neural::Shape& a_shape,
        const Neural::Settings& a_settings, const Neural::Resources& a_resources, bool a_reset,
        std::uint32_t a_stage = 0) noexcept;

    void DestroyFeature(std::uint32_t a_stage = 0) noexcept;
    void ReleasePass(std::uint32_t a_stage) noexcept;
    void AbandonPass(std::uint32_t a_stage) noexcept;
    void ReleaseRuntime() noexcept;

    void RearmLatch(std::uint32_t a_stageMask = Neural::kAllPasses) noexcept;

    struct State
    {
        bool runtimeReady{ false };
        bool featureReady{ false };
        bool latched{ false };
        bool faulted{ false };
        std::uint32_t lastResult{ 0 };
        unsigned long lastFault{ 0 };
        std::uint64_t evaluations{ 0 };
        std::uint32_t creates{ 0 };
        std::uintptr_t handleAddress{ 0 };
        int sdkVersion{ 0 };
        char reason[160]{};
        char runtimeVersion[32]{};
        char runtimeSignature[32]{};
        char runtimeSha[17]{};
        const char* profileLabel{ "" };
        bool profileValidated{ false };
    };
    [[nodiscard]] State Snapshot(std::uint32_t a_stage = 0) noexcept;
}
