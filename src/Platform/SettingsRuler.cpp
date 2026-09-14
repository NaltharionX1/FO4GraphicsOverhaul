#include "PCH.h"

#include "Platform/SettingsRuler.h"

#include "Platform/EnginePatch.h"

#include "RE/Bethesda/Settings.h"

#include <cctype>
#include <cstring>
#include <mutex>
#include <string_view>

namespace
{
    std::mutex g_mutex;
    struct Tally
    {
        bool ran{ false };
        std::uint32_t rows{ 0 };
        std::uint32_t ruled{ 0 };
        std::uint32_t alreadyCorrect{ 0 };
        std::uint32_t missing{ 0 };
        std::uint32_t wrongType{ 0 };
        std::uint32_t unverified{ 0 };
        std::uint32_t unenforced{ 0 };
    };
    bool g_latched = false;

    constexpr const char* kSsaoKey = "bSAOEnable:Display";
    std::atomic<int> g_startupSsao{ -1 };

    enum class Kind
    {
        kBinary,
        kInt,
        kFloat,
        kString
    };

    enum class Enforce
    {
        kWrite,
        kPatchedElsewhere,
        kRuntimeElsewhere,
    };

    struct Row
    {
        const char* key;
        Kind kind;
        double number;
        const char* text;
        Enforce enforce;
        const char* owner;
        const char* why;
    };

    constexpr std::uint64_t kDisplayConfigId = 133902;
    constexpr std::uint8_t kFullScreenOff[]{ 0xB8, 0x00, 0x00, 0x00, 0x00, 0x90, 0x90 };
    constexpr std::uint8_t kBorderlessOn[]{ 0xB8, 0x01, 0x00, 0x00, 0x00, 0x90, 0x90 };

    constexpr std::uint64_t kWindowModeId = 1547437;
    constexpr std::uint8_t kWindowModeCompare[]{ 0x41, 0x80, 0xFB, 0x01, 0x90, 0x90, 0x90 };
    constexpr std::uint8_t kWindowModeBranchWindowed[]{ 0x90, 0x90 };

    struct PatchSite
    {
        const char* row;
        std::uint64_t id;
        std::ptrdiff_t offset;
        std::span<const std::uint8_t> bytes;
        const char* name;
    };

    const PatchSite kPatchSites[]{
        { "bFull Screen:Display", kDisplayConfigId, 0x51, kFullScreenOff, "Window/ForceWindowed" },
        { "bBorderless:Display", kDisplayConfigId, 0x5C, kBorderlessOn, "Window/ForceBorderless" },
        { "bFull Screen:Display", kWindowModeId, 0xD0, kWindowModeCompare, "Window/WindowModeCompare" },
        { "bFull Screen:Display", kWindowModeId, 0x101, kWindowModeBranchWindowed,
            "Window/WindowModeBranch" },
    };

    bool g_patchesApplied = false;
    bool g_patchesLatched = false;

    [[nodiscard]] bool ReadSitePatchesInForce() noexcept
    {
        const std::scoped_lock lock{ g_mutex };
        return g_patchesLatched && g_patchesApplied;
    }

    [[nodiscard]] const char* EnforceName(Enforce a_enforce) noexcept
    {
        switch (a_enforce) {
        case Enforce::kPatchedElsewhere:
            return "read-site patch";
        case Enforce::kRuntimeElsewhere:
            return "runtime";
        case Enforce::kWrite:
        default:
            return "this write";
        }
    }

    constexpr Row kRows[]{
        { "sAntiAliasing:Display", Kind::kString, 0.0, "TAA", Enforce::kRuntimeElsewhere,
            "Fallout4Renderer's one-shot effectList[17] correction",
            "we own anti-aliasing (DLSS or nothing): FXAA blocked at source; TAA's resolve is dead "
            "at the root, so this only keeps the jitter + motion vectors the upscaler needs" },

        { "bSAOEnable:Display", Kind::kBinary, 1.0, nullptr, Enforce::kWrite,
            "this write primes the pass at startup; the live on/off is switch #2 (AoRenderSwitch)",
            "the engine's AO pass must RUN so RT25 (kSSAOFinal) stays live in the lighting "
            "composite — that is the buffer GtaoPass replaces after lighting" },
        { "bEnable:NVHBAO", Kind::kBinary, 1.0, nullptr, Enforce::kWrite,
            "this write raises the gate at startup; AoGate's "
            "session hold keeps it up at runtime",
            "GTAO needs the HBAO BLOCK to run — its setup is what binds the AO term into the "
            "deferred composite. Without it we render into a target nothing samples. "
            "We replace the producer, not the pass" },

        { "bBorderless:Display", Kind::kBinary, 1.0, nullptr, Enforce::kPatchedElsewhere,
            "WindowPolicy REL(133902)+0x5C — the bBorderless load itself",
            "the mod runs borderless-windowed always (WindowPolicy patches the engine's own read "
            "site); this makes the INI and the options menu report it instead of hiding it" },
        { "bFull Screen:Display", Kind::kBinary, 0.0, nullptr, Enforce::kPatchedElsewhere,
            "WindowPolicy REL(133902)+0x51, plus the window-mode compare/branch at REL(1547437)",
            "the presenting chain cannot be put into exclusive fullscreen (driven there, it shows a "
            "black screen) — already forced off in code; while its read-site patch is in force, this "
            "row makes every interface say so" },

    };

    [[nodiscard]] const char* KindName(Kind a_kind) noexcept
    {
        switch (a_kind) {
        case Kind::kBinary:
            return "bool";
        case Kind::kInt:
            return "int";
        case Kind::kFloat:
            return "float";
        case Kind::kString:
        default:
            return "string";
        }
    }

    [[nodiscard]] RE::Setting::SETTING_TYPE ExpectedType(Kind a_kind) noexcept
    {
        switch (a_kind) {
        case Kind::kBinary:
            return RE::Setting::SETTING_TYPE::kBinary;
        case Kind::kInt:
            return RE::Setting::SETTING_TYPE::kInt;
        case Kind::kFloat:
            return RE::Setting::SETTING_TYPE::kFloat;
        case Kind::kString:
        default:
            return RE::Setting::SETTING_TYPE::kString;
        }
    }

    [[nodiscard]] bool KeyEquals(std::string_view a_lhs, std::string_view a_rhs) noexcept
    {
        if (a_lhs.size() != a_rhs.size()) {
            return false;
        }
        for (std::size_t i = 0; i < a_lhs.size(); ++i) {
            const auto lhs = static_cast<unsigned char>(a_lhs[i]);
            const auto rhs = static_cast<unsigned char>(a_rhs[i]);
            if (std::tolower(lhs) != std::tolower(rhs)) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] RE::Setting* FindIn(RE::INISettingCollection* a_collection, const char* a_key) noexcept
    {
        if (!a_collection) {
            return nullptr;
        }
        for (auto* const setting : a_collection->settings) {
            if (setting != nullptr && KeyEquals(setting->GetKey(), a_key)) {
                return setting;
            }
        }
        return nullptr;
    }

    [[nodiscard]] RE::Setting* Resolve(const char* a_key) noexcept
    {
        RE::Setting* setting = FindIn(RE::INIPrefSettingCollection::GetSingleton(), a_key);
        if (!setting) {
            setting = FindIn(RE::INISettingCollection::GetSingleton(), a_key);
        }
        return setting;
    }

    [[nodiscard]] bool WriteString(RE::Setting& a_setting, const char* a_value) noexcept
    {
        const std::string_view current = a_setting.GetString();
        const std::size_t want = std::strlen(a_value);

        if (!current.empty() && current.size() >= want) {
            std::memcpy(const_cast<char*>(current.data()), a_value, want + 1U);
            return true;
        }

        char* const copy = ::_strdup(a_value);
        if (!copy) {
            return false;
        }
        a_setting.SetString(copy);
        return true;
    }

    [[nodiscard]] bool WriteAndVerify(RE::Setting& a_setting, const Row& a_row) noexcept
    {
        switch (a_row.kind) {
        case Kind::kBinary: {
            a_setting.SetBinary(a_row.number != 0.0);
            return a_setting.GetBinary() == (a_row.number != 0.0);
        }
        case Kind::kInt: {
            const auto want = static_cast<std::int32_t>(a_row.number);
            a_setting.SetInt(want);
            return a_setting.GetInt() == want;
        }
        case Kind::kFloat: {
            const auto want = static_cast<float>(a_row.number);
            a_setting.SetFloat(want);
            return a_setting.GetFloat() == want;
        }
        case Kind::kString:
        default: {
            if (!WriteString(a_setting, a_row.text)) {
                return false;
            }
            return a_setting.GetString() == std::string_view{ a_row.text };
        }
        }
    }

    [[nodiscard]] bool AlreadyCorrect(const RE::Setting& a_setting, const Row& a_row) noexcept
    {
        switch (a_row.kind) {
        case Kind::kBinary:
            return a_setting.GetBinary() == (a_row.number != 0.0);
        case Kind::kInt:
            return a_setting.GetInt() == static_cast<std::int32_t>(a_row.number);
        case Kind::kFloat:
            return a_setting.GetFloat() == static_cast<float>(a_row.number);
        case Kind::kString:
        default:
            return a_setting.GetString() == std::string_view{ a_row.text };
        }
    }

    [[nodiscard]] std::string Describe(const RE::Setting& a_setting, Kind a_kind) noexcept
    {
        try {
            switch (a_kind) {
            case Kind::kBinary:
                return a_setting.GetBinary() ? "1" : "0";
            case Kind::kInt:
                return std::to_string(a_setting.GetInt());
            case Kind::kFloat:
                return std::to_string(a_setting.GetFloat());
            case Kind::kString:
            default: {
                const std::string_view text = a_setting.GetString();
                return text.empty() ? std::string{ "(empty)" } : std::string{ text };
            }
            }
        } catch (...) {
            return std::string{ "?" };
        }
    }
}

namespace Platform::SettingsRuler
{
    RE::Setting* ResolveSetting(const char* a_key) noexcept
    {
        if (a_key == nullptr) {
            return nullptr;
        }
        try {
            return Resolve(a_key);
        } catch (...) {
            return nullptr;
        }
    }

    void Rule() noexcept
    {
        {
            const std::scoped_lock lock{ g_mutex };
            if (g_latched) {
                return;
            }
        }

        Tally status{};
        status.rows = static_cast<std::uint32_t>(std::size(kRows));

        try {
            if (!RE::INIPrefSettingCollection::GetSingleton() &&
                !RE::INISettingCollection::GetSingleton()) {
                static std::atomic<unsigned> s_tooEarly{ 0 };
                const unsigned attempt = s_tooEarly.fetch_add(1, std::memory_order_relaxed) + 1U;
                if (attempt <= 3U) {
                    logger::info("[Ruler] settings collections are not up yet (attempt {}) — nothing ruled, the next boundary retries", attempt);
                } else {
                    logger::warn("[Ruler] settings collections are STILL not up after {} attempts — nothing ruled; every scheduled boundary has passed", attempt);
                }
                return;
            }

            for (const Row& row : kRows) {
                RE::Setting* const setting = Resolve(row.key);
                if (!setting) {
                    ++status.missing;
                    logger::warn("[Ruler] '{}' not found in either collection — NOT ruled ({})",
                        row.key, row.why);
                    continue;
                }
                if (const std::string_view actual = setting->GetKey();
                    actual != std::string_view{ row.key }) {
                    logger::info("[Ruler] '{}' is registered by the engine as '{}' — matched "
                                 "case-insensitively",
                        row.key, actual);
                }

                if (KeyEquals(row.key, kSsaoKey) &&
                    g_startupSsao.load(std::memory_order_relaxed) < 0 &&
                    setting->GetType() == RE::Setting::SETTING_TYPE::kBinary) {
                    const int startup = setting->GetBinary() ? 1 : 0;
                    g_startupSsao.store(startup, std::memory_order_relaxed);
                    logger::info("[Ruler] '{}' STARTED as {} — this is what decides whether the "
                                 "engine's SSAO subsystem came up, and our write below cannot "
                                 "change that (it lands after materialisation)",
                        row.key, startup);
                }

                if (setting->GetType() != ExpectedType(row.kind)) {
                    ++status.wrongType;
                    logger::warn("[Ruler] '{}' is not a {} — refusing to write ({})", row.key,
                        KindName(row.kind), row.why);
                    continue;
                }

                if (row.enforce == Enforce::kPatchedElsewhere && !ReadSitePatchesInForce()) {
                    ++status.unenforced;
                    logger::warn("[Ruler] '{}' NOT written: its read-site patch did not apply, so the engine reads "
                                 "its own value — writing ours would only make the interfaces lie about it ({})",
                        row.key, row.owner);
                    continue;
                }

                if (AlreadyCorrect(*setting, row)) {
                    ++status.alreadyCorrect;
                    logger::info("[Ruler] '{}' already {} — nothing to do", row.key,
                        Describe(*setting, row.kind));
                    continue;
                }

                const std::string before = Describe(*setting, row.kind);
                if (WriteAndVerify(*setting, row)) {
                    ++status.ruled;
                    logger::info("[Ruler] '{}' was {} -> now {} (verified by read-back) — {}",
                        row.key, before, Describe(*setting, row.kind), row.why);
                } else {
                    ++status.unverified;
                    logger::warn("[Ruler] '{}' wrote our value but it read back as {} — something "
                                 "else owns this setting",
                        row.key, Describe(*setting, row.kind));
                }
            }

            status.ran = true;
        } catch (...) {
            logger::warn("[Ruler] threw while ruling settings — skipped (fail-open)");
            return;
        }

        {
            const std::scoped_lock lock{ g_mutex };
            g_latched = true;
        }

        logger::info("[Ruler] {} row(s): {} ruled, {} already correct, {} missing, {} wrong type, "
                     "{} unverified, {} left unwritten (read-site patch not in force) — our configuration is "
                     "now what the engine reads, and what the console reports",
            status.rows, status.ruled, status.alreadyCorrect, status.missing, status.wrongType,
            status.unverified, status.unenforced);

        for (const Row& row : kRows) {
            logger::info("[Ruler]   {} — enforced by: {} ({})", row.key, EnforceName(row.enforce),
                row.owner);
        }
    }

    bool ApplyReadSitePatches() noexcept
    {
        {
            const std::scoped_lock lock{ g_mutex };
            if (g_patchesLatched) {
                return g_patchesApplied;
            }
            g_patchesLatched = true;
        }

        bool all = true;
        try {
            for (const PatchSite& site : kPatchSites) {
                const bool ok =
                    EnginePatch::WriteBytes(site.id, site.offset, site.bytes, {}, site.name);
                all = all && ok;
                if (!ok) {
                    logger::warn("[Ruler] read-site patch '{}' for row '{}' did NOT apply — that "
                                 "row is no longer guaranteed, only written",
                        site.name, site.row);
                }
            }
        } catch (...) {
            logger::warn("[Ruler] applying read-site patches threw — skipped (fail-open)");
            all = false;
        }

        {
            const std::scoped_lock lock{ g_mutex };
            g_patchesApplied = all;
        }

        if (all) {
            logger::info("[Ruler] {} read-site patch(es) applied — the engine no longer READS the "
                         "settings behind them, so no INI edit, options-menu change or console "
                         "command can move them (verified against both a windowed and an "
                         "exclusive-fullscreen INI)",
                std::size(kPatchSites));
        } else {
            logger::warn("[Ruler] read-site patches only PARTIALLY applied — the game may still "
                         "open in its configured display mode; see the [Patch] lines above");
        }
        return all;
    }
}
