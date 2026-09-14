#include "PCH.h"

#include "Platform/AoSlot.h"

#include "Platform/AoGate.h"
#include "Platform/GtaoPass.h"

#include <d3d11.h>

#include <atomic>

namespace
{
    std::atomic<bool> g_integrationEnabled{ false };
    std::atomic<std::uint32_t> g_integrationIndex{ 25 };

    constexpr std::uint32_t kRenderTargetCount = 101;
}

namespace Platform::AoSlot
{
    void SetGtaoProduction(bool a_enabled) noexcept
    {
        g_integrationIndex.store(25U, std::memory_order_relaxed);
        Platform::GtaoPass::SetBlendMode(0U);
        if (g_integrationEnabled.exchange(a_enabled, std::memory_order_relaxed) != a_enabled) {
            logger::info("[AoSlot] GTAO production {} - post-lighting write into renderTargets[25], "
                         "blend=replace. The engine's AO pipeline keeps running either way; only "
                         "the producer of the final term changes.",
                a_enabled ? "ON (XeGTAO)" : "off (engine HBAO)");
        }
    }

    void ComputeForIntegration() noexcept
    {
        if (g_integrationEnabled.load(std::memory_order_relaxed)) {
            (void)Platform::GtaoPass::Execute();
        }
    }

    [[nodiscard]] std::uint32_t ResolveTargetIndex() noexcept
    {
        return g_integrationIndex.load(std::memory_order_relaxed);
    }

    void IntegrateAfterLighting() noexcept
    {
        const bool producing = g_integrationEnabled.load(std::memory_order_relaxed);

        if (!producing && !Platform::AoGate::EngineProducersRetired()) {
            return;
        }
        try {
            auto* const data = RE::BSGraphics::RendererData::GetSingleton();
            if (data == nullptr) {
                return;
            }
            const std::uint32_t index = ResolveTargetIndex();
            if (index >= kRenderTargetCount) {
                return;
            }
            auto& target = data->renderTargets[index];
            if (target.texture == nullptr || target.uaView == nullptr) {
                static std::atomic<bool> logged{ false };
                if (!logged.exchange(true, std::memory_order_relaxed)) {
                    logger::warn("[AoSlot] integration target {} has no UAV — the engine did not "
                                 "create one for it. Try the other addressing mode or index.",
                        index);
                }
                return;
            }

            if (!producing) {
                if (data->context == nullptr) {
                    return;
                }
                const FLOAT kNoOcclusion[4]{ 1.0F, 1.0F, 1.0F, 1.0F };
                data->context->ClearUnorderedAccessViewFloat(
                    static_cast<ID3D11UnorderedAccessView*>(target.uaView), kNoOcclusion);
                static std::atomic<bool> loggedNeutral{ false };
                if (!loggedNeutral.exchange(true, std::memory_order_relaxed)) {
                    logger::info("[AoSlot] AO off with the vanilla producer retired — the AO term "
                                 "(renderTargets[{}]) is now held NEUTRAL (1.0, no occlusion) per "
                                 "frame, the duty the engine draw used to perform when off.",
                        index);
                }
                return;
            }

            D3D11_TEXTURE2D_DESC desc{};
            target.texture->GetDesc(&desc);
            static std::atomic<std::uint32_t> loggedIndex{ 0xFFFFFFFFU };
            if (loggedIndex.exchange(index, std::memory_order_relaxed) != index) {
                logger::info("[AoSlot] integration target index {} -> {}x{} format={} (a plausible "
                             "AO target is render-sized with 1-2 channels)",
                    index, desc.Width, desc.Height, static_cast<int>(desc.Format));
            }
            (void)Platform::GtaoPass::IntegrateInto(target.uaView, target.srView, desc.Width,
                desc.Height);
        } catch (...) {
            if (g_integrationEnabled.exchange(false, std::memory_order_relaxed)) {
                logger::warn("[AoSlot] integration threw — disabled for the session.");
            }
        }
    }

}
