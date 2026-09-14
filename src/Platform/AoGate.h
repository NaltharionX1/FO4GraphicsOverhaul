#pragma once

#include <cstdint>

namespace Platform::AoGate
{
    void Bind() noexcept;

    [[nodiscard]] bool EverObservedEnabled() noexcept;

    void SetSessionHold(bool a_hold) noexcept;

    void EnforceFrame() noexcept;

    void RetireEngineProducers() noexcept;

    [[nodiscard]] bool EngineProducersRetired() noexcept;

}
