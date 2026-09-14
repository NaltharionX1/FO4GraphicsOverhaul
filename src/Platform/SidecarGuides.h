#pragma once

#include <d3d11.h>
#include <dxgiformat.h>

#include <cstdint>

namespace Platform::SidecarGuides
{
    template <class T>
    void ReleaseCom(T*& a_ptr) noexcept
    {
        if (a_ptr != nullptr) {
            a_ptr->Release();
            a_ptr = nullptr;
        }
    }

    [[nodiscard]] inline DXGI_FORMAT TypedColorFormat(DXGI_FORMAT a_format) noexcept
    {
        switch (a_format) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
        default: return a_format;
        }
    }

    [[nodiscard]] inline DXGI_FORMAT DepthViewFormat(DXGI_FORMAT a_format) noexcept
    {
        switch (a_format) {
        case DXGI_FORMAT_R24G8_TYPELESS:
        case DXGI_FORMAT_D24_UNORM_S8_UINT: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        case DXGI_FORMAT_R32_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT: return DXGI_FORMAT_R32_FLOAT;
        case DXGI_FORMAT_R32G8X24_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        case DXGI_FORMAT_R16_TYPELESS:
        case DXGI_FORMAT_D16_UNORM: return DXGI_FORMAT_R16_UNORM;
        default: return a_format;
        }
    }

    [[nodiscard]] inline DXGI_FORMAT MotionViewFormat(DXGI_FORMAT a_format) noexcept
    {
        switch (a_format) {
        case DXGI_FORMAT_R32G32_TYPELESS: return DXGI_FORMAT_R32G32_FLOAT;
        case DXGI_FORMAT_R16G16_TYPELESS: return DXGI_FORMAT_R16G16_FLOAT;
        case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        default: return a_format;
        }
    }

    [[nodiscard]] inline ID3D11ShaderResourceView* EnsureOwnSrv(ID3D11Device* a_device, ID3D11Texture2D* a_texture,
        DXGI_FORMAT a_viewFormat, ID3D11ShaderResourceView*& a_srv, ID3D11Texture2D*& a_cachedTexture) noexcept
    {
        if (a_srv != nullptr && a_cachedTexture == a_texture) {
            return a_srv;
        }
        ReleaseCom(a_srv);
        a_cachedTexture = nullptr;
        D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
        desc.Format = a_viewFormat;
        desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        desc.Texture2D.MipLevels = 1;
        if (FAILED(a_device->CreateShaderResourceView(a_texture, &desc, &a_srv))) {
            return nullptr;
        }
        a_cachedTexture = a_texture;
        return a_srv;
    }

    [[nodiscard]] inline bool CreateUav(ID3D11Device* a_device, ID3D11Texture2D* a_texture, DXGI_FORMAT a_format,
        ID3D11UnorderedAccessView*& a_out) noexcept
    {
        D3D11_UNORDERED_ACCESS_VIEW_DESC desc{};
        desc.Format = a_format;
        desc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
        return SUCCEEDED(a_device->CreateUnorderedAccessView(a_texture, &desc, &a_out)) && a_out != nullptr;
    }

    struct ComputeBindingsScope
    {
        static constexpr std::uint32_t kSrvSlots = 12;
        static constexpr std::uint32_t kUavSlots = 8;

        explicit ComputeBindingsScope(ID3D11DeviceContext* a_context) noexcept :
            context_(a_context)
        {
            context_->CSGetShader(&shader_, nullptr, nullptr);
            context_->CSGetShaderResources(0, kSrvSlots, srvs_);
            context_->CSGetUnorderedAccessViews(0, kUavSlots, uavs_);
            context_->CSGetConstantBuffers(0, 1, &cb0_);
        }
        ~ComputeBindingsScope()
        {
            ID3D11ShaderResourceView* nullSrvs[kSrvSlots]{};
            ID3D11UnorderedAccessView* nullUavs[kUavSlots]{};
            context_->CSSetShaderResources(0, kSrvSlots, nullSrvs);
            context_->CSSetUnorderedAccessViews(0, kUavSlots, nullUavs, nullptr);
            context_->CSSetShader(shader_, nullptr, 0);
            context_->CSSetShaderResources(0, kSrvSlots, srvs_);
            context_->CSSetUnorderedAccessViews(0, kUavSlots, uavs_, nullptr);
            context_->CSSetConstantBuffers(0, 1, &cb0_);
            ReleaseCom(shader_);
            ReleaseCom(cb0_);
            for (auto& srv : srvs_) {
                ReleaseCom(srv);
            }
            for (auto& uav : uavs_) {
                ReleaseCom(uav);
            }
        }
        ComputeBindingsScope(const ComputeBindingsScope&) = delete;
        ComputeBindingsScope& operator=(const ComputeBindingsScope&) = delete;

    private:
        ID3D11DeviceContext* context_;
        ID3D11ComputeShader* shader_{ nullptr };
        ID3D11ShaderResourceView* srvs_[kSrvSlots]{};
        ID3D11UnorderedAccessView* uavs_[kUavSlots]{};
        ID3D11Buffer* cb0_{ nullptr };
    };

    inline void Dispatch(ID3D11DeviceContext* a_context, ID3D11ComputeShader* a_shader, std::uint32_t a_slot,
        ID3D11ShaderResourceView* a_srv, ID3D11UnorderedAccessView* a_uav, std::uint32_t a_width,
        std::uint32_t a_height) noexcept
    {
        a_context->CSSetShader(a_shader, nullptr, 0);
        a_context->CSSetShaderResources(a_slot, 1, &a_srv);
        a_context->CSSetUnorderedAccessViews(a_slot, 1, &a_uav, nullptr);
        a_context->Dispatch((a_width + 7U) / 8U, (a_height + 7U) / 8U, 1U);
        ID3D11ShaderResourceView* nullSrv = nullptr;
        ID3D11UnorderedAccessView* nullUav = nullptr;
        a_context->CSSetShaderResources(a_slot, 1, &nullSrv);
        a_context->CSSetUnorderedAccessViews(a_slot, 1, &nullUav, nullptr);
    }
}
