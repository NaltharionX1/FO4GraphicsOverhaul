#pragma once

#include "Platform/NgxAbi.h"

#include <d3d12.h>

#include <cstdint>

namespace Platform::NgxD3D12
{
    struct Outcome
    {
        Ngx::Result result{ 0 };
        unsigned long faultCode{ 0 };
        [[nodiscard]] bool Ok() const noexcept { return faultCode == 0 && Ngx::Succeeded(result); }
        [[nodiscard]] bool Faulted() const noexcept { return faultCode != 0; }
    };

    [[nodiscard]] bool LoadCore() noexcept;
    [[nodiscard]] bool CoreLoaded() noexcept;
    [[nodiscard]] bool CoreFaulted() noexcept;
    [[nodiscard]] Outcome InitSession(ID3D12Device* a_device, const wchar_t* a_appDataPath,
        const wchar_t* a_snippetFolder) noexcept;
    void ShutdownSession() noexcept;
    [[nodiscard]] bool SessionReady() noexcept;
    [[nodiscard]] std::uint64_t SessionSerial() noexcept;
    [[nodiscard]] int NegotiatedSdkVersion() noexcept;
    [[nodiscard]] Ngx::Parameter* AllocateParameters() noexcept;
    void DestroyParameters(Ngx::Parameter* a_params) noexcept;
    void LogCapabilities() noexcept;
    [[nodiscard]] int SuperSamplingAvailable() noexcept;
    [[nodiscard]] int FrameGenerationAvailable() noexcept;

    [[nodiscard]] Outcome CoreCreate(ID3D12GraphicsCommandList* a_list, int a_feature,
        Ngx::Parameter* a_params, Ngx::Handle** a_handle) noexcept;
    [[nodiscard]] Outcome CoreEvaluate(ID3D12GraphicsCommandList* a_list, const Ngx::Handle* a_handle,
        const Ngx::Parameter* a_params) noexcept;
    [[nodiscard]] Outcome CoreRelease(Ngx::Handle* a_handle) noexcept;

    struct SnippetIdentity
    {
        wchar_t path[MAX_PATH]{};
        unsigned long long size{ 0 };
        char version[32]{};
        char sha256[65]{};
        char signature[32]{};
    };

    [[nodiscard]] bool LoadSnippet(const wchar_t* a_snippetPath, const wchar_t* a_shimPath,
        SnippetIdentity& a_identity) noexcept;
    [[nodiscard]] bool SnippetLoaded() noexcept;
    void UnloadSnippet() noexcept;
    [[nodiscard]] Outcome SnippetInit(ID3D12Device* a_device, const wchar_t* a_appDataPath) noexcept;
    [[nodiscard]] Outcome SnippetCreate(ID3D12GraphicsCommandList* a_list, int a_feature,
        Ngx::Parameter* a_params, Ngx::Handle** a_handle) noexcept;
    [[nodiscard]] Outcome SnippetEvaluate(ID3D12GraphicsCommandList* a_list, const Ngx::Handle* a_handle,
        const Ngx::Parameter* a_params) noexcept;
    [[nodiscard]] Outcome SnippetRelease(Ngx::Handle* a_handle) noexcept;

    void DescribeOutcome(const Outcome& a_outcome, char* a_out, std::size_t a_size) noexcept;
}
