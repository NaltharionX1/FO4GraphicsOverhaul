#pragma once
#include "Platform/RendererConstants.h"
#include "Platform/RendererContracts.h"

#include <cstdint>

namespace Platform
{
    struct Fallout4Renderer
    {
        [[nodiscard]] static FrameInputs Snapshot() noexcept;
        [[nodiscard]] static CameraConstants CameraSnapshot() noexcept;
        [[nodiscard]] static std::uint64_t HistoryEpoch() noexcept;
        [[nodiscard]] static const char* HistoryEpochReason() noexcept;
    };
}
