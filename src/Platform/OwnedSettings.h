#pragma once

#include "Platform/CommandCatalogue.h"

#include <cstddef>

namespace Platform::OwnedSettings
{
    void Bind() noexcept;

    void EnforceFrame() noexcept;

    void ApplyRow(const CatalogueRow& a_row, std::size_t a_globalIndex) noexcept;

    void ReassertOwnedSwitches() noexcept;

    void ReapplyAll() noexcept;

    [[nodiscard]] double Value(std::size_t a_globalIndex) noexcept;

    void LoadValue(std::size_t a_globalIndex, double a_value) noexcept;

    void SetValue(std::size_t a_globalIndex, double a_value) noexcept;

    [[nodiscard]] double ResetTargetFor(std::size_t a_globalIndex) noexcept;

    [[nodiscard]] bool ResetTargetIsEngineBaseline(std::size_t a_globalIndex) noexcept;

    [[nodiscard]] bool IsSet(std::size_t a_globalIndex) noexcept;

    struct Status
    {
        std::size_t ownedRows;
        std::size_t boundRows;
        std::size_t corrections;
        std::size_t consoleRowsSet;
    };
    [[nodiscard]] Status Snapshot() noexcept;
}
