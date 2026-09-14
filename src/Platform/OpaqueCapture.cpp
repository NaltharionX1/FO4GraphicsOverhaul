// SPDX-License-Identifier: GPL-3.0-or-later
// Portions adapted from Community Shaders for Fallout 4 (northaxosky), GPL-3.0.

#include "PCH.h"

#include "Platform/OpaqueCapture.h"

#include "Platform/FsrEngine.h"
#include "Platform/Streamline.h"

#include <d3d11.h>
#include <psapi.h>

#include <atomic>
#include <cstdio>
#include <cstring>

namespace
{
    std::atomic<bool> g_installAttempted{ false };
    bool g_refused{ false };
    bool g_loggedAllocFailure{ false };
    char g_reason[96]{};

    ID3D11Texture2D* g_opaque{ nullptr };
    std::uint32_t g_w{ 0 };
    std::uint32_t g_h{ 0 };
    DXGI_FORMAT g_fmt{ DXGI_FORMAT_UNKNOWN };
    bool g_freshThisFrame{ false };

    void Free() noexcept
    {
        if (g_opaque != nullptr) {
            g_opaque->Release();
            g_opaque = nullptr;
        }
        g_w = 0;
        g_h = 0;
        g_fmt = DXGI_FORMAT_UNKNOWN;
        g_freshThisFrame = false;
    }

    [[nodiscard]] bool Ensure(ID3D11Device* a_device, const D3D11_TEXTURE2D_DESC& a_src) noexcept
    {
        if (g_opaque != nullptr && g_w == a_src.Width && g_h == a_src.Height && g_fmt == a_src.Format) {
            return true;
        }
        Free();
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = a_src.Width;
        desc.Height = a_src.Height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = a_src.Format;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(a_device->CreateTexture2D(&desc, nullptr, &g_opaque))) {
            Free();
            return false;
        }
        g_w = a_src.Width;
        g_h = a_src.Height;
        g_fmt = a_src.Format;
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

    [[nodiscard]] bool Wanted() noexcept
    {
        return Platform::Streamline::AppliedEffectiveEngine() == Platform::AaEffectiveEngine::kFsr &&
               Platform::FsrEngine::MasksEnabled();
    }

    struct OpaqueFinishCall
    {
        static void thunk(void* a_accumulator)
        {
            func(a_accumulator);
            GUARD_BEGIN
            if (!Wanted()) {
                if (g_opaque != nullptr) {
                    Free();
                }
            } else if (!g_refused) {
                if (auto* const data = RE::BSGraphics::RendererData::GetSingleton();
                    data != nullptr && data->device != nullptr && data->context != nullptr) {
                    auto* const device = reinterpret_cast<ID3D11Device*>(data->device);
                    auto* const context = reinterpret_cast<ID3D11DeviceContext*>(data->context);
                    if (ID3D11Texture2D* const target = BoundColorTarget(context); target != nullptr) {
                        D3D11_TEXTURE2D_DESC desc{};
                        target->GetDesc(&desc);
                        if (desc.SampleDesc.Count == 1 && Ensure(device, desc)) {
                            context->CopyResource(g_opaque, target);
                            g_freshThisFrame = true;
                        } else {
                            const bool permanent = desc.SampleDesc.Count != 1;
                            std::snprintf(g_reason, sizeof(g_reason), "%s",
                                permanent ? "the bound colour target is multisampled (the copy needs a single-sample target)"
                                          : "the opaque copy could not be allocated");
                            if (permanent) {
                                g_refused = true;
                                logger::warn("[FSRMask] {} — the reactive/transparency masks are off for this session",
                                    g_reason);
                            } else if (!g_loggedAllocFailure) {
                                g_loggedAllocFailure = true;
                                logger::warn("[FSRMask] {} — retrying while the masks stay off", g_reason);
                            }
                        }
                        target->Release();
                    }
                }
            }
            GUARD_END("opaquecapture.thunk")
        }
        static inline REL::Relocation<decltype(thunk)> func;
    };
}

namespace Platform::OpaqueCapture
{
    void Install() noexcept
    {
        if (g_installAttempted.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        try {
            const std::uintptr_t callSite = REL::ID(338205).address() + 0x1DC;
            const auto* bytes = reinterpret_cast<const std::uint8_t*>(callSite);
            if (bytes[0] != 0xE8) {
                std::snprintf(g_reason, sizeof(g_reason), "call site %#llx does not hold a CALL (byte %02X)",
                    static_cast<unsigned long long>(callSite), bytes[0]);
                logger::warn("[FSRMask] {} — the reactive/transparency masks are NOT installed", g_reason);
                return;
            }
            std::int32_t rel = 0;
            std::memcpy(&rel, bytes + 1, sizeof(rel));
            const std::uintptr_t target = callSite + 5 + static_cast<std::uintptr_t>(static_cast<std::intptr_t>(rel));
            if (!InsideGameModule(target)) {
                std::snprintf(g_reason, sizeof(g_reason), "the CALL at %#llx targets %#llx, outside the game's code",
                    static_cast<unsigned long long>(callSite), static_cast<unsigned long long>(target));
                logger::warn("[FSRMask] {} — the reactive/transparency masks are NOT installed", g_reason);
                return;
            }
            OpaqueFinishCall::func = F4SE::GetTrampoline().write_call<5>(callSite, OpaqueFinishCall::thunk);
            logger::info("[FSRMask] opaque capture installed at the opaque-finish call (REL 338205 + 0x1DC -> {:#x}); "
                         "FSR's reactive mask and our transparency mask have their pre-alpha reference",
                target);
        } catch (...) {
            std::snprintf(g_reason, sizeof(g_reason), "the call-site thunk threw");
            logger::warn("[FSRMask] the opaque-finish thunk threw — the masks are inactive this session");
        }
    }

    ID3D11Texture2D* TakeThisFrame() noexcept
    {
        if (!g_freshThisFrame || g_opaque == nullptr) {
            return nullptr;
        }
        g_freshThisFrame = false;
        return g_opaque;
    }

    void Release() noexcept
    {
        Free();
    }
}
