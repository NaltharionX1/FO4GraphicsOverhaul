#pragma once

#include "RE/Bethesda/BSGraphics.h"

#include <d3d11.h>

namespace Platform
{
    struct ResolvedFrameBuffer
    {
        ID3D11Texture2D* texture{ nullptr };
        ID3D11Texture2D* owned{ nullptr };

        ResolvedFrameBuffer() = default;
        ResolvedFrameBuffer(const ResolvedFrameBuffer&) = delete;
        ResolvedFrameBuffer& operator=(const ResolvedFrameBuffer&) = delete;
        ~ResolvedFrameBuffer()
        {
            if (owned != nullptr) {
                owned->Release();
            }
        }
    };

    inline void ResolveFrameBufferTexture(
        const RE::BSGraphics::RenderTarget& a_target, ResolvedFrameBuffer& a_out) noexcept
    {
        a_out.texture = a_target.texture;
        if (a_out.texture != nullptr || a_target.rtView == nullptr) {
            return;
        }
        ID3D11Resource* resource = nullptr;
        a_target.rtView->GetResource(&resource);
        if (resource == nullptr) {
            return;
        }
        static_cast<void>(resource->QueryInterface(
            __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&a_out.owned)));
        resource->Release();
        a_out.texture = a_out.owned;
    }
}
