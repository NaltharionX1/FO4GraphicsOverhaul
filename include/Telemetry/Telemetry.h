#pragma once

#include "Telemetry/FrameTimings.h"
#include "Telemetry/Snapshot.h"

struct ID3D11Device;

namespace Telemetry
{
    void Start(bool (*a_wanted)() noexcept = nullptr) noexcept;

    void Stop() noexcept;

    void NoteFrame(bool a_menuOpen = false) noexcept;

    void ResetFrameHistory() noexcept;

    [[nodiscard]] FrameStats CurrentFrameStats() noexcept;

    void LogFrameStatsIfDue() noexcept;

    void MarkFrameStatsBoundary(const char* a_reason) noexcept;

    [[nodiscard]] Snapshot Read() noexcept;

    void NoteDevice(ID3D11Device* a_device) noexcept;

    void NoteLoadBegin() noexcept;
    void NoteLoadEnd() noexcept;

    bool TryRegisterLoadingMenuSink() noexcept;

    [[nodiscard]] const char* GpuSourceStatus() noexcept;
    [[nodiscard]] const char* CpuTempSourceStatus() noexcept;
}
