// SPDX-License-Identifier: GPL-3.0-or-later
// Portions ported from dlss5-bridge, Copyright (c) 2026 NIGos, MIT License.

#pragma once

#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include <cstdint>

namespace Platform::D3D12Sidecar
{
    struct SharedTexture
    {
        ID3D12Resource* d3d12{ nullptr };
        ID3D11Texture2D* d3d11{ nullptr };
        HANDLE handle{ nullptr };
        std::uint32_t width{ 0 };
        std::uint32_t height{ 0 };
        DXGI_FORMAT format{ DXGI_FORMAT_UNKNOWN };
        bool uav{ false };
        std::uint8_t route{ 0 };
        const char* name{ "" };

        [[nodiscard]] bool Valid() const noexcept { return d3d12 != nullptr && d3d11 != nullptr; }
    };

    struct AdapterIdentity
    {
        unsigned int vendorId{ 0 };
        unsigned int deviceId{ 0 };
        LUID luid{};
        wchar_t description[128]{};
    };

    [[nodiscard]] bool Open(ID3D11Device* a_device, ID3D11DeviceContext* a_context) noexcept;
    void Close() noexcept;
    [[nodiscard]] bool IsOpen() noexcept;
    [[nodiscard]] ID3D11Device* BoundDevice() noexcept;
    [[nodiscard]] ID3D12Device* Device() noexcept;
    [[nodiscard]] ID3D12CommandQueue* Queue() noexcept;
    [[nodiscard]] AdapterIdentity Adapter() noexcept;

    [[nodiscard]] bool Disabled() noexcept;
    [[nodiscard]] bool Unavailable() noexcept;
    [[nodiscard]] const char* DisableReason() noexcept;
    void Disable(const char* a_reason) noexcept;
    [[nodiscard]] bool DeviceRemoved() noexcept;
    [[nodiscard]] bool Quarantined() noexcept;
    [[nodiscard]] std::uint64_t ForcedValue() noexcept;
    [[nodiscard]] bool ProveRetired() noexcept;
    [[nodiscard]] bool SessionRetired(ID3D12Device* a_device) noexcept;
    void Rearm() noexcept;
    void RequestRearm() noexcept;
    [[nodiscard]] bool TakeRearmRequest() noexcept;

    [[nodiscard]] bool CreateShared(SharedTexture& a_out, const char* a_name, std::uint32_t a_width,
        std::uint32_t a_height, DXGI_FORMAT a_format, bool a_uav) noexcept;
    void ReleaseShared(SharedTexture& a_texture) noexcept;
    void ForgetShared(SharedTexture& a_texture) noexcept;

    [[nodiscard]] bool BeginCommands(ID3D12GraphicsCommandList*& a_list) noexcept;
    [[nodiscard]] std::uint64_t EndCommands(bool* a_listAccepted = nullptr) noexcept;
    [[nodiscard]] std::uint64_t AbandonCommands() noexcept;
    [[nodiscard]] bool ReplaceInvalidList(ID3D12GraphicsCommandList*& a_list, ID3D12CommandAllocator* a_allocator,
        const wchar_t* a_name) noexcept;

    [[nodiscard]] std::uint64_t SignalFromD3D11() noexcept;
    void WaitOnD3D12(std::uint64_t a_f11Value) noexcept;
    [[nodiscard]] bool WaitOnD3D11(std::uint64_t a_f12Value) noexcept;
    [[nodiscard]] std::uint64_t SignalMarker() noexcept;
    [[nodiscard]] bool WaitForValue(std::uint64_t a_f12Value, unsigned long a_timeoutMs) noexcept;
    [[nodiscard]] bool DrainGpu() noexcept;

    void DescribeStall(char* a_out, std::size_t a_size, std::uint64_t a_waitingForF12) noexcept;

    [[nodiscard]] bool PollHealth() noexcept;

    void Barrier(ID3D12GraphicsCommandList* a_list, ID3D12Resource* a_resource,
        D3D12_RESOURCE_STATES a_from, D3D12_RESOURCE_STATES a_to) noexcept;

    [[nodiscard]] bool TypedUavStoreSupported(DXGI_FORMAT a_format) noexcept;

    struct FrameLock
    {
        FrameLock() noexcept;
        ~FrameLock();
        FrameLock(const FrameLock&) = delete;
        FrameLock& operator=(const FrameLock&) = delete;

    private:
        bool locked_{ false };
    };

    struct Health
    {
        std::uint64_t submitted{ 0 };
        std::uint64_t completed{ 0 };
    };
    [[nodiscard]] Health HealthSnapshot() noexcept;

    inline constexpr std::uint32_t kTimestampLanes = 4;
    void TimestampBegin(ID3D12GraphicsCommandList* a_list, std::uint32_t a_lane) noexcept;
    void TimestampEnd(ID3D12GraphicsCommandList* a_list, std::uint32_t a_lane) noexcept;
    [[nodiscard]] bool TimestampTake(std::uint32_t a_lane, double& a_ms) noexcept;
    [[nodiscard]] bool TimestampsAvailable() noexcept;

    [[nodiscard]] ID3D12Fence* Fence12() noexcept;
    [[nodiscard]] ID3D12CommandList* CloseOrDrop(ID3D12GraphicsCommandList* a_list, const char* a_what) noexcept;
    [[nodiscard]] std::uint64_t ExecuteAndSignal(ID3D12CommandList* a_list) noexcept;
    void WaitOnQueue(std::uint64_t a_f12Value) noexcept;
}
