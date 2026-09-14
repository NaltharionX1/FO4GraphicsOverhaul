#pragma once

#include "Platform/DlaaSettings.h"

#include <filesystem>
#include <functional>
#include <iosfwd>

namespace Platform
{
    namespace DlaaSettingsIO
    {
        using StreamWriter = std::function<void(std::ostream&)>;
        using ReplaceOperation = bool (*)(const std::filesystem::path& temporary,
            const std::filesystem::path& destination) noexcept;

        struct SaveResult
        {
            DlaaSettings liveSettings{};
            bool persisted{ false };
        };

        [[nodiscard]] DlaaSettings Parse(std::istream& in, DlaaSettings defaults = {}) noexcept;

        [[nodiscard]] DlaaSettings Load(const std::filesystem::path& path,
            DlaaSettings defaults = {}) noexcept;

        [[nodiscard]] bool WriteAtomically(const std::filesystem::path& destination,
            const StreamWriter& writer, ReplaceOperation replace = nullptr) noexcept;

        [[nodiscard]] SaveResult Save(const std::filesystem::path& destination,
            DlaaSettings requested, ReplaceOperation replace = nullptr) noexcept;

        void WriteDlaaBlock(std::ostream& out, const DlaaSettings& settings);
    }
}
