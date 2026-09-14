#pragma once
#include <d3d11.h>

namespace RE::BSGraphics
{
    struct RenderTarget
    {
        ID3D11Texture2D* texture{ nullptr };
        ID3D11Texture2D* copyTexture{ nullptr };
        ID3D11RenderTargetView* rtView{ nullptr };
        ID3D11ShaderResourceView* srView{ nullptr };
        ID3D11ShaderResourceView* copySRView{ nullptr };
        ID3D11UnorderedAccessView* uaView{ nullptr };
    };

    struct DepthStencilTarget
    {
        ID3D11Texture2D* texture{ nullptr };
        ID3D11DepthStencilView* dsView[4]{};
        ID3D11DepthStencilView* dsViewReadOnlyDepth[4]{};
        ID3D11DepthStencilView* dsViewReadOnlyStencil[4]{};
        ID3D11DepthStencilView* dsViewReadOnlyDepthStencil[4]{};
        ID3D11ShaderResourceView* srViewDepth{ nullptr };
        ID3D11ShaderResourceView* srViewStencil{ nullptr };
    };

    struct RendererData
    {
        ID3D11Device* device{ nullptr };
        ID3D11DeviceContext* context{ nullptr };
        RenderTarget renderTargets[101]{};
        DepthStencilTarget depthStencilTargets[13]{};

        [[nodiscard]] static RendererData* GetSingleton() noexcept;
    };
}
