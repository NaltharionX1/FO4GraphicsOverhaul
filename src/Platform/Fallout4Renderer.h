// SPDX-License-Identifier: GPL-3.0-or-later
// Portions ported from Motion Vector Fixes (fo4test) by doodlum, GPL-3.0-or-later with its modding exception.

#pragma once

#include "Platform/RendererConstants.h"
#include "Platform/RendererContracts.h"
#include "Platform/RendererDevice.h"

namespace Platform
{
    class Fallout4Renderer final
    {
    public:
        [[nodiscard]] static bool Install() noexcept;
        [[nodiscard]] static FrameInputs Snapshot() noexcept;
        [[nodiscard]] static RenderDevice DeviceSnapshot() noexcept;
        [[nodiscard]] static void* Context() noexcept;
        [[nodiscard]] static CameraConstants CameraSnapshot() noexcept;
        static void LogSnapshot() noexcept;
        static void RequestHistoryReset(const char* a_reason = "unspecified") noexcept;
        [[nodiscard]] static std::uint64_t HistoryEpoch() noexcept;
        [[nodiscard]] static std::uint64_t FrameStampNow() noexcept;
        [[nodiscard]] static const char* HistoryEpochReason() noexcept;

        static void SetMipBias(float bias) noexcept;

        static void SetJitterScale(float scale) noexcept;

        [[nodiscard]] static float JitterScale() noexcept;

        static void CurrentJitterPixels(float& outX, float& outY) noexcept;

        static bool TryRegisterDynResMenuSink() noexcept;

        [[nodiscard]] static float CurrentDynamicResolutionRatio() noexcept;
    };
}
