// SPDX-License-Identifier: GPL-3.0-or-later
// Portions adapted from Community Shaders for Fallout 4 (northaxosky), GPL-3.0.

#include "PCH.h"

#include "Platform/FirstPersonMask.h"

#include "Platform/FrameGenEngine.h"
#include "Platform/SidecarGuides.h"

#include "CSFirstPersonMask.h"

#include <psapi.h>

#include <atomic>
#include <cstdio>
#include <cstring>

namespace
{
    using namespace Platform::SidecarGuides;

    std::atomic<bool> g_installAttempted{ false };
    std::atomic<bool> g_installed{ false };
    std::atomic<std::uint64_t> g_produced{ 0 };
    char g_reason[96]{};

    ID3D11Texture2D* g_pre{ nullptr };
    ID3D11Texture2D* g_post{ nullptr };
    ID3D11ShaderResourceView* g_preSrv{ nullptr };
    ID3D11ShaderResourceView* g_postSrv{ nullptr };
    ID3D11Texture2D* g_mask{ nullptr };
    ID3D11ShaderResourceView* g_maskSrv{ nullptr };
    ID3D11UnorderedAccessView* g_maskUav{ nullptr };
    ID3D11ComputeShader* g_cs{ nullptr };
    ID3D11Device* g_device{ nullptr };
    std::uint32_t g_w{ 0 }, g_h{ 0 };
    DXGI_FORMAT g_fmt{ DXGI_FORMAT_UNKNOWN };
    bool g_refused{ false };
    unsigned g_logged{ 0 };

    void ReleaseAll() noexcept
    {
        ReleaseCom(g_cs);
        ReleaseCom(g_maskUav);
        ReleaseCom(g_maskSrv);
        ReleaseCom(g_mask);
        ReleaseCom(g_postSrv);
        ReleaseCom(g_preSrv);
        ReleaseCom(g_post);
        ReleaseCom(g_pre);
        g_w = g_h = 0;
        g_fmt = DXGI_FORMAT_UNKNOWN;
        g_device = nullptr;
    }

    [[nodiscard]] bool Ensure(ID3D11Device* a_device, const D3D11_TEXTURE2D_DESC& a_desc) noexcept
    {
        if (g_device != a_device) {
            ReleaseAll();
            g_device = a_device;
        }
        if (g_cs == nullptr && FAILED(a_device->CreateComputeShader(g_csFirstPersonMask, sizeof(g_csFirstPersonMask), nullptr, &g_cs))) {
            return false;
        }
        if (g_pre != nullptr && g_w == a_desc.Width && g_h == a_desc.Height && g_fmt == a_desc.Format) {
            return true;
        }
        ReleaseCom(g_maskUav);
        ReleaseCom(g_maskSrv);
        ReleaseCom(g_mask);
        ReleaseCom(g_postSrv);
        ReleaseCom(g_preSrv);
        ReleaseCom(g_post);
        ReleaseCom(g_pre);
        D3D11_TEXTURE2D_DESC copy = a_desc;
        copy.Usage = D3D11_USAGE_DEFAULT;
        copy.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        copy.CPUAccessFlags = 0;
        copy.MiscFlags = 0;
        copy.MipLevels = 1;
        copy.ArraySize = 1;
        D3D11_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = TypedColorFormat(a_desc.Format);
        view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        view.Texture2D.MipLevels = 1;
        D3D11_TEXTURE2D_DESC mask{};
        mask.Width = a_desc.Width;
        mask.Height = a_desc.Height;
        mask.MipLevels = 1;
        mask.ArraySize = 1;
        mask.Format = DXGI_FORMAT_R8_UNORM;
        mask.SampleDesc.Count = 1;
        mask.Usage = D3D11_USAGE_DEFAULT;
        mask.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        if (FAILED(a_device->CreateTexture2D(&copy, nullptr, &g_pre)) ||
            FAILED(a_device->CreateTexture2D(&copy, nullptr, &g_post)) ||
            FAILED(a_device->CreateShaderResourceView(g_pre, &view, &g_preSrv)) ||
            FAILED(a_device->CreateShaderResourceView(g_post, &view, &g_postSrv)) ||
            FAILED(a_device->CreateTexture2D(&mask, nullptr, &g_mask)) ||
            FAILED(a_device->CreateShaderResourceView(g_mask, nullptr, &g_maskSrv)) ||
            !CreateUav(a_device, g_mask, DXGI_FORMAT_R8_UNORM, g_maskUav)) {
            ReleaseAll();
            return false;
        }
        g_device = a_device;
        g_w = a_desc.Width;
        g_h = a_desc.Height;
        g_fmt = a_desc.Format;
        logger::info("[FPMask] first-person mask textures: {}x{} (scene colour fmt={}, mask R8)", g_w, g_h,
            static_cast<int>(a_desc.Format));
        return true;
    }

    [[nodiscard]] ID3D11Texture2D* BoundColorTarget(ID3D11DeviceContext* a_context) noexcept
    {
        ID3D11RenderTargetView* rtv = nullptr;
        a_context->OMGetRenderTargets(1, &rtv, nullptr);
        if (rtv == nullptr) {
            return nullptr;
        }
        ID3D11Resource* resource = nullptr;
        rtv->GetResource(&resource);
        rtv->Release();
        if (resource == nullptr) {
            return nullptr;
        }
        ID3D11Texture2D* texture = nullptr;
        (void)resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&texture));
        resource->Release();
        return texture;
    }

    struct FirstPersonAlphaCall
    {
        static void thunk(void* a_accumulator)
        {
            ID3D11Texture2D* target = nullptr;
            ID3D11DeviceContext* context = nullptr;
            ID3D11Device* device = nullptr;
            bool captured = false;
            GUARD_BEGIN
            if (!g_refused && Platform::FrameGenEngine::Interpolating()) {
                if (auto* const data = RE::BSGraphics::RendererData::GetSingleton();
                    data != nullptr && data->device != nullptr && data->context != nullptr) {
                    device = reinterpret_cast<ID3D11Device*>(data->device);
                    context = reinterpret_cast<ID3D11DeviceContext*>(data->context);
                    target = BoundColorTarget(context);
                    if (target != nullptr) {
                        D3D11_TEXTURE2D_DESC desc{};
                        target->GetDesc(&desc);
                        if (desc.SampleDesc.Count == 1 && Ensure(device, desc)) {
                            context->CopyResource(g_pre, target);
                            captured = true;
                        } else {
                            g_refused = true;
                            logger::warn("[FPMask] {} — first-person conditioning is off for this session",
                                desc.SampleDesc.Count != 1 ? "the bound colour target is multisampled (the mask needs a single-sample copy)"
                                                           : "the mask shader or textures could not be created");
                        }
                    }
                }
            }
            GUARD_END("FPMask.pre")

            func(a_accumulator);

            GUARD_BEGIN
            if (captured) {
                {
                    context->CopyResource(g_post, target);
                    const ComputeBindingsScope csScope(context);
                    context->CSSetShader(g_cs, nullptr, 0);
                    ID3D11ShaderResourceView* srvs[] = { g_preSrv, g_postSrv };
                    context->CSSetShaderResources(5, 2, srvs);
                    ID3D11UnorderedAccessView* uavs[] = { g_maskUav };
                    context->CSSetUnorderedAccessViews(4, 1, uavs, nullptr);
                    context->Dispatch((g_w + 7U) / 8U, (g_h + 7U) / 8U, 1U);
                    ID3D11ShaderResourceView* noSrvs[] = { nullptr, nullptr };
                    context->CSSetShaderResources(5, 2, noSrvs);
                    ID3D11UnorderedAccessView* noUavs[] = { nullptr };
                    context->CSSetUnorderedAccessViews(4, 1, noUavs, nullptr);
                    g_produced.fetch_add(1, std::memory_order_release);
                    if ((g_logged & 1U) == 0U) {
                        g_logged |= 1U;
                        logger::info("[FPMask] first mask produced at the first-person alpha pass ({}x{})", g_w, g_h);
                    }
                }
            }
            GUARD_END("FPMask.post")
            if (target != nullptr) {
                target->Release();
            }
        }
        static inline REL::Relocation<decltype(thunk)> func;
    };

    [[nodiscard]] bool InsideGameModule(std::uintptr_t a_addr) noexcept
    {
        HMODULE game = ::GetModuleHandleW(nullptr);
        MODULEINFO info{};
        if (game == nullptr || !::GetModuleInformation(::GetCurrentProcess(), game, &info, sizeof(info))) {
            return false;
        }
        const auto base = reinterpret_cast<std::uintptr_t>(info.lpBaseOfDll);
        return a_addr >= base && a_addr < base + info.SizeOfImage;
    }
}

namespace Platform::FirstPersonMask
{
    void Install() noexcept
    {
        if (g_installAttempted.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        try {
            const std::uintptr_t callSite = REL::ID(338205).address() + 0x253;
            const auto* bytes = reinterpret_cast<const std::uint8_t*>(callSite);
            if (bytes[0] != 0xE8) {
                std::snprintf(g_reason, sizeof(g_reason), "call site %#llx does not hold a CALL (byte %02X)",
                    static_cast<unsigned long long>(callSite), bytes[0]);
                logger::warn("[FPMask] {} — first-person conditioning NOT installed", g_reason);
                return;
            }
            std::int32_t rel = 0;
            std::memcpy(&rel, bytes + 1, sizeof(rel));
            const std::uintptr_t target = callSite + 5 + static_cast<std::uintptr_t>(static_cast<std::intptr_t>(rel));
            if (!InsideGameModule(target)) {
                std::snprintf(g_reason, sizeof(g_reason), "the CALL at %#llx targets %#llx, outside the game's code",
                    static_cast<unsigned long long>(callSite), static_cast<unsigned long long>(target));
                logger::warn("[FPMask] {} — first-person conditioning NOT installed", g_reason);
                return;
            }
            FirstPersonAlphaCall::func = F4SE::GetTrampoline().write_call<5>(callSite, FirstPersonAlphaCall::thunk);
            g_installed.store(true, std::memory_order_release);
            logger::info("[FPMask] first-person conditioning installed at the first-person alpha call (REL 338205 + 0x253 -> {:#x}); "
                         "frame generation gets viewmodel pixels at camera depth with zero motion", target);
        } catch (...) {
            std::snprintf(g_reason, sizeof(g_reason), "the call-site thunk threw");
            logger::warn("[FPMask] the first-person alpha thunk threw — first-person conditioning inactive");
        }
    }

    bool TakeMask(std::uint64_t& a_cursor, ID3D11ShaderResourceView*& a_srv, std::uint32_t& a_width, std::uint32_t& a_height) noexcept
    {
        const std::uint64_t produced = g_produced.load(std::memory_order_acquire);
        if (produced == 0 || produced == a_cursor || g_maskSrv == nullptr) {
            return false;
        }
        a_cursor = produced;
        a_srv = g_maskSrv;
        a_width = g_w;
        a_height = g_h;
        return true;
    }

    State Snapshot() noexcept
    {
        State state{};
        state.installed = g_installed.load(std::memory_order_relaxed);
        state.refused = g_refused;
        state.produced = g_produced.load(std::memory_order_relaxed);
        std::snprintf(state.reason, sizeof(state.reason), "%s", g_refused ? "refused this session (see the [FPMask] lines)" : g_reason);
        return state;
    }
}
