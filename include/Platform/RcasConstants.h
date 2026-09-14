#pragma once

#include <cstddef>
#include <cstdint>

namespace Platform
{
    struct alignas(16) RcasConstants
    {
        float sharpness{};
        float pad[3]{};
    };

    static_assert(sizeof(RcasConstants) == 16);
    static_assert(alignof(RcasConstants) == 16);
    static_assert(offsetof(RcasConstants, sharpness) == 0);
    static_assert(offsetof(RcasConstants, pad) == 4);
}
