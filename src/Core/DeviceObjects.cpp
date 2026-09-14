#include "Core/DeviceObjects.h"

#include <atomic>
#include <mutex>

#include <d3d11.h>
#include <dxgi.h>

namespace Core::DeviceObjects
{
    namespace
    {
        thread_local int t_reentrancyDepth = 0;

        std::atomic<std::uint64_t> g_generationCounter{ 0 };

        std::mutex g_activeMutex;
        Snapshot g_active;

        std::mutex g_sizedMutex;
        ID3D11RenderTargetView* g_backbufferRtv = nullptr;
        IDXGISwapChain* g_rtvSource = nullptr;
    }

    ReentrancyGuard::ReentrancyGuard() noexcept :
        isOutermost_(t_reentrancyDepth == 0)
    {
        ++t_reentrancyDepth;
    }

    ReentrancyGuard::~ReentrancyGuard() noexcept
    {
        --t_reentrancyDepth;
    }

    Generation NoteDeviceCreated(
        bool a_isOutermostCall,
        ID3D11Device* a_device,
        IDXGISwapChain* a_swapChain,
        ID3D11DeviceContext* a_context,
        HWND a_hwnd) noexcept
    {
        const std::uint64_t id = g_generationCounter.fetch_add(1, std::memory_order_acq_rel) + 1;

        if (a_isOutermostCall) {
            try {
                std::scoped_lock lock(g_activeMutex);
                g_active.device = a_device;
                g_active.context = a_context;
                g_active.swapChain = a_swapChain;
                g_active.hwnd = a_hwnd;
                g_active.generation = id;
            } catch (...) {
            }
        }

        return Generation{ id, a_isOutermostCall };
    }

    Snapshot Current() noexcept
    {
        try {
            std::scoped_lock lock(g_activeMutex);
            return g_active;
        } catch (...) {
            return Snapshot{};
        }
    }

    ID3D11RenderTargetView* AcquireBackbufferRTV(IDXGISwapChain* a_swapChain) noexcept
    {
        if (!a_swapChain) {
            return nullptr;
        }
        try {
            std::scoped_lock lock(g_sizedMutex);

            if (g_backbufferRtv && g_rtvSource == a_swapChain) {
                return g_backbufferRtv;
            }
            if (g_backbufferRtv) {
                g_backbufferRtv->Release();
                g_backbufferRtv = nullptr;
                g_rtvSource = nullptr;
            }

            ID3D11Texture2D* backbuffer = nullptr;
            HRESULT hr = a_swapChain->GetBuffer(0, IID_PPV_ARGS(&backbuffer));
            if (FAILED(hr) || !backbuffer) {
                static std::atomic<int> s_logCount{ 0 };
                if (s_logCount.fetch_add(1, std::memory_order_relaxed) < 3) {
                    logger::error("[DeviceObjects] GetBuffer(0) failed (hr={:#x}) — canvas skips drawing",
                        static_cast<std::uint32_t>(hr));
                }
                return nullptr;
            }

            ID3D11Device* device = nullptr;
            backbuffer->GetDevice(&device);
            ID3D11RenderTargetView* rtv = nullptr;
            hr = device ? device->CreateRenderTargetView(backbuffer, nullptr, &rtv) : E_UNEXPECTED;
            if (device) {
                device->Release();
            }
            backbuffer->Release();

            if (FAILED(hr) || !rtv) {
                static std::atomic<int> s_logCount2{ 0 };
                if (s_logCount2.fetch_add(1, std::memory_order_relaxed) < 3) {
                    logger::error(
                        "[DeviceObjects] CreateRenderTargetView failed (hr={:#x}) — canvas skips drawing",
                        static_cast<std::uint32_t>(hr));
                }
                return nullptr;
            }

            g_backbufferRtv = rtv;
            g_rtvSource = a_swapChain;
            logger::info("[DeviceObjects] backbuffer RTV created, swapchain={}",
                static_cast<void*>(a_swapChain));
            return g_backbufferRtv;
        } catch (...) {
            return nullptr;
        }
    }

    void ReleaseSizedViews() noexcept
    {
        try {
            std::scoped_lock lock(g_sizedMutex);
            if (g_backbufferRtv) {
                g_backbufferRtv->Release();
                g_backbufferRtv = nullptr;
                g_rtvSource = nullptr;
                logger::info("[DeviceObjects] sized views released");
            }
        } catch (...) {
        }
    }
}
