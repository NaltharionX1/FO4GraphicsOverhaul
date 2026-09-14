#pragma once

#include <d3d11.h>

namespace Platform
{
    struct OutputMergerUnbindScope
    {
        static constexpr UINT kSlots = 8;

        explicit OutputMergerUnbindScope(ID3D11DeviceContext* a_context) noexcept :
            context(a_context)
        {
            context->OMGetRenderTargets(kSlots, savedRtv, &savedDsv);
            ID3D11RenderTargetView* nullRtv[kSlots]{};
            context->OMSetRenderTargets(kSlots, nullRtv, nullptr);
        }

        ~OutputMergerUnbindScope() noexcept
        {
            context->OMSetRenderTargets(kSlots, savedRtv, savedDsv);
            for (auto*& rtv : savedRtv) {
                if (rtv != nullptr) {
                    rtv->Release();
                    rtv = nullptr;
                }
            }
            if (savedDsv != nullptr) {
                savedDsv->Release();
                savedDsv = nullptr;
            }
        }

        OutputMergerUnbindScope(const OutputMergerUnbindScope&) = delete;
        OutputMergerUnbindScope(OutputMergerUnbindScope&&) = delete;
        OutputMergerUnbindScope& operator=(const OutputMergerUnbindScope&) = delete;
        OutputMergerUnbindScope& operator=(OutputMergerUnbindScope&&) = delete;

        ID3D11DeviceContext* context{ nullptr };
        ID3D11RenderTargetView* savedRtv[kSlots]{};
        ID3D11DepthStencilView* savedDsv{ nullptr };
    };
}
