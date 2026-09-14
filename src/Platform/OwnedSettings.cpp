#include "PCH.h"

#include "Platform/OwnedSettings.h"

#include "Platform/CharLightParams.h"
#include "Platform/EngineSwitches.h"
#include "Platform/FogOwner.h"
#include "Platform/BloomPass.h"
#include "Platform/EngineImod.h"
#include "Platform/GradingPass.h"
#include "Platform/FovModes.h"
#include "Platform/FovOwner.h"
#include "Platform/ImagespaceOverride.h"
#include "Platform/SettingCommands.h"
#include "Platform/SsrSwitch.h"

#include "Platform/CommandComposer.h"
#include "Platform/ConsoleCommand.h"
#include "Platform/EngineMemory.h"

#include <array>
#include <atomic>
#include <cmath>
#include <cstring>

namespace
{
    using namespace Platform;

    struct Binding
    {
        std::uintptr_t address{ 0 };
        bool bound{ false };
    };

    std::array<std::atomic<double>, kCatalogueRowTotal> g_values{};

    std::array<double, kCatalogueRowTotal> g_baseline{};
    std::array<bool, kCatalogueRowTotal> g_haveBaseline{};
    std::array<std::atomic<bool>, kCatalogueRowTotal> g_set{};
    std::array<Binding, kCatalogueRowTotal> g_bindings{};

    std::array<std::size_t, 16> g_ownedIndices{};
    std::size_t g_ownedCount{ 0 };

    std::atomic<std::size_t> g_corrections{ 0 };
    std::atomic<bool> g_bound{ false };

    [[nodiscard]] bool ShouldLogCorrection(std::uint64_t a_count) noexcept
    {
        return a_count <= 3U || (a_count % 1000U) == 0U;
    }

    [[nodiscard]] std::size_t WidthFor(ValueType a_type) noexcept
    {
        switch (a_type) {
        case ValueType::kBool:
            return 1U;
        case ValueType::kInt:
        case ValueType::kFloat:
            return 4U;
        default:
            return 0U;
        }
    }

    [[nodiscard]] bool ReadAddress(ValueType a_type, std::uintptr_t a_address, double& a_out) noexcept
    {
        const auto* const source = reinterpret_cast<const void*>(a_address);
        switch (a_type) {
        case ValueType::kFloat: {
            float value = 0.0F;
            if (!EngineMemory::SafeRead(&value, source, sizeof(value))) {
                return false;
            }
            a_out = static_cast<double>(value);
            return true;
        }
        case ValueType::kInt: {
            std::int32_t value = 0;
            if (!EngineMemory::SafeRead(&value, source, sizeof(value))) {
                return false;
            }
            a_out = static_cast<double>(value);
            return true;
        }
        case ValueType::kBool: {
            std::uint8_t value = 0U;
            if (!EngineMemory::SafeRead(&value, source, sizeof(value))) {
                return false;
            }
            a_out = (value != 0U) ? 1.0 : 0.0;
            return true;
        }
        default:
            return false;
        }
    }

    [[nodiscard]] bool EncodeValue(
        ValueType a_type, double a_value, std::uint8_t* a_bytes, std::size_t& a_width) noexcept
    {
        switch (a_type) {
        case ValueType::kFloat: {
            const auto encoded = static_cast<float>(a_value);
            std::memcpy(a_bytes, &encoded, sizeof(encoded));
            a_width = sizeof(encoded);
            return true;
        }
        case ValueType::kInt: {
            const auto encoded = static_cast<std::int32_t>(std::llround(a_value));
            std::memcpy(a_bytes, &encoded, sizeof(encoded));
            a_width = sizeof(encoded);
            return true;
        }
        case ValueType::kBool: {
            const std::uint8_t encoded = (std::llround(a_value) != 0) ? 1U : 0U;
            std::memcpy(a_bytes, &encoded, sizeof(encoded));
            a_width = sizeof(encoded);
            return true;
        }
        default:
            return false;
        }
    }

    [[nodiscard]] bool WriteAddress(ValueType a_type, std::uintptr_t a_address, double a_value) noexcept
    {
        std::uint8_t desired[8]{};
        std::size_t width = 0U;
        if (!EncodeValue(a_type, a_value, desired, width) || width == 0U) {
            return false;
        }

        auto* const target = reinterpret_cast<void*>(a_address);

        std::uint8_t current[8]{};
        if (!EngineMemory::SafeRead(current, target, width)) {
            return false;
        }
        if (std::memcmp(current, desired, width) == 0) {
            return true;
        }
        if (!EngineMemory::SafeWrite(target, desired, width)) {
            return false;
        }

        std::uint8_t verify[8]{};
        if (!EngineMemory::SafeRead(verify, target, width)) {
            return false;
        }
        return std::memcmp(verify, desired, width) == 0;
    }

    [[nodiscard]] bool AlreadyMatches(ValueType a_type, double a_current, double a_desired) noexcept
    {
        std::uint8_t lhs[8]{};
        std::uint8_t rhs[8]{};
        std::size_t lhsWidth = 0U;
        std::size_t rhsWidth = 0U;
        if (!EncodeValue(a_type, a_current, lhs, lhsWidth) ||
            !EncodeValue(a_type, a_desired, rhs, rhsWidth) || lhsWidth != rhsWidth) {
            return false;
        }
        return std::memcmp(lhs, rhs, lhsWidth) == 0;
    }

    void SendConsoleRow(const CatalogueRow& a_row) noexcept
    {
        if (a_row.command != nullptr && Platform::EngineSwitches::Owns(a_row.command)) {
            const double value = g_values[GlobalIndexForId(a_row.id)].load(
                std::memory_order_relaxed);
            if (!Platform::EngineSwitches::Write(a_row.command, std::llround(value) != 0)) {
                logger::error("[Owned] {}: the engine switch is owned but the write FAILED, and its "
                              "console command is blocked — this control did not take effect. "
                              "Toggle it again; if it keeps failing the chain is not resolving.",
                    a_row.id);
            }
            return;
        }

        if (a_row.command != nullptr && Platform::SettingCommands::Owns(a_row.command)) {
            const double value = g_values[GlobalIndexForId(a_row.id)].load(
                std::memory_order_relaxed);
            if (!Platform::SettingCommands::Write(a_row.command, value)) {
                logger::error("[Owned] {}: the Setting is owned but the write did not read back, "
                              "and its console command is blocked — this control did not take "
                              "effect. Move it again; if it keeps failing the Setting has gone "
                              "stale.",
                    a_row.id);
            }
            return;
        }

        if (a_row.command != nullptr && std::strcmp(a_row.command, "fov") == 0 &&
            Platform::FovOwner::Owns()) {
            Platform::FovModes::Apply();
            return;
        }

        if (a_row.command != nullptr && std::strcmp(a_row.command, "setfog") == 0 &&
            Platform::FogOwner::Owns()) {
            std::array<double, kCatalogueRowTotal> values{};
            for (std::size_t i = 0; i < kCatalogueRowTotal; ++i) {
                values[i] = g_values[i].load(std::memory_order_relaxed);
            }
            const CommandComposer::FogPair pair =
                CommandComposer::ResolveFogPair({ values.data(), values.size() });
            if (!Platform::FogOwner::Write(pair.first, pair.second)) {
                logger::error("[Owned] {}: fog is owned but the write was refused, and its console "
                              "command is blocked — this control did not take effect.",
                    a_row.id);
            }
            return;
        }

        if (a_row.command != nullptr && std::strcmp(a_row.command, "ssr") == 0 &&
            Platform::SsrSwitch::Owns()) {
            const double value = g_values[GlobalIndexForId(a_row.id)].load(
                std::memory_order_relaxed);
            if (!Platform::SsrSwitch::Write(std::llround(value) != 0)) {
                logger::error("[Owned] {}: screen-space reflections are owned but the write was "
                              "refused, and the console command is blocked — this control did "
                              "not take effect.",
                    a_row.id);
            }
            return;
        }

        if (a_row.command != nullptr && Platform::CharLightParams::Owns(a_row.command)) {
            const double value = g_values[GlobalIndexForId(a_row.id)].load(
                std::memory_order_relaxed);
            if (!Platform::CharLightParams::Write(a_row.command, static_cast<float>(value))) {
                logger::error("[Owned] {}: the character-light value is owned but the write did "
                              "not read back, and its console command is blocked — this control "
                              "did not take effect.",
                    a_row.id);
            }
            return;
        }

        if (a_row.command != nullptr &&
            (std::strcmp(a_row.command, "scp") == 0 || std::strcmp(a_row.command, "stp") == 0)) {
            const double value = g_values[GlobalIndexForId(a_row.id)].load(
                std::memory_order_relaxed);
            Platform::GradingPass::SetParam(a_row.command, a_row.argIndex,
                static_cast<float>(value));
            return;
        }

        if (a_row.command != nullptr && std::strcmp(a_row.command, "blm") == 0) {
            const double value = g_values[GlobalIndexForId(a_row.id)].load(
                std::memory_order_relaxed);
            Platform::BloomPass::SetParam(a_row.argIndex, static_cast<float>(value));
            return;
        }

        if (a_row.command != nullptr && std::strcmp(a_row.command, "shp") == 0) {
            const std::size_t index = GlobalIndexForId(a_row.id);
            const double value = g_values[index].load(std::memory_order_relaxed);
            const double target = Platform::OwnedSettings::ResetTargetFor(index);
            const bool set = std::fabs(value - target) > 1e-6;
            double engineValue = value;
            if (a_row.argIndex == 2U) {
                constexpr double kLoopStart = 0.238073;
                constexpr double kLoopSpan = 1.999999;
                engineValue = kLoopStart - value * kLoopSpan;
                if (engineValue < -1.0) {
                    engineValue += 2.0;
                }
            }
            Platform::EngineImod::SetHdrParam(
                a_row.argIndex, static_cast<float>(engineValue), set);
            return;
        }

        if (a_row.command != nullptr && std::strcmp(a_row.command, "ifx") == 0) {
            const std::size_t index = GlobalIndexForId(a_row.id);
            const double value = g_values[index].load(std::memory_order_relaxed);
            const double target = Platform::OwnedSettings::ResetTargetFor(index);
            const bool set = std::fabs(value - target) > 1e-6;
            Platform::EngineImod::SetEffectParam(a_row.argIndex, static_cast<float>(value), set);
            return;
        }

        if (a_row.command != nullptr &&
            (std::strcmp(a_row.command, "ssr") == 0 || std::strcmp(a_row.command, "lf") == 0 ||
                std::strcmp(a_row.command, "bok") == 0)) {
            logger::error("[Owned] {}: this control needs its engine switch, and it did not bind "
                          "on this build. Its console command only FLIPS state, so sending it "
                          "would move the setting at random — nothing was sent. Use the console "
                          "command '{}' directly if you need it this session.",
                a_row.id, a_row.command);
            return;
        }

        if (a_row.tier == Tier::kOwnedModule) {
            logger::error("[Owned] {}: its owner ('{}') did not bind on this build. This control "
                          "has no console command behind it — nothing was sent.",
                a_row.id, a_row.command != nullptr ? a_row.command : "?");
            return;
        }

        if (a_row.tier == Tier::kOwnedSetting) {
            logger::error("[Owned] {}: '{}' did not resolve on this build, and there is no console "
                          "command for it — nothing was sent. Use `setini \"{}\" <value>` directly "
                          "if you need it this session.",
                a_row.id, a_row.command != nullptr ? a_row.command : "?",
                a_row.command != nullptr ? a_row.command : "?");
            return;
        }

        std::array<double, kCatalogueRowTotal> snapshot{};
        for (std::size_t i = 0; i < kCatalogueRowTotal; ++i) {
            snapshot[i] = g_values[i].load(std::memory_order_relaxed);
        }

        char command[192]{};
        const CommandComposer::ValueView view{ snapshot.data(), snapshot.size() };
        if (!CommandComposer::Compose(a_row, view, command, sizeof(command))) {
            logger::warn("[Owned] could not compose a command for {} — nothing sent", a_row.id);
            return;
        }
        (void)ConsoleCommand::Queue(command);
    }
}

namespace Platform::OwnedSettings
{
    void Bind() noexcept
    {
        if (g_bound.exchange(true, std::memory_order_acq_rel)) {
            return;
        }

        std::size_t bound = 0U;
        g_ownedCount = 0U;

        VisitCatalogue([&](const CatalogueRow& a_row, std::size_t a_index) {
            if (a_row.tier != Tier::kOwnedAddress) {
                double live = 0.0;
                bool haveLive = a_row.tier == Tier::kOwnedSetting && a_row.command != nullptr &&
                                Platform::SettingCommands::Read(a_row.command, live);

                if (haveLive) {
                    g_baseline[a_index] = live;
                    g_haveBaseline[a_index] = true;
                }
                if (!g_set[a_index].load(std::memory_order_relaxed)) {
                    if (haveLive) {
                        g_values[a_index].store(live, std::memory_order_relaxed);
                        logger::info("[Owned] {} seeded from the engine — '{}' currently {}",
                            a_row.id, a_row.command, live);
                    } else {
                        g_values[a_index].store(a_row.vanillaDefault, std::memory_order_relaxed);
                    }
                }
                return;
            }

            std::uintptr_t address = 0U;
            try {
                address = REL::ID(static_cast<std::uint64_t>(a_row.relocationId)).address();
            } catch (...) {
                logger::warn("[Owned] {}: REL {} did not resolve — this control stays inert",
                    a_row.id, a_row.relocationId);
                return;
            }

            const std::size_t width = WidthFor(a_row.type);
            if (width == 0U || !EngineMemory::WithinGameImage(address, width)) {
                logger::warn("[Owned] {}: REL {} resolved outside the game image — refusing to own it",
                    a_row.id, a_row.relocationId);
                return;
            }

            double live = 0.0;
            if (!ReadAddress(a_row.type, address, live)) {
                logger::warn("[Owned] {}: reading REL {} faulted — this control stays inert",
                    a_row.id, a_row.relocationId);
                return;
            }

            g_bindings[a_index].address = address;
            g_bindings[a_index].bound = true;
            if (!g_set[a_index].load(std::memory_order_relaxed)) {
                g_values[a_index].store(live, std::memory_order_relaxed);
            }
            g_baseline[a_index] = live;
            g_haveBaseline[a_index] = true;
            if (!HasPolicy(a_row.policy, Policy::kNoFrameSweep)) {
                if (g_ownedCount < g_ownedIndices.size()) {
                    g_ownedIndices[g_ownedCount++] = a_index;
                }
            }
            ++bound;

            if (g_set[a_index].load(std::memory_order_relaxed)) {
                logger::info("[Owned] {} bound at REL {} — engine value {}, but your saved {} wins "
                             "and is applied",
                    a_row.id, a_row.relocationId, live,
                    g_values[a_index].load(std::memory_order_relaxed));
            } else {
                logger::info(
                    "[Owned] {} bound at REL {} — engine value {} adopted as the starting point",
                    a_row.id, a_row.relocationId, live);
            }
        });

        logger::info("[Owned] binding complete: {} owned row(s) live, {} of them in the per-frame "
                     "sweep (the rest are written on change and replayed here); console rows apply "
                     "on change",
            bound, g_ownedCount);

        ReapplyAll();
    }

    void EnforceFrame() noexcept
    {
        for (std::size_t i = 0; i < g_ownedCount; ++i) {
            const std::size_t index = g_ownedIndices[i];
            const Binding& binding = g_bindings[index];
            if (!binding.bound) {
                continue;
            }
            const auto* const row = RowByGlobalIndex(index);
            if (row == nullptr) {
                continue;
            }

            double current = 0.0;
            if (!ReadAddress(row->type, binding.address, current)) {
                continue;
            }

            const double desired = g_values[index].load(std::memory_order_relaxed);
            if (AlreadyMatches(row->type, current, desired)) {
                continue;
            }

            if (WriteAddress(row->type, binding.address, desired)) {
                const auto count = g_corrections.fetch_add(1, std::memory_order_relaxed) + 1U;
                if (ShouldLogCorrection(count)) {
                    logger::info(
                        "[Owned] {} was changed to {} by something else — corrected back to {} "
                        "(this setting is owned; console/INI/options changes to it are overridden; "
                        "correction #{})",
                        row->id, current, desired, count);
                }
            }
        }
    }

    void ApplyRow(const CatalogueRow& a_row, std::size_t a_globalIndex) noexcept
    {
        if (a_globalIndex >= kCatalogueRowTotal) {
            return;
        }
        g_set[a_globalIndex].store(true, std::memory_order_relaxed);

        if (a_row.tier == Tier::kOwnedAddress) {
            const Binding& binding = g_bindings[a_globalIndex];
            if (!binding.bound) {
                return;
            }
            const double desired = g_values[a_globalIndex].load(std::memory_order_relaxed);
            if (!WriteAddress(a_row.type, binding.address, desired)) {
                logger::warn("[Owned] {}: write did not read back — the control is not taking effect",
                    a_row.id);
            }
            return;
        }

        SendConsoleRow(a_row);
    }

    void ReassertOwnedSwitches() noexcept
    {
        VisitCatalogue([](const CatalogueRow& a_row, std::size_t a_index) {
            if (a_row.command == nullptr || !Platform::EngineSwitches::Owns(a_row.command) ||
                !g_set[a_index].load(std::memory_order_relaxed)) {
                return;
            }
            const double value = g_values[a_index].load(std::memory_order_relaxed);
            (void)Platform::EngineSwitches::Write(a_row.command, std::llround(value) != 0);
        });

        if (Platform::SsrSwitch::Owns()) {
            const std::size_t ssr = GlobalIndexForId("Ssr.Toggle");
            if (g_set[ssr].load(std::memory_order_relaxed)) {
                (void)Platform::SsrSwitch::Write(
                    std::llround(g_values[ssr].load(std::memory_order_relaxed)) != 0);
            }
        }

        if (Platform::FogOwner::Owns()) {
            const std::size_t mode = GlobalIndexForId("Fog.Mode");
            const std::size_t distance = GlobalIndexForId("Fog.Distance");
            const bool anyFogSet =
                (mode < kCatalogueRowTotal && g_set[mode].load(std::memory_order_relaxed)) ||
                (distance < kCatalogueRowTotal &&
                    g_set[distance].load(std::memory_order_relaxed));
            if (anyFogSet) {
                std::array<double, kCatalogueRowTotal> values{};
                for (std::size_t i = 0; i < kCatalogueRowTotal; ++i) {
                    values[i] = g_values[i].load(std::memory_order_relaxed);
                }
                const CommandComposer::FogPair pair =
                    CommandComposer::ResolveFogPair({ values.data(), values.size() });
                (void)Platform::FogOwner::Write(pair.first, pair.second);
            }
        }

        if (Platform::FovOwner::Owns()) {
            const std::size_t first = GlobalIndexForId("Fov.World");
            const std::size_t third = GlobalIndexForId("Fov.ThirdPerson");
            const bool anySet = (first < kCatalogueRowTotal &&
                                    g_set[first].load(std::memory_order_relaxed)) ||
                                (third < kCatalogueRowTotal &&
                                    g_set[third].load(std::memory_order_relaxed));
            if (anySet) {
                Platform::FovModes::Apply();
            }
        }
    }

    void ReapplyAll() noexcept
    {
        std::array<const char*, 32> sentCommands{};
        std::size_t sentCount = 0U;

        VisitCatalogue([&](const CatalogueRow& a_row, std::size_t a_index) {
            if (a_row.tier == Tier::kConsoleToggle) {
                return;
            }
            if (!g_set[a_index].load(std::memory_order_relaxed)) {
                return;
            }
            if (a_row.tier == Tier::kOwnedAddress) {
                if (!HasPolicy(a_row.policy, Policy::kNoFrameSweep)) {
                    return;
                }
                const Binding& binding = g_bindings[a_index];
                if (!binding.bound) {
                    return;
                }
                const double desired = g_values[a_index].load(std::memory_order_relaxed);
                if (!WriteAddress(a_row.type, binding.address, desired)) {
                    logger::warn("[Owned] {}: replaying your saved {} did not read back — the "
                                 "control is not taking effect",
                        a_row.id, desired);
                }
                return;
            }

            const bool perField = a_row.command != nullptr && a_row.argCount > 1;
            if (a_row.command != nullptr && !perField) {
                for (std::size_t i = 0; i < sentCount; ++i) {
                    if (std::strcmp(sentCommands[i], a_row.command) == 0) {
                        return;
                    }
                }
                if (sentCount < sentCommands.size()) {
                    sentCommands[sentCount++] = a_row.command;
                }
            }

            SendConsoleRow(a_row);
        });
        Platform::EngineImod::RequestRearm();
    }

    double Value(std::size_t a_globalIndex) noexcept
    {
        if (a_globalIndex >= kCatalogueRowTotal) {
            return 0.0;
        }
        return g_values[a_globalIndex].load(std::memory_order_relaxed);
    }

    double ResetTargetFor(std::size_t a_globalIndex) noexcept
    {
        if (a_globalIndex >= kCatalogueRowTotal) {
            return 0.0;
        }
        if (g_haveBaseline[a_globalIndex]) {
            return g_baseline[a_globalIndex];
        }
        const auto* const row = RowByGlobalIndex(a_globalIndex);
        return row != nullptr ? row->vanillaDefault : 0.0;
    }

    bool ResetTargetIsEngineBaseline(std::size_t a_globalIndex) noexcept
    {
        return a_globalIndex < kCatalogueRowTotal && g_haveBaseline[a_globalIndex];
    }

    void SetValue(std::size_t a_globalIndex, double a_value) noexcept
    {
        if (a_globalIndex >= kCatalogueRowTotal) {
            return;
        }
        if (const auto* const row = RowByGlobalIndex(a_globalIndex)) {
            if (row->type != ValueType::kNone) {
                a_value = a_value < row->range.min   ? row->range.min
                          : a_value > row->range.max ? row->range.max
                                                     : a_value;
            }
        }
        g_values[a_globalIndex].store(a_value, std::memory_order_relaxed);
        g_set[a_globalIndex].store(true, std::memory_order_relaxed);
    }

    void LoadValue(std::size_t a_globalIndex, double a_value) noexcept
    {
        if (a_globalIndex >= kCatalogueRowTotal) {
            return;
        }
        if (!std::isfinite(a_value)) {
            return;
        }
        if (const auto* const row = RowByGlobalIndex(a_globalIndex)) {
            if (row->type != ValueType::kNone) {
                a_value = a_value < row->range.min   ? row->range.min
                          : a_value > row->range.max ? row->range.max
                                                     : a_value;
            }
        }
        g_values[a_globalIndex].store(a_value, std::memory_order_relaxed);
        g_set[a_globalIndex].store(true, std::memory_order_relaxed);
    }

    bool IsSet(std::size_t a_globalIndex) noexcept
    {
        if (a_globalIndex >= kCatalogueRowTotal) {
            return false;
        }
        return g_set[a_globalIndex].load(std::memory_order_relaxed);
    }

    Status Snapshot() noexcept
    {
        Status status{};
        status.ownedRows = g_ownedCount;
        status.corrections = g_corrections.load(std::memory_order_relaxed);

        VisitCatalogue([&](const CatalogueRow& a_row, std::size_t a_index) {
            if (a_row.tier == Tier::kOwnedAddress) {
                if (g_bindings[a_index].bound) {
                    ++status.boundRows;
                }
            } else if (g_set[a_index].load(std::memory_order_relaxed)) {
                ++status.consoleRowsSet;
            }
        });
        return status;
    }
}
