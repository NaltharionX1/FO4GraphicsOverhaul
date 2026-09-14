#pragma once

#include <cstdint>

namespace Platform
{
    class ConsoleCommand
    {
    public:
        static bool Queue(const char* a_command) noexcept;

    };
}
