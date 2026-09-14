// SPDX-License-Identifier: GPL-3.0-or-later
// Portions adapted from Community Shaders for Fallout 4 (northaxosky), GPL-3.0.

#pragma once

#include <cstdint>

struct ID3D11ShaderResourceView;

namespace Platform::FirstPersonMask
{
    void Install() noexcept;

    [[nodiscard]] bool TakeMask(std::uint64_t& a_cursor, ID3D11ShaderResourceView*& a_srv, std::uint32_t& a_width,
        std::uint32_t& a_height) noexcept;

    struct State
    {
        bool installed{ false };
        bool refused{ false };
        std::uint64_t produced{ 0 };
        char reason[96]{};
    };
    [[nodiscard]] State Snapshot() noexcept;
}
