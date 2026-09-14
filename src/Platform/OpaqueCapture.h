// SPDX-License-Identifier: GPL-3.0-or-later
// Portions adapted from Community Shaders for Fallout 4 (northaxosky), GPL-3.0.

#pragma once

#include <cstdint>

struct ID3D11Texture2D;

namespace Platform::OpaqueCapture
{
    void Install() noexcept;

    [[nodiscard]] ID3D11Texture2D* TakeThisFrame() noexcept;

    void Release() noexcept;
}
