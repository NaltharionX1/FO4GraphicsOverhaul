#include "PCH.h"

#include "Platform/CommandBlocker.h"

#include "Platform/AoRenderSwitch.h"
#include "Platform/CharLightParams.h"
#include "Platform/ConsoleCommand.h"
#include "Platform/EngineMemory.h"
#include "Platform/EnginePatch.h"
#include "Platform/EngineSwitches.h"
#include "Platform/FogOwner.h"
#include "Platform/FovOwner.h"
#include "Platform/ImagespaceOverride.h"
#include "Platform/SettingCommands.h"
#include "Platform/SsrSwitch.h"

#include "RE/Bethesda/Console.h"
#include "RE/Bethesda/Script.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <string>

namespace
{
    std::mutex g_mutex;
    bool g_latched = false;

    struct InstallTally
    {
        bool ran;
        std::uint32_t requested;
        std::uint32_t blocked;
        std::uint32_t notFound;
        std::uint32_t refused;
    };

    constexpr std::uint64_t kConsoleFunctionsId = 901511;
    constexpr std::ptrdiff_t kEntrySize = 0x50;
    constexpr std::ptrdiff_t kExecuteOffset = 0x30;

    static_assert(sizeof(RE::SCRIPT_FUNCTION) == kEntrySize,
        "SCRIPT_FUNCTION changed size — recheck the console table stride");
    static_assert(offsetof(RE::SCRIPT_FUNCTION, executeFunction) == kExecuteOffset,
        "SCRIPT_FUNCTION::executeFunction moved — recheck the patch offset");

    struct Blocked
    {
        const char* name;
        const char* why;
    };

    constexpr Blocked kBlocked[]{
        { "TAA",
            "Anti-aliasing is owned by FO4GraphicsOverhaul (DLSS or nothing). Change it in the mod's "
            "menu." },
        { "FXAA",
            "FXAA is disabled by FO4GraphicsOverhaul and cannot be enabled — it would run underneath "
            "DLSS. Change anti-aliasing in the mod's menu." },

        { "sao",
            "The engine's SSAO is managed by FO4GraphicsOverhaul. Toggling it here would desync the "
            "mod's ambient occlusion state. Toggle it in the mod's menu." },
        { "ao",
            "Ambient occlusion is owned by FO4GraphicsOverhaul, which drives the engine's own "
            "switch directly. Toggle it in the mod's menu." },
        { "hbao",
            "The engine's HBAO pipeline is managed by FO4GraphicsOverhaul ('hbao off' permanently "
            "breaks ambient occlusion until restart). Toggle AO in the mod's menu." },

        { "gr",
            "Godrays are owned by FO4GraphicsOverhaul, which drives the engine's own switch "
            "directly. Change them in the mod's menu." },
        { "cl",
            "Character lighting is owned by FO4GraphicsOverhaul, which drives the engine's own "
            "switch directly. Change it in the mod's menu." },
        { "mb",
            "Motion blur is owned by FO4GraphicsOverhaul, which drives the engine's own switch "
            "directly. Change it in the mod's menu." },

        { "ssri",
            "SSR intensity is owned by FO4GraphicsOverhaul, which writes the engine's own setting "
            "directly. Change it in the mod's menu." },
        { "ssrbp",
            "SSR blend power is owned by FO4GraphicsOverhaul, which writes the engine's own "
            "setting directly. Change it in the mod's menu." },
        { "ssrat",
            "SSR angle threshold is owned by FO4GraphicsOverhaul, which writes the engine's own "
            "setting directly. Change it in the mod's menu." },
        { "ssrvap",
            "SSR vertical alignment is owned by FO4GraphicsOverhaul, which writes the engine's own "
            "setting directly. Change it in the mod's menu." },

        { "fov",
            "Field of view is owned by FO4GraphicsOverhaul, which drives the engine's own camera "
            "path directly. Change it in the mod's menu." },

        { "scp",
            "Cinematic colour (saturation/brightness/contrast) is owned by FO4GraphicsOverhaul, "
            "which writes the engine's own imagespace block directly. Change it in the mod's "
            "menu." },
        { "stp",
            "Tint is owned by FO4GraphicsOverhaul, which writes the engine's own imagespace "
            "block directly. Change it in the mod's menu." },
        { "shp",
            "HDR parameters are owned by FO4GraphicsOverhaul, which writes the engine's own "
            "imagespace block directly. Change them in the mod's menu." },

        { "ssr",
            "Screen-space reflections are owned by FO4GraphicsOverhaul, which drives the "
            "engine's own layered enable directly. Toggle them in the mod's menu." },
        { "lf",
            "Lens flare is owned by FO4GraphicsOverhaul, which writes the engine's own setting "
            "directly. Toggle it in the mod's menu." },

        { "bok",
            "Bokeh depth of field is owned by FO4GraphicsOverhaul, which writes the engine's own "
            "setting directly. Toggle it in the mod's menu." },

        { "setfog",
            "Fog is owned by FO4GraphicsOverhaul, which writes the engine's own fog values "
            "directly. Change it in the mod's menu." },
    };

    std::atomic<std::uint32_t> g_attempts{ 0 };

    void Report(std::size_t a_index) noexcept
    {
        const std::uint32_t total = g_attempts.fetch_add(1, std::memory_order_relaxed) + 1U;
        const Blocked& row = kBlocked[a_index];

        try {
            if (auto* const console = RE::ConsoleLog::GetSingleton()) {
                std::string line{ "[FO4GraphicsOverhaul] '" };
                line += row.name;
                line += "' is blocked. ";
                line += row.why;
                console->AddString(line.c_str());
            }
        } catch (...) {
        }

        if (total <= 4U) {
            logger::info("[Blocker] '{}' was typed and turned away (attempt #{}) — {}", row.name,
                total, row.why);
        }
    }

    template <std::size_t N>
    bool BlockedExecute(const RE::SCRIPT_PARAMETER*, const char*, RE::TESObjectREFR*,
        RE::TESObjectREFR*, RE::Script*, RE::ScriptLocals*, float&, std::uint32_t&)
    {
        Report(N);
        return true;
    }

    using Execute_t = RE::SCRIPT_FUNCTION::ExecuteFunction_t*;
    constexpr Execute_t kStubs[]{ &BlockedExecute<0>, &BlockedExecute<1>, &BlockedExecute<2>,
        &BlockedExecute<3>, &BlockedExecute<4>, &BlockedExecute<5>, &BlockedExecute<6>,
        &BlockedExecute<7>, &BlockedExecute<8>, &BlockedExecute<9>, &BlockedExecute<10>,
        &BlockedExecute<11>, &BlockedExecute<12>, &BlockedExecute<13>, &BlockedExecute<14>,
        &BlockedExecute<15>, &BlockedExecute<16>, &BlockedExecute<17>,
        &BlockedExecute<18>, &BlockedExecute<19> };
    static_assert(std::size(kStubs) == std::size(kBlocked),
        "every blocked command needs its own stub so the console message can name it");

    [[nodiscard]] bool NameMatches(const char* a_candidate, const char* a_want) noexcept
    {
        return a_candidate != nullptr && ::_stricmp(a_candidate, a_want) == 0;
    }

    void DumpTable(std::span<RE::SCRIPT_FUNCTION> a_functions) noexcept
    {
        logger::warn("[Blocker] dumping the console function table so the correct spelling can be "
                     "read off ({} entries):",
            a_functions.size());
        std::string line;
        std::size_t onLine = 0U;
        for (const RE::SCRIPT_FUNCTION& fn : a_functions) {
            if (fn.functionName == nullptr) {
                continue;
            }
            if (!line.empty()) {
                line += ", ";
            }
            line += fn.functionName;
            if (fn.shortName != nullptr && fn.shortName[0] != '\0' &&
                !NameMatches(fn.shortName, fn.functionName)) {
                line += " (";
                line += fn.shortName;
                line += ")";
            }
            if (++onLine >= 8U) {
                logger::warn("[Blocker]   {}", line);
                line.clear();
                onLine = 0U;
            }
        }
        if (!line.empty()) {
            logger::warn("[Blocker]   {}", line);
        }
    }
}

namespace Platform::CommandBlocker
{
    void Install() noexcept
    {
        {
            const std::scoped_lock lock{ g_mutex };
            if (g_latched) {
                return;
            }
        }

        InstallTally status{};
        status.requested = static_cast<std::uint32_t>(std::size(kBlocked));

        try {
            const std::span<RE::SCRIPT_FUNCTION> functions =
                RE::SCRIPT_FUNCTION::GetConsoleFunctions();

            {
                bool aoFound = false;
                for (const RE::SCRIPT_FUNCTION& fn : functions) {
                    if (NameMatches(fn.shortName, "ao")) {
                        Platform::AoRenderSwitch::BindFromHandler(
                            reinterpret_cast<const void*>(fn.executeFunction));
                        aoFound = true;
                        break;
                    }
                }
                if (!aoFound) {
                    logger::warn("[Blocker] 'ao' is not in the console function table — switch #2 "
                                 "cannot bind and ambient occlusion cannot switch");
                }
            }

            Platform::EngineSwitches::BindAll(functions);

            Platform::SettingCommands::BindAll();

            Platform::FovOwner::Bind(functions);

            Platform::ImagespaceOverride::Bind(functions);

            Platform::CharLightParams::Bind(functions);

            Platform::FogOwner::Bind(functions);

            Platform::SsrSwitch::Bind(functions);

            for (std::size_t row = 0; row < std::size(kBlocked); ++row) {
                const Blocked& want = kBlocked[row];

                if ((NameMatches(want.name, "gr") || NameMatches(want.name, "cl") ||
                        NameMatches(want.name, "mb")) &&
                    !Platform::EngineSwitches::Owns(want.name)) {
                    logger::warn("[Blocker] '{}' NOT blocked — its switch did not bind on this "
                                 "build, so the console command stays reachable and the mod keeps "
                                 "using it",
                        want.name);
                    continue;
                }
                if (NameMatches(want.name, "cl") &&
                    (!Platform::CharLightParams::Owns("cl rim") ||
                        !Platform::CharLightParams::Owns("cl fill"))) {
                    logger::warn("[Blocker] 'cl' NOT blocked — its rim/fill values did not bind "
                                 "on this build, so the console command stays reachable and the "
                                 "mod keeps using it for them");
                    continue;
                }
                if ((NameMatches(want.name, "ao") || NameMatches(want.name, "hbao")) &&
                    !Platform::AoRenderSwitch::Bound()) {
                    logger::warn("[Blocker] '{}' NOT blocked — switch #2 did not bind on this "
                                 "build, so the console command stays reachable as the manual "
                                 "fallback for ambient occlusion",
                        want.name);
                    continue;
                }
                if ((NameMatches(want.name, "ssri") || NameMatches(want.name, "ssrbp") ||
                        NameMatches(want.name, "ssrat") || NameMatches(want.name, "ssrvap")) &&
                    !Platform::SettingCommands::Owns(want.name)) {
                    logger::warn("[Blocker] '{}' NOT blocked — its Setting did not resolve on "
                                 "this build, so the console command stays reachable and the mod "
                                 "keeps using it",
                        want.name);
                    continue;
                }
                if (NameMatches(want.name, "fov") && !Platform::FovOwner::Owns()) {
                    logger::warn("[Blocker] 'fov' NOT blocked — its derivation refused on this "
                                 "build, so the console composite stays reachable and the mod "
                                 "keeps using it");
                    continue;
                }
                if (NameMatches(want.name, "ssr") && !Platform::SsrSwitch::Owns()) {
                    logger::warn("[Blocker] 'ssr' NOT blocked — its layered enable did not bind "
                                 "on this build, so the console command stays reachable and the "
                                 "mod keeps using it");
                    continue;
                }
                if (NameMatches(want.name, "setfog") && !Platform::FogOwner::Owns()) {
                    logger::warn("[Blocker] 'setfog' NOT blocked — its derivation refused on this "
                                 "build, so the console composite stays reachable and the mod "
                                 "keeps using it");
                    continue;
                }
                if (NameMatches(want.name, "bok") && !Platform::SettingCommands::Owns("bok")) {
                    logger::warn("[Blocker] 'bok' NOT blocked - its Setting did not resolve on "
                                 "this build, so the console command stays reachable and the mod "
                                 "keeps using it");
                    continue;
                }
                if (NameMatches(want.name, "lf") && !Platform::SettingCommands::Owns("lf")) {
                    logger::warn("[Blocker] 'lf' NOT blocked — its Setting did not resolve on "
                                 "this build, so the console command stays reachable and the mod "
                                 "keeps using it");
                    continue;
                }
                if ((NameMatches(want.name, "scp") || NameMatches(want.name, "stp") ||
                        NameMatches(want.name, "shp")) &&
                    !Platform::ImagespaceOverride::Owns(want.name)) {
                    logger::warn("[Blocker] '{}' NOT blocked — its derivation refused on this "
                                 "build, so the console composite stays reachable and the mod "
                                 "keeps using it",
                        want.name);
                    continue;
                }

                bool installed = false;
                bool refusedThisRow = false;
                for (std::size_t i = 0; i < functions.size(); ++i) {
                    const RE::SCRIPT_FUNCTION& fn = functions[i];
                    if (!NameMatches(fn.functionName, want.name) &&
                        !NameMatches(fn.shortName, want.name)) {
                        continue;
                    }

                    const Execute_t stub = kStubs[row];
                    std::uint8_t bytes[sizeof(Execute_t)]{};
                    std::memcpy(bytes, &stub, sizeof(bytes));

                    const std::ptrdiff_t offset =
                        static_cast<std::ptrdiff_t>(i) * kEntrySize + kExecuteOffset;
                    if (EnginePatch::WriteBytes(kConsoleFunctionsId, offset, bytes, {},
                            "CommandBlocker/console-execute")) {
                        ++status.blocked;
                        installed = true;
                        logger::info("[Blocker] '{}' (slot {}, short '{}') BLOCKED — the engine's "
                                     "handler is no longer reachable from the console",
                            fn.functionName, i,
                            fn.shortName != nullptr ? fn.shortName : "");
                    } else {
                        ++status.refused;
                        refusedThisRow = true;
                        logger::warn("[Blocker] '{}' found at slot {} but the execute pointer would "
                                     "not write — the command still works",
                            want.name, i);
                    }
                    break;
                }

                if (!installed && !refusedThisRow) {
                    ++status.notFound;
                    logger::warn("[Blocker] '{}' is not in the console function table — NOT blocked",
                        want.name);
                }
            }

            if (status.notFound != 0U) {
                DumpTable(functions);
            }

            status.ran = true;
        } catch (...) {
            logger::warn("[Blocker] threw while installing — no commands blocked (fail-open)");
            return;
        }

        {
            const std::scoped_lock lock{ g_mutex };
            g_latched = true;
        }

        logger::info("[Blocker] {} blocked, {} not found, {} refused. A blocked command is parsed "
                     "and discarded — the engine's handler never runs. `refini`/`setini` are NOT "
                     "touched (testing showed a reload moves the value without moving the "
                     "runtime). The whole AO family is blocked now: the mailbox drives the engine's "
                     "own switches directly and needs no console at all.",
            status.blocked, status.notFound, status.refused);
    }
}
