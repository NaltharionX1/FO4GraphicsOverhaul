#include "PCH.h"

#include "Platform/RenderTargetProxy.h"

#include "Platform/AoGate.h"
#include "Platform/DepthCopyShaderBytecode.h"
#include "Platform/Fallout4Renderer.h"
#include "Platform/RendererContracts.h"
#include "Platform/SubNativeMath.h"

#include "RE/Bethesda/BSGraphics.h"

#include <Detours.h>
#include <d3d11.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <span>

#ifdef near
#    undef near
#endif
#ifdef far
#    undef far
#endif

namespace
{
    constexpr REL::ID kRendererDataPreNG{ 1235449 };
    constexpr REL::ID kStatePreNG{ 600795 };
    constexpr REL::ID kRenderTargetManagerPreNG{ 1508457 };
    constexpr REL::ID kCameraNearPreNG{ 57985 };
    constexpr REL::ID kCameraFarPreNG{ 958877 };
    constexpr std::size_t kMainDepthIndex = 2;
    constexpr std::size_t kMainDepthMips = 39;

    constexpr int kPatchIndices[] = { 20, 57, 24, 25, 23, 58, 59, 28, 3, 9, 60, 61, 4, 29, 1,
        36, 37, 22, 10, 11, 7, 8, 64, 14, 16 };

    [[nodiscard]] RE::BSGraphics::RendererData* Renderer() noexcept
    {
        REL::Relocation<RE::BSGraphics::RendererData**> singleton{ kRendererDataPreNG };
        return *singleton;
    }
    [[nodiscard]] RE::BSGraphics::State* State() noexcept
    {
        REL::Relocation<RE::BSGraphics::State*> singleton{ kStatePreNG };
        return singleton.get();
    }
    [[nodiscard]] RE::BSGraphics::RenderTargetManager* Manager() noexcept
    {
        REL::Relocation<RE::BSGraphics::RenderTargetManager*> singleton{ kRenderTargetManagerPreNG };
        return singleton.get();
    }

    bool g_latchedOff = false;
    float g_builtRatio = 1.0F;
    void* g_builtDevice = nullptr;

    std::size_t g_builtCount = 0;

    [[nodiscard]] bool ProxiesReadyFor(const RE::BSGraphics::RendererData* a_renderer,
        float a_ratio) noexcept
    {
        return !g_latchedOff && g_builtCount != 0 && a_renderer &&
               a_renderer->device == g_builtDevice &&
               g_builtRatio < 0.999F && std::fabs(g_builtRatio - a_ratio) < 0.0005F;
    }

    RE::BSGraphics::RenderTarget g_original[101]{};
    RE::BSGraphics::RenderTarget g_proxy[101]{};
    RE::BSGraphics::RenderTargetProperties g_savedProps[100]{};
    bool g_overridden = false;

    ID3D11ShaderResourceView* g_savedDepthSRV = nullptr;
    bool g_depthOverridden = false;
    ID3D11Texture2D* g_tightDepth = nullptr;
    ID3D11ShaderResourceView* g_tightDepthSRV = nullptr;
    ID3D11UnorderedAccessView* g_tightDepthUAV = nullptr;

    ID3D11ComputeShader* g_linearDepthCS = nullptr;
    ID3D11ComputeShader* g_tightDepthCS = nullptr;
    ID3D11Buffer* g_upscalingCB = nullptr;
    std::uint32_t g_lastDepthCopyFrame = 0xFFFFFFFFU;
    bool g_depthFixOff = false;

    struct UpscalingCB
    {
        std::uint32_t screenSize[2];
        std::uint32_t renderSize[2];
        float cameraData[4];
    };
    static_assert(sizeof(UpscalingCB) == 32);

    using SetDynResViewportDefaultFn = void (*)(RE::BSGraphics::RenderTargetManager*, bool);
    SetDynResViewportDefaultFn g_viewportDefaultFn = nullptr;

    void LatchOff(const char* a_what) noexcept
    {
        if (!g_latchedOff) {
            g_latchedOff = true;
            logger::error("[SubNative] {} — fix-up layer latched OFF (sub-native rendering keeps "
                          "working; screen-space effects may show artifacts, never a crash)",
                a_what);
        }
    }

    void ReleaseProxy(int a_index) noexcept
    {
        auto& proxy = g_proxy[a_index];
        if (proxy.uaView) { proxy.uaView->Release(); }
        if (proxy.srView) { proxy.srView->Release(); }
        if (proxy.rtView) { proxy.rtView->Release(); }
        if (proxy.texture) { proxy.texture->Release(); }
        proxy = RE::BSGraphics::RenderTarget{};
    }

    void ReleaseTightDepth() noexcept
    {
        if (g_tightDepthUAV) { g_tightDepthUAV->Release(); g_tightDepthUAV = nullptr; }
        if (g_tightDepthSRV) { g_tightDepthSRV->Release(); g_tightDepthSRV = nullptr; }
        if (g_tightDepth) { g_tightDepth->Release(); g_tightDepth = nullptr; }
    }

    void ReleaseAllProxies() noexcept
    {
        for (const int index : kPatchIndices) {
            ReleaseProxy(index);
        }
        ReleaseTightDepth();
        g_builtCount = 0;
    }

    [[nodiscard]] bool BuildProxy(ID3D11Device* a_device, int a_index, float a_ratio) noexcept
    {
        const auto renderer = Renderer();
        if (!renderer) {
            return false;
        }
        g_original[a_index] = renderer->renderTargets[a_index];
        ReleaseProxy(a_index);
        const auto& original = g_original[a_index];
        if (!original.texture) {
            return true;
        }
        D3D11_TEXTURE2D_DESC desc{};
        original.texture->GetDesc(&desc);
        desc.Width = (std::max)(1U, static_cast<UINT>(static_cast<float>(desc.Width) * a_ratio));
        desc.Height = (std::max)(1U, static_cast<UINT>(static_cast<float>(desc.Height) * a_ratio));
        if (desc.MipLevels > 1U) {
            desc.MipLevels =
                (std::min)(desc.MipLevels, Platform::MaxMipLevelsForDims(desc.Width, desc.Height));
        }
        auto& proxy = g_proxy[a_index];
        if (FAILED(a_device->CreateTexture2D(&desc, nullptr, &proxy.texture)) || !proxy.texture) {
            return false;
        }
        if (original.rtView) {
            D3D11_RENDER_TARGET_VIEW_DESC vd{};
            original.rtView->GetDesc(&vd);
            if (FAILED(a_device->CreateRenderTargetView(proxy.texture, &vd, &proxy.rtView))) {
                return false;
            }
        }
        if (original.srView) {
            D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
            original.srView->GetDesc(&vd);
            if (vd.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D &&
                vd.Texture2D.MipLevels != static_cast<UINT>(-1) &&
                vd.Texture2D.MipLevels > desc.MipLevels) {
                vd.Texture2D.MipLevels = static_cast<UINT>(-1);
            }
            if (FAILED(a_device->CreateShaderResourceView(proxy.texture, &vd, &proxy.srView))) {
                return false;
            }
        }
        if (original.uaView) {
            D3D11_UNORDERED_ACCESS_VIEW_DESC vd{};
            original.uaView->GetDesc(&vd);
            if (FAILED(a_device->CreateUnorderedAccessView(proxy.texture, &vd, &proxy.uaView))) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool EnsureComputeResources(ID3D11Device* a_device) noexcept
    {
        if (!g_linearDepthCS) {
            const HRESULT hr = a_device->CreateComputeShader(
                Platform::kOverrideLinearDepthShaderBytecode,
                Platform::kOverrideLinearDepthShaderBytecodeSize, nullptr, &g_linearDepthCS);
            if (FAILED(hr) || !g_linearDepthCS) {
                logger::warn("[SubNative] linear-depth CS creation failed ({:#010x})",
                    static_cast<std::uint32_t>(hr));
                return false;
            }
        }
        if (!g_tightDepthCS) {
            const HRESULT hr = a_device->CreateComputeShader(
                Platform::kOverrideDepthShaderBytecode,
                Platform::kOverrideDepthShaderBytecodeSize, nullptr, &g_tightDepthCS);
            if (FAILED(hr) || !g_tightDepthCS) {
                logger::warn("[SubNative] tight-depth CS creation failed ({:#010x})",
                    static_cast<std::uint32_t>(hr));
                return false;
            }
        }
        if (!g_upscalingCB) {
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = sizeof(UpscalingCB);
            bd.Usage = D3D11_USAGE_DEFAULT;
            bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            const HRESULT hr = a_device->CreateBuffer(&bd, nullptr, &g_upscalingCB);
            if (FAILED(hr) || !g_upscalingCB) {
                logger::warn("[SubNative] upscaling CB creation failed ({:#010x})",
                    static_cast<std::uint32_t>(hr));
                return false;
            }
        }
        return true;
    }

    void CopyDepthNow() noexcept
    {
        const auto renderer = Renderer();
        const auto state = State();
        const auto manager = Manager();
        if (!renderer || !state || !manager || !g_tightDepthUAV || !g_linearDepthCS ||
            !g_tightDepthCS || !g_upscalingCB) {
            return;
        }
        auto* const context = reinterpret_cast<ID3D11DeviceContext*>(renderer->context);
        auto* const depthSRV = renderer->depthStencilTargets[kMainDepthIndex].srViewDepth;
        if (!context || !depthSRV) {
            return;
        }
        context->OMSetRenderTargets(0, nullptr, nullptr);

        const float screenW = static_cast<float>(state->screenWidth);
        const float screenH = static_cast<float>(state->screenHeight);
        const float renderW = screenW * manager->dynamicWidthRatio;
        const float renderH = screenH * manager->dynamicHeightRatio;

        UpscalingCB cb{};
        cb.screenSize[0] = state->screenWidth;
        cb.screenSize[1] = state->screenHeight;
        cb.renderSize[0] = (std::max)(1U, static_cast<UINT>(renderW));
        cb.renderSize[1] = (std::max)(1U, static_cast<UINT>(renderH));
        const float camFar = *reinterpret_cast<const float*>(kCameraFarPreNG.address());
        const float camNear = *reinterpret_cast<const float*>(kCameraNearPreNG.address());
        cb.cameraData[0] = camFar;
        cb.cameraData[1] = camNear;
        cb.cameraData[2] = camFar - camNear;
        cb.cameraData[3] = camFar * camNear;
        context->UpdateSubresource(g_upscalingCB, 0, nullptr, &cb, 0, 0);
        context->CSSetConstantBuffers(0, 1, &g_upscalingCB);

        ID3D11ShaderResourceView* views[1] = { depthSRV };
        context->CSSetShaderResources(0, 1, views);

        if (auto* const linearUAV = renderer->renderTargets[kMainDepthMips].uaView) {
            ID3D11UnorderedAccessView* uavs[1] = { linearUAV };
            context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
            context->CSSetShader(g_linearDepthCS, nullptr, 0);
            context->Dispatch(static_cast<UINT>(std::ceil(screenW / 8.0F)),
                static_cast<UINT>(std::ceil(screenH / 8.0F)), 1);
        }

        {
            ID3D11UnorderedAccessView* uavs[1] = { g_tightDepthUAV };
            context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
            context->CSSetShader(g_tightDepthCS, nullptr, 0);
            context->Dispatch(static_cast<UINT>(std::ceil(renderW / 8.0F)),
                static_cast<UINT>(std::ceil(renderH / 8.0F)), 1);
        }

        ID3D11ShaderResourceView* nullSRV[1] = { nullptr };
        context->CSSetShaderResources(0, 1, nullSRV);
        ID3D11UnorderedAccessView* nullUAV[1] = { nullptr };
        context->CSSetUnorderedAccessViews(0, 1, nullUAV, nullptr);
        context->CSSetShader(nullptr, nullptr, 0);
    }

    void OverrideDepthNow(bool a_copy) noexcept
    {
        const auto renderer = Renderer();
        if (g_latchedOff || g_depthFixOff || !renderer || !g_tightDepthSRV || g_depthOverridden ||
            renderer->device != g_builtDevice) {
            return;
        }
        g_savedDepthSRV = renderer->depthStencilTargets[kMainDepthIndex].srViewDepth;
        if (a_copy) {
            if (const auto state = State()) {
                if (g_lastDepthCopyFrame != state->frameCount) {
                    g_lastDepthCopyFrame = state->frameCount;
                    CopyDepthNow();
                }
            }
        }
        renderer->depthStencilTargets[kMainDepthIndex].srViewDepth = g_tightDepthSRV;
        g_depthOverridden = true;
    }

    void ResetDepthNow() noexcept
    {
        const auto renderer = Renderer();
        if (!renderer || !g_depthOverridden) {
            return;
        }
        renderer->depthStencilTargets[kMainDepthIndex].srViewDepth = g_savedDepthSRV;
        g_savedDepthSRV = nullptr;
        g_depthOverridden = false;
    }

    void OverrideAllNow(std::span<const int> a_copyList) noexcept
    {
        const auto renderer = Renderer();
        const auto manager = Manager();
        if (g_latchedOff || g_overridden || !renderer || !manager || g_builtRatio >= 0.999F ||
            renderer->device != g_builtDevice) {
            return;
        }
        auto* const context = reinterpret_cast<ID3D11DeviceContext*>(renderer->context);
        if (!context) {
            return;
        }
        for (const int index : kPatchIndices) {
            if (!g_proxy[index].texture) {
                continue;
            }
            g_original[index] = renderer->renderTargets[index];
            renderer->renderTargets[index] = g_proxy[index];
            if (Platform::ShouldCopyOnOverride(a_copyList.data(), a_copyList.size(), index) &&
                g_original[index].texture) {
                D3D11_TEXTURE2D_DESC dstDesc{};
                g_proxy[index].texture->GetDesc(&dstDesc);
                const D3D11_BOX box{ 0, 0, 0, dstDesc.Width, dstDesc.Height, 1 };
                context->CopySubresourceRegion(
                    g_proxy[index].texture, 0, 0, 0, 0, g_original[index].texture, 0, &box);
            }
        }
        for (int i = 0; i < 100; ++i) {
            g_savedProps[i] = manager->renderTargetData[i];
            manager->renderTargetData[i].width = static_cast<std::uint32_t>(
                static_cast<float>(manager->renderTargetData[i].width) * manager->dynamicWidthRatio);
            manager->renderTargetData[i].height = static_cast<std::uint32_t>(
                static_cast<float>(manager->renderTargetData[i].height) * manager->dynamicHeightRatio);
        }
        ID3D11ShaderResourceView* bound[16] = {};
        context->PSGetShaderResources(0, 16, bound);
        for (int slot = 0; slot < 16; ++slot) {
            if (!bound[slot]) {
                continue;
            }
            for (const int index : kPatchIndices) {
                if (bound[slot] == g_original[index].srView && g_proxy[index].srView) {
                    ID3D11ShaderResourceView* replacement = g_proxy[index].srView;
                    context->PSSetShaderResources(static_cast<UINT>(slot), 1, &replacement);
                    break;
                }
            }
            bound[slot]->Release();
        }
        if (g_viewportDefaultFn) {
            g_viewportDefaultFn(manager, false);
        }
        g_overridden = true;
    }

    void ResetAllNow(std::span<const int> a_copyList) noexcept
    {
        const auto renderer = Renderer();
        const auto manager = Manager();
        if (!g_overridden || !renderer || !manager) {
            return;
        }
        auto* const context = reinterpret_cast<ID3D11DeviceContext*>(renderer->context);
        if (!context) {
            return;
        }
        for (const int index : kPatchIndices) {
            if (!g_proxy[index].texture || !g_original[index].texture) {
                continue;
            }
            const bool copy = Platform::ShouldCopyOnReset(a_copyList.data(), a_copyList.size(), index);
            if (copy) {
                D3D11_TEXTURE2D_DESC srcDesc{};
                g_proxy[index].texture->GetDesc(&srcDesc);
                const D3D11_BOX box{ 0, 0, 0, srcDesc.Width, srcDesc.Height, 1 };
                context->CopySubresourceRegion(
                    g_original[index].texture, 0, 0, 0, 0, g_proxy[index].texture, 0, &box);
            }
            renderer->renderTargets[index] = g_original[index];
        }
        for (int i = 0; i < 100; ++i) {
            manager->renderTargetData[i] = g_savedProps[i];
        }
        ID3D11ShaderResourceView* bound[16] = {};
        context->PSGetShaderResources(0, 16, bound);
        for (int slot = 0; slot < 16; ++slot) {
            if (!bound[slot]) {
                continue;
            }
            for (const int index : kPatchIndices) {
                if (bound[slot] == g_proxy[index].srView && g_original[index].srView) {
                    ID3D11ShaderResourceView* replacement = g_original[index].srView;
                    context->PSSetShaderResources(static_cast<UINT>(slot), 1, &replacement);
                    break;
                }
            }
            bound[slot]->Release();
        }
        if (g_viewportDefaultFn) {
            g_viewportDefaultFn(manager, true);
        }
        g_overridden = false;
    }

    [[nodiscard]] bool BuildProxySeh(ID3D11Device* a_device, int a_index, float a_ratio) noexcept
    {
        __try {
            return BuildProxy(a_device, a_index, a_ratio);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            ReleaseProxy(a_index);
            return false;
        }
    }
    [[nodiscard]] bool EnsureComputeResourcesSeh(ID3D11Device* a_device) noexcept
    {
        __try {
            return EnsureComputeResources(a_device);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }
    void OverrideAllSeh(const int* a_copyList, std::size_t a_count) noexcept
    {
        __try {
            OverrideAllNow(std::span<const int>{ a_copyList, a_count });
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    void ResetAllSeh(const int* a_copyList, std::size_t a_count) noexcept
    {
        __try {
            ResetAllNow(std::span<const int>{ a_copyList, a_count });
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    void OverrideDepthSeh(bool a_copy) noexcept
    {
        __try {
            OverrideDepthNow(a_copy);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    void ResetDepthSeh() noexcept
    {
        __try {
            ResetDepthNow();
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }

    struct HbaoHook
    {
        static void thunk(void* a_this)
        {
            if (Platform::AoGate::EngineProducersRetired()) {
                return;
            }
            float savedW = 1.0F;
            float savedH = 1.0F;
            bool active = false;
            RE::BSGraphics::RenderTargetManager* manager = nullptr;
            try {
                manager = Manager();
                if (manager) {
                    savedW = manager->dynamicWidthRatio;
                    savedH = manager->dynamicHeightRatio;
                    active = (savedW != 1.0F || savedH != 1.0F) && ProxiesReadyFor(Renderer(), savedW);
                    if (active) {
                        OverrideDepthSeh(true);
                        constexpr int copyIn[] = { 20 };
                        OverrideAllSeh(copyIn, std::size(copyIn));
                        manager->dynamicWidthRatio = 1.0F;
                        manager->dynamicHeightRatio = 1.0F;
                    }
                }
            } catch (...) {
                active = false;
            }
            func(a_this);
            try {
                if (active && manager) {
                    ResetDepthSeh();
                    constexpr int copyOut[] = { 25 };
                    ResetAllSeh(copyOut, std::size(copyOut));
                    manager->dynamicWidthRatio = savedW;
                    manager->dynamicHeightRatio = savedH;
                }
            } catch (...) {
            }
        }
        static inline REL::Relocation<decltype(thunk)> func;
    };

    using RenderLensFlareFn = void (*)(void*);
    std::atomic<RenderLensFlareFn> g_lensFlareOriginal{ nullptr };

    struct LensFlareHook
    {
        static void thunk(void* a_camera)
        {
            bool active = false;
            try {
                const auto* const manager = Manager();
                if (manager) {
                    const float savedW = manager->dynamicWidthRatio;
                    active = (savedW != 1.0F || manager->dynamicHeightRatio != 1.0F) &&
                             ProxiesReadyFor(Renderer(), savedW);
                }
            } catch (...) {
                active = false;
            }
            if (active) {
                OverrideDepthSeh(true);
            }
            if (const auto original = g_lensFlareOriginal.load(std::memory_order_acquire)) {
                original(a_camera);
                static bool s_firstCallLogged = false;
                if (!s_firstCallLogged) {
                    s_firstCallLogged = true;
                    logger::info("[SubNative] lens-flare detour: first call returned safely "
                                 "(trampoline proven live; fix-up {})",
                        active ? "engaged (sub-native)" : "idle (native ratio)");
                }
            }
            if (active) {
                ResetDepthSeh();
            }
        }
    };

    struct DeferredCompositeHook
    {
        static void thunk(void* a_pass, std::uint32_t a_arg2, bool a_arg3)
        {
            float savedW = 1.0F;
            float savedH = 1.0F;
            bool active = false;
            RE::BSGraphics::RenderTargetManager* manager = nullptr;
            try {
                manager = Manager();
                if (manager) {
                    savedW = manager->dynamicWidthRatio;
                    savedH = manager->dynamicHeightRatio;
                    active = (savedW != 1.0F || savedH != 1.0F) && ProxiesReadyFor(Renderer(), savedW);
                    if (active) {
                        constexpr int copyIn[] = { 20, 25, 57, 24, 23, 58, 59, 3, 9, 60, 61, 28 };
                        OverrideAllSeh(copyIn, std::size(copyIn));
                        OverrideDepthSeh(true);
                        manager->dynamicWidthRatio = 1.0F;
                        manager->dynamicHeightRatio = 1.0F;
                    }
                }
            } catch (...) {
                active = false;
            }
            func(a_pass, a_arg2, a_arg3);
            try {
                if (active && manager) {
                    constexpr int copyOut[] = { 4 };
                    ResetAllSeh(copyOut, std::size(copyOut));
                    ResetDepthSeh();
                    manager->dynamicWidthRatio = savedW;
                    manager->dynamicHeightRatio = savedH;
                }
            } catch (...) {
            }
        }
        static inline REL::Relocation<decltype(thunk)> func;
    };

    struct VatsConstantHook
    {
        static void thunk(void* a_this, int a_row, float a_x, float a_y, float a_z, float a_w)
        {
            float ratio = 1.0F;
            try {
                ratio = Platform::Fallout4Renderer::CurrentDynamicResolutionRatio();
            } catch (...) {
                ratio = 1.0F;
            }
            func(a_this, a_row, a_x * ratio, a_y * ratio, a_z, a_w);
        }
        static inline REL::Relocation<decltype(thunk)> func;
    };

    template <class Hook>
    void InstallCallHook(REL::ID a_id, std::ptrdiff_t a_offset, const char* a_name) noexcept
    {
        try {
            REL::Relocation<std::uintptr_t> target{ a_id, a_offset };
            const auto opcode = *reinterpret_cast<const std::uint8_t*>(target.address());
            if (opcode != 0xE8) {
                logger::warn("[SubNative] {}: expected E8 at REL({})+{:#x}, found {:#04x} — hook skipped (fail-open)",
                    a_name, a_id.id(), a_offset, opcode);
                return;
            }
            Hook::func = F4SE::GetTrampoline().write_call<5>(target.address(), Hook::thunk);
            logger::info("[SubNative] {} hook installed (REL({})+{:#x})", a_name, a_id.id(), a_offset);
        } catch (const std::exception& e) {
            logger::warn("[SubNative] {} hook failed: {} (fail-open)", a_name, e.what());
        } catch (...) {
            logger::warn("[SubNative] {} hook failed with an unknown exception (fail-open)", a_name);
        }
    }

    template <class Hook, class Fn>
    void InstallEntryDetour(REL::ID a_id, std::atomic<Fn>& a_original, const char* a_name) noexcept
    {
        try {
            const auto target = a_id.address();
            if (!target) {
                logger::warn("[SubNative] {}: REL({}) resolved to address 0 — hook skipped (fail-open)",
                    a_name, a_id.id());
                return;
            }
            const auto* const bytes = reinterpret_cast<const std::uint8_t*>(target);
            if (bytes[0] == 0xE8 || bytes[0] == 0xE9) {
                logger::warn("[SubNative] {}: REL({}) starts with {:#04x} (call/jmp, not a prologue) — hook skipped (fail-open)",
                    a_name, a_id.id(), bytes[0]);
                return;
            }
            const auto trampoline = Detours::X64::DetourFunction(
                target, reinterpret_cast<std::uintptr_t>(&Hook::thunk));
            if (!trampoline) {
                logger::warn("[SubNative] {}: DetourFunction refused REL({}) — hook skipped (fail-open)",
                    a_name, a_id.id());
                return;
            }
            a_original.store(reinterpret_cast<Fn>(trampoline), std::memory_order_release);
            logger::info("[SubNative] {} hook installed (entry detour at REL({}), prologue relocated)",
                a_name, a_id.id());
        } catch (const std::exception& e) {
            logger::warn("[SubNative] {} entry detour failed: {} (fail-open)", a_name, e.what());
        } catch (...) {
            logger::warn("[SubNative] {} entry detour failed with an unknown exception (fail-open)", a_name);
        }
    }
}

namespace Platform
{
    void RenderTargetProxy::InstallHooks() noexcept
    {
        InstallCallHook<HbaoHook>(REL::ID(984743), 0x1BA, "HBAO dyn-res fix");
        InstallCallHook<DeferredCompositeHook>(REL::ID(728427), 0x8DC, "deferred-composite dyn-res fix");
        InstallCallHook<VatsConstantHook>(REL::ID(1042583), 0xBB, "VATS outline dyn-res fix");
        InstallEntryDetour<LensFlareHook>(REL::ID(676108), g_lensFlareOriginal, "lens-flare dyn-res fix");
        try {
            REL::Relocation<std::uintptr_t> site{ REL::ID(587723), 0xE1 };
            const auto* const bytes = reinterpret_cast<const std::uint8_t*>(site.address());
            if (bytes[0] == 0xE8) {
                const auto rel = *reinterpret_cast<const std::int32_t*>(site.address() + 1);
                g_viewportDefaultFn = reinterpret_cast<SetDynResViewportDefaultFn>(
                    static_cast<std::intptr_t>(site.address()) + 5 + rel);
            } else {
                logger::warn("[SubNative] viewport-default setter unresolved ({:#04x} at REL(587723)+0xE1) — overrides run without it", bytes[0]);
            }
        } catch (...) {
            logger::warn("[SubNative] viewport-default resolution threw — overrides run without it");
        }
        logger::info("[SubNative] fix-up layer armed (proxies build on first sub-native frame; SSLR deferred by evidence policy)");
    }

    void RenderTargetProxy::UpdateForRatio(float a_ratio) noexcept
    {
        if (g_latchedOff) {
            return;
        }
        if (g_overridden || g_depthOverridden) {
            try {
                ResetDepthSeh();
                ResetAllSeh(nullptr, 0);
            } catch (...) {
            }
            if (g_overridden || g_depthOverridden) {
                g_overridden = false;
                g_depthOverridden = false;
                LatchOff("override state leaked across a frame boundary and could not be unwound");
                return;
            }
            logger::warn("[SubNative] leaked override state unwound at frame start (a hook's reset half was skipped)");
        }
        try {
            if (g_builtDevice) {
                const auto renderer = Renderer();
                if (renderer && renderer->device != g_builtDevice) {
                    ReleaseAllProxies();
                    g_builtRatio = 1.0F;
                    g_builtDevice = nullptr;
                    if (g_upscalingCB) { g_upscalingCB->Release(); g_upscalingCB = nullptr; }
                    if (g_tightDepthCS) { g_tightDepthCS->Release(); g_tightDepthCS = nullptr; }
                    if (g_linearDepthCS) { g_linearDepthCS->Release(); g_linearDepthCS = nullptr; }
                    g_depthFixOff = false;
                    logger::info("[SubNative] device edge — proxy set released; rebuilds on the next sub-native frame");
                }
            }
        } catch (...) {
        }
        const float ratio = (a_ratio > 0.0F && a_ratio < 0.999F) ? a_ratio : 1.0F;
        if (ratio == g_builtRatio) {
            return;
        }
        try {
            if (ratio >= 0.999F) {
                ReleaseAllProxies();
                g_builtRatio = 1.0F;
                g_builtDevice = nullptr;
                logger::info("[SubNative] proxies released (native rendering)");
                return;
            }
            const auto renderer = Renderer();
            const auto state = State();
            if (!renderer || !state || !renderer->device) {
                return;
            }
            auto* const device = reinterpret_cast<ID3D11Device*>(renderer->device);
            if (!g_depthFixOff && !EnsureComputeResourcesSeh(device)) {
                g_depthFixOff = true;
                logger::warn("[SubNative] depth fix disabled for this device (resource creation failed — see the HRESULT line above); proxy overrides continue");
            }
            std::size_t skipped = 0;
            g_builtCount = 0;
            for (const int index : kPatchIndices) {
                if (!BuildProxySeh(device, index, ratio)) {
                    ReleaseProxy(index);
                    ++skipped;
                } else if (g_proxy[index].texture) {
                    ++g_builtCount;
                }
            }
            if (skipped != 0) {
                logger::warn("[SubNative] {} of {} proxy targets failed/faulted to build — those indices run un-proxied (cosmetic only, never fatal)",
                    skipped, std::size(kPatchIndices));
            }
            ReleaseTightDepth();
            if (!g_depthFixOff) {
                D3D11_TEXTURE2D_DESC td{};
                td.Width = (std::max)(1U,
                    static_cast<UINT>(static_cast<float>(state->screenWidth) * ratio));
                td.Height = (std::max)(1U,
                    static_cast<UINT>(static_cast<float>(state->screenHeight) * ratio));
                td.MipLevels = 1;
                td.ArraySize = 1;
                td.Format = DXGI_FORMAT_R32_FLOAT;
                td.SampleDesc.Count = 1;
                td.Usage = D3D11_USAGE_DEFAULT;
                td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
                if (FAILED(device->CreateTexture2D(&td, nullptr, &g_tightDepth)) ||
                    FAILED(device->CreateShaderResourceView(g_tightDepth, nullptr, &g_tightDepthSRV)) ||
                    FAILED(device->CreateUnorderedAccessView(g_tightDepth, nullptr, &g_tightDepthUAV))) {
                    ReleaseTightDepth();
                    g_depthFixOff = true;
                    logger::warn("[SubNative] tight depth texture creation failed — depth fix disabled for this device; proxy overrides continue");
                }
            }
            g_builtRatio = ratio;
            g_builtDevice = renderer->device;
            logger::info("[SubNative] proxies built: {}/{} live for ratio {:.3f} (depth fix {}; zero live = layer inert)",
                g_builtCount, std::size(kPatchIndices), ratio, g_depthFixOff ? "OFF" : "on");
        } catch (...) {
            ReleaseAllProxies();
            LatchOff("unexpected exception during proxy rebuild");
        }
    }

    bool RenderTargetProxy::Available() noexcept
    {
        return !g_latchedOff;
    }
}
