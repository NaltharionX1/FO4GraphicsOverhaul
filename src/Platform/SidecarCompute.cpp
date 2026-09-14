#include "PCH.h"

#include "Platform/SidecarCompute.h"

#include "CSNeuralCompose.h"
#include "CSNeuralResample.h"

#include <cstdio>
#include <cstring>

namespace
{
    constexpr UINT kSlotsPerTable = 4;
    constexpr UINT kTables = 256;

    struct State
    {
        ID3D12Device* device{ nullptr };
        ID3D12RootSignature* rootSignature{ nullptr };
        ID3D12DescriptorHeap* heap{ nullptr };
        ID3D12PipelineState* resample{ nullptr };
        ID3D12PipelineState* compose{ nullptr };
        UINT increment{ 0 };
        UINT cursor{ 0 };
        bool refused{ false };
        char reason[160]{};
    };
    State g{};

    template <class T>
    void SafeRelease(T*& a_ptr) noexcept
    {
        if (a_ptr != nullptr) {
            a_ptr->Release();
            a_ptr = nullptr;
        }
    }

    void Refuse(const char* a_what, HRESULT a_hr) noexcept
    {
        g.refused = true;
        std::snprintf(g.reason, sizeof(g.reason), "%s (0x%08lx)", a_what, static_cast<unsigned long>(a_hr));
        logger::warn("[Neural] the model-extent kernels are unavailable on this device: {} — the model works at 100 %", g.reason);
    }

    [[nodiscard]] bool BuildRootSignature() noexcept
    {
        D3D12_DESCRIPTOR_RANGE ranges[2]{};
        ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[0].NumDescriptors = 3;
        ranges[0].BaseShaderRegister = 0;
        ranges[0].OffsetInDescriptorsFromTableStart = 0;
        ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[1].NumDescriptors = 1;
        ranges[1].BaseShaderRegister = 0;
        ranges[1].OffsetInDescriptorsFromTableStart = 3;
        D3D12_ROOT_PARAMETER params[2]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[0].DescriptorTable.NumDescriptorRanges = 2;
        params[0].DescriptorTable.pDescriptorRanges = ranges;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[1].Constants.ShaderRegister = 0;
        params[1].Constants.Num32BitValues = 4;
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC desc{};
        desc.NumParameters = 2;
        desc.pParameters = params;
        desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
        ID3DBlob* blob = nullptr;
        ID3DBlob* error = nullptr;
        HRESULT hr = ::D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error);
        if (FAILED(hr) || blob == nullptr) {
            SafeRelease(error);
            Refuse("the root signature would not serialise", hr);
            return false;
        }
        hr = g.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&g.rootSignature));
        SafeRelease(blob);
        SafeRelease(error);
        if (FAILED(hr) || g.rootSignature == nullptr) {
            Refuse("the root signature would not create", hr);
            return false;
        }
        return true;
    }

    [[nodiscard]] bool BuildPipeline(const void* a_bytecode, std::size_t a_size, ID3D12PipelineState*& a_out, const char* a_name) noexcept
    {
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = g.rootSignature;
        desc.CS.pShaderBytecode = a_bytecode;
        desc.CS.BytecodeLength = a_size;
        const HRESULT hr = g.device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&a_out));
        if (FAILED(hr) || a_out == nullptr) {
            Refuse(a_name, hr);
            return false;
        }
        return true;
    }

    [[nodiscard]] bool Dispatch(ID3D12GraphicsCommandList* a_list, ID3D12PipelineState* a_pipeline,
        ID3D12Resource* const (&a_sources)[3], ID3D12Resource* a_dest, DXGI_FORMAT a_viewFormat, std::uint32_t a_mode) noexcept
    {
        if (a_list == nullptr || a_pipeline == nullptr || a_dest == nullptr || g.device == nullptr || g.heap == nullptr) {
            return false;
        }
        const D3D12_RESOURCE_DESC destDesc = a_dest->GetDesc();
        if (destDesc.Width == 0 || destDesc.Height == 0) {
            return false;
        }
        const UINT table = g.cursor++ % kTables;
        D3D12_CPU_DESCRIPTOR_HANDLE cpu = g.heap->GetCPUDescriptorHandleForHeapStart();
        D3D12_GPU_DESCRIPTOR_HANDLE gpu = g.heap->GetGPUDescriptorHandleForHeapStart();
        cpu.ptr += static_cast<SIZE_T>(table) * kSlotsPerTable * g.increment;
        gpu.ptr += static_cast<UINT64>(table) * kSlotsPerTable * g.increment;
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = a_viewFormat;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        for (UINT i = 0; i < 3; ++i) {
            D3D12_CPU_DESCRIPTOR_HANDLE slot = cpu;
            slot.ptr += static_cast<SIZE_T>(i) * g.increment;
            g.device->CreateShaderResourceView(a_sources[i], &srv, slot);
        }
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = a_viewFormat;
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        D3D12_CPU_DESCRIPTOR_HANDLE uavSlot = cpu;
        uavSlot.ptr += static_cast<SIZE_T>(3) * g.increment;
        g.device->CreateUnorderedAccessView(a_dest, nullptr, &uav, uavSlot);

        const UINT32 constants[4]{ a_mode, 0U, 0U, 0U };
        ID3D12DescriptorHeap* heaps[]{ g.heap };
        a_list->SetComputeRootSignature(g.rootSignature);
        a_list->SetPipelineState(a_pipeline);
        a_list->SetDescriptorHeaps(1, heaps);
        a_list->SetComputeRootDescriptorTable(0, gpu);
        a_list->SetComputeRoot32BitConstants(1, 4, constants, 0);
        const UINT groupsX = (static_cast<UINT>(destDesc.Width) + 7U) / 8U;
        const UINT groupsY = (destDesc.Height + 7U) / 8U;
        a_list->Dispatch(groupsX, groupsY, 1);
        return true;
    }
}

namespace Platform::SidecarCompute
{
    bool Ensure(ID3D12Device* a_device) noexcept
    {
        if (a_device == nullptr) {
            return false;
        }
        if (g.device == a_device) {
            return !g.refused && g.resample != nullptr && g.compose != nullptr;
        }
        Release();
        g.device = a_device;
        g.device->AddRef();
        if (!BuildRootSignature()) {
            return false;
        }
        D3D12_DESCRIPTOR_HEAP_DESC heap{};
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heap.NumDescriptors = kTables * kSlotsPerTable;
        heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        const HRESULT hr = g.device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&g.heap));
        if (FAILED(hr) || g.heap == nullptr) {
            Refuse("the descriptor heap would not create", hr);
            return false;
        }
        g.increment = g.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        if (!BuildPipeline(g_csNeuralResample, sizeof(g_csNeuralResample), g.resample, "the resample pipeline would not create") ||
            !BuildPipeline(g_csNeuralCompose, sizeof(g_csNeuralCompose), g.compose, "the compose pipeline would not create")) {
            return false;
        }
        logger::info("[Neural] model-extent kernels ready on the sidecar (area reduction, Catmull-Rom enlargement; {} descriptor tables)", kTables);
        return true;
    }

    const char* Reason() noexcept { return g.reason; }

    void Release() noexcept
    {
        SafeRelease(g.compose);
        SafeRelease(g.resample);
        SafeRelease(g.heap);
        SafeRelease(g.rootSignature);
        SafeRelease(g.device);
        g.increment = 0;
        g.cursor = 0;
        g.refused = false;
        g.reason[0] = '\0';
    }

    bool CreateTexture(ID3D12Device* a_device, std::uint32_t a_width, std::uint32_t a_height, DXGI_FORMAT a_format,
        const wchar_t* a_name, ID3D12Resource*& a_out) noexcept
    {
        SafeRelease(a_out);
        if (a_device == nullptr || a_width == 0U || a_height == 0U) {
            return false;
        }
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = a_width;
        desc.Height = a_height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = a_format;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        const HRESULT hr = a_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON,
            nullptr, IID_PPV_ARGS(&a_out));
        if (FAILED(hr) || a_out == nullptr) {
            logger::error("[Neural] a model-extent image ({}x{} fmt={}) would not create: {:#010x}", a_width, a_height,
                static_cast<int>(a_format), static_cast<unsigned>(hr));
            return false;
        }
        if (a_name != nullptr) {
            (void)a_out->SetName(a_name);
        }
        return true;
    }

    bool Resample(ID3D12GraphicsCommandList* a_list, ID3D12Resource* a_source, ID3D12Resource* a_dest,
        DXGI_FORMAT a_viewFormat, Filter a_filter) noexcept
    {
        ID3D12Resource* const sources[3]{ a_source, nullptr, nullptr };
        return a_source != nullptr && Dispatch(a_list, g.resample, sources, a_dest, a_viewFormat, static_cast<std::uint32_t>(a_filter));
    }

    bool Compose(ID3D12GraphicsCommandList* a_list, ID3D12Resource* a_base, ID3D12Resource* a_modelIn,
        ID3D12Resource* a_modelOut, ID3D12Resource* a_dest, DXGI_FORMAT a_viewFormat, Filter a_filter) noexcept
    {
        ID3D12Resource* const sources[3]{ a_base, a_modelIn, a_modelOut };
        return a_base != nullptr && a_modelIn != nullptr && a_modelOut != nullptr &&
               Dispatch(a_list, g.compose, sources, a_dest, a_viewFormat, static_cast<std::uint32_t>(a_filter));
    }
}
