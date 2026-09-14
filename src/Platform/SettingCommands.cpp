#include "PCH.h"

#include "Platform/SettingCommands.h"

#include "Platform/CommandCatalogue.h"
#include "Platform/SettingsRuler.h"

#include "RE/Bethesda/Settings.h"

#include <atomic>
#include <cmath>
#include <cstring>

namespace
{
    enum class Kind : std::uint8_t
    {
        kFloat,
        kBool,
        kInt,
    };

    struct Row
    {
        const char* command;
        const char* key;
        Kind kind;
    };

    constexpr Row kRows[]{
        { "ssri", "fSSLRIntensity:SSLR", Kind::kFloat },
        { "ssrbp", "fSSLRBlendingPower:SSLR", Kind::kFloat },
        { "ssrat", "fSSLRAngleThreshold:SSLR", Kind::kFloat },
        { "ssrvap", "fSSLRVerticalAlignmentPower:SSLR", Kind::kFloat },

        { "lf", "bLensFlare:ImageSpace", Kind::kBool },

        { "bDoDepthOfField:Imagespace", "bDoDepthOfField:Imagespace", Kind::kBool },

        { "bok", "bScreenSpaceBokeh:ImageSpace", Kind::kBool },

        { "iDirShadowSplits:Display", "iDirShadowSplits:Display", Kind::kInt },

        { "fSplitDistanceMult:TerrainManager", "fSplitDistanceMult:TerrainManager", Kind::kFloat },
        { "fBlockMaximumDistance:TerrainManager", "fBlockMaximumDistance:TerrainManager",
            Kind::kFloat },
        { "fMeshLODLevel1FadeDist:Display", "fMeshLODLevel1FadeDist:Display", Kind::kFloat },
        { "fMeshLODLevel2FadeDist:Display", "fMeshLODLevel2FadeDist:Display", Kind::kFloat },

        { "bMeshLODRenderAllLevels:LOD", "bMeshLODRenderAllLevels:LOD", Kind::kBool },

        { "bAllowShadowcasterNPCLights:Display", "bAllowShadowcasterNPCLights:Display",
            Kind::kBool },

        { "fShadowBiasScale:Display", "fShadowBiasScale:Display", Kind::kFloat },

        { "fDrySpeed:LightingShader", "fDrySpeed:LightingShader", Kind::kFloat },
        { "fWetSpeed:LightingShader", "fWetSpeed:LightingShader", Kind::kFloat },

        { "bEnableWetnessMaterials:Display", "bEnableWetnessMaterials:Display", Kind::kBool },
    };
    constexpr std::size_t kRowCount = std::size(kRows);

    consteval bool RowKindsMatchTheirKeys()
    {
        for (const Row& row : kRows) {
            if (row.key == nullptr || row.command == nullptr) {
                return false;
            }
            const char prefix = row.key[0];
            if (row.kind == Kind::kFloat && prefix != 'f') {
                return false;
            }
            if (row.kind == Kind::kBool && prefix != 'b') {
                return false;
            }
            if (row.kind == Kind::kInt && prefix != 'i') {
                return false;
            }
        }
        return true;
    }
    static_assert(RowKindsMatchTheirKeys(), "a row's Kind must match its key's type prefix");

    [[nodiscard]] constexpr bool SameKey(const char* a_lhs, const char* a_rhs) noexcept
    {
        while (*a_lhs != '\0' && *a_lhs == *a_rhs) {
            ++a_lhs;
            ++a_rhs;
        }
        return *a_lhs == '\0' && *a_rhs == '\0';
    }

    consteval bool EveryOwnedSettingRowIsRegistered()
    {
        for (const auto& table : Platform::kCatalogueTables) {
            for (std::size_t i = 0; i < table.count; ++i) {
                const Platform::CatalogueRow& row = table.rows[i];
                if (row.tier != Platform::Tier::kOwnedSetting) {
                    continue;
                }
                if (row.command == nullptr) {
                    return false;
                }
                bool found = false;
                for (const Row& known : kRows) {
                    if (SameKey(row.command, known.key)) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    return false;
                }
            }
        }
        return true;
    }
    static_assert(EveryOwnedSettingRowIsRegistered(),
        "a Tier::kOwnedSetting catalogue row has no matching entry in kRows — it would bind to "
        "nothing, grey out in the menu, and log absolutely nothing. Add its key here.");

    std::atomic<RE::Setting*> g_setting[kRowCount]{};

    [[nodiscard]] std::ptrdiff_t IndexOf(const char* a_command) noexcept
    {
        if (a_command == nullptr) {
            return -1;
        }
        for (std::size_t i = 0; i < kRowCount; ++i) {
            if (::_stricmp(a_command, kRows[i].command) == 0) {
                return static_cast<std::ptrdiff_t>(i);
            }
        }
        return -1;
    }
}

namespace Platform::SettingCommands
{
    void BindAll() noexcept
    {
        for (std::size_t i = 0; i < kRowCount; ++i) {
            if (g_setting[i].load(std::memory_order_acquire) != nullptr) {
                continue;
            }
            RE::Setting* const setting = Platform::SettingsRuler::ResolveSetting(kRows[i].key);
            if (setting == nullptr) {
                logger::warn("[SettingCmd] '{}' — '{}' did not resolve on this build; the command "
                             "stays reachable and the mod keeps sending it through the console",
                    kRows[i].command, kRows[i].key);
                continue;
            }
            g_setting[i].store(setting, std::memory_order_release);
            if (kRows[i].kind == Kind::kBool) {
                logger::info("[SettingCmd] {:<6} OWNED — '{}' resolved, engine value {}; direct "
                             "Setting writes from here on, no console",
                    kRows[i].command, kRows[i].key, setting->GetBinary() ? "on" : "off");
            } else if (kRows[i].kind == Kind::kInt) {
                logger::info("[SettingCmd] {:<6} OWNED — '{}' resolved, engine value {}; direct "
                             "Setting writes from here on, no console",
                    kRows[i].command, kRows[i].key, setting->GetInt());
            } else {
                logger::info("[SettingCmd] {:<6} OWNED — '{}' resolved, engine value {:.4f}; "
                             "direct Setting writes from here on, no console",
                    kRows[i].command, kRows[i].key, setting->GetFloat());
            }
        }
    }

    bool Owns(const char* a_command) noexcept
    {
        const std::ptrdiff_t index = IndexOf(a_command);
        return index >= 0 &&
               g_setting[static_cast<std::size_t>(index)].load(std::memory_order_acquire) !=
                   nullptr;
    }

    bool Read(const char* a_command, double& a_out) noexcept
    {
        const std::ptrdiff_t index = IndexOf(a_command);
        if (index < 0) {
            return false;
        }
        RE::Setting* const resolved =
            g_setting[static_cast<std::size_t>(index)].load(std::memory_order_acquire);
        if (resolved == nullptr) {
            return false;
        }
        switch (kRows[static_cast<std::size_t>(index)].kind) {
        case Kind::kBool:
            a_out = resolved->GetBinary() ? 1.0 : 0.0;
            return true;
        case Kind::kInt:
            a_out = static_cast<double>(resolved->GetInt());
            return true;
        case Kind::kFloat:
        default:
            a_out = static_cast<double>(resolved->GetFloat());
            return true;
        }
    }

    bool Write(const char* a_command, double a_value) noexcept
    {
        const std::ptrdiff_t index = IndexOf(a_command);
        if (index < 0) {
            return false;
        }
        RE::Setting* const resolved =
            g_setting[static_cast<std::size_t>(index)].load(std::memory_order_acquire);
        if (resolved == nullptr) {
            return false;
        }
        const Row& row = kRows[static_cast<std::size_t>(index)];
        RE::Setting& setting = *resolved;

        if (row.kind == Kind::kBool) {
            const bool want = std::llround(a_value) != 0;
            setting.SetBinary(want);
            const bool landed = setting.GetBinary() == want;
            if (landed) {
                logger::info("[SettingCmd] {} = {} — direct Setting write, no console", a_command,
                    want ? "on" : "off");
            }
            return landed;
        }

        if (row.kind == Kind::kInt) {
            const auto want = static_cast<std::int32_t>(std::llround(a_value));
            setting.SetInt(want);
            const bool landed = setting.GetInt() == want;
            if (landed) {
                logger::info("[SettingCmd] {} = {} — direct Setting write, no console", a_command,
                    want);
            }
            return landed;
        }

        const auto want = static_cast<float>(a_value);
        setting.SetFloat(want);
        const bool landed = setting.GetFloat() == want;
        if (landed) {
            logger::info("[SettingCmd] {} = {:.4f} — direct Setting write, no console",
                a_command, want);
        }
        return landed;
    }
}
