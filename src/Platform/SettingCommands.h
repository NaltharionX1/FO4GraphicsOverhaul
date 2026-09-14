#pragma once

namespace Platform::SettingCommands
{
    void BindAll() noexcept;

    [[nodiscard]] bool Owns(const char* a_command) noexcept;

    bool Write(const char* a_command, double a_value) noexcept;

    [[nodiscard]] bool Read(const char* a_command, double& a_out) noexcept;
}
