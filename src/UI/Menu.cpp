#include "UI/Menu.h"

#include "Core/FailOpen.h"
#include "Platform/CommandComposer.h"
#include "Platform/D3D12Sidecar.h"
#include "Platform/DlaaSettings.h"
#include "Platform/Fallout4Renderer.h"
#include "Platform/FovOwner.h"
#include "Platform/EngineImod.h"
#include "Platform/GradingPass.h"
#include "Platform/HavokFixes.h"
#include "Platform/NeuralPass.h"
#include "Platform/AoMailbox.h"
#include "Platform/GtaoPass.h"
#include "Platform/HdrDisplay.h"
#include "Platform/ImagespaceOverride.h"
#include "Platform/OwnedSettings.h"
#include "Platform/AdaptiveSync.h"
#include "Platform/PresentPolicy.h"
#include "Platform/RenderTargetProxy.h"
#include "Platform/SettingCommands.h"
#include "Platform/SsrSwitch.h"
#include "Platform/Dlss12Engine.h"
#include "Platform/FrameFingerprint.h"
#include "Platform/DynamicMultiplier.h"
#include "Platform/FrameGenEngine.h"
#include "Platform/FsrFrameGen.h"
#include "Platform/MfgTemporalFix.h"
#include "Platform/MotionVectorFixes.h"
#include "Platform/FirstPersonMask.h"
#include "Platform/NgxAbi.h"
#include "Platform/PresentProxy.h"
#include "Platform/Reflex.h"
#include "Platform/SidecarFrame.h"
#include "Platform/Streamline.h"
#include "Platform/WindowPolicy.h"
#include "Settings/MenuSettingsIO.h"
#include "Settings/MenuSettingsStore.h"
#include "Telemetry/Telemetry.h"
#include "UI/LiveControl.h"
#include "UI/Osd.h"

#include <cstdio>

#include <d3d11.h>

#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

namespace
{
    constexpr const char* kPresetComboItems = "J\0K\0L\0M\0";
    constexpr std::uint32_t kPresetValues[] = { 10, 11, 12, 13 };

    [[nodiscard]] std::uint32_t PresetIndexFromValue(std::uint32_t a_preset) noexcept
    {
        for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(std::size(kPresetValues)); ++i) {
            if (kPresetValues[i] == a_preset) {
                return i;
            }
        }
        return 1;
    }

    constexpr const char* kQualityModeComboItems =
        "DLAA (native)\0Ultra Quality\0Quality\0Balanced\0Performance\0Ultra Performance\0";

    [[nodiscard]] Platform::DlaaSettings& DlaaWorkingCopy()
    {
        static Platform::DlaaSettings instance = Platform::CurrentDlaaSettings();
        return instance;
    }

    [[nodiscard]] constexpr bool IsExtendedKey(std::uint32_t a_vk) noexcept
    {
        switch (a_vk) {
        case VK_INSERT:
        case VK_DELETE:
        case VK_HOME:
        case VK_END:
        case VK_PRIOR:
        case VK_NEXT:
        case VK_LEFT:
        case VK_RIGHT:
        case VK_UP:
        case VK_DOWN:
        case VK_NUMLOCK:
        case VK_SNAPSHOT:
        case VK_DIVIDE:
        case VK_RCONTROL:
        case VK_RMENU:
            return true;
        default:
            return false;
        }
    }

    void FormatKeyName(std::uint32_t a_vk, char* a_buffer, int a_bufferSize)
    {
        const UINT scan = ::MapVirtualKeyA(a_vk, MAPVK_VK_TO_VSC);
        if (scan != 0) {
            LONG lParam = static_cast<LONG>(scan) << 16;
            if (IsExtendedKey(a_vk)) {
                lParam |= 1L << 24;
            }
            if (::GetKeyNameTextA(lParam, a_buffer, a_bufferSize) > 0) {
                return;
            }
        }
        std::snprintf(a_buffer, static_cast<std::size_t>(a_bufferSize), "VK %u", a_vk);
    }

    constexpr Platform::DlaaSettings kDlaaDefaults{};
    constexpr Settings::MenuSettings kMenuDefaults{};

    bool g_geometryDirty = false;
    double g_geometryChangedAt = 0.0;

    constexpr float kMinVisibleStrip = Settings::kMinWindowSize;

    void ClampGeometryOnScreen(
        float& a_x, float& a_y, float& a_w, float& a_h, const ImVec2& a_display)
    {
        if (a_display.x <= 0.0F || a_display.y <= 0.0F) {
            return;
        }
        if (a_w > a_display.x) {
            a_w = a_display.x;
        }
        if (a_h > a_display.y) {
            a_h = a_display.y;
        }
        const float minX = kMinVisibleStrip - a_w;
        const float maxX = a_display.x - kMinVisibleStrip;
        if (maxX < minX) {
            a_x = 0.0F;
        } else if (a_x < minX) {
            a_x = minX;
        } else if (a_x > maxX) {
            a_x = maxX;
        }
        const float maxY = a_display.y - kMinVisibleStrip;
        if (maxY < 0.0F) {
            a_y = 0.0F;
        } else if (a_y < 0.0F) {
            a_y = 0.0F;
        } else if (a_y > maxY) {
            a_y = maxY;
        }
    }

    void NoteWindowGeometry(
        Settings::MenuSettings& a_settings, const ImVec2& a_pos, const ImVec2& a_size,
        bool a_collapsed)
    {
        bool changed = false;
        if (a_pos.x != a_settings.windowX || a_pos.y != a_settings.windowY) {
            a_settings.windowX = a_pos.x;
            a_settings.windowY = a_pos.y;
            changed = true;
        }
        if (!a_collapsed &&
            (a_size.x != a_settings.windowW || a_size.y != a_settings.windowH)) {
            a_settings.windowW = a_size.x;
            a_settings.windowH = a_size.y;
            changed = true;
        }
        if (!changed) {
            return;
        }
        g_geometryDirty = true;
        g_geometryChangedAt = ImGui::GetTime();
    }

    Platform::DlaaSettings g_lastPersistedDlaa{};
    bool g_dlaaBaselineSet = false;

    void FlushGeometry(bool a_force)
    {
        if (Settings::TakeDirty() && !g_geometryDirty) {
            g_geometryDirty = true;
            if (!a_force) {
                g_geometryChangedAt = ImGui::GetTime();
            }
        }

        if (!g_dlaaBaselineSet) {
            g_lastPersistedDlaa = DlaaWorkingCopy();
            g_dlaaBaselineSet = true;
        }
        const bool dlaaChanged = !(DlaaWorkingCopy() == g_lastPersistedDlaa);
        if (dlaaChanged && !g_geometryDirty) {
            g_geometryDirty = true;
            if (!a_force) {
                g_geometryChangedAt = ImGui::GetTime();
            }
        }

        if (!g_geometryDirty) {
            return;
        }
        if (!a_force && !ImGui::IsMouseReleased(ImGuiMouseButton_Left) &&
            (ImGui::GetTime() - g_geometryChangedAt) <= 2.0) {
            return;
        }
        g_geometryDirty = false;
        const std::string& iniPath = Settings::RuntimeIniPath();
        if (iniPath.empty() || !Settings::IO::Save(iniPath.c_str(), Settings::Menu())) {
            static std::atomic<bool> s_logged{ false };
            if (!s_logged.exchange(true, std::memory_order_acq_rel)) {
                logger::warn("[Settings] menu/OSD settings not persisted — INI save failed (logged once)");
            }
        }
        if (dlaaChanged) {
            Platform::SaveDlaaSettings(DlaaWorkingCopy());
            g_lastPersistedDlaa = DlaaWorkingCopy();
        }
    }
}

namespace UI
{
    bool Menu::Init(HWND a_hwnd, ID3D11Device* a_device, ID3D11DeviceContext* a_context)
    {
        if (IsInitialized()) {
            return true;
        }
        if (Core::FailOpen::Tripped("canvas-init")) {
            return false;
        }
        if (!a_hwnd || !a_device || !a_context) {
            Core::FailOpen::Trip("canvas-init", "null hwnd/device/context handed to Menu::Init");
            return false;
        }

        {
            char forced[8]{};
            const DWORD len =
                ::GetEnvironmentVariableA("FO4GO_FORCE_CANVAS_INIT_FAILURE", forced, sizeof(forced));
            if (len == 1 && forced[0] == '1') {
                Core::FailOpen::Trip("canvas-init",
                    "FORCED test failure (FO4GO_FORCE_CANVAS_INIT_FAILURE=1) — fail-open proof");
                return false;
            }
        }

        IMGUI_CHECKVERSION();
        if (!ImGui::CreateContext()) {
            Core::FailOpen::Trip("canvas-init", "ImGui::CreateContext failed");
            return false;
        }

        ImGui::GetIO().IniFilename = nullptr;

        ImGui::GetIO().ConfigErrorRecoveryEnableAssert = false;

        if (!ImGui_ImplWin32_Init(a_hwnd)) {
            ImGui::DestroyContext();
            Core::FailOpen::Trip("canvas-init", "ImGui_ImplWin32_Init failed");
            return false;
        }
        if (!ImGui_ImplDX11_Init(a_device, a_context)) {
            ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext();
            Core::FailOpen::Trip("canvas-init", "ImGui_ImplDX11_Init failed");
            return false;
        }

        MenuStyle::Apply(Settings::Menu().menuStyle);

        initialized_.store(true, std::memory_order_release);
        initDevice_ = a_device;
        logger::info("[Menu] canvas initialized (ImGui {})", IMGUI_VERSION);
        return true;
    }

    void Menu::HandleDeviceEdge(HWND a_hwnd, ID3D11Device* a_device, ID3D11DeviceContext* a_context)
    {
        std::scoped_lock lock(InputMutex());

        if (!IsInitialized()) {
            Init(a_hwnd, a_device, a_context);
            return;
        }
        if (a_device == initDevice_) {
            return;
        }

        if (isShow_.exchange(false, std::memory_order_acq_rel)) {
            pendingForcedClose_.store(true, std::memory_order_release);
            logger::info("[Menu] menu force-closed by device edge — input/cursor restore deferred to the window thread");
        }
        FlushGeometry(true);
        logger::info("[Menu] DEVICE EDGE: old={}, new={} — tearing down canvas and rebooting",
            static_cast<void*>(initDevice_), static_cast<void*>(a_device));
        Osd::NotifyContextReset();
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        initialized_.store(false, std::memory_order_release);
        initDevice_ = nullptr;
        logger::info("[Menu] canvas teardown complete — ImGui backend/context released, re-initializing");

        Init(a_hwnd, a_device, a_context);
    }

    void Menu::Render()
    {
        const bool wantMenu = IsOpen();
        const bool wantOsd = Osd::Visible();
        if (!wantMenu && !wantOsd) {
            return;
        }

        std::scoped_lock lock(InputMutex());
        if (!IsInitialized()) {
            return;
        }

        Osd::ServicePendingFontLoad();

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        if (wantMenu) {
            DrawShell();
        }
        if (wantOsd) {
            Osd::Draw(wantMenu);
        }

        ImGui::Render();
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    }

    namespace
    {
        void PushDlaaLive(const Platform::DlaaSettings& a_dlaa)
        {
            Platform::Streamline::ApplyDlaaSettingsLive(
                a_dlaa.enable, a_dlaa.preset, a_dlaa.autoExposure);
            Platform::Fallout4Renderer::SetMipBias(a_dlaa.mipBias);
            Platform::Streamline::SetSharpness(a_dlaa.sharpness);
            Platform::Streamline::SetExposureScale(a_dlaa.exposureScale);
            Platform::Streamline::SetResolutionScale(a_dlaa.resolutionScale, true);
            Platform::Streamline::SetQualityMode(a_dlaa.qualityMode, true);
            Platform::Reflex::SetMode(a_dlaa.reflexMode);
            Platform::Fallout4Renderer::SetJitterScale(a_dlaa.jitterScale);
            Platform::Streamline::SetRequestedEngine(Platform::RequestedEngineOf(a_dlaa));
            Platform::Streamline::SetFsrVelocity(a_dlaa.fsrVelocity);
            Platform::Streamline::SetFsrReactiveness(a_dlaa.fsrReactiveness);
            Platform::Streamline::SetFsrShadingChange(a_dlaa.fsrShadingChange);
            Platform::Streamline::SetFsrAccumulation(a_dlaa.fsrAccumulation);
            Platform::Streamline::SetFsrMinDisocclusion(a_dlaa.fsrMinDisocclusion);
            Platform::Streamline::SetFsrMasks(a_dlaa.fsrMasks);
            Platform::Streamline::SetFsrTransparencyScale(a_dlaa.fsrTransparencyScale);
            Platform::FsrFrameGen::SetSelected(a_dlaa.fsrFrameGeneration);
            Platform::FrameGenEngine::SetEnabled(a_dlaa.frameGeneration);
            Platform::FrameGenEngine::SetFrames(a_dlaa.frameGenerationFrames);
            Platform::FrameGenEngine::SetDynamic(a_dlaa.frameGenerationDynamic);
            Platform::FrameGenEngine::SetDynamicTargetHz(a_dlaa.frameGenerationDynamicTargetHz);
            Platform::FrameGenEngine::SetDepthSeparation(a_dlaa.frameGenerationDepthSeparation);
        }

        void PushPresentLive(const Settings::MenuSettings& a_settings)
        {
            Platform::PresentPolicy::SetUserOptions(a_settings.vsyncEnabled,
                a_settings.vsyncInterval,
                a_settings.fpsUnlimited ? 0U : a_settings.fpsLimit,
                a_settings.loadingScreenUnlimited ? 0U : a_settings.loadingScreenFpsLimit);
            Platform::WindowPolicy::SetCursorLockEnabled(a_settings.lockCursor);
            Platform::AdaptiveSync::Apply(a_settings.gsyncFlickerFix, a_settings.fpsUnlimited ? 0U : a_settings.fpsLimit);
        }

        void PushEffectsLive(const Settings::MenuSettings& a_settings)
        {
            if (Platform::GradingPass::Enabled() != a_settings.gradingEnabled) {
                Platform::GradingPass::SetEnabled(a_settings.gradingEnabled);
            }
            if (Platform::EngineImod::EngineGradingNeutralRequested() !=
                a_settings.engineGradingNeutral) {
                Platform::EngineImod::SetEngineGradingNeutral(a_settings.engineGradingNeutral);
            }
            if (Platform::EngineImod::LightShapeSpecialTestRequested() !=
                a_settings.sceneExposureDisabled) {
                Platform::EngineImod::SetLightShapeSpecialTest(a_settings.sceneExposureDisabled);
            }
            if (Platform::EngineImod::AdaptationOffRequested() !=
                a_settings.gameAutoExposureDisabled) {
                Platform::EngineImod::SetAdaptationOff(a_settings.gameAutoExposureDisabled);
            }
            if (Platform::EngineImod::RadialBlurOffRequested() != a_settings.radialBlurSuppressed) {
                Platform::EngineImod::SetRadialBlurOff(a_settings.radialBlurSuppressed);
            }
            if (Platform::EngineImod::DoubleVisionOffRequested() !=
                a_settings.doubleVisionSuppressed) {
                Platform::EngineImod::SetDoubleVisionOff(a_settings.doubleVisionSuppressed);
            }

            Platform::GtaoPass::SetQuality(a_settings.aoQuality);
            Platform::GtaoPass::SetDenoisePasses(a_settings.aoDenoisePasses);
            Platform::GtaoPass::SetRadius(a_settings.aoRadius);
            Platform::GtaoPass::SetRadiusMultiplier(a_settings.aoRadiusMultiplier);
            Platform::GtaoPass::SetFalloffRange(a_settings.aoFalloffRange);
            Platform::GtaoPass::SetSampleDistributionPower(a_settings.aoSampleDistributionPower);
            Platform::GtaoPass::SetOccluderThickness(a_settings.aoOccluderThickness);
            Platform::GtaoPass::SetFinalValuePower(a_settings.aoFinalValuePower);
            Platform::GtaoPass::SetMinScreenRadius(a_settings.aoMinScreenRadius);
            Platform::GtaoPass::SetDepthFadeEnabled(a_settings.aoDepthFadeEnabled);
            Platform::GtaoPass::SetDepthFadeRange(
                a_settings.aoDepthFadeStart, a_settings.aoDepthFadeEnd);

            const bool wantGtao = a_settings.aoMode == 2U && a_settings.aoEnabled;
            const bool liveGtao = Platform::AoMailbox::Snapshot().requested ==
                                  Platform::AoMailbox::Point::kGtao;
            if (wantGtao != liveGtao) {
                Platform::AoMailbox::Publish(wantGtao ? Platform::AoMailbox::Point::kGtao
                                                      : Platform::AoMailbox::Point::kOff,
                    wantGtao);
            }

            Platform::NeuralPass::ApplyMenuSettings(a_settings);
        }

        void DrawFpsLimiter(const char* a_toggleLabel, const char* a_sliderLabel, bool& a_unlimited,
            std::uint32_t& a_value, bool a_unlimitedDefault, std::uint32_t a_valueDefault,
            const Settings::MenuSettings& a_settings)
        {
            LiveControl::CheckboxD(a_toggleLabel, a_unlimited, a_unlimitedDefault,
                [&a_settings](bool) {
                    PushPresentLive(a_settings);
                    Settings::MarkDirty();
                });

            ImGui::BeginDisabled(a_unlimited);
            int value = static_cast<int>(a_value);
            if (ImGui::SliderInt(a_sliderLabel, &value, static_cast<int>(Settings::kFpsLimitMin),
                    static_cast<int>(Settings::kFpsLimitMax))) {
                a_value = Settings::ClampFpsLimit(static_cast<std::uint32_t>(
                    value < 0 ? 0 : value));
                PushPresentLive(a_settings);
                Settings::MarkDirty();
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(a_unlimited || a_value == a_valueDefault);
            ImGui::PushID(a_sliderLabel);
            if (ImGui::SmallButton("Rst")) {
                a_value = a_valueDefault;
                PushPresentLive(a_settings);
                Settings::MarkDirty();
            }
            ImGui::PopID();
            ImGui::EndDisabled();
        }

        constexpr float kDefaultWindowW = 620.0F;
        constexpr float kDefaultWindowH = 600.0F;

        [[nodiscard]] double ToGiB(std::uint64_t a_bytes) noexcept
        {
            return static_cast<double>(a_bytes) / (1024.0 * 1024.0 * 1024.0);
        }
    }

    void Menu::DrawShell()
    {
        auto& settings = Settings::Menu();

        if (settings.HasWindowGeometry()) {
            float x = settings.windowX;
            float y = settings.windowY;
            float w = settings.windowW;
            float h = settings.windowH;
            ClampGeometryOnScreen(x, y, w, h, ImGui::GetIO().DisplaySize);
            ImGui::SetNextWindowPos(ImVec2(x, y), ImGuiCond_Once);
            ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Once);
        } else {
            ImGui::SetNextWindowSize(
                ImVec2(kDefaultWindowW, kDefaultWindowH), ImGuiCond_FirstUseEver);
        }
        if (settings.transparentMenu) {
            ImGui::SetNextWindowBgAlpha(0.35F);
        }
        const bool contentVisible = ImGui::Begin("FO4GraphicsOverhaul", nullptr);
        if (contentVisible) {
            ImGui::SetWindowFontScale(settings.fontScale);

            const float footerHeight =
                ImGui::GetFrameHeightWithSpacing() + ImGui::GetTextLineHeightWithSpacing();
            ImGui::BeginChild("##fo4go-tabs", ImVec2(0.0F, -footerHeight));
            if (ImGui::BeginTabBar("##fo4go-maintabs")) {
                DrawTabGeneral();
                DrawTabDisplay();
                DrawTabVisualEffects();
                DrawTabOsdUi();
                DrawTabKeybindings();
                DrawTabAdvanced();
                DrawTabDebug();
                ImGui::EndTabBar();
            }
            ImGui::EndChild();

            DrawFooter();
        }
        NoteWindowGeometry(
            settings, ImGui::GetWindowPos(), ImGui::GetWindowSize(), ImGui::IsWindowCollapsed());
        ImGui::End();
        FlushGeometry(false);
    }

    void Menu::DrawTabGeneral()
    {
        if (!ImGui::BeginTabItem("General")) {
            return;
        }
        SectionHeader("Graphics presets");
        Planned::Combo("Quality preset", "Low\0Medium\0High\0Ultra\0",
            "One-click presets that will drive the whole stack at once.");
        ImGui::TextDisabled("Not wired yet — every setting stays available in its own tab.");
        ImGui::EndTabItem();
    }

    void Menu::DrawTabDisplay()
    {
        if (!ImGui::BeginTabItem("Display")) {
            return;
        }
        auto& dlaa = DlaaWorkingCopy();

        SectionHeader("Global");

        LiveControl::SliderFloatD("Supersampling (TSR)", dlaa.resolutionScale, 1.0F, 2.0F,
            kDlaaDefaults.resolutionScale, [&dlaa](float a_value) {
                dlaa.resolutionScale = Platform::ClampResolutionScale(a_value);
                Platform::Streamline::SetResolutionScale(dlaa.resolutionScale, true);
                char reason[96]{};
                std::snprintf(reason, sizeof(reason), "supersampling (TSR) = %.2f",
                    static_cast<double>(dlaa.resolutionScale));
                Telemetry::MarkFrameStatsBoundary(reason);
            });
        Help("Raises the DLSS reconstruction target ABOVE the preset's render resolution "
             "(render x TSR), then resolves to the monitor — down or up. The preset keeps its "
             "full render cost/quality ladder: Quality + TSR 2.0 at 1080p = render 720p, "
             "reconstruct 1440p, resolve 1080p; Ultra Performance + TSR 2.0 = render 360p, "
             "reconstruct 720p, resolve 1080p. 1.0 = reconstruct straight to the monitor. "
             "Resolves through whichever engine is active - DLSS or FSR.");

        {
            auto& settings = Settings::Menu();

            LiveControl::CheckboxD("VSync", settings.vsyncEnabled, kMenuDefaults.vsyncEnabled,
                [&settings](bool) {
                    PushPresentLive(settings);
                    Settings::MarkDirty();
                });
            Help("Synchronises presentation to the monitor. Off is the default. The tearing flag is never "
                 "requested - the chain that presents sets its own. The game runs borderless fullscreen "
                 "whatever its display setting says: the presenting chain cannot serve exclusive "
                 "fullscreen (it shows a black screen there).");

            ImGui::BeginDisabled(!settings.vsyncEnabled);
            int interval = static_cast<int>(settings.vsyncInterval);
            if (ImGui::SliderInt("VSync interval", &interval, 1,
                    static_cast<int>(Settings::kVSyncIntervalMax))) {
                settings.vsyncInterval =
                    Settings::ClampVSyncInterval(static_cast<std::uint32_t>(interval));
                PushPresentLive(settings);
                Settings::MarkDirty();
            }
            ImGui::EndDisabled();
            Help("Minimum display refreshes per presented frame while VSync is on. Higher values "
                 "lower the maximum presentation rate and can add latency. At 240 Hz, interval 2 "
                 "allows at most 120 new images per second on the native/DLSS-G path. Generated "
                 "frames count too. This is a display-rate ceiling, not a fixed division of the game's FPS.");

            DrawFpsLimiter("Unlimited##fps", "FPS cap", settings.fpsUnlimited, settings.fpsLimit,
                kMenuDefaults.fpsUnlimited, kMenuDefaults.fpsLimit, settings);
            Help("Frame-rate limiter, off by default. Uncheck \"Unlimited\" to engage it. The "
                 "limiter sleeps while there is time to spare and spins only for the last moment, "
                 "which holds the cap accurately without burning a core; after a hitch or an "
                 "alt-tab it re-anchors instead of running a burst of catch-up frames. While this mod's "
                 "player-update detour is active and the game thread is running frames, the wait "
                 "happens THERE, right before the game reads your input (the top of the frame, where "
                 "NVIDIA puts a limiter), and the wait at the present stands down. Without that detour "
                 "(the standalone Motion Vector Fixes plugin loaded), and during menus and loading "
                 "screens, the wait is at the present as it always was.");

            DrawFpsLimiter("Unlimited##loading", "Loading-screen FPS cap",
                settings.loadingScreenUnlimited, settings.loadingScreenFpsLimit,
                kMenuDefaults.loadingScreenUnlimited, kMenuDefaults.loadingScreenFpsLimit, settings);
            Help("A separate cap that applies only while a loading screen is up — an uncapped "
                 "loading screen renders a near-static image at whatever rate the GPU can manage, "
                 "for no benefit. This is the only per-situation limiter this mod ships. It ships "
                 "unlimited like the general one, so nothing is capped unless you ask for it.");

            LiveControl::CheckboxD("G-Sync flicker fix", settings.gsyncFlickerFix, kMenuDefaults.gsyncFlickerFix,
                [&settings](bool) {
                    PushPresentLive(settings);
                    Settings::MarkDirty();
                });
            Help("If your screen flashes black with G-SYNC on, do ONE of these: enable this, or disable G-SYNC "
                 "for the game in the NVIDIA Control Panel (Set up G-SYNC). What this does: while an FPS cap is "
                 "set, the mod asks the NVIDIA driver to repeat a frame whenever the next one is more than half "
                 "a frame period late, so the panel is shown every frame twice on a steady beat (120 Hz at a 60 "
                 "cap) and never hops between refresh modes - the hop is the flash. Needs an NVIDIA driver and "
                 "an FPS cap; does nothing otherwise. It is the display's setting while the game runs and is "
                 "restored on exit; a crash skips that restore until the next launch or a reboot. Experimental: "
                 "the Debug tab shows what the driver reports.");

            ImGui::Spacing();
            ImGui::TextDisabled("DirectX 12 presentation (always on)");
            Help("The game's frames are presented through a DirectX 12 swap chain on the private device: "
                 "the game renders into a plain texture, each frame is copied across and presented by "
                 "DirectX 12 - the foundation DLSS Frame Generation stands on. Always on, no setting. "
                 "Where the adapter has no usable DirectX 12 device the game presents its own swap chain "
                 "and every DirectX 12 feature is off for the session; the reason shows here and in the "
                 "log's [Sidecar] lines.");
            {
                const auto proxy = Platform::PresentProxy::Snapshot();
                if (proxy.active && proxy.d3d11Fallback) {
                    ImGui::TextDisabled("Active through DirectX 11 this session (%s): %ux%u", proxy.reason, proxy.width, proxy.height);
                } else if (proxy.active) {
                    ImGui::TextDisabled("Active: %ux%u, %u buffers", proxy.width, proxy.height, proxy.bufferCount);
                } else if (Platform::D3D12Sidecar::Unavailable()) {
                    ImGui::TextDisabled("Off: %s", Platform::D3D12Sidecar::DisableReason());
                } else {
                    ImGui::TextDisabled("Not active: %s",
                        proxy.reason[0] != '\0' ? proxy.reason : "no swap chain has been proxied yet");
                }
            }
        }

        LiveControl::ComboD("Reflex", dlaa.reflexMode, kDlaaDefaults.reflexMode,
            "Off\0On\0On + Boost\0", [&dlaa](std::uint32_t a_value) {
                dlaa.reflexMode = Platform::ClampReflexMode(a_value);
                Platform::Reflex::SetMode(dlaa.reflexMode);
            });
        Help("NVIDIA Reflex latency reduction, driven through NVAPI (every RTX generation supports it through the "
             "driver). It binds to the DirectX 12 presenter while the proxy presents on D3D12 and to the game's "
             "DirectX 11 device otherwise - decided per frame, nothing to choose. The frame markers are set and the "
             "driver's own latency is shown below. Reflex keeps running while frame generation interpolates. If the "
             "flip queue ever stalls while Reflex is driving generated frames, low-latency mode steps aside for "
             "generated frames for the rest of the session and says so here. NOTE: the driver latency below cannot see the "
             "generated frames, so it does not measure how frame generation feels. The driver latency is "
             "simulation start -> "
             "the end of the BOUND device's GPU work: on the DirectX 12 presenter that is the sidecar queue, "
             "fence-ordered after the whole DirectX 11 frame; on the DirectX 11 device it is the game's own render.");
        {
            const auto rf = Platform::Reflex::Snapshot();
            if (rf.pacedByFrameGen) {
                ImGui::TextDisabled("Reflex on %s: stepping aside while frame generation paces — the flip queue stalled "
                                    "earlier this session, so the old rule is back until a restart.",
                    rf.boundDevice == 1 ? "the DirectX 12 presenter" : "the DirectX 11 game device");
            } else if (rf.latched) {
                ImGui::TextDisabled("Reflex OFF: %s", rf.reason);
                if (ImGui::SmallButton("Retry##reflex")) {
                    Platform::Reflex::RequestRetry();
                }
                ImGui::SameLine();
                ImGui::TextDisabled(rf.faulted ? "(a fault inside the driver: stays off until the game restarts)"
                                               : "(Retry tries again on the next frame)");
            } else if (rf.idle) {
                ImGui::TextDisabled("Reflex idle: %s", rf.reason);
            } else if (rf.boundDevice == 0) {
                ImGui::TextDisabled("Reflex: waiting for a device to bind.");
            } else {
                ImGui::TextDisabled("Reflex on %s: mode %s, driver latency %.1f ms (simulation start to GPU render end)%s%s",
                    rf.boundDevice == 1 ? "the DirectX 12 presenter" : "the DirectX 11 game device",
                    rf.mode == 0U ? "Off" : rf.mode == 1U ? "On" : "On + Boost", static_cast<double>(rf.latencyMs),
                    Platform::PresentPolicy::CapOnGameThread() ? ", the cap waited out on the game thread before input" : "",
                    rf.gameSeamLive ? ", sleeping on the game thread before input"
                                    : (rf.gameSleeps != 0 ? ", sleeping at the present's tail (the game thread is not running frames)" : ""));
            }
            if (Platform::Reflex::SteppedAsideAfterStall() && !rf.pacedByFrameGen) {
                ImGui::TextDisabled("Reflex stepped aside from frame generation earlier this session (the flip "
                                    "queue stalled); a restart re-arms it.");
            }
        }

        float mipDetail = -dlaa.mipBias;
        LiveControl::SliderFloatD("Texture Detail", mipDetail, 0.0F, 3.0F,
            -kDlaaDefaults.mipBias, [&dlaa](float a_value) {
                dlaa.mipBias = Platform::ClampMipBias(-a_value);
                Platform::Fallout4Renderer::SetMipBias(dlaa.mipBias);
            });
        Help("More texture detail — higher is more detailed. The engine applies it as a negative "
             "mip-LOD bias. Works with or without DLAA (with no temporal resolve, very high values "
             "can shimmer).");
        ImGui::TextDisabled("        engine mip-LOD bias: %.2f", static_cast<double>(dlaa.mipBias));

        LiveControl::SliderFloatD("Texture Sharpness", dlaa.sharpness, 0.0F, 1.0F,
            kDlaaDefaults.sharpness, [&dlaa](float a_value) {
                dlaa.sharpness = Platform::ClampSharpness(a_value);
                Platform::Streamline::SetSharpness(dlaa.sharpness);
            });
        Help("Post-sharpening (RCAS). 0 disables it. Works with or without DLAA — with DLAA it runs "
             "on the resolved frame; without, it sharpens the native frame directly. The same slider "
             "drives FSR's own native RCAS sharpening too.");

        LiveControl::SliderFloatD("Jitter scale", dlaa.jitterScale, 0.0F, 3.0F,
            kDlaaDefaults.jitterScale, [&dlaa](float a_value) {
                dlaa.jitterScale = Platform::ClampJitterScale(a_value);
                Platform::Fallout4Renderer::SetJitterScale(dlaa.jitterScale);
            });
        Help("Halton sub-pixel jitter amplitude. 1.0 is the standard pattern; raising it spreads the "
             "samples wider and can resolve residual pixelation.");

        SectionHeader("Upscaling / anti-aliasing");
        const auto activeEngine = Platform::Streamline::AppliedEffectiveEngine();
        const bool fsrActive = activeEngine == Platform::AaEffectiveEngine::kFsr;
        const bool dlssActive = activeEngine == Platform::AaEffectiveEngine::kDlss;
        const bool dx12Off = Platform::D3D12Sidecar::Unavailable();
        const auto sidecarRecovery = [&](const char* a_id) {
            if (dx12Off || !Platform::D3D12Sidecar::Disabled()) {
                return;
            }
            ImGui::PushID(a_id);
            if (Platform::D3D12Sidecar::Quarantined()) {
                ImGui::TextDisabled("  The DirectX 12 sidecar released a stuck wait from the CPU (F12 forced to %llu): the resources "
                                    "behind it are retained, not freed, until 'Reset & retry DirectX 12' proves the sidecar's queue retired "
                                    "the stalled work.",
                    static_cast<unsigned long long>(Platform::D3D12Sidecar::ForcedValue()));
            } else {
                ImGui::TextDisabled("  DirectX 12 features are off this session: %s", Platform::D3D12Sidecar::DisableReason());
            }
            if (ImGui::SmallButton("Reset & retry DirectX 12")) {
                Platform::D3D12Sidecar::RequestRearm();
            }
            Help("Re-arms the DirectX 12 sidecar: proves the GPU retired the work it was stalled on (up to 10 s), then the "
                 "DirectX 12 DLSS path, DLSS Frame Generation, DLSS 5 neural rendering and DirectX 12 presentation come back "
                 "on their next use. Refused, with the reason in the log, while the proof fails; try again later.");
            ImGui::PopID();
        };
        const auto setEngine = [&dlaa](Platform::AaEngineRequest a_want, bool a_on) {
            if (a_on) {
                if (a_want == Platform::AaEngineRequest::kFsr && dlaa.qualityMode == 1U) {
                    dlaa.qualityMode = 2U;
                    Platform::Streamline::SetQualityMode(dlaa.qualityMode, true);
                }
                dlaa.engineFsr = a_want == Platform::AaEngineRequest::kFsr;
                Platform::Streamline::SetRequestedEngine(a_want);
                dlaa.enable = true;
            } else {
                dlaa.enable = false;
            }
            Platform::Streamline::ApplyDlaaSettingsLive(dlaa.enable, dlaa.preset, dlaa.autoExposure);
            Telemetry::MarkFrameStatsBoundary(
                !dlaa.enable                                ? "AA disabled (native)"
                : a_want == Platform::AaEngineRequest::kFsr ? "FSR enabled"
                                                            : "DLSS enabled");
        };

        const auto drawDlssControls = [&dlaa](const char* a_suffix) {
            char label[64]{};
            std::snprintf(label, sizeof(label), "Resolution%s", a_suffix);
            LiveControl::ComboD(label, dlaa.qualityMode, kDlaaDefaults.qualityMode,
                kQualityModeComboItems, [&dlaa](std::uint32_t a_value) {
                    dlaa.qualityMode = Platform::ClampQualityMode(a_value);
                    Platform::Streamline::SetQualityMode(dlaa.qualityMode, true);
                    char reason[96]{};
                    std::snprintf(reason, sizeof(reason), "DLSS resolution = %s",
                        Platform::DlaaQualityModeName(dlaa.qualityMode));
                    Telemetry::MarkFrameStatsBoundary(reason);
                });
            Help("DLSS render resolution. DLAA = full native resolution (best quality). The "
                 "other modes render the scene at a lower internal resolution and DLSS "
                 "reconstructs the full image - lower GPU cost the stronger the mode; the "
                 "window never changes. Switches live. Composes with Supersampling (TSR).");

            std::uint32_t presetIndex = PresetIndexFromValue(dlaa.preset);
            std::snprintf(label, sizeof(label), "Preset%s", a_suffix);
            LiveControl::ComboD(label, presetIndex, PresetIndexFromValue(kDlaaDefaults.preset),
                kPresetComboItems, [&dlaa](std::uint32_t a_index) {
                    const auto count = static_cast<std::uint32_t>(std::size(kPresetValues));
                    dlaa.preset = kPresetValues[a_index < count ? a_index : 5U];
                    Platform::Streamline::ApplyDlaaSettingsLive(
                        dlaa.enable, dlaa.preset, dlaa.autoExposure);
                });
            Help("DLSS model. Changing this recreates the DLSS feature.");

            std::snprintf(label, sizeof(label), "Auto exposure%s", a_suffix);
            LiveControl::CheckboxD(label, dlaa.autoExposure, kDlaaDefaults.autoExposure,
                [&dlaa](bool a_on) {
                    Platform::Streamline::ApplyDlaaSettingsLive(dlaa.enable, dlaa.preset, a_on);
                });
            Help("NGX analyses image brightness itself. Off = a fixed 1.0 exposure (try it if "
                 "bright objects look dimmed).");

            std::snprintf(label, sizeof(label), "Exposure scale%s", a_suffix);
            LiveControl::SliderFloatD(label, dlaa.exposureScale, 0.1F, 16.0F,
                kDlaaDefaults.exposureScale, [&dlaa](float a_value) {
                    dlaa.exposureScale = Platform::ClampExposureScale(a_value);
                    Platform::Streamline::SetExposureScale(dlaa.exposureScale);
                });
            Help("DLSS exposure multiplier. 1.0 = neutral.");
        };

        ImGui::Spacing();
        ImGui::TextDisabled("------ DLSS ------");
        bool dlssOn = dlaa.enable && !dlaa.engineFsr;
        ImGui::BeginDisabled(dx12Off);
        if (ImGui::Checkbox("Enable DLSS / DLAA", &dlssOn)) {
            setEngine(Platform::AaEngineRequest::kDlss, dlssOn);
        }
        ImGui::EndDisabled();
        Help("NVIDIA DLSS as the anti-aliasing/upscaling engine (RTX adapters). It runs on a private "
             "DirectX 12 device beside the game's renderer - the path that carries DLSS Frame Generation. "
             "Neural rendering is a separate filter and runs beside DLSS or FSR. Ticking this turns FSR off - "
             "one engine runs at a time. Off with FSR also off = native resolution with NO anti-aliasing: the "
             "game's TAA and FXAA stay disabled at the root and are never a fallback.");
        if (dx12Off) {
            ImGui::TextDisabled("Greyed out: no DirectX 12 this session (%s) - DLSS needs the private DirectX 12 "
                                "device. FSR is the only engine here%s.",
                Platform::D3D12Sidecar::DisableReason(),
                dlssOn ? " (the saved DLSS choice falls forward to FSR automatically)" : "");
        }
        if (dlssOn) {
            const bool decided = activeEngine == Platform::AaEffectiveEngine::kNone ||
                                 Platform::AaEffectiveEngineReady(activeEngine);
            if (decided && activeEngine == Platform::AaEffectiveEngine::kNone) {
                ImGui::TextDisabled("DLSS cannot run on this adapter (or is failure-latched) - no AA is "
                                    "active. Never a silent revert; the log has the reason.");
            } else if (decided && fsrActive) {
                ImGui::TextDisabled("DLSS is unavailable here - FSR is running as the automatic fallback "
                                    "(the saved choice stays DLSS).");
            }
            ImGui::Indent();
            if (!dx12Off) {
                const auto dx12 = Platform::Dlss12Engine::Snapshot();
                if (dx12.latched) {
                    ImGui::TextDisabled("DLSS OFF: %s (re-select the engine or change the preset to retry)", dx12.reason);
                    sidecarRecovery("dlss");
                } else if (dx12.featureReady) {
                    ImGui::TextDisabled("DLSS: render %ux%u -> out %ux%u", dx12.renderW, dx12.renderH, dx12.outW, dx12.outH);
                } else if (dlssActive) {
                    ImGui::TextDisabled("DLSS: waiting for the first frame");
                }
            }

            drawDlssControls("");

            ImGui::Unindent();
        }

        ImGui::Spacing();
        ImGui::TextDisabled("------ FSR ------");
        bool fsrOn = dlaa.enable && Platform::RequestedEngineOf(dlaa) == Platform::AaEngineRequest::kFsr;
        if (ImGui::Checkbox("Enable FSR", &fsrOn)) {
            setEngine(Platform::AaEngineRequest::kFsr, fsrOn);
        }
        Help("AMD FidelityFX Super Resolution 3.1 (MIT, vendor-neutral) as the anti-aliasing/"
             "upscaling engine - works on any DX11 GPU with typed-UAV support, including "
             "non-NVIDIA cards (where a DLSS request already falls forward to FSR automatically). "
             "Ticking this turns DLSS off - one engine runs at a time. Switches live through the "
             "same staged handoff as every engine change. An FSR request on an adapter that "
             "cannot run it shows no AA rather than silently reverting.");
        if (fsrOn && activeEngine == Platform::AaEffectiveEngine::kNone) {
            ImGui::TextDisabled("FSR cannot run on this adapter (or is failure-latched) - no AA "
                                "is active. Never a silent revert; the log has the reason.");
        }
        if (fsrOn || (dlaa.enable && fsrActive)) {
            ImGui::Indent();
            static constexpr std::uint32_t kFsrPresetModes[]{ 0U, 2U, 3U, 4U, 5U };
            std::uint32_t fsrPresetIndex = 0U;
            for (std::uint32_t k = 0; k < static_cast<std::uint32_t>(std::size(kFsrPresetModes)); ++k) {
                if (kFsrPresetModes[k] == dlaa.qualityMode) {
                    fsrPresetIndex = k;
                }
            }
            LiveControl::ComboD("Resolution##fsr", fsrPresetIndex, 0U,
                "Native AA\0Quality\0Balanced\0Performance\0Ultra Performance\0",
                [&dlaa](std::uint32_t a_index) {
                    const auto count = static_cast<std::uint32_t>(std::size(kFsrPresetModes));
                    dlaa.qualityMode = kFsrPresetModes[a_index < count ? a_index : 0U];
                    Platform::Streamline::SetQualityMode(dlaa.qualityMode, true);
                    char reason[96]{};
                    std::snprintf(reason, sizeof(reason), "FSR resolution = %s",
                        Platform::DlaaQualityModeName(dlaa.qualityMode));
                    Telemetry::MarkFrameStatsBoundary(reason);
                });
            Help("FSR render resolution preset. Native AA = full resolution (best quality). "
                 "Quality / Balanced / Performance / Ultra Performance render the scene at "
                 "1/1.5, 1/1.7, 1/2 and 1/3 scale and FSR reconstructs the full image - lower "
                 "GPU cost the stronger the preset; the window never changes. Switches live. "
                 "Composes with Supersampling (TSR).");

            LiveControl::SliderFloatD("FSR velocity factor", dlaa.fsrVelocity, 0.0F, 1.0F,
                kDlaaDefaults.fsrVelocity,
                [](float a_value) { Platform::Streamline::SetFsrVelocity(a_value); });
            Help("AMD's own FSR 3.1 tuning constant (fVelocityFactor). 1.0 is the AMD "
                 "default; lowering toward 0.0 can improve temporal stability of bright "
                 "pixels at some cost in responsiveness of moving detail.");

            LiveControl::SliderFloatD("FSR reactiveness scale", dlaa.fsrReactiveness, 0.0F, 2.0F,
                kDlaaDefaults.fsrReactiveness,
                [](float a_value) { Platform::Streamline::SetFsrReactiveness(a_value); });
            Help("AMD's fReactivenessScale. Scales the reactive mask's values, so higher means FSR trusts "
                 "accumulated history less on fast-changing surfaces - less ghosting, more noise. 1.0 is the "
                 "AMD default.");
            LiveControl::SliderFloatD("FSR shading-change scale", dlaa.fsrShadingChange, 0.0F, 2.0F,
                kDlaaDefaults.fsrShadingChange,
                [](float a_value) { Platform::Streamline::SetFsrShadingChange(a_value); });
            Help("AMD's fShadingChangeScale. Raising it makes FSR treat a change in shading as more reactive "
                 "(lighting and material changes reset sooner). 1.0 is the AMD default.");
            LiveControl::SliderFloatD("FSR accumulation per frame", dlaa.fsrAccumulation, 0.0F, 1.0F,
                kDlaaDefaults.fsrAccumulation,
                [](float a_value) { Platform::Streamline::SetFsrAccumulation(a_value); });
            Help("AMD's fAccumulationAddedPerFrame - how fast FSR rebuilds history where the image was "
                 "disoccluded. LOWERING it reduces ghosting behind moving objects but makes thin features "
                 "(grass, wires, antennas) flicker more. 0.333 is the AMD default.");
            LiveControl::SliderFloatD("FSR min disocclusion accumulation", dlaa.fsrMinDisocclusion, -1.0F, 1.0F,
                kDlaaDefaults.fsrMinDisocclusion,
                [](float a_value) { Platform::Streamline::SetFsrMinDisocclusion(a_value); });
            Help("AMD's fMinDisocclusionAccumulation. RAISING it reduces white-pixel flicker around swaying "
                 "thin objects that keep disoccluding each other - foliage and wires in wind - at the cost of "
                 "more ghosting if pushed too far. -0.333 is the AMD default.");

            LiveControl::CheckboxD("FSR reactive + transparency masks", dlaa.fsrMasks, kDlaaDefaults.fsrMasks,
                [](bool a_value) { Platform::Streamline::SetFsrMasks(a_value); });
            Help("Tells FSR which pixels it must not trust from history. An opaque-only copy of the frame is "
                 "compared against the finished frame; where they differ the pixel is translucent or freshly "
                 "composited. Reduces ghost trails behind particles, foliage alpha, glass and effects. Off "
                 "submits no hints and upscales exactly as before.");
            LiveControl::SliderFloatD("FSR transparency scale", dlaa.fsrTransparencyScale, 0.0F, 2.0F,
                kDlaaDefaults.fsrTransparencyScale,
                [](float a_value) { Platform::Streamline::SetFsrTransparencyScale(a_value); });
            Help("How strongly the difference above marks a pixel as transparent. 1.0 is the reference value; "
                 "higher marks more of the frame, which cuts ghosting further but lets more noise through. Only "
                 "does anything while the masks above are on.");

            ImGui::Unindent();
        }

        SectionHeader("Frame generation");
        const auto aaCapabilities = Platform::Streamline::EngineSelectionSnapshot().capabilities;
        const bool dx12Down = dx12Off || Platform::D3D12Sidecar::Disabled();
        const bool dlssFgAvailable = !dx12Down && aaCapabilities.dlss == Platform::AaAvailability::kAvailable;
        const bool fsrAaAvailable = aaCapabilities.fsr == Platform::AaAvailability::kAvailable;
        const bool anyAaAvailable = dlssFgAvailable || fsrAaAvailable;
        const auto fsrFgAvailability = Platform::FsrFrameGen::Snapshot();
        const bool fsrFgAvailable = !dx12Down && anyAaAvailable && fsrFgAvailability.available &&
                                    fsrFgAvailability.chainCreated && fsrFgAvailability.contextReady;
        const auto enableAaFor = [&](bool a_fsrBackend, bool a_mayExchangeEngine) {
            if (!dlaa.frameGeneration) {
                return;
            }
            if (!dlaa.enable) {
                const auto engine = !a_fsrBackend ? Platform::AaEngineRequest::kDlss
                    : (fsrAaAvailable && (dlaa.engineFsr || !dlssFgAvailable)) ? Platform::AaEngineRequest::kFsr
                                                                              : Platform::AaEngineRequest::kDlss;
                logger::info("[FrameGen] auto-enable: {} supplies the AA inputs frame generation reads",
                    engine == Platform::AaEngineRequest::kFsr ? "FSR" : "DLSS / DLAA");
                setEngine(engine, true);
                return;
            }
            if (a_mayExchangeEngine && !a_fsrBackend && dlaa.engineFsr) {
                logger::info("[FrameGen] DLSS / DLAA requested: DLSS-G cannot generate on the FSR upscaler");
                setEngine(Platform::AaEngineRequest::kDlss, true);
            } else if (a_fsrBackend && dlaa.engineFsr && !fsrAaAvailable && dlssFgAvailable) {
                logger::info("[FrameGen] auto-enable: DLSS / DLAA supplies the AA inputs (FSR is unavailable this session)");
                setEngine(Platform::AaEngineRequest::kDlss, true);
            }
        };
        const bool selectedFgAvailable = dlaa.fsrFrameGeneration ? fsrFgAvailable : dlssFgAvailable;
        ImGui::BeginDisabled(!selectedFgAvailable && !dlaa.frameGeneration);
        LiveControl::CheckboxD("Frame generation", dlaa.frameGeneration, kDlaaDefaults.frameGeneration,
            [&](bool a_on) {
                enableAaFor(dlaa.fsrFrameGeneration, false);
                Platform::FsrFrameGen::SetSelected(dlaa.fsrFrameGeneration);
                if (a_on) Platform::FrameGenEngine::RequestRetry();
                Platform::FrameGenEngine::SetEnabled(a_on);
                Telemetry::MarkFrameStatsBoundary(a_on ? "Frame generation ON" : "Frame generation OFF");
            });
        ImGui::EndDisabled();
        Help("Master switch for the selected frame-generation backend. Enabling it also enables the required "
             "upscaler. Both backends pause while the menu or a loading screen is open.");

        const auto selectFgBackend = [&](bool a_fsr) {
            dlaa.fsrFrameGeneration = a_fsr;
            enableAaFor(a_fsr, true);
            if (dlaa.frameGeneration) Platform::FrameGenEngine::RequestRetry();
            Platform::FsrFrameGen::SetSelected(a_fsr);
            logger::info("[FrameGen] backend requested: {}", a_fsr ? "FSR-FG" : "DLSS-G");
            Telemetry::MarkFrameStatsBoundary(a_fsr ? "Frame generation backend: FSR-FG" : "Frame generation backend: DLSS-G");
        };
        ImGui::TextUnformatted("Backend:");
        ImGui::SameLine();
        ImGui::BeginDisabled(!dlssFgAvailable);
        if (ImGui::RadioButton("DLSS-G", !dlaa.fsrFrameGeneration) && dlaa.fsrFrameGeneration) {
            selectFgBackend(false);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!fsrFgAvailable);
        if (ImGui::RadioButton("FSR-FG", dlaa.fsrFrameGeneration) && !dlaa.fsrFrameGeneration) {
            selectFgBackend(true);
        }
        ImGui::EndDisabled();
        Help("Switches the generator live on the same swap chain. DLSS-G needs DLSS / DLAA and supports 2x to 6x. "
             "FSR-FG works with either upscaler and keeps your current upscaler when it is enabled and available. "
             "Selecting a backend while frame generation is off keeps it off. A grey choice is unavailable this session.");
        if (!dlaa.fsrFrameGeneration) {
            if (!dlaa.frameGeneration) {
                ImGui::TextDisabled("  Frame generation is OFF (the master above). The controls below take effect once it is on.");
            }
            ImGui::BeginDisabled(!dlssFgAvailable || !dlaa.frameGeneration);
            {
                LiveControl::CheckboxD("Dynamic multiplier##fg", dlaa.frameGenerationDynamic, kDlaaDefaults.frameGenerationDynamic,
                    [&](bool a_on) {
                        Platform::FrameGenEngine::SetDynamic(a_on);
                        Telemetry::MarkFrameStatsBoundary(a_on ? "Frame generation dynamic ON" : "Frame generation dynamic OFF");
                    });
                Help("Varies the generated-frame count per real frame to hold a target output rate (the display's refresh "
                     "by default), the way NVIDIA's Dynamic Multi Frame Generation does, through this mod's own controller: "
                     "the feature is created for the session's maximum and each real-frame pair asks for what the measured "
                     "interval needs (NVIDIA's header defines the count per pair). One step at a time, after a third of a "
                     "second of agreement. The fixed Multiplier below is ignored while this is on. If the runtime refuses a "
                     "per-pair count the log says so and the feature is re-created on each change instead.");
                if (dlaa.frameGenerationDynamic) {
                    int targetHz = static_cast<int>(dlaa.frameGenerationDynamicTargetHz);
                    if (ImGui::InputInt("Target Hz (0 = display)##fg", &targetHz, 0, 0)) {
                        dlaa.frameGenerationDynamicTargetHz = Platform::ClampDynamicTargetHz(targetHz < 0 ? 0U : static_cast<std::uint32_t>(targetHz));
                        Platform::FrameGenEngine::SetDynamicTargetHz(dlaa.frameGenerationDynamicTargetHz);
                    }
                    Help("0 follows the refresh rate of the monitor the game window is on (queried every 2 s). Otherwise 30 to 1000 Hz.");
                    const auto dyn = Platform::FrameGenEngine::Snapshot();
                    ImGui::TextDisabled("  now %ux (%u generated per real frame) | real frame period %.2f ms = %.0f Hz presented | target %s%.0f Hz | display %.0f Hz%s",
                        dyn.pairFrames + 1U, dyn.pairFrames, static_cast<double>(dyn.pairIntervalMs),
                        Platform::DynamicMultiplier::PresentedHz(dyn.pairFrames, static_cast<double>(dyn.pairIntervalMs)),
                        dyn.dynamicTargetHz != 0U ? "" : "display ", dyn.dynamicTargetHz != 0U ? static_cast<double>(dyn.dynamicTargetHz) : static_cast<double>(dyn.displayHz),
                        static_cast<double>(dyn.displayHz),
                        dyn.dynamicRefused ? " | the runtime refused per-pair counts: re-creating on each change" : "");
                }
                ImGui::BeginDisabled(dlaa.frameGenerationDynamic);
                int multiplierIndex = static_cast<int>(Platform::ClampFrameGenerationFrames(dlaa.frameGenerationFrames)) - 1;
                static constexpr const char* kBoundaryLabels[]{ "Frame generation 2x", "Frame generation 3x",
                    "Frame generation 4x", "Frame generation 5x", "Frame generation 6x" };
                if (ImGui::Combo("Multiplier##fg", &multiplierIndex,
                        "2x (one generated frame)\0" "3x (two)\0" "4x (three)\0" "5x (four)\0" "6x (five)\0")) {
                    dlaa.frameGenerationFrames = static_cast<std::uint32_t>(multiplierIndex + 1);
                    Platform::FrameGenEngine::SetFrames(dlaa.frameGenerationFrames);
                    Telemetry::MarkFrameStatsBoundary(kBoundaryLabels[multiplierIndex < 0 ? 0 : multiplierIndex > 4 ? 4 : multiplierIndex]);
                }
                Help("Generated frames per real frame, 2x to 6x. Above 2x is DLSS Multi Frame Generation: the runtime opens "
                     "it on Blackwell (RTX 50); on RTX 40 this mod rewrites the runtime's two architecture gates in memory the "
                     "moment the NGX core maps the runtime, and corrects the interpolation timing its kernel ships with (in this "
                     "process only, nothing on disk; the [MFG] log lines say whether both applied). Live: the feature is re-created with the new count; if the "
                     "runtime refuses, the session falls back to 2x and the log says so. Our own present thread paces the "
                     "generated frames at even fractions of the frame interval.");
                ImGui::EndDisabled();
                {
                    float separation = dlaa.frameGenerationDepthSeparation;
                    if (ImGui::SliderFloat("Depth-edge separation##fg", &separation, 1.0F, 1000.0F, "%.0f", ImGuiSliderFlags_Logarithmic)) {
                        dlaa.frameGenerationDepthSeparation = Platform::ClampDepthSeparation(separation);
                        Platform::FrameGenEngine::SetDepthSeparation(dlaa.frameGenerationDepthSeparation);
                    }
                    ImGui::SameLine();
                    if (ImGui::SmallButton("40##fgsep")) {
                        dlaa.frameGenerationDepthSeparation = Platform::kDepthSeparationDefault;
                        Platform::FrameGenEngine::SetDepthSeparation(dlaa.frameGenerationDepthSeparation);
                    }
                    Help("Experimental. DLSS-G's minimum relative linear-depth separation between two objects, the "
                         "runtime's own disocclusion heuristic (NVIDIA's header: 'optional', default 40). Lower values may "
                         "improve disocclusion around nearby objects such as the hands and weapon against distant "
                         "background in some scenes; the best value is integration-specific. 40 is never pushed on a fresh "
                         "session; another value is pushed when it changes, and 40 is pushed once more to restore the "
                         "default afterwards.");
                }
            }
            Help("DLSS Frame Generation (DLSS-G, NGX feature 11 on the DirectX 12 device) shows the chosen number of "
                 "generated frames between every two rendered ones (2x to 6x, the Multiplier), paced at even fractions "
                 "of the measured frame interval by the proxy's present thread. Needs the DirectX 12 path (the DirectX 12 present proxy is always "
                 "on). Real frames only while the menu or a loading screen is up. The "
                 "[FrameGen] and [Proxy] log lines carry the facts; the [Perf] line counts REAL frames, the "
                 "OSD's FPS row shows real + generated. "
                 "KNOWN LIMIT: the game's HUD and this mod's OSD are not recomposited onto generated frames "
                 "(Fallout 4 has no separate UI buffer to hand the runtime), so HUD elements update at the "
                 "real-frame rate while interpolating.");
            ImGui::EndDisabled();
        }

        const auto fg = Platform::FrameGenEngine::Snapshot();
        const auto fsrFg = Platform::FsrFrameGen::Snapshot();
        const auto fgProxy = Platform::PresentProxy::Snapshot();
        const char* const fgName = dlaa.fsrFrameGeneration ? "FSR-FG" : "DLSS-G";
        const char* const fgBlocked = [&]() -> const char* {
            if (dx12Down) {
                return Platform::D3D12Sidecar::DisableReason();
            }
            if (fgProxy.d3d11Fallback) {
                return "presentation fell back to DirectX 11";
            }
            if (!fgProxy.active) {
                return fgProxy.reason[0] != '\0' ? fgProxy.reason : "no swap chain has been proxied yet";
            }
            if (aaCapabilities.state == Platform::AaCapabilityState::kUnknown) {
                return nullptr;
            }
            if (aaCapabilities.state != Platform::AaCapabilityState::kResolved) {
                return "device capabilities could not be confirmed";
            }
            if (dlaa.fsrFrameGeneration) {
                if (!anyAaAvailable) {
                    return "no upscaler is available to supply its inputs";
                }
                if (!fsrFg.available || !fsrFg.chainCreated || !fsrFg.contextReady) {
                    return fsrFg.reason[0] != '\0' ? fsrFg.reason : "the frame-generation context is not ready";
                }
            } else if (!dlssFgAvailable) {
                return "no DLSS-capable adapter this session";
            }
            return nullptr;
        }();
        if (!dlaa.frameGeneration) {
            if (fgBlocked != nullptr) {
                ImGui::TextDisabled("Frame generation: off (%s selected, and unavailable this session: %s)", fgName, fgBlocked);
            } else {
                ImGui::TextDisabled("Frame generation: off (%s selected)", fgName);
            }
        } else if (fgBlocked != nullptr) {
            const bool dlssGeneratesInstead = dlaa.fsrFrameGeneration && dlssFgAvailable && fgProxy.active &&
                !fgProxy.d3d11Fallback && !fg.latched && dlaa.enable &&
                Platform::Streamline::AppliedEffectiveEngine() == Platform::AaEffectiveEngine::kDlss;
            if (dlssGeneratesInstead) {
                ImGui::TextDisabled("FSR-FG unavailable: %s - DLSS-G generates instead", fgBlocked);
            } else {
                ImGui::TextDisabled("%s unavailable: %s", fgName, fgBlocked);
            }
            sidecarRecovery("fg");
        } else if (aaCapabilities.state == Platform::AaCapabilityState::kUnknown) {
            ImGui::TextDisabled("%s selected: waiting for device capabilities", fgName);
        } else if (dlaa.fsrFrameGeneration ? fsrFg.latched : fg.latched) {
            ImGui::TextDisabled("%s stopped: %s", fgName,
                dlaa.fsrFrameGeneration ? "AMD's runtime refused repeated generation attempts" : fg.reason);
            ImGui::SameLine();
            if (ImGui::SmallButton("Retry##fg")) {
                enableAaFor(dlaa.fsrFrameGeneration, false);
                Platform::FrameGenEngine::RequestRetry();
                if (dlaa.fsrFrameGeneration) Platform::FsrFrameGen::ReArmAfterEnable();
            }
        } else if (!dlaa.enable) {
            ImGui::TextDisabled("%s selected: anti-aliasing is off - turn it back on above, because frame "
                                "generation reads the upscaler's own depth and motion vectors", fgName);
        } else if (!dlaa.fsrFrameGeneration && Platform::Streamline::AppliedEffectiveEngine() != Platform::AaEffectiveEngine::kDlss) {
            if (dlaa.engineFsr) {
                ImGui::TextDisabled("DLSS-G cannot generate on the FSR upscaler: choose DLSS / DLAA above, or pick FSR-FG");
            } else {
                ImGui::TextDisabled("DLSS-G selected: waiting for the DLSS / DLAA handoff");
            }
        } else if (dlaa.fsrFrameGeneration && !Platform::AaEffectiveEngineReady(Platform::Streamline::AppliedEffectiveEngine())) {
            ImGui::TextDisabled("FSR-FG selected: waiting for the upscaler handoff");
        } else if (dlaa.fsrFrameGeneration && fg.latched) {
            ImGui::TextDisabled("FSR-FG selected: generating without the HUD-less, so the HUD ghosts on generated frames (%s)", fg.reason);
            ImGui::SameLine();
            if (ImGui::SmallButton("Retry##fg")) {
                Platform::FrameGenEngine::RequestRetry();
            }
        } else {
            ImGui::TextDisabled("%s selected: real frames while the menu is open", fgName);
        }

        {
            SectionHeader("Neural Rendering Filter");
            auto& settings = Settings::Menu();
            auto& cascade = settings.neural;
            const auto neural = Platform::NeuralPass::Snapshot();
            const auto pushNeural = [&settings]() {
                Platform::NeuralPass::ApplyMenuSettings(settings);
                Settings::MarkDirty();
            };
            ImGui::BeginDisabled(dx12Off);
            LiveControl::CheckboxD("Enable neural rendering##nr", cascade.enabled,
                kMenuDefaults.neural.enabled, [&](bool on) {
                    pushNeural();
                    Telemetry::MarkFrameStatsBoundary(on ? "Neural rendering ON" : "Neural rendering OFF");
                });
            if (cascade.enabled) {
                std::uint32_t selection = Platform::Neural::ClampPassCount(cascade.passCount) - 1U;
                LiveControl::ComboD("Passes##nr", selection, kMenuDefaults.neural.passCount - 1U,
                    "Standard (1 pass)\0Extreme (2 passes)\0Extreme (3 passes)\0", [&](std::uint32_t value) {
                        cascade.passCount = Platform::Neural::ClampPassCount(value + 1U);
                        pushNeural();
                        Telemetry::MarkFrameStatsBoundary("Neural pass count changed");
                    });
                Help("Each pass processes the preceding pass's result with its own settings. Pass 1 sees the finished "
                     "scene and keeps the temporal history; the later passes run stateless on that already-stable image "
                     "(a second history on top of the first flickers, so the later passes run stateless; "
                     "judge Extreme against the original image, not only against the previous pass). Two or three passes can strengthen "
                     "the result and also amplify artifacts; each adds GPU work. Hidden pass settings are kept when "
                     "reducing the count.");
                {
                    const std::uint32_t outW = neural.outWidth, outH = neural.outHeight;
                    const bool beyondPlay = cascade.modelBeyondPlay;
                    std::uint32_t shownPercent = cascade.modelPercent;
                    LiveControl::SliderIntCommitD("Model resolution##nr", cascade.modelPercent,
                        static_cast<int>(Platform::Neural::kModelPercentMin),
                        static_cast<int>(beyondPlay ? Platform::Neural::kModelPercentMax : Platform::Neural::kModelPercentPlayMax),
                        kMenuDefaults.neural.modelPercent, "%d%%", shownPercent, [&](std::uint32_t) {
                            pushNeural();
                            Telemetry::MarkFrameStatsBoundary("Neural model resolution changed");
                        });
                    const auto preview = Platform::Neural::ComputeModelExtent(outW, outH, shownPercent);
                    ImGui::SameLine();
                    if (outW == 0U || outH == 0U) {
                        ImGui::TextDisabled("(the output size is not known yet)");
                    } else if (preview.Valid()) {
                        ImGui::TextDisabled("%u x %u of %u x %u", preview.width, preview.height, outW, outH);
                    } else {
                        ImGui::TextDisabled("unavailable: below the 32 px floor at %u x %u", outW, outH);
                    }
                    Help("The size the model works on, as a share of the output (50 to 150 %; 200 % and 300 % open with the gate below). 100 % is the exact path: the "
                         "model reads and writes the output's own size. Below 100 % the model works on a reduced image and "
                         "only its repaint (the difference it made) is enlarged back onto the untouched full-resolution "
                         "frame: less model work, coarser model detail, the game's own pixels unchanged. Above 100 % the "
                         "model works on an enlarged image and its repaint is reduced back: finer model scale, higher cost "
                         "(a quality knob; the model's time scales with the pixel count: 4x at 200 %, 9x at 300 %, and a "
                         "size the runtime refuses is reported here). Applies when the slider is released (the features are re-created at the new "
                         "size); the status line and the OSD show the size in effect. Native carrier only: an FP16 or "
                         "sRGB-typed frame buffer stays at 100 % and the status says so.");
                    if (neural.modelReason[0] != '\0') {
                        ImGui::TextDisabled("  not applied: %s", neural.modelReason);
                    }
                    char gateLabel[64]{};
                    std::snprintf(gateLabel, sizeof(gateLabel), "Allow model resolution above %u%%##nr", Platform::Neural::kModelPercentPlayMax);
                    LiveControl::CheckboxD(gateLabel, cascade.modelBeyondPlay, kMenuDefaults.neural.modelBeyondPlay, [&](bool on) {
                        const bool lowered = !on && cascade.modelPercent > Platform::Neural::kModelPercentPlayMax;
                        if (lowered) {
                            cascade.modelPercent = Platform::Neural::kModelPercentPlayMax;
                        }
                        pushNeural();
                        if (lowered) {
                            Telemetry::MarkFrameStatsBoundary("Neural model resolution changed");
                        }
                    });
                    Help("Opens 200 % and 300 %. The model's time grows with its pixel count: about 1.7x the 150 % cost at 200 % and "
                         "4x at 300 % (measured at 1080p on an RTX 4090: 6.5, 10.8 and 25.1 ms per pass). These are still-shot "
                         "extents, not for active play. Closing the gate brings a higher setting down to 150 %.");
                    if (beyondPlay) {
                        ImGui::TextColored(ImVec4(1.0F, 0.80F, 0.25F, 1.0F),
                            "  Above %u%% the model's cost grows with its pixel count (about 4x at 300%%): for still shots, not active play.",
                            Platform::Neural::kModelPercentPlayMax);
                    }
                }
            }
            ImGui::EndDisabled();
            Help("NVIDIA DLSS 5 neural rendering repaints the finished scene using its depth and motion vectors. "
                 "This independent filter works beside DLSS or FSR, before the HUD. Detailed per-pass status and "
                 "runtime identity are in Debug. The frame-generation multiplier is a separate setting.");
            ImGui::TextDisabled("%s", dx12Off ? Platform::D3D12Sidecar::DisableReason() : neural.status);
            if (cascade.enabled && !dx12Off) {
                const std::uint32_t count = Platform::Neural::ClampPassCount(cascade.passCount);
                for (std::uint32_t i = 0; i < count; ++i) {
                    ImGui::PushID("neural-stage");
                    ImGui::PushID(static_cast<int>(i));
                    char label[32]{};
                    std::snprintf(label, sizeof(label), "Pass %u", i + 1U);
                    if (ImGui::CollapsingHeader(label, ImGuiTreeNodeFlags_DefaultOpen)) {
                        ImGui::Indent();
                        auto& pass = cascade.passes[i];
                        const auto& defaults = kMenuDefaults.neural.passes[i];
                        LiveControl::ComboD("Style##nr", pass.style, defaults.style,
                            "Default\0Natural\0Cinematic\0", [&](std::uint32_t) { pushNeural(); });
                        Help("The look of this pass. Applies live; later passes process this configured result.");
                        LiveControl::SliderFloatD("Intensity##nr", pass.intensity, 0.0F, 1.0F,
                            defaults.intensity, [&](float) { pushNeural(); });
                        Help("Strength of this pass's repaint. The supported range stays within 0 to 1.");
                        LiveControl::SliderFloatD("Local tone##nr", pass.localTone, 0.0F, 1.0F,
                            defaults.localTone, [&](float) { pushNeural(); });
                        Help("This pass's low-frequency lighting and colour response.");
                        LiveControl::SliderFloatD("Local structure##nr", pass.localStructure, 0.0F, 1.0F,
                            defaults.localStructure, [&](float) { pushNeural(); });
                        Help("This pass's fine material and surface detail.");
                        bool autoSkin = !pass.autoMask;
                        LiveControl::CheckboxD("Auto skin##nr", autoSkin, !defaults.autoMask, [&](bool automatic) {
                            pass.autoMask = !automatic;
                            pushNeural();
                        });
                        Help("Checked: skin follows Local structure automatically. Unchecked: use the separate "
                             "skin mask and Skin structure strength for this pass.");
                        ImGui::BeginDisabled(!pass.autoMask);
                        LiveControl::SliderFloatD("Skin structure##nr", pass.skinStructure, 0.0F, 1.0F,
                            defaults.skinStructure, [&](float) { pushNeural(); });
                        ImGui::EndDisabled();
                        Help("Structure strength on skin (DLSSNR.SkinStructureStrength, 0-1; 1.0 = the stable maximum), inside "
                             "the semantic skin mask - active while 'Auto skin' is unchecked. Sent as a plain value: the old -1 "
                             "'auto' moved the world in motion, never the skin.");
                        ImGui::Unindent();
                    }
                    ImGui::PopID();
                    ImGui::PopID();
                }
                if (ImGui::Button("Reset & retry filter##nr")) Platform::NeuralPass::RequestRetry();
                sidecarRecovery("nr");
                Help("Clears every pass's failure latch and re-creates the features. A runtime that raised an exception "
                     "stays off until the game restarts - the retry cannot help there, and the status line keeps the reason.");
            }
        }

        ImGui::EndTabItem();
    }

    namespace
    {
        [[nodiscard]] ImGuiSliderFlags FlagsFor(const Platform::CatalogueRow& a_row) noexcept
        {
            ImGuiSliderFlags flags = ImGuiSliderFlags_AlwaysClamp;
            if (a_row.scale == Platform::SliderScale::kLogarithmic) {
                flags |= ImGuiSliderFlags_Logarithmic;
            }
            const double span = a_row.range.max - a_row.range.min;
            const bool fewSteps = a_row.type == Platform::ValueType::kInt && span <= 16.0;
            if (fewSteps) {
                flags |= ImGuiSliderFlags_NoInput;
            }
            return flags;
        }

        [[nodiscard]] const char* FormatFor(const Platform::CatalogueRow& a_row) noexcept
        {
            if (a_row.type == Platform::ValueType::kInt) {
                return "%.0f";
            }
            return a_row.range.max <= 10.0 ? "%.6f" : "%.2f";
        }
    }

    void Menu::DrawCatalogueRow(const Platform::CatalogueRow& a_row, std::size_t a_globalIndex)
    {
        ImGui::PushID(static_cast<int>(a_globalIndex));

        const double stored = Platform::OwnedSettings::Value(a_globalIndex);

        if (std::strcmp(a_row.id, "Fog.Mode") == 0) {
            const auto mode = static_cast<int>(stored + 0.5);
            ImGui::TextUnformatted("Fog");
            const auto pick = [&](const char* a_label, int a_value) {
                if (ImGui::RadioButton(a_label, mode == a_value)) {
                    Platform::OwnedSettings::SetValue(a_globalIndex, static_cast<double>(a_value));
                    Platform::OwnedSettings::ApplyRow(a_row, a_globalIndex);
                    Settings::MarkDirty();
                }
            };
            pick("Enabled", 0);
            ImGui::SameLine();
            pick("Disabled", 1);
            ImGui::SameLine();
            pick("Custom distance", 2);
            Help(a_row.help);
            ImGui::PopID();
            return;
        }

        const bool promotedButUnowned =
            (std::strcmp(a_row.id, "Ssr.Toggle") == 0 && !Platform::SsrSwitch::Owns()) ||
            (std::strcmp(a_row.id, "Scene.LensFlare") == 0 &&
                !Platform::SettingCommands::Owns("lf"));
        if (promotedButUnowned) {
            bool shown = stored >= 0.5;
            ImGui::BeginDisabled();
            ImGui::Checkbox(a_row.label, &shown);
            ImGui::EndDisabled();
            Help("This control could not bind to the engine on this build, and its console "
                 "command only flips state rather than setting it — so the mod will not touch "
                 "it rather than move it at random. The console command is left available.");
            ImGui::PopID();
            return;
        }

        const bool ownedSettingUnbound =
            (a_row.tier == Platform::Tier::kOwnedSetting &&
                !Platform::SettingCommands::Owns(a_row.command)) ||
            (a_row.tier == Platform::Tier::kOwnedModule && !Platform::FovOwner::Owns());
        if (ownedSettingUnbound) {
            ImGui::TextDisabled("(this control could not bind to the engine on this build - it is "
                                "inactive)");
            ImGui::BeginDisabled();
        }

        if (std::strcmp(a_row.id, "Ssr.Toggle") == 0 && Platform::SsrSwitch::Owns() &&
            Platform::SsrSwitch::EngineDemand() && !Platform::SsrSwitch::IntentEnabled()) {
            ImGui::TextDisabled("(the scene is requesting reflections — the engine keeps them on "
                                "while this is off)");
        }

        if (a_row.type != Platform::ValueType::kNone) {
            const double target = Platform::OwnedSettings::ResetTargetFor(a_globalIndex);
            const bool atTarget = std::fabs(stored - target) < 1e-9;

            ImGui::BeginDisabled(atTarget);
            if (ImGui::SmallButton("Rst")) {
                Platform::OwnedSettings::SetValue(a_globalIndex, target);
                Platform::OwnedSettings::ApplyRow(a_row, a_globalIndex);
                Settings::MarkDirty();
            }
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip("Reset to %.6g\n(%s)%s", target,
                    Platform::OwnedSettings::ResetTargetIsEngineBaseline(a_globalIndex)
                        ? "the value your game had at startup"
                        : "the game's vanilla default",
                    atTarget ? "\nAlready at this value." : "");
            }
            ImGui::SameLine();
        }

        switch (a_row.type) {
        case Platform::ValueType::kNone: {
            if (ImGui::Button(a_row.label)) {
                Platform::OwnedSettings::ApplyRow(a_row, a_globalIndex);
            }
            break;
        }
        case Platform::ValueType::kBool: {
            bool on = stored >= 0.5;
            if (ImGui::Checkbox(a_row.label, &on)) {
                Platform::OwnedSettings::SetValue(a_globalIndex, on ? 1.0 : 0.0);
                Platform::OwnedSettings::ApplyRow(a_row, a_globalIndex);
                Settings::MarkDirty();
            }
            break;
        }
        default: {
            auto value = static_cast<float>(stored);
            if (ImGui::SliderFloat(a_row.label, &value, static_cast<float>(a_row.range.min),
                    static_cast<float>(a_row.range.max), FormatFor(a_row), FlagsFor(a_row))) {
                Platform::OwnedSettings::SetValue(a_globalIndex, static_cast<double>(value));

                if (std::strcmp(a_row.id, "Fog.Distance") == 0) {
                    const std::size_t modeIndex = Platform::GlobalIndexForId("Fog.Mode");
                    Platform::OwnedSettings::SetValue(modeIndex,
                        static_cast<double>(Platform::CommandComposer::FogMode::kCustom));
                }

                Platform::OwnedSettings::ApplyRow(a_row, a_globalIndex);
                Settings::MarkDirty();
            }
            break;
        }
        }

        if (ownedSettingUnbound) {
            ImGui::EndDisabled();
        }

        if (a_row.help != nullptr && a_row.help[0] != '\0') {
            Help(a_row.help);
        }
        ImGui::PopID();
    }

    void Menu::DrawSectionHeading(const char* a_section)
    {
        ImGui::Spacing();
        ImGui::TextDisabled("-------- %s --------", a_section);
        ImGui::Spacing();
    }

    void Menu::DrawSectionExtras(const char* a_group, const char* a_section)
    {
        if (std::strcmp(a_group, "Lighting") != 0) {
            return;
        }
        if (std::strcmp(a_section, "Scene Exposure") == 0) {
            bool bypass = Platform::EngineImod::LightShapeSpecialTestRequested();
            if (ImGui::Checkbox("Disable scene exposure", &bypass)) {
                Platform::EngineImod::SetLightShapeSpecialTest(bypass);
                Settings::Menu().sceneExposureDisabled = bypass;
                Settings::MarkDirty();
            }
            Help("Switches the engine's scene-exposure stage off entirely (its bypass value, "
                 "found by testing) - the Scene exposure slider is overridden while this is "
                 "on. An exposure-LIKE effect; what the shader actually is remains underived. "
                 "Unrelated to both the game's auto-exposure and DLSS auto-exposure.");

            bool adaptationOff = Platform::EngineImod::AdaptationOffRequested();
            if (ImGui::Checkbox("Disable game auto-exposure", &adaptationOff)) {
                Platform::EngineImod::SetAdaptationOff(adaptationOff);
                Settings::Menu().gameAutoExposureDisabled = adaptationOff;
                Settings::MarkDirty();
            }
            Help("Stops the GAME's auto-exposure (eye-adapt speed and strength forced to zero "
                 "through the reversible modifier layer) - exposure holds steady instead of "
                 "drifting between bright and dark areas.\n\nThis is the game engine's "
                 "auto-exposure, separate from the DLSS auto-exposure option. (Renamed from "
                 "'adaptation': Fallout 4 uses that word for an unrelated internal technique.)");
            return;
        }
    }

    void Menu::DrawCatalogueGroup(const char* a_group)
    {
        const bool isGrading = std::strcmp(a_group, "Colour grading") == 0;

        bool gradingOn = false;
        if (isGrading) {
            gradingOn = Platform::GradingPass::Enabled();
            if (ImGui::Checkbox("Enable colour grading", &gradingOn)) {
                Platform::GradingPass::SetEnabled(gradingOn);
                Settings::Menu().gradingEnabled = gradingOn;
                Settings::MarkDirty();
            }
            Help("Off by default; saved with your settings and restored at launch.\n\n"
                 "Saturation, brightness, contrast and tint are applied by this mod's own pass "
                 "AFTER the game's per-weather grading - the engine's values are never written, "
                 "your weather art stays untouched, and switching this off restores vanilla "
                 "instantly.");
            ImGui::SameLine();
            bool gradingNeutral = Platform::EngineImod::EngineGradingNeutralRequested();
            if (ImGui::Checkbox("Neutral engine grading", &gradingNeutral)) {
                Platform::EngineImod::SetEngineGradingNeutral(gradingNeutral);
                Settings::Menu().engineGradingNeutral = gradingNeutral;
                Settings::MarkDirty();
            }
            Help("Forces the ENGINE's own per-weather saturation/brightness/contrast to exactly "
                 "1.0 (neutral) so this mod's grading sliders are the only colour hand on the "
                 "wheel. Reversible the moment it is unticked; the weather's values are never "
                 "written.\n\nMost weathers already sit at neutral (measured), so this often "
                 "changes nothing visibly - it matters in the weathers that DO grade.");
            if (!gradingOn) {
                ImGui::TextDisabled("Grading is off - your game's own per-weather colour is in "
                                    "charge.");
            }
            ImGui::Separator();
        }

        if (std::strcmp(a_group, "Debug") == 0) {
            bool radialOff = Platform::EngineImod::RadialBlurOffRequested();
            if (ImGui::Checkbox("Suppress radial blur", &radialOff)) {
                Platform::EngineImod::SetRadialBlurOff(radialOff);
                Settings::Menu().radialBlurSuppressed = radialOff;
                Settings::MarkDirty();
            }
            Help("Drives the engine's radial-blur channels (hit/explosion screen smear) to "
                 "zero through the reversible modifier layer. Verdict still pending a natural "
                 "gameplay trigger - if hits still smear, the fallback is a root veto.");
            ImGui::SameLine();
            bool doubleOff = Platform::EngineImod::DoubleVisionOffRequested();
            if (ImGui::Checkbox("Suppress double vision", &doubleOff)) {
                Platform::EngineImod::SetDoubleVisionOff(doubleOff);
                Settings::Menu().doubleVisionSuppressed = doubleOff;
                Settings::MarkDirty();
            }
            Help("Drives the engine's double-vision channel (concussion/drug effect) to zero "
                 "through the same reversible layer.");
            ImGui::Separator();
        }

        if (isGrading || std::strcmp(a_group, "Lighting") == 0 ||
            std::strcmp(a_group, "Debug") == 0) {
            const auto imod = Platform::EngineImod::Snapshot();
            if (imod.phase == Platform::EngineImod::Phase::kUnavailable) {
                ImGui::TextDisabled("Engine-modifier layer unavailable this session - the log "
                                    "has the reason; nothing was changed.");
            } else if (!imod.sismeOn) {
                ImGui::TextDisabled("The game's IMOD system is disabled (sisme 0) - the engine "
                                    "controls here have no effect until it is re-enabled.");
            }
        }

        const bool isLighting = std::strcmp(a_group, "Lighting") == 0;
        const bool isDebug = std::strcmp(a_group, "Debug") == 0;
        bool anyModified =
            (isGrading && (Platform::GradingPass::Enabled() ||
                              Platform::EngineImod::EngineGradingNeutralRequested())) ||
            (isLighting && (Platform::EngineImod::LightShapeSpecialTestRequested() ||
                               Platform::EngineImod::AdaptationOffRequested())) ||
            (isDebug && (Platform::EngineImod::RadialBlurOffRequested() ||
                            Platform::EngineImod::DoubleVisionOffRequested()));
        Platform::VisitCatalogue([&](const Platform::CatalogueRow& a_row, std::size_t a_index) {
            if (std::strcmp(a_row.group, a_group) != 0 ||
                a_row.type == Platform::ValueType::kNone) {
                return;
            }
            const double target = Platform::OwnedSettings::ResetTargetFor(a_index);
            if (std::fabs(Platform::OwnedSettings::Value(a_index) - target) >= 1e-9) {
                anyModified = true;
            }
        });

        ImGui::BeginDisabled(!anyModified);
        if (ImGui::SmallButton("Reset this group")) {
            Platform::VisitCatalogue([&](const Platform::CatalogueRow& a_row, std::size_t a_index) {
                if (std::strcmp(a_row.group, a_group) != 0 ||
                    a_row.type == Platform::ValueType::kNone) {
                    return;
                }
                Platform::OwnedSettings::SetValue(
                    a_index, Platform::OwnedSettings::ResetTargetFor(a_index));
                Platform::OwnedSettings::ApplyRow(a_row, a_index);
            });
            auto& menuSettings = Settings::Menu();
            if (isGrading) {
                if (Platform::GradingPass::Enabled()) {
                    Platform::GradingPass::SetEnabled(false);
                }
                if (Platform::EngineImod::EngineGradingNeutralRequested()) {
                    Platform::EngineImod::SetEngineGradingNeutral(false);
                }
                menuSettings.gradingEnabled = false;
                menuSettings.engineGradingNeutral = false;
            } else if (isLighting) {
                if (Platform::EngineImod::LightShapeSpecialTestRequested()) {
                    Platform::EngineImod::SetLightShapeSpecialTest(false);
                }
                if (Platform::EngineImod::AdaptationOffRequested()) {
                    Platform::EngineImod::SetAdaptationOff(false);
                }
                menuSettings.sceneExposureDisabled = false;
                menuSettings.gameAutoExposureDisabled = false;
            } else if (isDebug) {
                if (Platform::EngineImod::RadialBlurOffRequested()) {
                    Platform::EngineImod::SetRadialBlurOff(false);
                }
                if (Platform::EngineImod::DoubleVisionOffRequested()) {
                    Platform::EngineImod::SetDoubleVisionOff(false);
                }
                menuSettings.radialBlurSuppressed = false;
                menuSettings.doubleVisionSuppressed = false;
            }
            Settings::MarkDirty();
        }
        ImGui::EndDisabled();
        Help("Puts every control in this group back to its reset value - the value your game had "
             "at startup for settings we own, the vanilla default for the rest. Greyed out when "
             "nothing here has been changed.");
        ImGui::Separator();

        if (isGrading) {
            ImGui::BeginDisabled(!gradingOn);
        }

        constexpr std::size_t kMaxSections = 12;
        const char* sections[kMaxSections]{};
        std::size_t sectionCount = 0;
        bool hasUnsectioned = false;
        for (const auto& entry : Platform::kGroupSectionOrder) {
            if (std::strcmp(entry.group, a_group) != 0) {
                continue;
            }
            for (const char* const declared : entry.sections) {
                if (declared == nullptr || sectionCount >= kMaxSections) {
                    break;
                }
                sections[sectionCount++] = declared;
            }
            break;
        }
        Platform::VisitCatalogue([&](const Platform::CatalogueRow& a_row, std::size_t) {
            if (std::strcmp(a_row.group, a_group) != 0) {
                return;
            }
            if (a_row.section == nullptr) {
                hasUnsectioned = true;
                return;
            }
            for (std::size_t i = 0; i < sectionCount; ++i) {
                if (std::strcmp(sections[i], a_row.section) == 0) {
                    return;
                }
            }
            if (sectionCount < kMaxSections) {
                sections[sectionCount++] = a_row.section;
            }
        });

        if (hasUnsectioned) {
            Platform::VisitCatalogue([&](const Platform::CatalogueRow& a_row, std::size_t a_index) {
                if (std::strcmp(a_row.group, a_group) == 0 && a_row.section == nullptr) {
                    DrawCatalogueRow(a_row, a_index);
                }
            });
        }
        for (std::size_t i = 0; i < sectionCount; ++i) {
            const char* const section = sections[i];
            bool sectionHasRows = false;
            Platform::VisitCatalogue([&](const Platform::CatalogueRow& a_row, std::size_t) {
                if (std::strcmp(a_row.group, a_group) == 0 && a_row.section != nullptr &&
                    std::strcmp(a_row.section, section) == 0) {
                    sectionHasRows = true;
                }
            });
            if (!sectionHasRows) {
                continue;
            }
            DrawSectionHeading(section);
            DrawSectionExtras(a_group, section);
            Platform::VisitCatalogue([&](const Platform::CatalogueRow& a_row, std::size_t a_index) {
                if (std::strcmp(a_row.group, a_group) == 0 && a_row.section != nullptr &&
                    std::strcmp(a_row.section, section) == 0) {
                    DrawCatalogueRow(a_row, a_index);
                }
            });
        }

        if (isGrading) {
            ImGui::EndDisabled();
        }
        if (std::strcmp(a_group, "Lighting") == 0) {
            ImGui::Spacing();
            ImGui::TextDisabled("Engine sliders drive the game through its own reversible "
                                "modifier layer; one left at its default leaves the weather in "
                                "charge. Bloom sliders marked (our pass) are this mod's own.");
        }
    }

    void Menu::DrawAmbientOcclusionControls()
    {
        const auto ao = Platform::AoMailbox::Snapshot();

        bool enabled = ao.requested == Platform::AoMailbox::Point::kGtao;
        if (ImGui::Checkbox("Ambient occlusion", &enabled)) {
            Platform::AoMailbox::Publish(enabled ? Platform::AoMailbox::Point::kGtao
                                                 : Platform::AoMailbox::Point::kOff,
                enabled);
            Settings::Menu().aoMode =
                enabled ? static_cast<std::uint32_t>(Platform::AoMailbox::Point::kGtao) : 0U;
            Settings::Menu().aoEnabled = enabled;
            Settings::MarkDirty();
        }
        Help("Toggle freely at any time — no restart. XeGTAO (Intel's ground-truth-based ambient "
             "occlusion, MIT) runs in our own compute passes and takes over PRODUCTION, while the "
             "engine keeps carrying and applying the result, which is why switching is instant. "
             "Saved with your settings and restored at launch.\n\n"
             "The engine's own AO never draws: its SSAO/HBAO producers are retired at the code "
             "level each session (the same treatment TAA got), so no INI or console path can "
             "bring them back — this mod's GTAO is the only producer.");
        ImGui::Spacing();

        if (enabled) {
            const auto gtao = Platform::GtaoPass::Snapshot();
            ImGui::TextDisabled("Ranges are the algorithm's own documented bounds.");

            int quality = static_cast<int>(gtao.quality);
            static const char* const kQualityNames[]{ "Low", "Medium", "High", "Ultra",
                "Extreme" };
            if (ImGui::SliderInt("Quality", &quality, 0, 4, kQualityNames[quality],
                    ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_NoInput)) {
                Platform::GtaoPass::SetQuality(static_cast<std::uint32_t>(quality));
                Settings::Menu().aoQuality = static_cast<std::uint32_t>(quality);
                Settings::MarkDirty();
            }
            Help("Low through Ultra are XeGTAO's own presets (Ultra: 9 slices x 3 steps, 54 "
                 "samples per pixel). Extreme is this mod's own tier past them - 16 slices x 4 "
                 "steps, sampling the occlusion horizon about 2.4x as densely as Ultra. Sharper, "
                 "more accurate contact shadows on thin and cluttered geometry, at roughly 2.4x "
                 "Ultra's main-pass GPU cost. If Extreme looks clean, one denoise pass is usually "
                 "enough - extra passes only soften what the samples already resolved.");
            int denoise = static_cast<int>(gtao.denoisePasses);
            if (ImGui::SliderInt("Denoise passes", &denoise, 0, 3, "%d",
                    ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_NoInput)) {
                Platform::GtaoPass::SetDenoisePasses(static_cast<std::uint32_t>(denoise));
                Settings::Menu().aoDenoisePasses = static_cast<std::uint32_t>(denoise);
                Settings::MarkDirty();
            }

            float radiusMetres = gtao.radius / Platform::GtaoPass::kGameUnitsPerMetre;
            if (ImGui::SliderFloat("Effect radius (m)", &radiusMetres, 0.1F, 10.0F, "%.2f m",
                    ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic)) {
                Platform::GtaoPass::SetRadius(
                    radiusMetres * Platform::GtaoPass::kGameUnitsPerMetre);
                Settings::Menu().aoRadius =
                    radiusMetres * Platform::GtaoPass::kGameUnitsPerMetre;
                Settings::MarkDirty();
            }
            Help("Occlusion radius in METRES - the same unit the engine's own HBAO radius uses, so "
                 "the two are directly comparable. Default 1.0 m, which is HBAO's own default.\n\n"
                 "Fallout's world is ~70 game units to the metre, and the conversion happens here so "
                 "you never have to think in engine units. Ctrl+Click to type an exact value.");

            if (ImGui::Button("Restore GTAO defaults")) {
                Platform::GtaoPass::RestoreDefaults();
                auto& menu = Settings::Menu();
                const Settings::MenuSettings defaults{};
                menu.aoQuality = defaults.aoQuality;
                menu.aoDenoisePasses = defaults.aoDenoisePasses;
                menu.aoRadius = defaults.aoRadius;
                menu.aoRadiusMultiplier = defaults.aoRadiusMultiplier;
                menu.aoFalloffRange = defaults.aoFalloffRange;
                menu.aoSampleDistributionPower = defaults.aoSampleDistributionPower;
                menu.aoOccluderThickness = defaults.aoOccluderThickness;
                menu.aoFinalValuePower = defaults.aoFinalValuePower;
                menu.aoMinScreenRadius = defaults.aoMinScreenRadius;
                menu.aoDepthFadeEnabled = defaults.aoDepthFadeEnabled;
                menu.aoDepthFadeStart = defaults.aoDepthFadeStart;
                menu.aoDepthFadeEnd = defaults.aoDepthFadeEnd;
                Settings::MarkDirty();
            }
            Help("Puts every control on this panel back to its built-in default. Use it before "
                 "reporting a result: a description given after a slider sweep cannot be "
                 "interpreted, because nothing records where the sliders were.");

            float minScreenRadius = gtao.minScreenRadius;
            if (ImGui::SliderFloat("Min screen radius (px)", &minScreenRadius, 0.0F, 16.0F, "%.1f",
                    ImGuiSliderFlags_AlwaysClamp)) {
                Platform::GtaoPass::SetMinScreenRadius(minScreenRadius);
                Settings::Menu().aoMinScreenRadius = minScreenRadius;
                Settings::MarkDirty();
            }
            Help("Floors the projected radius so distant geometry still receives AO. Fallout's "
                 "outdoor depths are enormous, and without this a sane world radius shrinks to "
                 "sub-pixel and computes nothing. Default 3.");

            bool depthFade = gtao.depthFadeEnabled;
            if (ImGui::Checkbox("Distance fade", &depthFade)) {
                Platform::GtaoPass::SetDepthFadeEnabled(depthFade);
                Settings::Menu().aoDepthFadeEnabled = depthFade;
                Settings::MarkDirty();
            }
            Help("Fades the AO out across a far band, where the depth buffer no longer carries "
                 "enough detail per pixel for screen-space AO to be trustworthy. Turning it OFF is "
                 "a real disable - the shader multiplies by a constant 1.0 and the AO runs at every "
                 "distance, at no extra cost.");
            if (depthFade) {
                ImGui::Indent();
                float fadeStart = gtao.depthFadeStart;
                float fadeEnd = gtao.depthFadeEnd;
                bool rangeChanged = ImGui::SliderFloat("Fade start", &fadeStart, 1000.0F, 100000.0F,
                    "%.0f", ImGuiSliderFlags_AlwaysClamp);
                rangeChanged |= ImGui::SliderFloat("Fade end", &fadeEnd, 1000.0F, 120000.0F, "%.0f",
                    ImGuiSliderFlags_AlwaysClamp);
                if (rangeChanged) {
                    Platform::GtaoPass::SetDepthFadeRange(fadeStart, fadeEnd);
                    const auto applied = Platform::GtaoPass::Snapshot();
                    Settings::Menu().aoDepthFadeStart = applied.depthFadeStart;
                    Settings::Menu().aoDepthFadeEnd = applied.depthFadeEnd;
                    Settings::MarkDirty();
                }
                Help("Game units. AO is at full strength out to the start and gone by the end. "
                     "Defaults 40000 -> 50000, the values the working in-engine integration ships.");
                ImGui::Unindent();
            }

            if (ImGui::TreeNode("Advanced (algorithm heuristics)")) {
                float radiusMultiplier = gtao.radiusMultiplier;
                if (ImGui::SliderFloat("Radius multiplier", &radiusMultiplier, 0.3F, 3.0F, "%.3f",
                        ImGuiSliderFlags_AlwaysClamp)) {
                    Platform::GtaoPass::SetRadiusMultiplier(radiusMultiplier);
                    Settings::Menu().aoRadiusMultiplier = radiusMultiplier;
                    Settings::MarkDirty();
                }
                Help("Multiplies the effect radius; the two combine, so this is a fine-tune on top "
                     "of the radius, not an independent control. XeGTAO's range: 0.3 to 3.0.");
                float falloffRange = gtao.falloffRange;
                if (ImGui::SliderFloat("Falloff range", &falloffRange, 0.0F, 1.0F, "%.3f",
                        ImGuiSliderFlags_AlwaysClamp)) {
                    Platform::GtaoPass::SetFalloffRange(falloffRange);
                    Settings::Menu().aoFalloffRange = falloffRange;
                    Settings::MarkDirty();
                }
                float sampleDistribution = gtao.sampleDistributionPower;
                if (ImGui::SliderFloat("Sample distribution power", &sampleDistribution, 1.0F, 3.0F,
                        "%.2f", ImGuiSliderFlags_AlwaysClamp)) {
                    Platform::GtaoPass::SetSampleDistributionPower(sampleDistribution);
                    Settings::Menu().aoSampleDistributionPower = sampleDistribution;
                    Settings::MarkDirty();
                }
                float occluderThickness = gtao.occluderThickness;
                if (ImGui::SliderFloat("Occluder thickness", &occluderThickness, 0.5F, 100.0F,
                        "%.1f units", ImGuiSliderFlags_AlwaysClamp |
                            ImGuiSliderFlags_Logarithmic)) {
                    Platform::GtaoPass::SetOccluderThickness(occluderThickness);
                    Settings::Menu().aoOccluderThickness = occluderThickness;
                    Settings::MarkDirty();
                }
                Help("THE HALO CONTROL, and it is now a real distance rather than a blend factor.\n\n"
                     "Screen-space AO cannot see how thick anything is. Left to itself it assumes "
                     "every occluder extends away from the camera FOREVER, so a character standing "
                     "in front of a wall kills the AO across the whole wall behind them and leaves "
                     "a halo.\n\n"
                     "This is how far behind its visible face an occluder is assumed to reach, in "
                     "game units - the same scale as the radius, where 70 is one metre. Each sample "
                     "then covers a bounded arc instead of everything behind it.\n\n"
                     "TOO SMALL and objects go transparent to occlusion - they stop casting AO "
                     "almost entirely. TOO LARGE and it approaches the old behaviour, since a very "
                     "deep occluder is nearly an infinite one, and the halo comes back.\n\n"
                     "Default 8 is PROVISIONAL - derived from the only comparable implementation "
                     "available, not from in-game measurement. The previous 'thin occluder compensation' "
                     "value does not carry over: it was dimensionless, and as a distance it would "
                     "mean a sub-millimetre occluder.");
                float finalPower = gtao.finalValuePower;
                if (ImGui::SliderFloat("Final power", &finalPower, 0.5F, 5.0F, "%.2f",
                        ImGuiSliderFlags_AlwaysClamp)) {
                    Platform::GtaoPass::SetFinalValuePower(finalPower);
                    Settings::Menu().aoFinalValuePower = finalPower;
                    Settings::MarkDirty();
                }
                Help("The first three ranges are XeGTAO's own documented bounds. Occluder "
                     "thickness's range is this project's own — it is a distance in game units, a "
                     "quantity stock XeGTAO does not have.\n\n"
                     "The defaults here were tuned by eye in-game, not derived as an "
                     "optimum. They were tuned at Ultra; sample distribution power and falloff range "
                     "both interact with sample count, so at lower quality levels they may want "
                     "adjusting. Change them freely - 'Restore GTAO defaults' always comes back "
                     "here.");
                ImGui::TreePop();
            }
        }
        ImGui::Spacing();
    }

    void Menu::DrawTabVisualEffects()
    {
        if (!ImGui::BeginTabItem("Visual Effects")) {
            return;
        }

        ImGui::TextDisabled(
            "Ranges come from in-game testing, not inference, and are enforced as safety clamps - "
            "typing past them is not possible. Ctrl+Click any slider to enter an exact value.");
        ImGui::Spacing();

        if (ImGui::BeginTabBar("##fo4go-vfx")) {
            if (ImGui::BeginTabItem("Ambient occlusion")) {
                DrawAmbientOcclusionControls();
                ImGui::EndTabItem();
            }
            for (const char* const group : Platform::kCatalogueGroups) {
                if (ImGui::BeginTabItem(group)) {
                    DrawCatalogueGroup(group);
                    ImGui::EndTabItem();
                }
            }
            ImGui::EndTabBar();
        }
        ImGui::EndTabItem();
    }

    void Menu::DrawTabOsdUi()
    {
        if (!ImGui::BeginTabItem("OSD/UI")) {
            return;
        }
        auto& settings = Settings::Menu();

        SectionHeader("On-screen display");
        LiveControl::CheckboxD("Enable OSD", settings.osdEnabled, kMenuDefaults.osdEnabled,
            [](bool a_on) {
                Settings::OsdEnabledMirror().store(a_on, std::memory_order_release);
                Settings::MarkDirty();
                logger::info("[OSD] overlay {} (live)", a_on ? "shown" : "hidden");
            });
        Help("Always-on overlay: FPS (while DLSS Frame Generation interpolates: the displayed rate with its "
             "real + generated split), frametime, CPU/GPU load, RAM/VRAM and GPU temperature. It is "
             "click-through while the menu is closed, so it can never steal a keypress or a shot.");

        LiveControl::ComboD("Screen position", settings.osdAnchor, kMenuDefaults.osdAnchor,
            "Top-left\0Top-right\0Bottom-left\0Bottom-right\0Custom\0",
            [&settings](std::uint32_t a_value) {
                settings.osdAnchor = Settings::ClampOsdAnchor(a_value);
                Settings::MarkDirty();
            });
        Help("A corner anchor stays glued to that corner even if you change resolution. Pick "
             "\"Custom\" to place it by hand — then drag it while this menu is open.");

        LiveControl::SliderFloatD("Font size", settings.osdFontSize, Settings::kOsdFontSizeMin,
            Settings::kOsdFontSizeMax, kMenuDefaults.osdFontSize, [&settings](float a_value) {
                settings.osdFontSize = Settings::ClampOsdFontSize(a_value);
                Settings::MarkDirty();
            }, "%.0f");

        if (ImGui::InputText("Font file (.ttf)", settings.osdFontPath.data(),
                settings.osdFontPath.size(), ImGuiInputTextFlags_EnterReturnsTrue)) {
            settings.osdFontPath = Settings::ClampFontPath(settings.osdFontPath);
            Osd::RequestFontReload();
            Settings::MarkDirty();
        }
        Help("Full path to a .ttf/.otf file, then press Enter. Leave empty for the built-in font. "
             "A missing or corrupt file falls back to the built-in font and logs — it never crashes.");
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear")) {
            settings.osdFontPath = Settings::FontPath{};
            Osd::RequestFontReload();
            Settings::MarkDirty();
        }
        ImGui::TextDisabled("        font: %s", Osd::FontStatus());

        float textColor[4] = { settings.osdTextR, settings.osdTextG, settings.osdTextB,
            settings.osdTextA };
        if (ImGui::ColorEdit4("Text colour", textColor)) {
            settings.osdTextR = Settings::ClampColorChannel(textColor[0]);
            settings.osdTextG = Settings::ClampColorChannel(textColor[1]);
            settings.osdTextB = Settings::ClampColorChannel(textColor[2]);
            settings.osdTextA = Settings::ClampColorChannel(textColor[3]);
            Settings::MarkDirty();
        }
        float bgColor[4] = { settings.osdBgR, settings.osdBgG, settings.osdBgB, settings.osdBgA };
        if (ImGui::ColorEdit4("Background + transparency", bgColor,
                ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreviewHalf)) {
            settings.osdBgR = Settings::ClampColorChannel(bgColor[0]);
            settings.osdBgG = Settings::ClampColorChannel(bgColor[1]);
            settings.osdBgB = Settings::ClampColorChannel(bgColor[2]);
            settings.osdBgA = Settings::ClampColorChannel(bgColor[3]);
            Settings::MarkDirty();
        }
        Help("The alpha channel is the overlay's background transparency — drag it to 0 for text "
             "with no panel behind it.");

        ImGui::Spacing();
        ImGui::TextDisabled("Shown metrics");
        const auto metricToggle = [&settings](const char* a_label, bool& a_field, bool a_default) {
            LiveControl::CheckboxD(a_label, a_field, a_default, [](bool) {
                Settings::MarkDirty();
            });
        };
        metricToggle("FPS", settings.osdShowFps, kMenuDefaults.osdShowFps);
        metricToggle("Frametime (ms)", settings.osdShowFrametime, kMenuDefaults.osdShowFrametime);
        metricToggle("1% / 0.1% lows", settings.osdShowLows, kMenuDefaults.osdShowLows);
        metricToggle("CPU load", settings.osdShowCpu, kMenuDefaults.osdShowCpu);
        metricToggle("RAM", settings.osdShowRam, kMenuDefaults.osdShowRam);
        metricToggle("VRAM", settings.osdShowVram, kMenuDefaults.osdShowVram);
        metricToggle("GPU load", settings.osdShowGpu, kMenuDefaults.osdShowGpu);
        metricToggle("GPU temperature", settings.osdShowGpuTemp, kMenuDefaults.osdShowGpuTemp);
        metricToggle("CPU temperature", settings.osdShowCpuTemp, kMenuDefaults.osdShowCpuTemp);
        Help("Always displays N/A. A trustworthy CPU temperature needs kernel-level (ring-0) hardware "
             "access this mod deliberately does not ship — the only driver-free source on this class "
             "of board is a frozen constant that never moves under load. GPU temperature is real.");
        metricToggle("Last load time", settings.osdShowLoad, kMenuDefaults.osdShowLoad);
        metricToggle("Neural model size", settings.osdShowNeural, kMenuDefaults.osdShowNeural);
        Help("The neural filter's pass count and the size the model works on, in effect (not the requested one), "
             "so a screenshot documents the model resolution it was taken at.");

        SectionHeader("Menu / UI");
        LiveControl::ComboD("Style", settings.menuStyle, kMenuDefaults.menuStyle,
            MenuStyle::kComboItems, [&settings](std::uint32_t a_value) {
                settings.menuStyle = Settings::ClampMenuStyle(a_value);
                MenuStyle::Apply(settings.menuStyle);
                Settings::MarkDirty();
            });
        LiveControl::SliderFloatD("Font scale", settings.fontScale, 0.5F, 2.0F,
            kMenuDefaults.fontScale, [&settings](float a_value) {
                settings.fontScale = Settings::ClampFontScale(a_value);
                Settings::MarkDirty();
            });
        LiveControl::CheckboxD("Transparent menu", settings.transparentMenu,
            kMenuDefaults.transparentMenu, [](bool a_on) {
                Settings::MarkDirty();
                logger::info("[Menu] transparent menu {} (live)", a_on ? "on" : "off");
            });
        Help("Fades this window's background so the scene stays readable while tuning.");

        ImGui::Spacing();
        if (ImGui::Button("Reset window position and size")) {
            ImGui::SetWindowPos("FO4GraphicsOverhaul", ImVec2(60.0F, 60.0F));
            ImGui::SetWindowSize(
                "FO4GraphicsOverhaul", ImVec2(kDefaultWindowW, kDefaultWindowH));
            logger::info("[Menu] window geometry reset to defaults");
        }
        Help("Moves this window back to its default place and size — the way out if it ends up "
             "somewhere awkward after a resolution change.");
        ImGui::TextDisabled("        stored: %.0f, %.0f   %.0f x %.0f",
            static_cast<double>(settings.windowX), static_cast<double>(settings.windowY),
            static_cast<double>(settings.windowW), static_cast<double>(settings.windowH));

        ImGui::EndTabItem();
    }

    void Menu::DrawTabKeybindings()
    {
        if (!ImGui::BeginTabItem("Keybindings")) {
            return;
        }
        auto& settings = Settings::Menu();

        SectionHeader("Menu");
        char keyName[64];
        FormatKeyName(settings.openHotkey, keyName, sizeof(keyName));
        if (IsRebindArmed()) {
            ImGui::TextUnformatted("Open / close menu:  <press any key — Esc cancels>");
        } else {
            ImGui::Text("Open / close menu:  %s", keyName);
            ImGui::SameLine();
            if (ImGui::SmallButton("Rebind")) {
                ArmRebind();
            }
        }
        Help("Modifier keys are rejected: a modifier binding would pop the menu on every sprint or "
             "aim press. Pressing the current hotkey while rebinding cancels and still closes.");

        SectionHeader("OSD");
        char osdKeyName[64];
        FormatKeyName(settings.osdHotkey, osdKeyName, sizeof(osdKeyName));
        if (IsOsdRebindArmed()) {
            ImGui::TextUnformatted("Show / hide OSD:    <press any key — Esc cancels>");
        } else {
            ImGui::Text("Show / hide OSD:    %s", osdKeyName);
            ImGui::SameLine();
            if (ImGui::SmallButton("Rebind##osd")) {
                ArmOsdRebind();
            }
        }
        Help("Independent of the menu key — the overlay can be toggled without opening the menu.");

        SectionHeader("Diagnostics (optional keys, unset by default)");
        {
            const auto optionalRow = [&](const char* a_label, std::uint32_t& a_field, std::atomic<std::uint32_t>& a_mirror,
                                         bool a_armed, auto&& a_arm, const char* a_id) {
                char name[64];
                if (a_field == 0U) {
                    std::snprintf(name, sizeof(name), "unset");
                } else {
                    FormatKeyName(a_field, name, sizeof(name));
                }
                if (a_armed) {
                    ImGui::Text("%s  <press any key — Esc cancels>", a_label);
                    return;
                }
                ImGui::Text("%s  %s", a_label, name);
                ImGui::SameLine();
                if (ImGui::SmallButton(fmt::format("Rebind##{}", a_id).c_str())) {
                    a_arm();
                }
                if (a_field != 0U) {
                    ImGui::SameLine();
                    if (ImGui::SmallButton(fmt::format("Clear##{}", a_id).c_str())) {
                        a_field = 0U;
                        a_mirror.store(0U, std::memory_order_release);
                        Settings::MarkDirty();
                        logger::info("[Menu] {} hotkey cleared (unset)", a_label);
                    }
                }
            };
            optionalRow("Frame fingerprint on / off:     ", settings.fingerprintToggleHotkey, Settings::FingerprintToggleHotkeyMirror(),
                IsFingerprintToggleRebindArmed(), [&] { ArmFingerprintToggleRebind(); }, "fpt");
            optionalRow("Frame fingerprint: mark incident:", settings.fingerprintMarkHotkey, Settings::FingerprintMarkHotkeyMirror(),
                IsFingerprintMarkRebindArmed(), [&] { ArmFingerprintMarkRebind(); }, "fpm");
            Help("The Debug tab's frame fingerprint (where do the pixels first change?) can be switched on and off, and an "
                 "incident marked, from a key during play. Both keys are OPTIONAL: unset by default and matching nothing "
                 "until you bind one. The Debug tab's own checkbox and button always work.");
        }

        ImGui::EndTabItem();
    }

    void Menu::DrawTabAdvanced()
    {
        if (!ImGui::BeginTabItem("Advanced")) {
            return;
        }
        auto& settings = Settings::Menu();

        SectionHeader("Window and cursor");
        LiveControl::CheckboxD("Lock cursor to window", settings.lockCursor,
            kMenuDefaults.lockCursor, [&settings](bool) {
                PushPresentLive(settings);
                Settings::MarkDirty();
            });
        Help("Confines the mouse cursor to the game window while it has focus — the fix for a "
             "cursor that escapes onto a second monitor. Applies immediately. This menu always "
             "wins: while it is open the cursor is free, and closing it puts the lock back. "
             "Alt-tabbing releases the cursor and returning re-applies it.");

        ImGui::Spacing();
        ImGui::TextDisabled("Display mode: BORDERLESS FULLSCREEN, always. The game's own bFull Screen / "
                            "bBorderless are patched at the engine's read site, because the chain that presents "
                            "cannot serve exclusive fullscreen (it shows a black screen there). You "
                            "need not change your INI: set what you like, you get borderless fullscreen. "
                            "Tearing is never requested.");

        SectionHeader("Character and NPC fade");
        LiveControl::CheckboxD("Disable NPC fade", settings.disableActorFade,
            kMenuDefaults.disableActorFade, [](bool) { Settings::MarkDirty(); });
        LiveControl::CheckboxD("Disable player fade", settings.disablePlayerFade,
            kMenuDefaults.disablePlayerFade, [](bool) { Settings::MarkDirty(); });
        ImGui::TextDisabled("        both take effect at the next game start");
        Help("Stops characters fading out when the camera gets close to them. Saved immediately, "
             "but the patch itself is written into the engine during startup, so the change is "
             "visible after a restart.");

        SectionHeader("About");
        ImGui::Text("%s v%u.%u.%u", Plugin::NAME.data(),
            Plugin::VERSION_MAJOR, Plugin::VERSION_MINOR, Plugin::VERSION_PATCH);
        ImGui::TextWrapped("Standalone graphics overhaul for Fallout 4 1.10.163 (pre-NG), "
                           "F4SE 0.6.23. Self-hosted canvas — no ENB, no post-process host.");
        ImGui::TextDisabled("UI: Dear ImGui %s (MIT)", IMGUI_VERSION);
        ImGui::EndTabItem();
    }

    void Menu::DrawTabDebug()
    {
        if (!ImGui::BeginTabItem("Debug")) {
            return;
        }
        SectionHeader("Diagnostics");
        if (ImGui::Button("Log debug snapshot")) {
            Platform::Streamline::LogDebugSnapshot();
            Platform::Fallout4Renderer::LogSnapshot();
        }
        Help("Writes a full Streamline + renderer state dump to FO4GraphicsOverhaul.log.");
        {
            const auto fp = Platform::FrameFingerprint::Snapshot();
            bool on = fp.enabled;
            if (ImGui::Checkbox("Frame fingerprint - where do the pixels first change?", &on)) {
                Platform::FrameFingerprint::SetEnabled(on);
            }
            Help("Per frame, the mean luma change against the previous frame at five points - the neural pass's INPUT, its "
                 "OUTPUT, the game buffer AT PRESENT (before our canvas), AFTER THE CANVAS, and at the actual PROXY INPUT "
                 "after the limiter / other hooks - over a 4x4 tile grid, kept in a ring of 600 frames with the context (reset, "
                 "epoch, guides, frame generation, menu, AA drain). When the screen flickers press the mark key (Hotkeys tab, "
                 "optional) or the button: 120 frames later the ring is written as CSV beside the log and one line names where "
                 "the pixels changed first: input = upstream of the neural pass; output = the neural contract/model/history; "
                 "at Present = the HUD or the D3D11 path before our canvas; after canvas = the menu/OSD draw; proxy input = the "
                 "handoff after the canvas/limiter; none = the D3D12 "
                 "proxy's own copy, the generated frames or scan-out. A spike counts on the whole frame or on one "
                 "of 16 tiles. Turning capture off also saves the latest completed frames. Generated frames and physical "
                 "scan-out are not sampled. OFF stops sampling; pending readbacks retire before release.");
            if (fp.enabled) {
                if (!fp.allocated) {
                    ImGui::TextDisabled("  starting on the next frame%s%s", fp.reason[0] != '\0' ? " - " : "", fp.reason);
                } else {
                    ImGui::Text("  input %s | output %s | at Present %s | after canvas %s | proxy input %s  (mean |delta| luma, 0-255) | ring %u/%u | dumps %u | dropped reads %u",
                        fp.lastValid[0] ? fmt::format("{:.2f}", fp.lastDelta[0]).c_str() : "-",
                        fp.lastValid[1] ? fmt::format("{:.2f}", fp.lastDelta[1]).c_str() : "-",
                        fp.lastValid[2] ? fmt::format("{:.2f}", fp.lastDelta[2]).c_str() : "-",
                        fp.lastValid[3] ? fmt::format("{:.2f}", fp.lastDelta[3]).c_str() : "-",
                        fp.lastValid[4] ? fmt::format("{:.2f}", fp.lastDelta[4]).c_str() : "-",
                        fp.ringFrames, Platform::FrameFingerprint::kRingFrames, fp.dumps, fp.droppedReads);
                    if (fp.markArmed) {
                        ImGui::Text("  incident marked: the dump follows in %u frames", fp.markCountdown);
                    } else if (ImGui::Button("Mark incident now##fp")) {
                        Platform::FrameFingerprint::MarkIncident();
                    }
                    if (fp.lastVerdict[0] != '\0') ImGui::TextWrapped("  last verdict: %s", fp.lastVerdict);
                    if (fp.lastDump[0] != '\0') ImGui::TextWrapped("  last dump: %s", fp.lastDump);
                }
            } else {
                ImGui::TextDisabled("  off (nothing allocated)%s%s", fp.reason[0] != '\0' ? " - the last start failed: " : "", fp.reason);
            }
        }

        SectionHeader("Enforced settings");
        {
            const auto owned = Platform::OwnedSettings::Snapshot();
            ImGui::Text("Owned by address: %zu of %zu bound | corrections made: %zu",
                owned.boundRows, owned.ownedRows, owned.corrections);
            Help("A setting we hold an address for is re-asserted every frame, just before the "
                 "engine reads it. Console commands, INI edits and the game's own options menu are "
                 "all corrected before they can produce a frame.\n\n"
                 "'Corrections' counts how many times something else changed an owned value and we "
                 "put it back. A number that climbs while you are not touching anything means "
                 "something is fighting us every frame - which is worth seeing, not hiding.");
            if (owned.consoleRowsSet != 0U) {
                ImGui::Text("Console-driven settings you have set: %zu (re-asserted on load, not per frame)",
                    owned.consoleRowsSet);
            }
        }

        {
            const float ratio = Platform::Fallout4Renderer::CurrentDynamicResolutionRatio();
            const std::uint32_t mode = Platform::Streamline::CurrentQualityMode();
            const ImGuiIO& io = ImGui::GetIO();
            const auto outW = static_cast<std::uint32_t>(io.DisplaySize.x);
            const auto outH = static_cast<std::uint32_t>(io.DisplaySize.y);
            if (ratio < 0.999F) {
                ImGui::Text("Render: %ux%u -> %ux%u  (%s, ratio %.3f)",
                    static_cast<unsigned>(static_cast<float>(outW) * ratio),
                    static_cast<unsigned>(static_cast<float>(outH) * ratio),
                    outW, outH, Platform::DlaaQualityModeName(mode), static_cast<double>(ratio));
            } else {
                ImGui::Text("Render: native %ux%u  (%s)", outW, outH,
                    Platform::DlaaQualityModeName(mode));
            }

            std::uint32_t colorFmt = 0;
            const bool colorHdr = Platform::Streamline::ColorPathIsHdr(colorFmt);
            if (colorFmt != 0U) {
                ImGui::Text("DLSS color path: %s (DXGI format %u)",
                    colorHdr ? "HDR (pre-tonemap linear)" : "SDR", colorFmt);
            } else {
                ImGui::TextDisabled("DLSS color path: not evaluated yet");
            }

            {
                const Platform::HdrDisplay::Info hdr = Platform::HdrDisplay::Current();
                if (!hdr.observed) {
                    ImGui::TextDisabled("Display: not observed yet");
                } else if (hdr.displayInHdrMode) {
                    ImGui::Text("Display: HDR mode ON — %.0f nits peak, %u bpc",
                        static_cast<double>(hdr.maxLuminanceNits), hdr.bitsPerColor);
                } else {
                    ImGui::Text("Display: SDR (HDR off in Windows) — %u bpc", hdr.bitsPerColor);
                }
                if (hdr.observed) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("| scRGB presentable: %s", hdr.scrgbPresentable ? "yes" : "no");
                    Help(
                        "Whether the presenting swap chain can present scRGB (the format true HDR "
                        "output needs). A 'no' is expected: the chain that presents is our "
                        "DirectX 12 flip-model chain, built in the game's 8-bit format, and scRGB needs "
                        "an FP16 one. This line is measurement, not "
                        "a fault.");
                }
            }

            if (!Platform::RenderTargetProxy::Available()) {
                ImGui::TextColored(ImVec4(1.0F, 0.6F, 0.2F, 1.0F),
                    "Sub-native fix-up layer: LATCHED OFF (see log)");
            } else {
                ImGui::Text("Sub-native fix-up layer: available");
            }
        }

        SectionHeader("Engine fixes");
        {
            const Platform::HavokFixes::Status havok = Platform::HavokFixes::CurrentStatus();
            ImGui::Text("Load-time batch: %s   |   after-settings batch: %s",
                havok.loadTimeRan ? "ran" : "NOT RUN", havok.afterSettingsRan ? "ran" : "NOT RUN");
            ImGui::Text("Untie speed from FPS: %s   |   iFPSClamp: %d -> %d %s",
                havok.untieApplied ? "applied" : "NOT applied", havok.fpsClampBefore,
                havok.fpsClampAfter, havok.fpsClampCleared ? "(cleared)" : "(NOT cleared)");
            Help("These two are one fix in two halves. Untying alone is not enough — the engine's "
                 "own iFPSClamp would re-impose a framerate-dependent simulation cap on top of it.");
            ImGui::Text("White screen: %s   |   loading model: %s   |   OCBP: %s",
                havok.whiteScreenApplied ? "applied" : "NOT applied",
                havok.loadingModelApplied ? "applied" : "NOT applied",
                havok.ocbpDetected ? "detected (no patch)" : "not present");
            ImGui::Text("NPC fade: %s   |   player fade: %s",
                havok.actorFadeApplied ? "disabled" : "vanilla",
                havok.playerFadeApplied ? "disabled" : "vanilla");
            ImGui::TextDisabled("Stutter, wind, rotation, sitting rotation and motion fixes are "
                                "generated-code patches — per-site results are in the log.");

            const Platform::WindowPolicy::Status window = Platform::WindowPolicy::CurrentStatus();
            ImGui::Text("Display mode: %s   |   ghosting: %s",
                (window.fullscreenForcedOff && window.borderlessForcedOn) ? "borderless fullscreen (forced)"
                                                                         : "NOT forced - a patch site refused (see the log)",
                window.ghostingDisabled ? "disabled" : "untouched");
            ImGui::Text("Cursor lock: %s", window.cursorLockEnabled ? "on" : "off");
        }

        SectionHeader("Present policy");
        {
            const Platform::PresentPolicy::State present = Platform::PresentPolicy::CurrentState();
            if (!present.descApplied) {
                ImGui::TextDisabled("Swap chain not seen yet — the policy applies at device creation.");
            } else {
                static constexpr const char* kEffectNames[] = { "auto", "discard", "sequential",
                    "flip_sequential", "flip_discard" };
                const auto effectIndex = static_cast<std::size_t>(present.resolvedEffect);
                ImGui::Text("Swap effect: %s   |   buffers: %u   |   chain allows tearing: %s",
                    effectIndex < std::size(kEffectNames) ? kEffectNames[effectIndex] : "?",
                    present.resolvedBufferCount, present.chainAllowsTearing ? "yes" : "no");
                Help("The policy's resolution for the descriptor the game's OWN chain would get (the "
                     "fail-open DirectX 11 path): chosen from what the DXGI factory reports, flip model only "
                     "while windowed. The DirectX 12 proxy builds its own flip-model chain in every window "
                     "mode - see 'Presentation' below for the chain that actually presents. The tearing flag "
                     "is never requested.");
            }
            ImGui::Text("Active frame cap: %s   |   loading screen: %s",
                present.activeFpsLimit == 0U ? "unlimited" : "see value",
                present.loadingScreenActive ? "ACTIVE" : "inactive");
            if (present.activeFpsLimit != 0U) {
                ImGui::SameLine();
                ImGui::Text("(%u fps)", present.activeFpsLimit);
            }
        }

        SectionHeader("DLSS / Frame generation");
        {
            const auto proxy = Platform::PresentProxy::Snapshot();
            if (proxy.active) {
                ImGui::Text("Presentation: %s, %ux%u, %u buffers, tearing %s | %llu presents, %u resize(s), %.2f ms CPU per present",
                    proxy.d3d11Fallback ? "DirectX 11 fallback chain" : "DirectX 12 chain", proxy.width, proxy.height,
                    proxy.bufferCount, proxy.tearing ? "on" : "off", static_cast<unsigned long long>(proxy.presents),
                    proxy.resizes, static_cast<double>(proxy.cpuMs));
                if (proxy.reason[0] != '\0') {
                    ImGui::TextDisabled("  note: %s", proxy.reason);
                }
                const Platform::WindowPolicy::Status windowNow = Platform::WindowPolicy::CurrentStatus();
                const bool borderlessForced = windowNow.fullscreenForcedOff && windowNow.borderlessForcedOn;
                ImGui::TextDisabled("  window mode: %s%s (the presenting chain cannot serve exclusive fullscreen)",
                    borderlessForced ? "borderless windowed, forced at the engine's read site"
                                     : "windowed chain - borderless NOT confirmed: a read-site patch refused (see Window policy above and the log)",
                    proxy.fullscreenIgnored ? "; the game asked for exclusive fullscreen and was answered windowed" : "");
                ImGui::Text("  cadence: longest gap %.1f ms over %u presents this window | %u past the %.1f ms floor (%s), %llu this session",
                    static_cast<double>(proxy.cadenceLongestMs), proxy.cadencePresents, proxy.cadenceOverFloorWindow,
                    static_cast<double>(proxy.cadenceFloorMs), proxy.cadenceFloorMeasured ? "the driver's" : "assumed 48 Hz",
                    static_cast<unsigned long long>(proxy.cadenceOverFloorLifetime));
                Help("Measured at every present to the real chain, on the thread that made it: the worker's generated and "
                     "real frames, and the render thread's pass-through presents. The [Perf] line counts only the game's "
                     "own presents and resets on every setting change; this does not. The first present after a resize, a "
                     "backend switch or a loading screen is not a gap.");
                const auto vrr = Platform::AdaptiveSync::Snapshot();
                if (!vrr.observed) {
                    ImGui::TextDisabled("  variable refresh: not observed yet");
                } else if (!vrr.available) {
                    ImGui::TextDisabled("  variable refresh: not available - %s", vrr.reason);
                } else {
                    ImGui::Text("  variable refresh: %s (%s) at %u Hz, active on this mode: %s | adaptive sync %s | driver max interval at launch %s | flicker fix %s | last flip shown %u time(s)",
                        vrr.vrrCapable ? "supported" : "not supported", vrr.trueGsync ? "G-SYNC module" : "adaptive-sync panel",
                        vrr.refreshHz, vrr.vrrActiveNow ? "yes" : "no", vrr.adaptiveSyncDisabled ? "DISABLED by you" : "enabled",
                        vrr.driverMaxIntervalUs == 0U ? "0 (EDID default, not reported)" : (std::to_string(vrr.driverMaxIntervalUs) + " us, a leftover").c_str(),
                        vrr.appliedIntervalUs != 0U ? (std::string("ON, holding ") + std::to_string(vrr.appliedIntervalUs) + " us").c_str()
                                                    : (vrr.fixRequested ? (vrr.capRequested != 0U ? "ON, nothing to hold at this cap (see the log)" : "ON, waiting for an FPS cap") : "off"),
                        vrr.lastFlipRepeats);
                    if (vrr.reason[0] != '\0') {
                        ImGui::TextDisabled("    note: %s", vrr.reason);
                    }
                }
            } else {
                ImGui::TextDisabled("Presentation: not proxied (%s)", proxy.reason[0] != '\0' ? proxy.reason : "no swap chain yet");
            }
            const auto dx12 = Platform::Dlss12Engine::Snapshot();
            if (dx12.latched) {
                ImGui::Text("DLSS: OFF - %s", dx12.reason);
            } else if (dx12.featureReady) {
                ImGui::Text("DLSS: render %ux%u -> out %ux%u, %s carrier | %llu evaluations, %.2f ms CPU/frame, %u create(s) | last NGX %s",
                    dx12.renderW, dx12.renderH, dx12.outW, dx12.outH, dx12.fp16Carrier ? "RGBA16F" : "native",
                    static_cast<unsigned long long>(dx12.evaluations), static_cast<double>(dx12.cpuMs), dx12.creates,
                    Platform::Ngx::ResultName(static_cast<Platform::Ngx::Result>(dx12.lastResult)));
            } else {
                ImGui::TextDisabled("DLSS: feature not created");
            }
            {
                const auto shared = Platform::SidecarFrame::Snapshot();
                const auto fgs = Platform::FrameGenEngine::Snapshot();
                const auto ns = Platform::NeuralPass::Snapshot();
                ImGui::Text("Shared pipeline: guides -> neural: %s | HUD-less -> frame generation: %s | published %llu / %llu, taken %llu / %llu | earlier-frame records refused %llu",
                    ns.guidesSource == 1 ? "the DLSS engine's (dilated)" : ns.guidesSource == 2 ? "own derivation (dilated)" : "-",
                    fgs.hudlessSource == 1 ? "the neural output" : fgs.hudlessSource == 0 ? "RT0 capture" : "-",
                    static_cast<unsigned long long>(shared.guidesSerial), static_cast<unsigned long long>(shared.hudlessSerial),
                    static_cast<unsigned long long>(shared.guidesTaken), static_cast<unsigned long long>(shared.hudlessTaken),
                    static_cast<unsigned long long>(shared.staleRefused));
                const auto fix = Platform::MfgTemporalFix::Snapshot();
                ImGui::Text("Frame generation: %s | %ux applied (cap %ux) | runtime capability %s | %llu evaluations, %u create(s) | last NGX %s | real interval %.2f ms | %llu generated / %llu real",
                    fgs.latched ? "OFF (latched)" : fgs.interpolating ? "interpolating" : fgs.enabled ? "real frames only" : "off",
                    fgs.frames + 1U, fgs.frameCap + 1U,
                    fgs.capability == 1 ? "available" : fgs.capability == 0 ? "NOT available on this GPU/runtime" : "not probed yet",
                    static_cast<unsigned long long>(fgs.evaluations), fgs.creates,
                    Platform::Ngx::ResultName(static_cast<Platform::Ngx::Result>(fgs.lastResult)),
                    static_cast<double>(proxy.intervalMs), static_cast<unsigned long long>(proxy.interpolated),
                    static_cast<unsigned long long>(proxy.presents));
                ImGui::Text("HUD alpha for generated frames: %s",
                    fgs.uiAlphaRefused ? "refused by the runtime - off this session"
                                       : fgs.uiAlphaActive ? "carried by the last evaluate (always on)" : "always on; waiting for a HUD-less frame to diff");
                ImGui::Text("Multi-frame on RTX 40: architecture gates %s | kernel program %s (%s): %s%s%s",
                    fix.gatesApplied ? "rewritten in the mapped runtime" : fix.gatesRefused ? "REFUSED" : fix.installed ? "pending" : "off",
                    fix.applied_program[0] != '\0' ? fix.applied_program : fix.program, fix.applied_program[0] != '\0' ? "applied" : "requested",
                    fix.applied ? "applied" : fix.refused ? "refused" : fix.installed ? "armed" : "off",
                    fix.build[0] != '\0' ? " | verified build " : "", fix.build);
                if (fix.applied && fix.kernels > 1U) {
                    ImGui::TextDisabled("  %u kernel(s), %u descriptor(s): the driver built the Blackwell programs for Ada", fix.kernels, fix.slots);
                } else if (fix.applied) {
                    ImGui::TextDisabled("  %u descriptor(s), %u blend weights follow the frame's own time", fix.slots, fix.constants);
                }
                if (fix.fallbackReason[0] != '\0') {
                    ImGui::TextDisabled("  Blackwell refused: %s", fix.fallbackReason);
                }
                if (!fix.applied && fix.reason[0] != '\0') {
                    ImGui::TextDisabled("  %s", fix.reason);
                }
                if (fix.gatesApplied || fix.gatesRefused) {
                    ImGui::TextDisabled("  gates: %s", fix.gateReason);
                }
                const auto fp = Platform::FirstPersonMask::Snapshot();
                ImGui::Text("First-person conditioning for frame generation: %s | masks produced %llu | last evaluate handed conditioned guides: %s | motion-vector dilation %s",
                    !fp.installed ? "NOT installed" : fp.refused ? "installed, REFUSED this session" : "installed",
                    static_cast<unsigned long long>(fp.produced),
                    Platform::Dlss12Engine::FirstPersonConditionedLastFrame() ? "yes" : "no",
                    Platform::Dlss12Engine::MotionDilationApplied() ? "applied last evaluate" : "NOT applied (see the [DLSS12] lines)");
                if (fp.reason[0] != '\0') {
                    ImGui::TextDisabled("  %s", fp.reason);
                }
                const auto mv = Platform::MotionVectorFixes::Snapshot();
                if (mv.standingDown) {
                    ImGui::Text("Motion vector fixes: standing down - the standalone Motion Vector Fixes plugin provides them");
                } else {
                    ImGui::Text("Motion vector fixes: weapon transform %s | animated objects %s | frozen-time/LOD %s",
                        mv.weapon ? "on" : "OFF", mv.animated ? "on" : "OFF",
                        mv.frozenLod ? (mv.sinkLive ? "on" : "hooked, waiting for the LoadingMenu sink") : "OFF");
                    if (!mv.animated && mv.animatedReason[0] != '\0') {
                        ImGui::TextDisabled("  animated objects: %s", mv.animatedReason);
                    }
                }
            }
            {
                const auto rf = Platform::Reflex::Snapshot();
                if (rf.boundDevice != 0 && !rf.latched && !rf.idle) {
                    ImGui::Text("Reflex on %s: mode %s | driver latency %.1f ms (sim start -> GPU end) over %u frames | GPU frame %.1f ms | %llu sleeps (%llu on the game thread before input, %llu at the present tail), %llu markers%s",
                        rf.boundDevice == 1 ? "the DirectX 12 presenter" : "the DirectX 11 game device",
                        rf.mode == 0U ? "Off" : rf.mode == 1U ? "On" : "On + Boost", static_cast<double>(rf.latencyMs),
                        rf.reportedFrames, static_cast<double>(rf.gpuFrameMs), static_cast<unsigned long long>(rf.sleeps),
                        static_cast<unsigned long long>(rf.gameSleeps), static_cast<unsigned long long>(rf.tailSleeps),
                        static_cast<unsigned long long>(rf.markers), rf.pacedByFrameGen ? " | stepping aside for frame generation" : "");
                } else {
                    ImGui::TextDisabled("Reflex: %s", rf.latched ? rf.reason : rf.idle ? rf.reason : "waiting for a device to bind");
                }
                if (rf.capFrames != 0 || rf.freeFrames != 0) {
                    ImGui::TextDisabled("  game-thread waits: capped %llu frames (ours %.2f ms + driver %.2f ms) | uncapped %llu frames (driver %.2f ms)",
                        static_cast<unsigned long long>(rf.capFrames), static_cast<double>(rf.capPaceMs), static_cast<double>(rf.capSleepMs),
                        static_cast<unsigned long long>(rf.freeFrames), static_cast<double>(rf.freeSleepMs));
                }
                ImGui::TextDisabled("  frame-id ring: depth at pop max %u (1 = each simulation rendered as it closes) | %llu render frames found nothing handed over | %llu ids dropped (ring full)",
                    rf.ringDepthMax, static_cast<unsigned long long>(rf.ringEmptyFinds), static_cast<unsigned long long>(rf.ringFullDrops));
            }
        }

        SectionHeader("Neural Rendering Filter");
        {
            const auto neural = Platform::NeuralPass::Snapshot();
            ImGui::Text("%s", neural.status);
            if (neural.pending) {
                ImGui::Text("Mailbox: generation %llu pending (%s) - applied %llu; the render thread commits at the seam",
                    static_cast<unsigned long long>(neural.latestGeneration), neural.pendingScope,
                    static_cast<unsigned long long>(neural.appliedGeneration));
            }
            ImGui::Text("GPU: %s - %s", neural.family, neural.expectation);
            if (neural.runtimeVersion[0] != '\0') {
                ImGui::Text("Runtime %s | %s | sha %s | profile: %s%s", neural.runtimeVersion, neural.runtimeSignature, neural.runtimeSha,
                    neural.profileLabel, neural.profileValidated ? "" : " - UNVALIDATED (the 310.8 policy is applied unverified)");
            }
            if (neural.frames > 0) {
                ImGui::Text("Cascade%s: %.2f ms CPU/frame | GPU %s | %llu frames, %llu stage evaluations | %u rebuilds | last NGX %s | carrier %s",
                    neural.active ? "" : " (last active sample - the filter is not running)", static_cast<double>(neural.cpuMs),
                    neural.gpuTimed ? "medians summed below (the D3D11 hops between stages excluded)" : "unavailable (the sidecar's timestamps did not set up)",
                    static_cast<unsigned long long>(neural.frames), static_cast<unsigned long long>(neural.evaluations),
                    neural.rebuilds, neural.lastResultName, neural.carrier);
                if (neural.gpuTimed) {
                    ImGui::Text("  GPU per frame, all active stages: %.2f ms (the stages' own medians added up; not a latency)", static_cast<double>(neural.gpuMsMedianSum));
                }
                ImGui::Text("  Transfers, last frame: %u stage(s) delivered | %u colour copies (%.1f MB) | %u cross-API return(s)",
                    neural.frameStagesDelivered, neural.frameColourCopies, static_cast<double>(neural.frameCopyBytes) / 1048576.0, neural.frameCrossApiTrips);
                if (!neural.d3d11Timed) {
                    ImGui::Text("  D3D11 legs: brackets unavailable on this device (timestamp queries could not be created)");
                } else if (neural.d3d11Windows == 0U) {
                    ImGui::Text("  D3D11 legs: no report window closed yet (%llu preparation, %llu return brackets retired so far)",
                        static_cast<unsigned long long>(neural.d3d11PrepBrackets), static_cast<unsigned long long>(neural.d3d11ReturnBrackets));
                } else {
                    ImGui::Text("  D3D11 legs, last closed window: preparation median %.3f ms, return median %.3f ms (the game's context; each leg on its own, not a latency; %u window(s) so far)",
                        static_cast<double>(neural.d3d11PrepMsMedian), static_cast<double>(neural.d3d11ReturnMsMedian), neural.d3d11Windows);
                }
            }
            for (std::uint32_t i = 0; i < Platform::Neural::kMaxPasses; ++i) {
                const auto& pass = neural.passes[i];
                ImGui::Text("Pass %u: %s | %s | %.2f ms CPU%s | GPU median %.2f ms, p99 %.2f ms | %llu evaluated, %u created | handle %p",
                    i + 1U, pass.active ? "active" : pass.ready ? "ready" : "inactive", pass.carrier,
                    static_cast<double>(pass.cpuMs), pass.active ? "" : " (last sample)",
                    static_cast<double>(pass.gpuMsMedian), static_cast<double>(pass.gpuMsP99),
                    static_cast<unsigned long long>(pass.evaluations), pass.creates,
                    reinterpret_cast<void*>(pass.handleAddress));
                if (pass.reason[0] != '\0') ImGui::TextWrapped("  %s", pass.reason);
            }
        }

        SectionHeader("Live telemetry");
        {
            static Telemetry::FrameStats s_frame{};
            static double s_lastCompute = -1.0;
            const double now = ImGui::GetTime();
            if (now - s_lastCompute > 0.2) {
                s_frame = Telemetry::CurrentFrameStats();
                s_lastCompute = now;
            }
            const Telemetry::Snapshot snap = Telemetry::Read();

            if (s_frame.sampleCount > 0) {
                ImGui::Text("Frame:  %.2f ms   %.0f FPS   (1%% low %.0f, 0.1%% low %.0f)",
                    static_cast<double>(s_frame.averageMs), static_cast<double>(s_frame.averageFps),
                    static_cast<double>(s_frame.onePercentLowFps),
                    static_cast<double>(s_frame.pointOnePercentLowFps));
            } else {
                ImGui::TextDisabled("Frame:  no samples yet");
            }

            if (snap.cpuValid) {
                ImGui::Text("CPU:    process %.1f%%   system %.1f%%",
                    static_cast<double>(snap.processCpuPercent),
                    static_cast<double>(snap.systemCpuPercent));
            } else {
                ImGui::TextDisabled("CPU:    N/A");
            }

            if (snap.memValid) {
                ImGui::Text("RAM:    game %.2f GB   system %.1f / %.1f GB free",
                    ToGiB(snap.processPrivateBytes), ToGiB(snap.systemAvailBytes),
                    ToGiB(snap.systemTotalBytes));
            } else {
                ImGui::TextDisabled("RAM:    N/A");
            }

            if (snap.vramValid) {
                ImGui::Text("VRAM:   game %.2f GB   budget %.2f GB   card %.1f GB",
                    ToGiB(snap.vramUsedBytes), ToGiB(snap.vramBudgetBytes),
                    ToGiB(snap.vramTotalBytes));
                Help("\"game\" is this process's own VRAM usage (DXGI reports it per-process), not "
                     "total card usage across every application.");
            } else {
                ImGui::TextDisabled("VRAM:   N/A");
            }

            if (snap.gpuValid) {
                ImGui::Text("GPU:    %u%%   %u C", snap.gpuUtilPercent, snap.gpuTempC);
            } else {
                ImGui::TextDisabled("GPU:    N/A — %s", Telemetry::GpuSourceStatus());
            }

            ImGui::TextDisabled("CPU temp: N/A — %s", Telemetry::CpuTempSourceStatus());

            if (snap.loadValid) {
                ImGui::Text("Last load: %.1f s", static_cast<double>(snap.lastLoadSeconds));
            } else {
                ImGui::TextDisabled("Last load: not measured yet (shown after a loading screen)");
            }
        }

        SectionHeader("Fail-open latches");
        ImGui::TextDisabled("A tripped latch means that feature disabled itself for this session "
                            "instead of crashing. The reason is in the log.");
        ImGui::Text("canvas-init:      %s",
            Core::FailOpen::Tripped("canvas-init") ? "TRIPPED — menu unavailable" : "clear");
        ImGui::Text("wndproc-install:  %s",
            Core::FailOpen::Tripped("wndproc-install") ? "TRIPPED — input degraded" : "clear");
        ImGui::Text("clipcursor-hook:  %s",
            Core::FailOpen::Tripped("clipcursor-hook") ? "TRIPPED — cursor may re-clip" : "clear");

        ImGui::EndTabItem();
    }

    void Menu::DrawFooter()
    {
        auto& settings = Settings::Menu();
        auto& dlaa = DlaaWorkingCopy();

        ImGui::Separator();
        if (ImGui::Button("Save settings")) {
            const std::string& iniPath = Settings::RuntimeIniPath();
            const bool menuOk = !iniPath.empty() && Settings::IO::Save(iniPath.c_str(), settings);
            Platform::SaveDlaaSettings(dlaa);
            if (menuOk) {
                logger::info("[Settings] menu INI saved to {}", iniPath);
            } else {
                logger::warn("[Settings] menu INI save FAILED — check the path/permissions");
            }
        }
        Help("Writes BOTH files now. Settings already persist automatically a moment after you "
             "change them — this is the explicit \"write it immediately\" button.");

        ImGui::SameLine();
        if (ImGui::Button("Reload settings")) {
            const std::string& iniPath = Settings::RuntimeIniPath();
            if (!iniPath.empty() && Settings::IO::Load(iniPath.c_str(), settings)) {
                MenuStyle::Apply(settings.menuStyle);
                Settings::RepublishMirrors();
                PushPresentLive(settings);
                PushEffectsLive(settings);
                Platform::OwnedSettings::ReapplyAll();
                Osd::RequestFontReload();
            }
            dlaa = Platform::LoadDlaaSettings();
            PushDlaaLive(dlaa);
            logger::info("[Settings] reloaded both INIs from disk");
        }

        ImGui::SameLine();
        if (ImGui::Button("Restore all defaults")) {
            const float keepX = settings.windowX;
            const float keepY = settings.windowY;
            const float keepW = settings.windowW;
            const float keepH = settings.windowH;
            settings = Settings::MenuSettings{};
            settings.windowX = keepX;
            settings.windowY = keepY;
            settings.windowW = keepW;
            settings.windowH = keepH;
            MenuStyle::Apply(settings.menuStyle);
            Settings::RepublishMirrors();
            PushPresentLive(settings);
            PushEffectsLive(settings);
            Osd::RequestFontReload();

            dlaa = Platform::DlaaSettings{};
            PushDlaaLive(dlaa);
            Settings::MarkDirty();
            Platform::SaveDlaaSettings(dlaa);
            logger::info("[Settings] all defaults restored live and persisted (window placement kept)");
        }
        Help("Restores every setting to its built-in default, applied live AND written to disk — "
             "the same as every other change here. Use the per-control Rst buttons if you only want "
             "to reset one thing.");

        ImGui::SameLine();
        ImGui::TextDisabled("|  DLAA %s  |  ImGui %s", dlaa.enable ? "on" : "off", IMGUI_VERSION);
    }

    void Menu::SetOpen(bool a_open)
    {
        if (!IsInitialized()) {
            static std::atomic<bool> s_logged{ false };
            if (!s_logged.exchange(true, std::memory_order_acq_rel)) {
                logger::warn("[Menu] open request ignored — canvas is not initialized this session");
            }
            return;
        }

        const bool was = isShow_.exchange(a_open, std::memory_order_acq_rel);
        if (was == a_open) {
            return;
        }

        ::ShowCursor(a_open ? TRUE : FALSE);

        if (auto* controlMap = RE::ControlMap::GetSingleton()) {
            controlMap->SetIgnoreKeyboardMouse(a_open);
        } else {
            static std::atomic<bool> s_logged{ false };
            if (!s_logged.exchange(true, std::memory_order_acq_rel)) {
                logger::warn("[Menu] ControlMap unavailable — game input NOT suppressed while menu open");
            }
        }

        {
            std::scoped_lock lock(InputMutex());
            if (IsInitialized()) {
                auto& io = ImGui::GetIO();
                io.ClearEventsQueue();
                io.ClearInputMouse();
                io.ClearInputKeys();
            }
            if (::GetCapture() != nullptr) {
                ::ReleaseCapture();
            }
            if (!a_open) {
                FlushGeometry(true);
            }
        }

        logger::info("[Menu] menu {}", a_open ? "opened" : "closed");
    }

    void Menu::ServicePendingForcedClose()
    {
        if (!pendingForcedClose_.exchange(false, std::memory_order_acq_rel)) {
            return;
        }
        ::ShowCursor(FALSE);
        if (auto* controlMap = RE::ControlMap::GetSingleton()) {
            controlMap->SetIgnoreKeyboardMouse(false);
        }
        logger::info("[Menu] deferred restore after a device-edge force-close: game input re-enabled, cursor hidden");
    }

    bool Menu::OfferRebindKey(std::uint32_t a_vk)
    {
        const RebindTarget target = rebindTarget_.load(std::memory_order_acquire);
        if (target == RebindTarget::kNone) {
            return false;
        }
        const bool optional = target == RebindTarget::kFingerprintToggle || target == RebindTarget::kFingerprintMark;
        const char* what = target == RebindTarget::kOsdHotkey            ? "OSD"
                           : target == RebindTarget::kFingerprintToggle  ? "fingerprint on/off"
                           : target == RebindTarget::kFingerprintMark    ? "fingerprint mark"
                                                                         : "menu";
        std::atomic<std::uint32_t>& mirror = target == RebindTarget::kOsdHotkey           ? Settings::OsdHotkeyMirror()
                                             : target == RebindTarget::kFingerprintToggle ? Settings::FingerprintToggleHotkeyMirror()
                                             : target == RebindTarget::kFingerprintMark   ? Settings::FingerprintMarkHotkeyMirror()
                                                                                          : Settings::OpenHotkeyMirror();

        if (a_vk == VK_ESCAPE) {
            rebindTarget_.store(RebindTarget::kNone, std::memory_order_release);
            logger::info("[Menu] {} hotkey rebind cancelled", what);
            return true;
        }
        const std::uint32_t currentBinding = mirror.load(std::memory_order_acquire);
        if (currentBinding != 0U && a_vk == currentBinding) {
            rebindTarget_.store(RebindTarget::kNone, std::memory_order_release);
            logger::info("[Menu] {} hotkey rebind cancelled (current hotkey pressed) — binding unchanged",
                what);
            return false;
        }
        switch (a_vk) {
        case VK_SHIFT: case VK_CONTROL: case VK_MENU:
        case VK_LSHIFT: case VK_RSHIFT: case VK_LCONTROL: case VK_RCONTROL:
        case VK_LMENU: case VK_RMENU: case VK_LWIN: case VK_RWIN:
            logger::warn("[Menu] rebind rejected: modifier keys cannot be a hotkey (still listening; Esc cancels)");
            return true;
        default:
            break;
        }
        rebindTarget_.store(RebindTarget::kNone, std::memory_order_release);
        {
            std::scoped_lock lock(InputMutex());
            auto& settings = Settings::Menu();
            std::uint32_t& field = target == RebindTarget::kOsdHotkey           ? settings.osdHotkey
                                   : target == RebindTarget::kFingerprintToggle ? settings.fingerprintToggleHotkey
                                   : target == RebindTarget::kFingerprintMark   ? settings.fingerprintMarkHotkey
                                                                                : settings.openHotkey;
            field = optional ? Settings::ClampOptionalHotkey(a_vk) : Settings::ClampHotkey(a_vk);
            mirror.store(field, std::memory_order_release);
            Settings::MarkDirty();
        }
        logger::info("[Menu] {} hotkey rebound to VK {}", what, mirror.load(std::memory_order_relaxed));
        return true;
    }
}
