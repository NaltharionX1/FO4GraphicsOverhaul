// SPDX-License-Identifier: GPL-3.0-or-later
// Portions ported from dlss5-bridge, Copyright (c) 2026 NIGos, MIT License.

#include "PCH.h"

#include "Platform/D3D12Sidecar.h"

#include <atomic>
#include <mutex>
#include <cstring>
#include <cwchar>

namespace
{
    using namespace Platform::D3D12Sidecar;

    constexpr int kFramesInFlight = 12;
    constexpr std::uint32_t kTimestampRing = 4;

    struct TimestampSlot
    {
        std::uint64_t f12{ 0 };
        bool recorded{ false };
    };

    struct TimestampLane
    {
        TimestampSlot slots[kTimestampRing]{};
        std::uint32_t next{ 0 };
        std::uint32_t recording{ ~0U };
    };

    struct State
    {
        bool open{ false };
        std::atomic<bool> disabled{ false };
        bool unavailable{ false };
        char reason[160]{};
        std::mutex latchMutex;

        ID3D11Device* device11{ nullptr };
        ID3D11DeviceContext4* context4{ nullptr };
        ID3D11Multithread* multithread{ nullptr };
        ID3D11Fence* gpuFence11{ nullptr };
        ID3D11Fence* gameFence11{ nullptr };

        ID3D12Device* device12{ nullptr };
        ID3D12CommandQueue* queue{ nullptr };
        ID3D12CommandAllocator* allocators[kFramesInFlight]{};
        std::uint64_t allocatorFence[kFramesInFlight]{};
        int slot{ 0 };
        ID3D12GraphicsCommandList* list{ nullptr };
        ID3D12Fence* gpuFence{ nullptr };
        ID3D12Fence* gameFence{ nullptr };
        HANDLE fenceEvent{ nullptr };
        ID3D12QueryHeap* tsHeap{ nullptr };
        ID3D12Resource* tsReadback{ nullptr };
        std::uint64_t tsFrequency{ 0 };
        bool tsUnavailable{ false };
        TimestampLane tsLanes[kTimestampLanes]{};
        std::atomic<std::uint64_t> gpuValue{ 0 };
        std::atomic<std::uint64_t> gameValue{ 0 };
        std::mutex queueMutex;
        std::mutex gameMutex;
        std::atomic<std::uint64_t> lastGpuSignal{ 0 };
        std::atomic<std::uint64_t> lastGameSignal{ 0 };
        std::atomic<std::uint64_t> lastGpuWait{ 0 };
        std::atomic<std::uint64_t> lastGameWait{ 0 };

        std::atomic<std::uint64_t> pendingOut{ 0 };
        std::atomic<std::uint64_t> lastCompleted{ 0 };
        std::atomic<ULONGLONG> pendingSince{ 0 };
        std::atomic<std::uint64_t> forcedF12{ 0 };
        struct ClosedSession
        {
            ID3D12Device* device{ nullptr };
            bool retired{ false };
        };
        ClosedSession closedSessions[8]{};
        std::uint32_t closedNext{ 0 };
        std::atomic<bool> rearmRequested{ false };

        AdapterIdentity adapter{};
        std::uint8_t preferredRoute{ 0 };
        bool routeRefusalLogged{ false };
    };

    State g_state;

    [[nodiscard]] std::string Narrow(const wchar_t* a_text)
    {
        if (a_text == nullptr || *a_text == L'\0') {
            return {};
        }
        const int needed = ::WideCharToMultiByte(CP_UTF8, 0, a_text, -1, nullptr, 0, nullptr, nullptr);
        if (needed <= 1) {
            return {};
        }
        std::string out(static_cast<std::size_t>(needed - 1), '\0');
        ::WideCharToMultiByte(CP_UTF8, 0, a_text, -1, out.data(), needed, nullptr, nullptr);
        return out;
    }

    template <class T>
    void SafeRelease(T*& a_ptr) noexcept
    {
        if (a_ptr != nullptr) {
            a_ptr->Release();
            a_ptr = nullptr;
        }
    }

    int CaptureCode(unsigned long a_code, unsigned long* a_out) noexcept
    {
        *a_out = a_code;
        return EXCEPTION_EXECUTE_HANDLER;
    }

    HRESULT SehCreateDevice(IDXGIAdapter* a_adapter, ID3D12Device** a_out, unsigned long* a_code) noexcept
    {
        __try {
            return D3D12CreateDevice(a_adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(a_out));
        } __except (CaptureCode(GetExceptionCode(), a_code)) {
            return E_FAIL;
        }
    }

    [[nodiscard]] bool Check(HRESULT a_hr, const char* a_what) noexcept
    {
        if (SUCCEEDED(a_hr)) {
            return true;
        }
        static std::atomic<int> s_logged{ 0 };
        if (s_logged.fetch_add(1, std::memory_order_relaxed) < 5) {
            logger::error("[Sidecar] {} failed ({:#010x}); the session stops rather than hand out a token that can "
                          "never complete", a_what, static_cast<unsigned>(a_hr));
        }
        Disable("a fence signal or wait failed");
        return false;
    }

    [[nodiscard]] HRESULT CreateSharedFence(ID3D11Device* a_device, const wchar_t* a_name,
        ID3D12Fence*& a_out12, ID3D11Fence*& a_out11) noexcept
    {
        HANDLE handle = nullptr;
        HRESULT hr = g_state.device12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&a_out12));
        if (SUCCEEDED(hr)) {
            hr = g_state.device12->CreateSharedHandle(a_out12, nullptr, GENERIC_ALL, nullptr, &handle);
        }
        ID3D11Device5* device5 = nullptr;
        if (SUCCEEDED(hr) && SUCCEEDED(a_device->QueryInterface(IID_PPV_ARGS(&device5))) && device5 != nullptr) {
            hr = device5->OpenSharedFence(handle, IID_PPV_ARGS(&a_out11));
            device5->Release();
        } else if (SUCCEEDED(hr)) {
            hr = E_NOINTERFACE;
        }
        if (handle != nullptr) {
            ::CloseHandle(handle);
        }
        if (SUCCEEDED(hr) && a_out11 == nullptr) {
            hr = E_FAIL;
        }
        if (SUCCEEDED(hr)) {
            a_out12->SetName(a_name);
        }
        return hr;
    }

    [[nodiscard]] bool CpuWait(ID3D12Fence* a_fence, std::uint64_t a_value, unsigned long a_timeoutMs) noexcept
    {
        if (a_fence == nullptr || g_state.fenceEvent == nullptr) {
            return false;
        }
        if (a_fence->GetCompletedValue() >= a_value) {
            return true;
        }
        if (FAILED(a_fence->SetEventOnCompletion(a_value, g_state.fenceEvent))) {
            return false;
        }
        if (::WaitForSingleObject(g_state.fenceEvent, a_timeoutMs) != WAIT_OBJECT_0) {
            return false;
        }
        return a_fence->GetCompletedValue() >= a_value;
    }

    void ReleaseSession() noexcept;

    void AbandonSession() noexcept
    {
        for (int i = 0; i < kFramesInFlight; ++i) {
            g_state.allocators[i] = nullptr;
            g_state.allocatorFence[i] = 0;
        }
        g_state.list = nullptr;
        g_state.queue = nullptr;
        g_state.gpuFence = nullptr;
        g_state.gameFence = nullptr;
        g_state.tsHeap = nullptr;
        g_state.tsReadback = nullptr;
        g_state.device12 = nullptr;
        ReleaseSession();
    }

    void ReleaseSession() noexcept
    {
        for (int i = 0; i < kFramesInFlight; ++i) {
            SafeRelease(g_state.allocators[i]);
            g_state.allocatorFence[i] = 0;
        }
        SafeRelease(g_state.list);
        SafeRelease(g_state.queue);
        SafeRelease(g_state.gpuFence11);
        SafeRelease(g_state.gameFence11);
        SafeRelease(g_state.gpuFence);
        SafeRelease(g_state.gameFence);
        SafeRelease(g_state.tsHeap);
        SafeRelease(g_state.tsReadback);
        g_state.tsFrequency = 0;
        g_state.tsUnavailable = false;
        for (auto& lane : g_state.tsLanes) lane = TimestampLane{};
        if (g_state.fenceEvent != nullptr) {
            ::CloseHandle(g_state.fenceEvent);
            g_state.fenceEvent = nullptr;
        }
        SafeRelease(g_state.multithread);
        SafeRelease(g_state.context4);
        SafeRelease(g_state.device12);
        g_state.device11 = nullptr;
        g_state.open = false;
        g_state.slot = 0;
        g_state.pendingOut = 0;
        g_state.lastCompleted = 0;
        g_state.pendingSince = 0;
        g_state.gpuValue.store(0, std::memory_order_relaxed);
        g_state.gameValue.store(0, std::memory_order_relaxed);
        g_state.lastGpuSignal.store(0, std::memory_order_relaxed);
        g_state.lastGameSignal.store(0, std::memory_order_relaxed);
        g_state.lastGpuWait.store(0, std::memory_order_relaxed);
        g_state.lastGameWait.store(0, std::memory_order_relaxed);
    }

    [[nodiscard]] bool DeviceLost() noexcept
    {
        if (g_state.gpuFence == nullptr) {
            return false;
        }
        if (g_state.gpuFence->GetCompletedValue() != UINT64_MAX) {
            return false;
        }
        const HRESULT why = g_state.device12 != nullptr ? g_state.device12->GetDeviceRemovedReason() : 0;
        const char* text = "unknown";
        switch (why) {
        case DXGI_ERROR_DEVICE_HUNG: text = "the GPU stopped responding to work submitted on it (TDR)"; break;
        case DXGI_ERROR_DEVICE_REMOVED: text = "the adapter was removed or reset underneath the process"; break;
        case DXGI_ERROR_DEVICE_RESET: text = "the device was reset after an invalid command"; break;
        case DXGI_ERROR_DRIVER_INTERNAL_ERROR: text = "the driver reported an internal error"; break;
        case DXGI_ERROR_INVALID_CALL: text = "an invalid call reached the queue (a command list that failed to record was executed)"; break;
        default: break;
        }
        logger::error("[Sidecar] the D3D12 device has been removed (reason {:#010x}: {}). Nothing can "
                      "be recovered from here; the sidecar stops and the game keeps its own frame.",
            static_cast<unsigned>(why), text);
        Disable("the D3D12 device was removed");
        return true;
    }
}

namespace Platform::D3D12Sidecar
{
    void MarkUnavailable(const char* a_reason) noexcept;

    bool Open(ID3D11Device* a_device, ID3D11DeviceContext* a_context) noexcept
    {
        if (a_device == nullptr || a_context == nullptr) {
            return false;
        }
        if (g_state.disabled) {
            return false;
        }
        if (g_state.open) {
            if (g_state.device11 == a_device) {
                return true;
            }
            logger::info("[Sidecar] the game's D3D11 device changed; reopening the sidecar on the new one");
            Close();
        }
        try {
            if (a_context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) {
                MarkUnavailable("the render context is deferred (the cross-API fence needs the immediate context)");
                return false;
            }

            IDXGIDevice* dxgiDevice = nullptr;
            IDXGIAdapter* adapter = nullptr;
            if (SUCCEEDED(a_device->QueryInterface(IID_PPV_ARGS(&dxgiDevice))) && dxgiDevice != nullptr) {
                dxgiDevice->GetAdapter(&adapter);
                dxgiDevice->Release();
            }
            if (adapter != nullptr) {
                IDXGIAdapter1* adapter1 = nullptr;
                if (SUCCEEDED(adapter->QueryInterface(IID_PPV_ARGS(&adapter1))) && adapter1 != nullptr) {
                    DXGI_ADAPTER_DESC1 desc{};
                    if (SUCCEEDED(adapter1->GetDesc1(&desc))) {
                        g_state.adapter.vendorId = desc.VendorId;
                        g_state.adapter.deviceId = desc.DeviceId;
                        g_state.adapter.luid = desc.AdapterLuid;
                        ::wcsncpy_s(g_state.adapter.description, desc.Description, _TRUNCATE);
                    }
                    adapter1->Release();
                }
            }
            logger::info("[Sidecar] opening the D3D12 session on {} (vendor {:#06x} device {:#06x}, LUID {:08x}:{:08x})",
                Narrow(g_state.adapter.description),
                g_state.adapter.vendorId, g_state.adapter.deviceId,
                static_cast<unsigned>(g_state.adapter.luid.HighPart),
                static_cast<unsigned>(g_state.adapter.luid.LowPart));

            unsigned long code = 0;
            HRESULT hr = SehCreateDevice(adapter, &g_state.device12, &code);
            if (code != 0 && adapter != nullptr) {
                logger::warn("[Sidecar] D3D12CreateDevice raised {:#010x} on the game's adapter; trying once on the default adapter", code);
                unsigned long code2 = 0;
                SafeRelease(g_state.device12);
                hr = SehCreateDevice(nullptr, &g_state.device12, &code2);
                code = code2;
            }
            if (adapter != nullptr) {
                adapter->Release();
                adapter = nullptr;
            }
            if (code != 0) {
                logger::error("[Sidecar] D3D12CreateDevice raised {:#010x}; the sidecar cannot exist here", code);
                MarkUnavailable("no usable DirectX 12 device on this adapter (D3D12CreateDevice raised an exception)");
                ReleaseSession();
                return false;
            }
            if (FAILED(hr) || g_state.device12 == nullptr) {
                logger::error("[Sidecar] D3D12CreateDevice failed {:#010x}", static_cast<unsigned>(hr));
                char reason[160]{};
                std::snprintf(reason, sizeof(reason), "no usable DirectX 12 device on this adapter (D3D12CreateDevice failed 0x%08lx)",
                    static_cast<unsigned long>(hr));
                MarkUnavailable(reason);
                ReleaseSession();
                return false;
            }

            D3D12_COMMAND_QUEUE_DESC queueDesc{};
            queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
            queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
            bool highPriority = true;
            if (FAILED(g_state.device12->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&g_state.queue)))) {
                highPriority = false;
                queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
                if (FAILED(g_state.device12->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&g_state.queue)))) {
                    MarkUnavailable("the D3D12 command queue could not be created");
                    ReleaseSession();
                    return false;
                }
            }
            for (int i = 0; i < kFramesInFlight; ++i) {
                if (FAILED(g_state.device12->CreateCommandAllocator(
                        D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_state.allocators[i])))) {
                    MarkUnavailable("a D3D12 command allocator could not be created");
                    ReleaseSession();
                    return false;
                }
            }
            if (FAILED(g_state.device12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                    g_state.allocators[0], nullptr, IID_PPV_ARGS(&g_state.list)))) {
                MarkUnavailable("the D3D12 command list could not be created");
                ReleaseSession();
                return false;
            }
            g_state.list->Close();
            g_state.fenceEvent = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (g_state.fenceEvent == nullptr) {
                MarkUnavailable("the fence event could not be created");
                ReleaseSession();
                return false;
            }

            hr = CreateSharedFence(a_device, L"FO4GraphicsOverhaul F12 (the sidecar queue signals it)",
                g_state.gpuFence, g_state.gpuFence11);
            if (SUCCEEDED(hr)) {
                hr = CreateSharedFence(a_device, L"FO4GraphicsOverhaul F11 (the game's context signals it)",
                    g_state.gameFence, g_state.gameFence11);
            }
            if (FAILED(hr)) {
                logger::error("[Sidecar] shared fence setup failed {:#010x} (CreateFence / CreateSharedHandle / "
                              "ID3D11Device5::OpenSharedFence)", static_cast<unsigned>(hr));
                MarkUnavailable("a shared fence could not be opened on the game's D3D11 device");
                ReleaseSession();
                return false;
            }
            if (FAILED(a_context->QueryInterface(IID_PPV_ARGS(&g_state.context4))) || g_state.context4 == nullptr) {
                MarkUnavailable("ID3D11DeviceContext4 is unavailable on the game's context");
                ReleaseSession();
                return false;
            }
            if (SUCCEEDED(a_context->QueryInterface(IID_PPV_ARGS(&g_state.multithread))) && g_state.multithread != nullptr) {
                logger::info("[Sidecar] multithread protection: {}",
                    g_state.multithread->GetMultithreadProtected() ? "on (the frame sequence takes the lock)" : "off");
            }
            g_state.queue->SetName(L"FO4GraphicsOverhaul sidecar queue");
            g_state.list->SetName(L"FO4GraphicsOverhaul sidecar list");
            {
                constexpr std::uint32_t queries = 2U * kTimestampLanes * kTimestampRing;
                D3D12_QUERY_HEAP_DESC heap{};
                heap.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
                heap.Count = queries;
                D3D12_HEAP_PROPERTIES readbackHeap{};
                readbackHeap.Type = D3D12_HEAP_TYPE_READBACK;
                D3D12_RESOURCE_DESC buffer{};
                buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                buffer.Width = static_cast<UINT64>(queries) * sizeof(std::uint64_t);
                buffer.Height = 1;
                buffer.DepthOrArraySize = 1;
                buffer.MipLevels = 1;
                buffer.SampleDesc.Count = 1;
                buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                if (FAILED(g_state.device12->CreateQueryHeap(&heap, IID_PPV_ARGS(&g_state.tsHeap))) ||
                    FAILED(g_state.device12->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &buffer,
                        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g_state.tsReadback))) ||
                    FAILED(g_state.queue->GetTimestampFrequency(&g_state.tsFrequency)) || g_state.tsFrequency == 0) {
                    SafeRelease(g_state.tsHeap);
                    SafeRelease(g_state.tsReadback);
                    g_state.tsFrequency = 0;
                    g_state.tsUnavailable = true;
                    logger::warn("[Sidecar] GPU timestamps unavailable (the query heap, its readback buffer or the queue's frequency was refused); the Debug tab's GPU figures stay blank");
                } else {
                    g_state.tsReadback->SetName(L"FO4GraphicsOverhaul sidecar timestamps");
                    logger::info("[Sidecar] GPU timestamps armed: {} lanes x {} brackets, {} ticks/s", kTimestampLanes, kTimestampRing, g_state.tsFrequency);
                }
            }

            g_state.device11 = a_device;
            g_state.open = true;
            g_state.gpuValue.store(0, std::memory_order_relaxed);
            g_state.gameValue.store(0, std::memory_order_relaxed);
            g_state.lastGpuSignal.store(0, std::memory_order_relaxed);
            g_state.lastGameSignal.store(0, std::memory_order_relaxed);
            g_state.lastGpuWait.store(0, std::memory_order_relaxed);
            g_state.lastGameWait.store(0, std::memory_order_relaxed);
            logger::info("[Sidecar] session open: queue={} at {} GPU priority, list={} F12={} (queue-signalled) F11={} "
                         "(context-signalled) ({} frames in flight)",
                fmt::ptr(g_state.queue),
                highPriority ? "HIGH" : "NORMAL (HIGH was refused)", fmt::ptr(g_state.list),
                fmt::ptr(g_state.gpuFence), fmt::ptr(g_state.gameFence), kFramesInFlight);
            return true;
        } catch (...) {
            MarkUnavailable("C++ exception while opening the D3D12 session");
            ReleaseSession();
            return false;
        }
    }

    [[nodiscard]] bool Drain(bool a_contextTailRequired) noexcept;

    namespace
    {
        void RecordClosedSession(ID3D12Device* a_device, bool a_retired) noexcept
        {
            auto& slot = g_state.closedSessions[g_state.closedNext % static_cast<std::uint32_t>(std::size(g_state.closedSessions))];
            slot.device = a_device;
            slot.retired = a_retired;
            ++g_state.closedNext;
        }
    }

    void Close() noexcept
    {
        if (!g_state.open) {
            return;
        }
        ID3D12Device* const closing = g_state.device12;
        const std::uint64_t forced = g_state.forcedF12.load(std::memory_order_relaxed);
        if (g_state.gpuFence != nullptr && !DeviceRemoved() && !Drain(forced == 0)) {
            logger::warn("[Sidecar] the session is closing on a live device that did not prove retirement{}: its queue, fences, "
                         "command list and {} allocators are RETAINED (leaked), not released — a leak is the honest outcome",
                forced != 0 ? fmt::format(" (F12 was forced to {})", forced) : "", kFramesInFlight);
            AbandonSession();
            RecordClosedSession(closing, false);
        } else {
            ReleaseSession();
            RecordClosedSession(closing, true);
        }
        g_state.forcedF12.store(0, std::memory_order_relaxed);
        logger::info("[Sidecar] session closed");
    }

    bool IsOpen() noexcept { return g_state.open; }
    ID3D11Device* BoundDevice() noexcept { return g_state.device11; }
    ID3D12Device* Device() noexcept { return g_state.device12; }
    ID3D12CommandQueue* Queue() noexcept { return g_state.queue; }
    AdapterIdentity Adapter() noexcept { return g_state.adapter; }
    bool Disabled() noexcept { return g_state.disabled; }
    const char* DisableReason() noexcept { return g_state.reason; }

    void DiscardTimestamps() noexcept
    {
        for (auto& lane : g_state.tsLanes) {
            for (auto& slot : lane.slots) slot = TimestampSlot{};
            lane.recording = ~0U;
        }
    }

    void Disable(const char* a_reason) noexcept
    {
        DiscardTimestamps();
        const std::scoped_lock latch(g_state.latchMutex);
        if (!g_state.disabled) {
            std::snprintf(g_state.reason, sizeof(g_state.reason), "%s", a_reason != nullptr ? a_reason : "unknown");
            g_state.disabled = true;
            logger::warn("[Sidecar] DISABLED: {}. Every DirectX 12 feature is off from here (the DirectX 12 DLSS path, DLSS Frame "
                         "Generation, DLSS 5 neural rendering, DirectX 12 presentation){}", g_state.reason,
                g_state.unavailable ? " for the whole session (the sidecar cannot be built on this machine)."
                                    : " until a staged recreate or 'Reset & retry' re-arms the sidecar.");
        }
        const std::uint64_t pending = g_state.pendingOut.load(std::memory_order_relaxed);
        if (g_state.gpuFence != nullptr && pending != 0 &&
            g_state.gpuFence->GetCompletedValue() != UINT64_MAX &&
            g_state.gpuFence->GetCompletedValue() < pending) {
            std::uint64_t forced = g_state.forcedF12.load(std::memory_order_relaxed);
            while (pending > forced && !g_state.forcedF12.compare_exchange_weak(forced, pending, std::memory_order_acq_rel)) {}
            const HRESULT hr = g_state.gpuFence->Signal(pending);
            g_state.pendingOut.store(0, std::memory_order_relaxed);
            if (FAILED(hr)) {
                logger::error("[Sidecar] the rescue Signal of F12 to {} FAILED ({:#010x}): the game's wait on it is not released", pending,
                    static_cast<unsigned>(hr));
            } else {
                logger::warn("[Sidecar] F12 forced to {} from the CPU to release the game's wait — that value proves nothing: every "
                             "release of what may still reference the stalled work needs the GPU's own proof ('Reset & retry filter' runs it)",
                    pending);
            }
        }
    }

    bool DeviceRemoved() noexcept
    {
        if (g_state.gpuFence == nullptr) {
            return false;
        }
        if (g_state.gpuFence->GetCompletedValue() == UINT64_MAX) {
            return true;
        }
        return g_state.device12 != nullptr && FAILED(g_state.device12->GetDeviceRemovedReason());
    }

    bool Quarantined() noexcept
    {
        return g_state.disabled && g_state.forcedF12.load(std::memory_order_relaxed) != 0 && !DeviceRemoved();
    }

    std::uint64_t ForcedValue() noexcept { return g_state.forcedF12.load(std::memory_order_relaxed); }

    bool ProveRetired() noexcept
    {
        if (!g_state.open || g_state.queue == nullptr || g_state.gpuFence == nullptr) {
            return true;
        }
        if (DeviceRemoved()) {
            return true;
        }
        return Drain(true);
    }

    bool SessionRetired(ID3D12Device* a_device) noexcept
    {
        if (a_device == nullptr) {
            return false;
        }
        for (const auto& slot : g_state.closedSessions) {
            if (slot.device == a_device) {
                return slot.retired;
            }
        }
        return false;
    }

    void RequestRearm() noexcept { g_state.rearmRequested.store(true, std::memory_order_relaxed); }
    bool TakeRearmRequest() noexcept { return g_state.rearmRequested.exchange(false, std::memory_order_relaxed); }

    bool Unavailable() noexcept { return g_state.unavailable; }

    void MarkUnavailable(const char* a_reason) noexcept
    {
        g_state.unavailable = true;
        Disable(a_reason);
        logger::error("[Sidecar] NO DIRECTX 12 ON THIS MACHINE ({}): the sidecar cannot be built, so every DirectX 12 feature "
                      "stays off for the whole session — DLSS is unavailable (it falls forward to FSR); the game presents "
                      "its own swap chain; DLSS Frame Generation and DLSS 5 neural rendering are off.", g_state.reason);
    }

    void Rearm() noexcept
    {
        if (g_state.unavailable) {
            logger::info("[Sidecar] retry refused: {}", g_state.reason);
            return;
        }
        if (g_state.disabled) {
            const std::uint64_t forced = g_state.forcedF12.load(std::memory_order_relaxed);
            if (forced != 0 && !DeviceRemoved()) {
                logger::info("[Sidecar] retry: proving the sidecar queue retired the work stalled behind the forced F12 value {} (up to 10 s)", forced);
                if (!Drain(false)) {
                    logger::warn("[Sidecar] retry REFUSED: the GPU has not proven retirement of the work behind F12 {} — everything tied "
                                 "to it stays retained (a leak, never a use-after-free); the session stays disabled, try again later",
                        forced);
                    return;
                }
                logger::info("[Sidecar] the GPU retired everything queued before the forced release: the forced value {} is void", forced);
            }
            std::uint64_t seen = forced;
            if (!g_state.forcedF12.compare_exchange_strong(seen, 0, std::memory_order_acq_rel)) {
                logger::warn("[Sidecar] retry REFUSED: another forced release ({}) landed while the proof for {} ran; the session stays "
                             "disabled, try again", seen, forced);
                return;
            }
            DiscardTimestamps();
            {
                const std::scoped_lock latch(g_state.latchMutex);
                g_state.disabled = false;
                g_state.reason[0] = '\0';
            }
            logger::info("[Sidecar] latch cleared by request; the next use reopens the session");
        }
    }

    namespace
    {
        [[nodiscard]] HRESULT TryRouteA(ID3D11Device1* a_device1, SharedTexture& a_out) noexcept
        {
            D3D12_HEAP_PROPERTIES heap{};
            heap.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            desc.Width = a_out.width;
            desc.Height = a_out.height;
            desc.DepthOrArraySize = 1;
            desc.MipLevels = 1;
            desc.Format = a_out.format;
            desc.SampleDesc.Count = 1;
            desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS |
                         (a_out.uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE);
            HRESULT hr = g_state.device12->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED, &desc,
                D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&a_out.d3d12));
            if (SUCCEEDED(hr)) {
                hr = g_state.device12->CreateSharedHandle(a_out.d3d12, nullptr, GENERIC_ALL, nullptr, &a_out.handle);
            }
            if (SUCCEEDED(hr)) {
                hr = a_device1->OpenSharedResource1(a_out.handle, IID_PPV_ARGS(&a_out.d3d11));
            }
            if (SUCCEEDED(hr) && a_out.d3d11 != nullptr) {
                a_out.route = 1;
                return S_OK;
            }
            SafeRelease(a_out.d3d11);
            SafeRelease(a_out.d3d12);
            if (a_out.handle != nullptr) {
                ::CloseHandle(a_out.handle);
                a_out.handle = nullptr;
            }
            return FAILED(hr) ? hr : E_FAIL;
        }

        [[nodiscard]] HRESULT TryRouteB(SharedTexture& a_out) noexcept
        {
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = a_out.width;
            desc.Height = a_out.height;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = a_out.format;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | (a_out.uav ? D3D11_BIND_UNORDERED_ACCESS : 0U);
            desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;
            HRESULT hr = g_state.device11->CreateTexture2D(&desc, nullptr, &a_out.d3d11);
            IDXGIResource1* dxgiResource = nullptr;
            if (SUCCEEDED(hr)) {
                hr = a_out.d3d11->QueryInterface(IID_PPV_ARGS(&dxgiResource));
            }
            if (SUCCEEDED(hr) && dxgiResource != nullptr) {
                hr = dxgiResource->CreateSharedHandle(nullptr,
                    DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &a_out.handle);
                dxgiResource->Release();
            }
            if (SUCCEEDED(hr)) {
                hr = g_state.device12->OpenSharedHandle(a_out.handle, IID_PPV_ARGS(&a_out.d3d12));
            }
            if (SUCCEEDED(hr) && a_out.d3d12 != nullptr) {
                a_out.route = 2;
                return S_OK;
            }
            SafeRelease(a_out.d3d11);
            SafeRelease(a_out.d3d12);
            if (a_out.handle != nullptr) {
                ::CloseHandle(a_out.handle);
                a_out.handle = nullptr;
            }
            return FAILED(hr) ? hr : E_FAIL;
        }
    }

    bool CreateShared(SharedTexture& a_out, const char* a_name, std::uint32_t a_width,
        std::uint32_t a_height, DXGI_FORMAT a_format, bool a_uav) noexcept
    {
        ReleaseShared(a_out);
        if (!g_state.open || g_state.device11 == nullptr) {
            return false;
        }
        a_out.name = a_name != nullptr ? a_name : "";
        a_out.width = a_width;
        a_out.height = a_height;
        a_out.format = a_format;
        a_out.uav = a_uav;

        ID3D11Device1* device1 = nullptr;
        if (FAILED(g_state.device11->QueryInterface(IID_PPV_ARGS(&device1))) || device1 == nullptr) {
            logger::error("[Sidecar] ID3D11Device1 unavailable; shared textures need OpenSharedResource1");
            return false;
        }

        HRESULT hrA = S_OK;
        HRESULT hrB = S_OK;
        bool made = false;
        if (g_state.preferredRoute == 2) {
            made = SUCCEEDED(hrB = TryRouteB(a_out)) || SUCCEEDED(hrA = TryRouteA(device1, a_out));
        } else {
            made = SUCCEEDED(hrA = TryRouteA(device1, a_out));
            if (!made) {
                if (!g_state.routeRefusalLogged) {
                    g_state.routeRefusalLogged = true;
                    logger::info("[Sidecar] the D3D12->D3D11 sharing route is refused on this driver ({:#010x}); "
                                 "using D3D11->D3D12 for every shared texture from here on",
                        static_cast<unsigned>(hrA));
                }
                made = SUCCEEDED(hrB = TryRouteB(a_out));
            }
        }
        device1->Release();
        if (!made) {
            logger::error("[Sidecar] shared {} {}x{} fmt={}: both routes refused (A {:#010x}, B {:#010x}) - this "
                          "format cannot be shared on this driver",
                a_out.name, a_width, a_height, static_cast<int>(a_format), static_cast<unsigned>(hrA),
                static_cast<unsigned>(hrB));
            ReleaseShared(a_out);
            return false;
        }
        g_state.preferredRoute = a_out.route;
        logger::info("[Sidecar] shared {} {}x{} fmt={} via {}{}", a_out.name, a_width, a_height,
            static_cast<int>(a_format), a_out.route == 1 ? "D3D12->D3D11" : "D3D11->D3D12",
            a_uav ? " (UAV)" : "");
        return true;
    }

    void ReleaseShared(SharedTexture& a_texture) noexcept
    {
        SafeRelease(a_texture.d3d11);
        SafeRelease(a_texture.d3d12);
        if (a_texture.handle != nullptr) {
            ::CloseHandle(a_texture.handle);
            a_texture.handle = nullptr;
        }
        a_texture.route = 0;
    }

    void ForgetShared(SharedTexture& a_texture) noexcept
    {
        SafeRelease(a_texture.d3d11);
        a_texture.d3d12 = nullptr;
        if (a_texture.handle != nullptr) {
            ::CloseHandle(a_texture.handle);
            a_texture.handle = nullptr;
        }
        a_texture.route = 0;
    }

    void DescribeStall(char* a_out, std::size_t a_size, std::uint64_t a_waitingForF12) noexcept
    {
        if (a_out == nullptr || a_size == 0) {
            return;
        }
        const std::uint64_t gpuRec = g_state.lastGpuSignal.load(std::memory_order_acquire);
        const std::uint64_t gpuDone = g_state.gpuFence != nullptr ? g_state.gpuFence->GetCompletedValue() : 0;
        const std::uint64_t gameRec = g_state.lastGameSignal.load(std::memory_order_acquire);
        const std::uint64_t gameDone = g_state.gameFence != nullptr ? g_state.gameFence->GetCompletedValue() : 0;
        const std::uint64_t queueWaits = g_state.lastGpuWait.load(std::memory_order_acquire);
        const std::uint64_t contextWaits = g_state.lastGameWait.load(std::memory_order_acquire);
        const auto done = [](std::uint64_t a_completed, std::uint64_t a_value) noexcept {
            return a_value == 0 ? "none" : a_completed >= a_value ? "completed" : "NOT completed";
        };
        std::snprintf(a_out, a_size,
            "F12 (queue-signalled) completed %llu of %llu recorded, this wait is for #%llu (%s) | F11 (context-"
            "signalled) completed %llu of %llu recorded | latest RECORDED waits: the queue for F11 #%llu (%s), the "
            "game's context for F12 #%llu (%s)",
            static_cast<unsigned long long>(gpuDone), static_cast<unsigned long long>(gpuRec),
            static_cast<unsigned long long>(a_waitingForF12), done(gpuDone, a_waitingForF12),
            static_cast<unsigned long long>(gameDone), static_cast<unsigned long long>(gameRec),
            static_cast<unsigned long long>(queueWaits), done(gameDone, queueWaits),
            static_cast<unsigned long long>(contextWaits), done(gpuDone, contextWaits));
    }

    bool BeginCommands(ID3D12GraphicsCommandList*& a_list) noexcept
    {
        a_list = nullptr;
        if (!g_state.open || g_state.disabled) {
            return false;
        }
        const int slot = g_state.slot;
        const std::uint64_t retire = g_state.allocatorFence[slot];
        if (retire != 0 && g_state.gpuFence->GetCompletedValue() < retire) {
            g_state.gpuFence->SetEventOnCompletion(retire, g_state.fenceEvent);
            if (::WaitForSingleObject(g_state.fenceEvent, 2000) != WAIT_OBJECT_0 ||
                g_state.gpuFence->GetCompletedValue() < retire) {
                char why[400]{};
                DescribeStall(why, sizeof(why), retire);
                logger::error("[Sidecar] the GPU did not retire allocator slot {} within 2 s; stopping rather "
                              "than stalling the render thread — {}", slot, why);
                Disable("the GPU stopped completing sidecar work");
                return false;
            }
        }
        if (g_state.disabled) {
            return false;
        }
        if (g_state.allocators[slot] == nullptr) {
            logger::error("[Sidecar] allocator slot {} is missing; no commands recorded this frame", slot);
            return false;
        }
        const HRESULT allocatorReset = g_state.allocators[slot]->Reset();
        if (FAILED(allocatorReset)) {
            static std::atomic<std::uint32_t> reports{ 0 };
            if (reports.fetch_add(1, std::memory_order_relaxed) < 5U) {
                logger::error("[Sidecar] allocator slot {} Reset failed ({:#010x}); device reason {:#010x}, "
                              "F12 completed {} / slot retires at {}. No commands recorded this frame",
                    slot, static_cast<unsigned>(allocatorReset), static_cast<unsigned>(g_state.device12->GetDeviceRemovedReason()),
                    g_state.gpuFence->GetCompletedValue(), retire);
            }
            return false;
        }
        const HRESULT listReset = g_state.list->Reset(g_state.allocators[slot], nullptr);
        if (FAILED(listReset)) {
            static std::atomic<std::uint32_t> reports{ 0 };
            if (reports.fetch_add(1, std::memory_order_relaxed) < 5U) {
                logger::error("[Sidecar] command list Reset failed ({:#010x}) on slot {}; device reason {:#010x}, "
                              "F12 completed {} / slot retires at {}. No commands recorded this frame",
                    static_cast<unsigned>(listReset), slot, static_cast<unsigned>(g_state.device12->GetDeviceRemovedReason()),
                    g_state.gpuFence->GetCompletedValue(), retire);
            }
            return false;
        }
        a_list = g_state.list;
        return true;
    }

    ID3D12CommandList* CloseOrDrop(ID3D12GraphicsCommandList* a_list, const char* a_what) noexcept
    {
        const HRESULT hr = a_list->Close();
        if (SUCCEEDED(hr)) {
            return a_list;
        }
        static std::atomic<int> s_logged{ 0 };
        if (s_logged.fetch_add(1, std::memory_order_relaxed) < 5) {
            logger::error("[Sidecar] {} failed to close ({:#010x}): an invalid command was recorded on it; the list is "
                          "dropped, not executed, so the device survives (its work is missing from this frame)",
                a_what, static_cast<unsigned>(hr));
        }
        return nullptr;
    }

    bool ReplaceInvalidList(ID3D12GraphicsCommandList*& a_list, ID3D12CommandAllocator* a_allocator, const wchar_t* a_name) noexcept
    {
        SafeRelease(a_list);
        if (g_state.device12 == nullptr || a_allocator == nullptr ||
            FAILED(g_state.device12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, a_allocator, nullptr, IID_PPV_ARGS(&a_list)))) {
            a_list = nullptr;
            Disable("a command list whose Close failed could not be replaced");
            return false;
        }
        if (FAILED(a_list->Close())) {
            SafeRelease(a_list);
            Disable("a replaced command list would not close: the original Close failure was the device's, not a bad command");
            return false;
        }
        a_list->SetName(a_name);
        logger::warn("[Sidecar] a command list whose Close failed was replaced (a list in that state cannot be Reset; the old object is released, "
                     "its allocator retires on the value just signalled)");
        return true;
    }

    void SettleBrackets(bool a_executed, std::uint64_t a_value) noexcept
    {
        for (auto& lane : g_state.tsLanes) {
            if (lane.recording != ~0U) {
                auto& slot = lane.slots[lane.recording];
                if (a_executed && a_value != 0) {
                    slot.f12 = a_value;
                } else {
                    slot.recorded = false;
                }
                lane.recording = ~0U;
            }
        }
    }

    std::uint64_t EndCommands(bool* a_listAccepted) noexcept
    {
        ID3D12CommandList* const closed = CloseOrDrop(g_state.list, "the sidecar's list");
        const std::uint64_t value = ExecuteAndSignal(closed);
        if (a_listAccepted != nullptr) *a_listAccepted = closed != nullptr && value != 0 && !g_state.disabled;
        g_state.allocatorFence[g_state.slot] = value;
        if (closed == nullptr) {
            (void)ReplaceInvalidList(g_state.list, g_state.allocators[g_state.slot], L"FO4GraphicsOverhaul sidecar list");
        }
        g_state.slot = (g_state.slot + 1) % kFramesInFlight;
        SettleBrackets(closed != nullptr, value);
        return value;
    }

    std::uint64_t AbandonCommands() noexcept
    {
        ID3D12CommandList* const closed = CloseOrDrop(g_state.list, "the sidecar's list (abandoned)");
        const std::uint64_t value = ExecuteAndSignal(nullptr);
        g_state.allocatorFence[g_state.slot] = value;
        if (closed == nullptr) {
            (void)ReplaceInvalidList(g_state.list, g_state.allocators[g_state.slot], L"FO4GraphicsOverhaul sidecar list");
        }
        g_state.slot = (g_state.slot + 1) % kFramesInFlight;
        SettleBrackets(false, value);
        return value;
    }

    bool TimestampsAvailable() noexcept
    {
        return g_state.open && !g_state.disabled && !g_state.tsUnavailable && g_state.tsHeap != nullptr;
    }

    void TimestampBegin(ID3D12GraphicsCommandList* a_list, std::uint32_t a_lane) noexcept
    {
        if (a_list == nullptr || a_lane >= kTimestampLanes || !TimestampsAvailable()) {
            return;
        }
        auto& lane = g_state.tsLanes[a_lane];
        const std::uint32_t slot = lane.next;
        lane.next = (lane.next + 1U) % kTimestampRing;
        lane.slots[slot] = TimestampSlot{};
        lane.recording = slot;
        a_list->EndQuery(g_state.tsHeap, D3D12_QUERY_TYPE_TIMESTAMP, (a_lane * kTimestampRing + slot) * 2U);
    }

    void TimestampEnd(ID3D12GraphicsCommandList* a_list, std::uint32_t a_lane) noexcept
    {
        if (a_list == nullptr || a_lane >= kTimestampLanes || !TimestampsAvailable()) {
            return;
        }
        auto& lane = g_state.tsLanes[a_lane];
        if (lane.recording == ~0U) {
            return;
        }
        const std::uint32_t query = (a_lane * kTimestampRing + lane.recording) * 2U;
        a_list->EndQuery(g_state.tsHeap, D3D12_QUERY_TYPE_TIMESTAMP, query + 1U);
        a_list->ResolveQueryData(g_state.tsHeap, D3D12_QUERY_TYPE_TIMESTAMP, query, 2U, g_state.tsReadback,
            static_cast<UINT64>(query) * sizeof(std::uint64_t));
        lane.slots[lane.recording].recorded = true;
    }

    bool TimestampTake(std::uint32_t a_lane, double& a_ms) noexcept
    {
        a_ms = 0.0;
        if (a_lane >= kTimestampLanes || !TimestampsAvailable() || g_state.gpuFence == nullptr) {
            return false;
        }
        auto& lane = g_state.tsLanes[a_lane];
        const std::uint64_t completed = g_state.gpuFence->GetCompletedValue();
        for (std::uint32_t k = 0; k < kTimestampRing; ++k) {
            const std::uint32_t slot = (lane.next + k) % kTimestampRing;
            auto& entry = lane.slots[slot];
            if (!entry.recorded || entry.f12 == 0 || completed < entry.f12) {
                continue;
            }
            const std::uint32_t query = (a_lane * kTimestampRing + slot) * 2U;
            const D3D12_RANGE range{ static_cast<SIZE_T>(query) * sizeof(std::uint64_t),
                static_cast<SIZE_T>(query + 2U) * sizeof(std::uint64_t) };
            void* mapped = nullptr;
            if (FAILED(g_state.tsReadback->Map(0, &range, &mapped)) || mapped == nullptr) {
                entry.recorded = false;
                return false;
            }
            const auto* ticks = static_cast<const std::uint64_t*>(mapped) + query;
            const std::uint64_t begin = ticks[0], end = ticks[1];
            const D3D12_RANGE none{ 0, 0 };
            g_state.tsReadback->Unmap(0, &none);
            entry.recorded = false;
            if (end < begin || g_state.tsFrequency == 0) {
                return false;
            }
            a_ms = 1000.0 * static_cast<double>(end - begin) / static_cast<double>(g_state.tsFrequency);
            return true;
        }
        return false;
    }

    std::uint64_t ExecuteAndSignal(ID3D12CommandList* a_list) noexcept
    {
        try {
            const std::scoped_lock lock(g_state.queueMutex);
            if (g_state.queue == nullptr || g_state.gpuFence == nullptr) {
                return 0;
            }
            if (a_list != nullptr) {
                ID3D12CommandList* lists[]{ a_list };
                g_state.queue->ExecuteCommandLists(1, lists);
            }
            const std::uint64_t value = g_state.gpuValue.load(std::memory_order_relaxed) + 1;
            if (!Check(g_state.queue->Signal(g_state.gpuFence, value), "Signal on the sidecar queue (F12)")) {
                return 0;
            }
            g_state.gpuValue.store(value, std::memory_order_release);
            g_state.lastGpuSignal.store(value, std::memory_order_release);
            return value;
        } catch (...) {
            return 0;
        }
    }

    void WaitOnQueue(std::uint64_t a_f12Value) noexcept
    {
        if (a_f12Value != 0 && g_state.queue != nullptr && g_state.gpuFence != nullptr) {
            (void)Check(g_state.queue->Wait(g_state.gpuFence, a_f12Value), "the sidecar queue's Wait on F12");
        }
    }

    std::uint64_t SignalFromD3D11() noexcept
    {
        try {
            const std::scoped_lock lock(g_state.gameMutex);
            if (g_state.context4 == nullptr || g_state.gameFence11 == nullptr) {
                return 0;
            }
            const std::uint64_t value = g_state.gameValue.load(std::memory_order_relaxed) + 1;
            if (!Check(g_state.context4->Signal(g_state.gameFence11, value), "Signal on the game's context (F11)")) {
                return 0;
            }
            g_state.context4->Flush();
            g_state.gameValue.store(value, std::memory_order_release);
            g_state.lastGameSignal.store(value, std::memory_order_release);
            return value;
        } catch (...) {
            return 0;
        }
    }

    void WaitOnD3D12(std::uint64_t a_f11Value) noexcept
    {
        if (a_f11Value == 0 || g_state.queue == nullptr || g_state.gameFence == nullptr) {
            return;
        }
        if (Check(g_state.queue->Wait(g_state.gameFence, a_f11Value), "the sidecar queue's Wait on F11")) {
            g_state.lastGpuWait.store(a_f11Value, std::memory_order_release);
        }
    }

    bool WaitOnD3D11(std::uint64_t a_f12Value) noexcept
    {
        if (a_f12Value == 0) {
            return true;
        }
        if (g_state.context4 == nullptr || g_state.gpuFence11 == nullptr) {
            return false;
        }
        if (!Check(g_state.context4->Wait(g_state.gpuFence11, a_f12Value), "the game's context's Wait on F12")) {
            return false;
        }
        g_state.lastGameWait.store(a_f12Value, std::memory_order_release);
        g_state.pendingOut = a_f12Value;
        return true;
    }

    std::uint64_t SignalMarker() noexcept
    {
        return ExecuteAndSignal(nullptr);
    }

    bool WaitForValue(std::uint64_t a_f12Value, unsigned long a_timeoutMs) noexcept
    {
        return CpuWait(g_state.gpuFence, a_f12Value, a_timeoutMs);
    }

    [[nodiscard]] bool Drain(bool a_contextTailRequired) noexcept
    {
        if (g_state.queue == nullptr || g_state.gpuFence == nullptr) {
            return true;
        }
        std::uint64_t tail = 0;
        try {
            const std::scoped_lock lock(g_state.queueMutex);
            tail = g_state.gpuValue.load(std::memory_order_relaxed) + 1;
            if (tail <= g_state.forcedF12.load(std::memory_order_relaxed)) {
                logger::error("[Sidecar] the drain marker {} is not above the forced F12 value {}: the proof cannot be trusted", tail,
                    g_state.forcedF12.load(std::memory_order_relaxed));
                return false;
            }
            if (!Check(g_state.queue->Signal(g_state.gpuFence, tail), "the drain marker's Signal on the sidecar queue (F12)")) {
                return false;
            }
            g_state.gpuValue.store(tail, std::memory_order_release);
            g_state.lastGpuSignal.store(tail, std::memory_order_release);
        } catch (...) {
            return false;
        }
        if (!CpuWait(g_state.gpuFence, tail, 5000)) {
            logger::warn("[Sidecar] timed out draining the sidecar queue before a teardown; nothing is released this time");
            return false;
        }
        if (const std::uint64_t forcedNow = g_state.forcedF12.load(std::memory_order_acquire); tail <= forcedNow) {
            logger::error("[Sidecar] F12 was forced to {} while the drain waited on its marker {}: the proof is void, nothing is "
                          "released this time", forcedNow, tail);
            return false;
        }
        if (g_state.context4 != nullptr && g_state.gameFence != nullptr) {
            std::uint64_t gameTail = 0;
            {
                const FrameLock frameLock;
                gameTail = SignalFromD3D11();
            }
            if (gameTail == 0 || !CpuWait(g_state.gameFence, gameTail, 5000)) {
                if (a_contextTailRequired) {
                    logger::warn("[Sidecar] timed out draining the game's context before a teardown; nothing is released this time");
                    return false;
                }
                logger::warn("[Sidecar] the game's context has not caught up after the forced release (its own timeline is stuck); the "
                             "sidecar queue HAS retired everything, so the DirectX 12 side is proven and the DirectX 11 side is left to "
                             "the runtime's own deferred destruction");
            }
        }
        for (int i = 0; i < kFramesInFlight; ++i) {
            g_state.allocatorFence[i] = 0;
        }
        return true;
    }

    bool DrainGpu() noexcept { return Drain(true); }

    bool PollHealth() noexcept
    {
        if (g_state.disabled || !g_state.open || g_state.gpuFence == nullptr) {
            return false;
        }
        if (DeviceLost()) {
            return false;
        }
        const std::uint64_t done = g_state.gpuFence->GetCompletedValue();
        const ULONGLONG now = ::GetTickCount64();
        if (done != g_state.lastCompleted) {
            g_state.lastCompleted = done;
            g_state.pendingSince = now;
            if (g_state.pendingOut != 0 && done >= g_state.pendingOut) {
                g_state.pendingOut = 0;
            }
            return true;
        }
        if (g_state.pendingOut == 0 || done >= g_state.pendingOut) {
            return true;
        }
        if (g_state.pendingSince == 0) {
            g_state.pendingSince = now;
            return true;
        }
        if (now - g_state.pendingSince < 500) {
            return true;
        }
        logger::error("[Sidecar] the D3D12 side has completed nothing for 500 ms while the game's queue waits on "
                      "fence {} (completed {}). Releasing that wait from the CPU so the game can present again.",
            g_state.pendingOut.load(std::memory_order_relaxed), done);
        Disable("the D3D12 side stopped completing");
        return false;
    }

    void Barrier(ID3D12GraphicsCommandList* a_list, ID3D12Resource* a_resource,
        D3D12_RESOURCE_STATES a_from, D3D12_RESOURCE_STATES a_to) noexcept
    {
        if (a_list == nullptr || a_resource == nullptr || a_from == a_to) {
            return;
        }
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = a_resource;
        barrier.Transition.StateBefore = a_from;
        barrier.Transition.StateAfter = a_to;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        a_list->ResourceBarrier(1, &barrier);
    }

    bool TypedUavStoreSupported(DXGI_FORMAT a_format) noexcept
    {
        if (g_state.device12 == nullptr) {
            return false;
        }
        D3D12_FEATURE_DATA_FORMAT_SUPPORT support{};
        support.Format = a_format;
        if (FAILED(g_state.device12->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support)))) {
            return false;
        }
        return (support.Support1 & D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW) != 0 &&
               (support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) != 0;
    }

    ID3D12Fence* Fence12() noexcept { return g_state.gpuFence; }

    FrameLock::FrameLock() noexcept
    {
        if (g_state.multithread != nullptr && g_state.multithread->GetMultithreadProtected()) {
            g_state.multithread->Enter();
            locked_ = true;
        }
    }

    FrameLock::~FrameLock()
    {
        if (locked_ && g_state.multithread != nullptr) {
            g_state.multithread->Leave();
        }
    }

    Health HealthSnapshot() noexcept
    {
        Health health{};
        health.submitted = g_state.gpuValue.load(std::memory_order_relaxed);
        health.completed = g_state.gpuFence != nullptr ? g_state.gpuFence->GetCompletedValue() : 0;
        return health;
    }
}
