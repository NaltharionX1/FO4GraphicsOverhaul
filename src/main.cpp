#include "PCH.h"

#include "Hooks/D3DHook.h"
#include "Platform/AoMailbox.h"
#include "Platform/DlaaSettings.h"
#include "Platform/EngineImod.h"
#include "Platform/Fallout4Renderer.h"
#include "Platform/FovModes.h"
#include "Platform/EngineMemory.h"
#include "Platform/GradingPass.h"
#include "Platform/GtaoPass.h"
#include "Platform/NeuralPass.h"
#include "Platform/FrameGenEngine.h"
#include "Platform/MfgTemporalFix.h"
#include "Platform/MotionVectorFixes.h"
#include "Platform/FirstPersonMask.h"
#include "Platform/OpaqueCapture.h"
#include "Platform/FsrFrameGen.h"
#include "Platform/Reflex.h"
#include "Platform/HavokFixes.h"
#include "Platform/OwnedSettings.h"
#include "Platform/AdaptiveSync.h"
#include "Platform/PresentPolicy.h"
#include "Platform/CommandBlocker.h"
#include "Platform/RenderTargetProxy.h"
#include "Platform/SettingsRuler.h"
#include "Platform/Streamline.h"
#include "Platform/WindowPolicy.h"
#include "Settings/MenuSettingsIO.h"
#include "Settings/MenuSettingsStore.h"
#include "UI/Menu.h"
#include "Telemetry/Telemetry.h"

namespace
{
    void LogBuildFingerprint() noexcept
    {
        try {
            HMODULE self = nullptr;
            if (::GetModuleHandleExW(
                    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    reinterpret_cast<LPCWSTR>(&LogBuildFingerprint), &self) == 0 ||
                self == nullptr) {
                logger::warn("[Host] build fingerprint unavailable (no module handle)");
                return;
            }

            const auto* const base = reinterpret_cast<const std::uint8_t*>(self);
            const auto* const dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
            if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
                logger::warn("[Host] build fingerprint unavailable (bad DOS signature)");
                return;
            }
            const auto* const nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
            if (nt->Signature != IMAGE_NT_SIGNATURE) {
                logger::warn("[Host] build fingerprint unavailable (bad NT signature)");
                return;
            }

            logger::info("[Host] ★ BUILD {:08X}/{:08X} — quote these two words when reporting an "
                         "issue; they identify the exact image that ran.",
                nt->FileHeader.TimeDateStamp, nt->OptionalHeader.SizeOfImage);
        } catch (...) {
            logger::warn("[Host] build fingerprint threw — ignored, it is diagnostic only");
        }
    }

    void InitializeLog()
    {
#ifndef NDEBUG
        auto sink = std::make_shared<spdlog::sinks::msvc_sink_mt>();
#else
        auto path = logger::log_directory();
        if (!path) {
            throw std::runtime_error("F4SE log directory is unavailable");
        }
        *path /= std::string(Plugin::NAME) + ".log";
        {
            std::error_code ec;
            const std::filesystem::path previous = std::filesystem::path(*path).replace_extension(".prev.log");
            if (std::filesystem::exists(*path, ec)) {
                std::filesystem::remove(previous, ec);
                std::filesystem::rename(*path, previous, ec);
            }
        }
        auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true);
#endif

        auto log = std::make_shared<spdlog::logger>("global", std::move(sink));
#ifndef NDEBUG
        log->set_level(spdlog::level::trace);
#else
        log->set_level(spdlog::level::info);
#endif
        log->flush_on(spdlog::level::warn);
        spdlog::flush_every(std::chrono::seconds(2));
        spdlog::set_default_logger(std::move(log));
        spdlog::set_pattern("%Y-%m-%d %H:%M:%S.%e [%^%l%$] %v");
    }

    void GraphicsLoadKey(const char* a_key, const char* a_value) noexcept
    {
        if (a_key == nullptr || a_value == nullptr) {
            return;
        }
        const auto index = Platform::GlobalIndexForId(a_key);
        if (index >= Platform::kCatalogueRowTotal) {
            return;
        }
        const auto* const row = Platform::RowByGlobalIndex(index);
        if (row != nullptr && !Platform::ShouldPersist(*row)) {
            logger::info("[Settings] '{}' is session-only — the saved value is ignored so the "
                         "game's own grading is left untouched at launch",
                a_key);
            return;
        }
        double parsed = 0.0;
        const auto* const begin = a_value;
        const auto* const end = a_value + std::strlen(a_value);
        if (std::from_chars(begin, end, parsed).ec == std::errc{}) {
            Platform::OwnedSettings::LoadValue(index, parsed);
        }
    }

    void GraphicsSaveSection(std::string& a_out) noexcept
    {
        bool wroteHeader = false;
        Platform::VisitCatalogue([&](const Platform::CatalogueRow& a_row, std::size_t a_index) {
            if (!Platform::ShouldPersist(a_row) || !Platform::OwnedSettings::IsSet(a_index)) {
                return;
            }
            if (!wroteHeader) {
                a_out += "[Graphics]\n";
                wroteHeader = true;
            }
            char buffer[64]{};
            const double value = Platform::OwnedSettings::Value(a_index);
            const auto result = (a_row.type == Platform::ValueType::kFloat)
                ? std::to_chars(buffer, buffer + sizeof(buffer), value, std::chars_format::fixed, 6)
                : std::to_chars(
                      buffer, buffer + sizeof(buffer), static_cast<long long>(std::llround(value)));
            if (result.ec != std::errc{}) {
                return;
            }
            a_out += a_row.id;
            a_out += '=';
            a_out.append(buffer, static_cast<std::size_t>(result.ptr - buffer));
            a_out += '\n';
        });
    }

    void LoadMenuSettings()
    {
        Settings::IO::SetGraphicsHandlers(&GraphicsLoadKey, &GraphicsSaveSection);

        HMODULE self = nullptr;
        if (!::GetModuleHandleExA(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCSTR>(&LoadMenuSettings), &self)) {
            logger::warn("[Settings] cannot locate own module — INI persistence disabled this session");
            return;
        }
        char buffer[MAX_PATH]{};
        const DWORD length = ::GetModuleFileNameA(self, buffer, MAX_PATH);
        if (length == 0 || length >= MAX_PATH) {
            logger::warn("[Settings] module path unavailable — INI persistence disabled this session");
            return;
        }
        std::string path(buffer, length);
        const auto lastSlash = path.find_last_of("\\/");
        path.resize(lastSlash + 1);
        path += "FO4GraphicsOverhaul.ini";

        Settings::RuntimeIniPath() = path;
        if (Settings::IO::Load(path.c_str(), Settings::Menu())) {
            logger::info("[Settings] loaded {}", path);
        } else {
            logger::info("[Settings] no INI at {} — defaults in effect (Save in the menu creates it)", path);
        }
        Settings::RepublishMirrors();

        const auto& menu = Settings::Menu();

        if (menu.gradingEnabled) {
            Platform::GradingPass::SetEnabled(true);
        }
        if (menu.engineGradingNeutral) {
            Platform::EngineImod::SetEngineGradingNeutral(true);
        }
        if (menu.sceneExposureDisabled) {
            Platform::EngineImod::SetLightShapeSpecialTest(true);
        }
        if (menu.gameAutoExposureDisabled) {
            Platform::EngineImod::SetAdaptationOff(true);
        }
        if (menu.radialBlurSuppressed) {
            Platform::EngineImod::SetRadialBlurOff(true);
        }
        if (menu.doubleVisionSuppressed) {
            Platform::EngineImod::SetDoubleVisionOff(true);
        }

        Platform::GtaoPass::SetQuality(menu.aoQuality);
        Platform::GtaoPass::SetDenoisePasses(menu.aoDenoisePasses);
        Platform::GtaoPass::SetRadius(menu.aoRadius);
        Platform::GtaoPass::SetRadiusMultiplier(menu.aoRadiusMultiplier);
        Platform::GtaoPass::SetFalloffRange(menu.aoFalloffRange);
        Platform::GtaoPass::SetSampleDistributionPower(menu.aoSampleDistributionPower);
        Platform::GtaoPass::SetOccluderThickness(menu.aoOccluderThickness);
        Platform::GtaoPass::SetFinalValuePower(menu.aoFinalValuePower);
        Platform::GtaoPass::SetMinScreenRadius(menu.aoMinScreenRadius);
        Platform::GtaoPass::SetDepthFadeEnabled(menu.aoDepthFadeEnabled);
        Platform::GtaoPass::SetDepthFadeRange(menu.aoDepthFadeStart, menu.aoDepthFadeEnd);

        Platform::NeuralPass::ApplyMenuSettings(menu);

        const bool wantGtao = menu.aoMode == 2U && menu.aoEnabled;
        Platform::AoMailbox::Publish(wantGtao ? Platform::AoMailbox::Point::kGtao
                                              : Platform::AoMailbox::Point::kOff,
            wantGtao);
        logger::info("[Settings] startup replay published (grading {}, AO {})",
            menu.gradingEnabled ? "on" : "off", wantGtao ? "GTAO" : "Off");
    }

    void MessageHandler(F4SE::MessagingInterface::Message* a_message)
    {
        if (!a_message) {
            return;
        }
        switch (a_message->type) {
        case F4SE::MessagingInterface::kPostLoad:
        case F4SE::MessagingInterface::kPostPostLoad:
            Platform::SettingsRuler::Rule();
            if (a_message->type == F4SE::MessagingInterface::kPostPostLoad) {
                Platform::MotionVectorFixes::Install();
                Platform::Reflex::InstallInputHook();
                Platform::FirstPersonMask::Install();
                Platform::OpaqueCapture::Install();
                Platform::FsrFrameGen::Probe();
            }
            break;
        case F4SE::MessagingInterface::kGameDataReady:
            Platform::HavokFixes::ApplyAfterGameSettings();
            Platform::SettingsRuler::Rule();
            Platform::CommandBlocker::Install();
            Platform::Fallout4Renderer::TryRegisterDynResMenuSink();
            Telemetry::TryRegisterLoadingMenuSink();
            Platform::MotionVectorFixes::OnDataLoaded();
            break;
        case F4SE::MessagingInterface::kPostLoadGame:
            if (a_message->data) {
                Platform::Fallout4Renderer::RequestHistoryReset("load");
                Telemetry::ResetFrameHistory();
            }
            Telemetry::TryRegisterLoadingMenuSink();
            Platform::MotionVectorFixes::OnDataLoaded();
            Platform::Fallout4Renderer::TryRegisterDynResMenuSink();
            Platform::SettingsRuler::Rule();
            Platform::AoMailbox::Republish();
            Platform::FovModes::OnLoadBoundary();
            Platform::OwnedSettings::ReassertOwnedSwitches();
            Platform::EngineImod::RequestRearm();
            break;
        case F4SE::MessagingInterface::kNewGame:
            Platform::Fallout4Renderer::RequestHistoryReset("new game");
            Telemetry::ResetFrameHistory();
            Telemetry::TryRegisterLoadingMenuSink();
            Platform::Fallout4Renderer::TryRegisterDynResMenuSink();
            Platform::AoMailbox::Republish();
            Platform::FovModes::OnLoadBoundary();
            Platform::OwnedSettings::ReassertOwnedSwitches();
            Platform::EngineImod::RequestRearm();
            break;
        default:
            break;
        }
    }
}

extern "C" DLLEXPORT bool F4SEAPI F4SEPlugin_Query(
    const F4SE::QueryInterface* a_f4se,
    F4SE::PluginInfo* a_info)
{
    if (!a_f4se || !a_info) {
        return false;
    }

    a_info->infoVersion = F4SE::PluginInfo::kVersion;
    a_info->name = Plugin::NAME.data();
    a_info->version = (Plugin::VERSION_MAJOR << 16) |
                      (Plugin::VERSION_MINOR << 8) |
                      Plugin::VERSION_PATCH;

    try {
        InitializeLog();
    } catch (...) {
        return false;
    }

    if (a_f4se->IsEditor()) {
        logger::critical("[Host] Creation Kit is unsupported");
        return false;
    }

    const auto runtime = a_f4se->RuntimeVersion();
    if (runtime != F4SE::RUNTIME_1_10_163) {
        logger::critical("[Host] unsupported Fallout 4 runtime {}; required 1.10.163.0", runtime.string());
        return false;
    }

    logger::info("[Host] accepted Fallout 4 runtime {}", runtime.string());
    LogBuildFingerprint();
    return true;
}

extern "C" DLLEXPORT bool F4SEAPI F4SEPlugin_Load(const F4SE::LoadInterface* a_f4se)
{

    if (!a_f4se) {
        return false;
    }

    try {
        F4SE::Init(a_f4se);

        const auto runtime = a_f4se->RuntimeVersion();
        logger::info("[Host] canvas scaffold loaded, v0 (Fallout 4 {}.{}.{})",
            runtime[0], runtime[1], runtime[2]);

        LoadMenuSettings();

        Platform::SettingsRuler::Rule();

        Hooks::D3DHook::Install();

        if (const auto messaging = F4SE::GetMessagingInterface()) {
            messaging->RegisterListener(MessageHandler);
        }

        {
            Platform::PresentPolicy::Settings present{};
            Platform::PresentPolicy::Configure(present);

            const auto& menu = Settings::Menu();
            Platform::PresentPolicy::SetUserOptions(menu.vsyncEnabled, menu.vsyncInterval,
                menu.fpsUnlimited ? 0U : menu.fpsLimit,
                menu.loadingScreenUnlimited ? 0U : menu.loadingScreenFpsLimit);
            Platform::AdaptiveSync::Apply(menu.gsyncFlickerFix, menu.fpsUnlimited ? 0U : menu.fpsLimit);

            Platform::WindowPolicy::ApplyLoadTime();
            Platform::WindowPolicy::SetCursorLockEnabled(menu.lockCursor);

        }

        F4SE::AllocTrampoline(16 * 1024);
        if (F4SE::GetTrampoline().empty()) {
            logger::critical("[Host] executable trampoline allocation failed — DLAA hooks skipped this session");
        } else if (!Platform::Fallout4Renderer::Install()) {
            logger::warn("[Host] Fallout4Renderer::Install failed — DLAA unavailable this session (canvas unaffected)");
        } else {
            Platform::RenderTargetProxy::InstallHooks();
        }

        {
            Platform::HavokFixes::Settings havok{};
            havok.disableActorFade = Settings::Menu().disableActorFade;
            havok.disablePlayerFade = Settings::Menu().disablePlayerFade;
            Platform::HavokFixes::ApplyLoadTime(havok);
        }
        const auto dlaa = Platform::LoadDlaaSettings();
        Platform::Streamline::InitMailbox(dlaa);
        Platform::Streamline::ApplyDlaaSettings(dlaa.enable, dlaa.preset, dlaa.autoExposure);
        Platform::Fallout4Renderer::SetMipBias(dlaa.mipBias);
        Platform::Streamline::SetSharpness(dlaa.sharpness);
        Platform::Streamline::SetExposureScale(dlaa.exposureScale);
        Platform::Streamline::SetResolutionScale(dlaa.resolutionScale, false);
        Platform::Streamline::SetQualityMode(dlaa.qualityMode, false);
        Platform::Reflex::SetMode(dlaa.reflexMode);
        Platform::Fallout4Renderer::SetJitterScale(dlaa.jitterScale);
        Platform::Streamline::SetFsrVelocity(dlaa.fsrVelocity);
        Platform::Streamline::SetFsrReactiveness(dlaa.fsrReactiveness);
        Platform::Streamline::SetFsrShadingChange(dlaa.fsrShadingChange);
        Platform::Streamline::SetFsrAccumulation(dlaa.fsrAccumulation);
        Platform::Streamline::SetFsrMinDisocclusion(dlaa.fsrMinDisocclusion);
        Platform::Streamline::SetFsrMasks(dlaa.fsrMasks);
        Platform::FsrFrameGen::SetSelected(dlaa.fsrFrameGeneration);
        Platform::Streamline::SetFsrTransparencyScale(dlaa.fsrTransparencyScale);
        Platform::FrameGenEngine::SetEnabled(dlaa.frameGeneration);
        Platform::FrameGenEngine::SetFrames(dlaa.frameGenerationFrames);
        Platform::FrameGenEngine::SetDynamic(dlaa.frameGenerationDynamic);
        Platform::FrameGenEngine::SetDynamicTargetHz(dlaa.frameGenerationDynamicTargetHz);
        Platform::FrameGenEngine::SetDepthSeparation(dlaa.frameGenerationDepthSeparation);
        Platform::MfgTemporalFix::Install(Platform::MfgTemporalFix::Program::kBlackwell);

        Telemetry::Start([]() noexcept {
            return Settings::OsdEnabledMirror().load(std::memory_order_relaxed) ||
                   UI::Menu::GetSingleton().IsOpen();
        });

        return true;
    } catch (const std::exception& e) {
        logger::critical("[Host] load failed: {}", e.what());
    } catch (...) {
        logger::critical("[Host] load failed with an unknown C++ exception");
    }

    return false;
}
