// SPDX-License-Identifier: GPL-3.0-or-later
// Portions adapted from Community Shaders for Fallout 4 (northaxosky), GPL-3.0.

#include "Hooks/D3DHook.h"

#include "Core/DeviceObjects.h"
#include "Core/FailOpen.h"
#include "Hooks/WndProcHook.h"
#include "Platform/HdrDisplay.h"
#include "Platform/FrameFingerprint.h"
#include "Platform/HfpfCoexist.h"
#include "Platform/PresentPolicy.h"
#include "Platform/Reflex.h"
#include "Platform/PresentProxy.h"
#include "Platform/Streamline.h"
#include "Telemetry/Telemetry.h"
#include "UI/Menu.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>

#include <d3d11.h>
#include <dxgi.h>

namespace Hooks
{

    void* PatchIAT(const char* a_dllName, const char* a_funcName, void* a_detour) noexcept
    {
        const HMODULE exeModule = ::GetModuleHandleW(nullptr);
        if (!exeModule) {
            return nullptr;
        }

        const auto base = reinterpret_cast<std::uintptr_t>(exeModule);
        const auto* dosHeader = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE) {
            return nullptr;
        }

        const auto* ntHeaders = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dosHeader->e_lfanew);
        if (ntHeaders->Signature != IMAGE_NT_SIGNATURE) {
            return nullptr;
        }

        const auto& importDir = ntHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (importDir.VirtualAddress == 0 || importDir.Size == 0) {
            return nullptr;
        }

        auto* importDesc = reinterpret_cast<PIMAGE_IMPORT_DESCRIPTOR>(base + importDir.VirtualAddress);
        for (; importDesc->Name != 0; ++importDesc) {
            const auto* dllName = reinterpret_cast<const char*>(base + importDesc->Name);
            if (_stricmp(dllName, a_dllName) != 0) {
                continue;
            }

            if (importDesc->OriginalFirstThunk == 0) {
                return nullptr;
            }

            auto* origThunk = reinterpret_cast<PIMAGE_THUNK_DATA>(base + importDesc->OriginalFirstThunk);
            auto* iatThunk = reinterpret_cast<PIMAGE_THUNK_DATA>(base + importDesc->FirstThunk);

            for (; origThunk->u1.AddressOfData != 0; ++origThunk, ++iatThunk) {
                if (IMAGE_SNAP_BY_ORDINAL(origThunk->u1.Ordinal)) {
                    continue;
                }
                const auto* importByName =
                    reinterpret_cast<PIMAGE_IMPORT_BY_NAME>(base + origThunk->u1.AddressOfData);
                if (std::strcmp(importByName->Name, a_funcName) != 0) {
                    continue;
                }

                void** slot = reinterpret_cast<void**>(&iatThunk->u1.Function);
                DWORD oldProtect = 0;
                if (!::VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
                    return nullptr;
                }
                void* original = *slot;
                *slot = a_detour;
                DWORD ignored = 0;
                ::VirtualProtect(slot, sizeof(void*), oldProtect, &ignored);
                return original;
            }
            return nullptr;
        }
        return nullptr;
    }
}

namespace Hooks::D3DHook
{
    namespace
    {

        using PFN_D3D11CreateDeviceAndSwapChain = decltype(&D3D11CreateDeviceAndSwapChain);
        using PFN_Present = HRESULT(__stdcall*)(IDXGISwapChain*, UINT, UINT);
        using PFN_ResizeBuffers = HRESULT(__stdcall*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

        constexpr std::size_t kPresentSlot = 8;
        constexpr std::size_t kResizeBuffersSlot = 13;

        std::atomic<PFN_D3D11CreateDeviceAndSwapChain> g_realCreateDevice{ nullptr };
        std::atomic<PFN_Present> g_realPresent{ nullptr };
        std::atomic<PFN_ResizeBuffers> g_realResizeBuffers{ nullptr };
        std::atomic<IDXGISwapChain*> g_presentArmedOn{ nullptr };
        thread_local bool t_inPresent = false;

        [[nodiscard]] bool BeginVTablePatch(
            void** a_vtbl, std::size_t a_slot, DWORD& a_outOldProtect, void** a_outOriginal) noexcept
        {
            if (!::VirtualProtect(&a_vtbl[a_slot], sizeof(void*), PAGE_READWRITE, &a_outOldProtect)) {
                return false;
            }
            *a_outOriginal = a_vtbl[a_slot];
            return true;
        }

        void CommitVTablePatch(void** a_vtbl, std::size_t a_slot, void* a_detour, DWORD a_oldProtect) noexcept
        {
            a_vtbl[a_slot] = a_detour;
            DWORD ignored = 0;
            ::VirtualProtect(&a_vtbl[a_slot], sizeof(void*), a_oldProtect, &ignored);
        }

        [[nodiscard]] HRESULT CreateOwnSwapChain(
            ID3D11Device* a_device, const DXGI_SWAP_CHAIN_DESC& a_desc, IDXGISwapChain** a_out) noexcept
        {
            if (!a_device || !a_out) {
                return E_POINTER;
            }
            IDXGIDevice* dxgiDevice = nullptr;
            IDXGIAdapter* adapter = nullptr;
            IDXGIFactory* factory = nullptr;
            HRESULT hr = a_device->QueryInterface(IID_PPV_ARGS(&dxgiDevice));
            if (SUCCEEDED(hr) && dxgiDevice) {
                hr = dxgiDevice->GetAdapter(&adapter);
                dxgiDevice->Release();
            }
            if (SUCCEEDED(hr) && adapter) {
                hr = adapter->GetParent(IID_PPV_ARGS(&factory));
                adapter->Release();
            }
            if (SUCCEEDED(hr) && factory) {
                DXGI_SWAP_CHAIN_DESC desc = a_desc;
                hr = factory->CreateSwapChain(a_device, &desc, a_out);
                if (SUCCEEDED(hr)) {
                    factory->MakeWindowAssociation(desc.OutputWindow, DXGI_MWA_NO_ALT_ENTER);
                }
                factory->Release();
            }
            return hr;
        }

        struct RestoreRenderTargets
        {
            ID3D11DeviceContext* context;
            ID3D11RenderTargetView* rtv{ nullptr };
            ID3D11DepthStencilView* dsv{ nullptr };
            ID3D11HullShader* hs{ nullptr };
            ID3D11DomainShader* ds{ nullptr };
            ID3D11ComputeShader* cs{ nullptr };
            ID3D11ClassInstance* hsInstances[256]{};
            ID3D11ClassInstance* dsInstances[256]{};
            ID3D11ClassInstance* csInstances[256]{};
            UINT hsCount{ 256 };
            UINT dsCount{ 256 };
            UINT csCount{ 256 };

            explicit RestoreRenderTargets(ID3D11DeviceContext* a_context) noexcept :
                context(a_context)
            {
                context->OMGetRenderTargets(1, &rtv, &dsv);
                context->HSGetShader(&hs, hsInstances, &hsCount);
                context->DSGetShader(&ds, dsInstances, &dsCount);
                context->CSGetShader(&cs, csInstances, &csCount);
            }

            ~RestoreRenderTargets() noexcept
            {
                context->OMSetRenderTargets(1, &rtv, dsv);
                context->HSSetShader(hs, hsCount != 0 ? hsInstances : nullptr, hsCount);
                context->DSSetShader(ds, dsCount != 0 ? dsInstances : nullptr, dsCount);
                context->CSSetShader(cs, csCount != 0 ? csInstances : nullptr, csCount);
                if (rtv) {
                    rtv->Release();
                }
                if (dsv) {
                    dsv->Release();
                }
                ReleaseStage(hs, hsInstances, hsCount);
                ReleaseStage(ds, dsInstances, dsCount);
                ReleaseStage(cs, csInstances, csCount);
            }

            template <class Shader>
            static void ReleaseStage(Shader* a_shader, ID3D11ClassInstance** a_instances, UINT a_count) noexcept
            {
                for (UINT i = 0; i < a_count; ++i) {
                    if (a_instances[i]) {
                        a_instances[i]->Release();
                    }
                }
                if (a_shader) {
                    a_shader->Release();
                }
            }

            RestoreRenderTargets(const RestoreRenderTargets&) = delete;
            RestoreRenderTargets& operator=(const RestoreRenderTargets&) = delete;
        };

        void RenderCanvasFrame_Guarded(IDXGISwapChain* a_swapChain)
        {
            GUARD_BEGIN
                if (!Core::FailOpen::Tripped("fingerprint")) {
                    Platform::FrameFingerprint::SamplePresented(a_swapChain);
                }
                if (UI::Menu::GetSingleton().IsInitialized()) {
                    const auto snap = Core::DeviceObjects::Current();
                    if (snap.context && snap.swapChain == a_swapChain) {
                        if (ID3D11RenderTargetView* rtv =
                                Core::DeviceObjects::AcquireBackbufferRTV(a_swapChain)) {
                            RestoreRenderTargets restore(snap.context);
                            snap.context->OMSetRenderTargets(1, &rtv, nullptr);
                            UI::Menu::GetSingleton().Render();
                        }
                    }
                }
            GUARD_END_TRIP("canvas.present.render", "canvas-render", "C++ exception during canvas render")
        }

        void RenderCanvasFrame_SEHLeaf(IDXGISwapChain* a_swapChain) noexcept
        {
            __try {
                RenderCanvasFrame_Guarded(a_swapChain);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                Core::FailOpen::Trip("canvas-render", "SEH exception (AV) during canvas render");
            }
        }

        void FingerprintCanvas_SEHLeaf(IDXGISwapChain* a_swapChain) noexcept
        {
            __try {
                Platform::FrameFingerprint::SampleCanvas(a_swapChain);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                Core::FailOpen::Trip("fingerprint", "SEH exception (AV) in the after-canvas fingerprint");
                Platform::FrameFingerprint::TripOff("a hardware fault after the canvas; fail-open tripped");
            }
        }

        HRESULT __stdcall hk_Present(IDXGISwapChain* a_swapChain, UINT a_syncInterval, UINT a_flags) noexcept
        {
            const PFN_Present real = g_realPresent.load(std::memory_order_acquire);
            if (!real) {
                return E_UNEXPECTED;
            }
            if (t_inPresent) {
                return real(a_swapChain, a_syncInterval, a_flags);
            }
            t_inPresent = true;
            Telemetry::NoteFrame(UI::Menu::GetSingleton().IsOpen());
            Telemetry::LogFrameStatsIfDue();
            if (!Core::FailOpen::Tripped("canvas-render")) {
                RenderCanvasFrame_SEHLeaf(a_swapChain);
            }
            if (!Core::FailOpen::Tripped("fingerprint")) {
                FingerprintCanvas_SEHLeaf(a_swapChain);
            }
            UINT syncInterval = a_syncInterval;
            UINT presentFlags = a_flags;
            GUARD_BEGIN
                const auto params = Platform::PresentPolicy::ParamsFor();
                syncInterval = params.syncInterval;
                presentFlags = (a_flags & ~static_cast<UINT>(Platform::kPresentAllowTearing)) |
                               params.flags;
                if (!Platform::PresentPolicy::LimitAfterPresent()) {
                    Platform::PresentPolicy::PaceFrame();
                }
            GUARD_END("present.policy")

            if (!Platform::PresentProxy::Active()) {
                Platform::FrameFingerprint::EndFrame(a_swapChain);
            }
            const HRESULT hr = real(a_swapChain, syncInterval, presentFlags);

            GUARD_BEGIN
                if (Platform::PresentPolicy::LimitAfterPresent()) {
                    Platform::PresentPolicy::PaceFrame();
                }
            GUARD_END("present.pace.post")

            GUARD_BEGIN
                Platform::Reflex::AfterPresent();
            GUARD_END("present.reflex.tail")

            t_inPresent = false;
            return hr;
        }

        HRESULT __stdcall hk_ResizeBuffers(
            IDXGISwapChain* a_swapChain, UINT a_bufferCount, UINT a_width, UINT a_height,
            DXGI_FORMAT a_format, UINT a_flags) noexcept
        {
            const PFN_ResizeBuffers real = g_realResizeBuffers.load(std::memory_order_acquire);
            if (!real) {
                return E_UNEXPECTED;
            }

            GUARD_BEGIN
                Core::DeviceObjects::ReleaseSizedViews();
                logger::info("[D3DHook] resize buffers requested: count={}, {}x{}, format={}, flags={}",
                    a_bufferCount, a_width, a_height, static_cast<unsigned>(a_format), a_flags);
            GUARD_END("resizebuffers.pre")

            const HRESULT hr = real(a_swapChain, a_bufferCount, a_width, a_height, a_format, a_flags);

            if (SUCCEEDED(hr)) {
                GUARD_BEGIN
                    logger::info("[D3DHook] resize buffers succeeded: {}x{}", a_width, a_height);
                    Platform::HdrDisplay::Observe(a_swapChain);
                GUARD_END("resizebuffers.post")
            } else {
                Core::FailOpen::Trip("resize-buffers", "real ResizeBuffers failed");
            }

            return hr;
        }

        [[nodiscard]] const char* SwapEffectName(DXGI_SWAP_EFFECT a_effect) noexcept
        {
            switch (a_effect) {
            case DXGI_SWAP_EFFECT_DISCARD:         return "DISCARD (bitblt)";
            case DXGI_SWAP_EFFECT_SEQUENTIAL:      return "SEQUENTIAL (bitblt)";
            case DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL: return "FLIP_SEQUENTIAL";
            case DXGI_SWAP_EFFECT_FLIP_DISCARD:    return "FLIP_DISCARD";
            default:                               return "unknown";
            }
        }

        void LogRequestedSwapChainDesc(const DXGI_SWAP_CHAIN_DESC* a_desc) noexcept
        {
            if (!a_desc) {
                logger::info("[D3DHook] swap chain REQUESTED: (none — device created without a swap chain)");
                return;
            }
            logger::info(
                "[D3DHook] swap chain REQUESTED: {}x{} format={} buffers={} effect={} samples={}/{} flags={:#x} windowed={}",
                a_desc->BufferDesc.Width, a_desc->BufferDesc.Height,
                static_cast<unsigned>(a_desc->BufferDesc.Format), a_desc->BufferCount,
                SwapEffectName(a_desc->SwapEffect), a_desc->SampleDesc.Count,
                a_desc->SampleDesc.Quality, a_desc->Flags, a_desc->Windowed ? "yes" : "no");
        }

        void LogSwapChainCensus(IDXGISwapChain* a_swapChain) noexcept
        {
            if (!a_swapChain) {
                return;
            }
            DXGI_SWAP_CHAIN_DESC desc{};
            if (FAILED(a_swapChain->GetDesc(&desc))) {
                logger::warn("[D3DHook] swap-chain census unavailable (GetDesc failed)");
                return;
            }
            logger::info(
                "[D3DHook] swap chain CREATED: {}x{} format={} buffers={} effect={} samples={}/{} flags={:#x} windowed={}",
                desc.BufferDesc.Width, desc.BufferDesc.Height,
                static_cast<unsigned>(desc.BufferDesc.Format), desc.BufferCount,
                SwapEffectName(desc.SwapEffect), desc.SampleDesc.Count, desc.SampleDesc.Quality,
                desc.Flags, desc.Windowed ? "yes" : "no");
        }

        bool ArmSwapchainHooks(IDXGISwapChain* a_swapChain) noexcept
        {
            if (!a_swapChain) {
                return false;
            }
            if (g_presentArmedOn.load(std::memory_order_acquire) == a_swapChain) {
                return true;
            }

            static std::mutex s_armMutex;
            try {
                std::scoped_lock lock(s_armMutex);

                if (g_presentArmedOn.load(std::memory_order_acquire) == a_swapChain) {
                    return true;
                }

                void** vtbl = *reinterpret_cast<void***>(a_swapChain);

                if (vtbl[kPresentSlot] == reinterpret_cast<void*>(&hk_Present)) {
                    g_presentArmedOn.store(a_swapChain, std::memory_order_release);
                    return true;
                }

                DWORD presentOldProtect = 0;
                void* originalPresent = nullptr;
                if (!BeginVTablePatch(vtbl, kPresentSlot, presentOldProtect, &originalPresent)) {
                    Core::FailOpen::Trip("present-hook-protect",
                        "VirtualProtect failed arming Present -- will retry on next ArmSwapchainHooks call");
                    return false;
                }
                g_realPresent.store(reinterpret_cast<PFN_Present>(originalPresent), std::memory_order_release);
                std::atomic_thread_fence(std::memory_order_release);
                CommitVTablePatch(vtbl, kPresentSlot, reinterpret_cast<void*>(&hk_Present), presentOldProtect);

                if (vtbl[kResizeBuffersSlot] != reinterpret_cast<void*>(&hk_ResizeBuffers)) {
                    DWORD resizeOldProtect = 0;
                    void* originalResize = nullptr;
                    if (BeginVTablePatch(vtbl, kResizeBuffersSlot, resizeOldProtect, &originalResize)) {
                        g_realResizeBuffers.store(
                            reinterpret_cast<PFN_ResizeBuffers>(originalResize), std::memory_order_release);
                        std::atomic_thread_fence(std::memory_order_release);
                        CommitVTablePatch(
                            vtbl, kResizeBuffersSlot, reinterpret_cast<void*>(&hk_ResizeBuffers), resizeOldProtect);
                    } else {
                        Core::FailOpen::Trip("resize-hook-protect",
                            "VirtualProtect failed arming ResizeBuffers -- will retry on next ArmSwapchainHooks call");
                    }
                }

                g_presentArmedOn.store(a_swapChain, std::memory_order_release);

                GUARD_BEGIN
                    logger::info(
                        "[D3DHook] present hook armed, swapchain={}", static_cast<void*>(a_swapChain));
                GUARD_END("d3dhook.arm.log")

                return true;
            } catch (...) {
                Core::FailOpen::Trip("present-hook-arm-lock", "locking the swapchain-arm mutex failed");
                return false;
            }
        }

        HRESULT WINAPI hk_CreateDeviceAndSwapChain(
            IDXGIAdapter* a_adapter,
            D3D_DRIVER_TYPE a_driverType,
            HMODULE a_software,
            UINT a_flags,
            const D3D_FEATURE_LEVEL* a_featureLevels,
            UINT a_featureLevelCount,
            UINT a_sdkVersion,
            const DXGI_SWAP_CHAIN_DESC* a_swapChainDesc,
            IDXGISwapChain** a_swapChain,
            ID3D11Device** a_device,
            D3D_FEATURE_LEVEL* a_featureLevel,
            ID3D11DeviceContext** a_context) noexcept
        {
            const Core::DeviceObjects::ReentrancyGuard reentrancy;

            GUARD_BEGIN
                Platform::HfpfCoexist::Observe();
                LogRequestedSwapChainDesc(a_swapChainDesc);
            GUARD_END("d3dhook.create.pre")

            DXGI_SWAP_CHAIN_DESC policyDesc{};
            const DXGI_SWAP_CHAIN_DESC* effectiveDesc = a_swapChainDesc;
            if (a_swapChainDesc) {
                GUARD_BEGIN
                    policyDesc = *a_swapChainDesc;
                    Platform::PresentPolicy::ApplyToDesc(policyDesc);
                    effectiveDesc = &policyDesc;
                GUARD_END("d3dhook.create.policy")
            }

            PFN_D3D11CreateDeviceAndSwapChain real = g_realCreateDevice.load(std::memory_order_acquire);
            if (!real) {
                if (const HMODULE d3d11 = ::GetModuleHandleW(L"d3d11.dll")) {
                    real = reinterpret_cast<PFN_D3D11CreateDeviceAndSwapChain>(
                        ::GetProcAddress(d3d11, "D3D11CreateDeviceAndSwapChain"));
                }
                if (!real) {
                    Core::FailOpen::Trip("device-hook-null-original",
                        "real D3D11CreateDeviceAndSwapChain unresolved -- refusing to fabricate a device");
                    return E_UNEXPECTED;
                }
            }

            static constexpr D3D_FEATURE_LEVEL kPreferredLevels[]{
                D3D_FEATURE_LEVEL_11_1,
                D3D_FEATURE_LEVEL_11_0,
                D3D_FEATURE_LEVEL_10_1,
                D3D_FEATURE_LEVEL_10_0,
            };
            const bool offeredOurLevels = (a_featureLevels == nullptr || a_featureLevelCount == 0);
            const D3D_FEATURE_LEVEL* const wantedLevels = offeredOurLevels ? kPreferredLevels : a_featureLevels;
            const UINT wantedLevelCount =
                offeredOurLevels ? static_cast<UINT>(sizeof(kPreferredLevels) / sizeof(kPreferredLevels[0]))
                                 : a_featureLevelCount;

            const bool proxyPath = reentrancy.IsOutermost() &&
                                   effectiveDesc != nullptr && a_swapChain != nullptr && a_device != nullptr &&
                                   a_context != nullptr;
            const HRESULT hr = proxyPath
                ? real(a_adapter, a_driverType, a_software, a_flags, wantedLevels, wantedLevelCount,
                      a_sdkVersion, nullptr, nullptr, a_device, a_featureLevel, a_context)
                : real(a_adapter, a_driverType, a_software, a_flags, wantedLevels, wantedLevelCount,
                      a_sdkVersion, effectiveDesc, a_swapChain, a_device, a_featureLevel, a_context);
            if (FAILED(hr)) {
                Core::FailOpen::Trip(
                    "device-create-failed", "real D3D11CreateDeviceAndSwapChain returned a failure HRESULT");
                return hr;
            }
            if (proxyPath) {
                IDXGISwapChain* proxyChain = nullptr;
                GUARD_BEGIN
                    if (*a_device && *a_context &&
                        !Platform::PresentProxy::TakeOver(*a_device, *a_context, *effectiveDesc, &proxyChain)) {
                        proxyChain = nullptr;
                    }
                GUARD_END("d3dhook.create.proxy")
                if (proxyChain != nullptr) {
                    *a_swapChain = proxyChain;
                } else {
                    HRESULT chainHr = E_FAIL;
                    GUARD_BEGIN
                        chainHr = CreateOwnSwapChain(*a_device, *effectiveDesc, a_swapChain);
                    GUARD_END("d3dhook.create.fallbackchain")
                    if (FAILED(chainHr) || *a_swapChain == nullptr) {
                        Core::FailOpen::Trip("proxy-fallback-chain-failed",
                            "the proxy refused AND the game's own swap chain could not be created from the factory");
                        if (*a_context) { (*a_context)->Release(); *a_context = nullptr; }
                        if (*a_device) { (*a_device)->Release(); *a_device = nullptr; }
                        *a_swapChain = nullptr;
                        return FAILED(chainHr) ? chainHr : E_FAIL;
                    }
                    logger::warn("[D3DHook] present proxy fell back: the game presents its own D3D11 swap chain this session");
                }
            }

            if (a_device && *a_device && a_swapChain && *a_swapChain && a_context && *a_context) {
                GUARD_BEGIN
                    Platform::Streamline::NoteDeviceCreated(*a_device);
                    Platform::Streamline::ObserveAaDevice(*a_device);

                    Telemetry::NoteDevice(*a_device);

                    const HWND hwnd = a_swapChainDesc ? a_swapChainDesc->OutputWindow : nullptr;
                    const auto gen = Core::DeviceObjects::NoteDeviceCreated(
                        reentrancy.IsOutermost(), *a_device, *a_swapChain, *a_context, hwnd);
                    logger::info(
                        "[D3DHook] device created, gen={}, outermost={}", gen.id, gen.isOutermostCall);
                    {
                        const auto created = (*a_device)->GetFeatureLevel();
                        const auto requestedFirst = (a_featureLevels != nullptr && a_featureLevelCount > 0) ? a_featureLevels[0] : D3D_FEATURE_LEVEL{};
                        logger::info("[D3DHook] feature level: created {:#x} (the game asked for {} level(s), first {:#x}{}) — compute UAV slots {}",
                            static_cast<unsigned>(created), a_featureLevelCount, static_cast<unsigned>(requestedFirst),
                            offeredOurLevels ? ", so we offered 11_1 first (FSR needs nine UAV slots)" : "",
                            created >= D3D_FEATURE_LEVEL_11_1 ? 64 : 8);
                    }
                    if (gen.isOutermostCall) {
                        ArmSwapchainHooks(*a_swapChain);
                        LogSwapChainCensus(*a_swapChain);
                        Platform::HdrDisplay::Observe(*a_swapChain);
                        Core::DeviceObjects::ReleaseSizedViews();
                        UI::Menu::GetSingleton().HandleDeviceEdge(hwnd, *a_device, *a_context);
                        if (UI::Menu::GetSingleton().IsInitialized()) {
                            Hooks::WndProcHook::Install(hwnd);
                        } else {
                            logger::warn("[D3DHook] WndProc install skipped — canvas failed to initialize");
                        }
                    }
                GUARD_END("d3dhook.create.post")
            }

            return hr;
        }
    }

    bool Install()
    {
        void* original = PatchIAT(
            "d3d11.dll", "D3D11CreateDeviceAndSwapChain", reinterpret_cast<void*>(&hk_CreateDeviceAndSwapChain));
        if (!original) {
            Core::FailOpen::Trip("device-hook-install",
                "IAT entry not found for d3d11.dll!D3D11CreateDeviceAndSwapChain -- canvas cannot exist this session");
            return false;
        }

        g_realCreateDevice.store(
            reinterpret_cast<PFN_D3D11CreateDeviceAndSwapChain>(original), std::memory_order_release);
        GUARD_BEGIN
            logger::info("[D3DHook] device-creation hook installed");
        GUARD_END("d3dhook.install.log")
        return true;
    }
}
