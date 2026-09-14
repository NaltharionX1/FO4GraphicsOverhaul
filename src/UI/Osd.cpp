#include "UI/Osd.h"

#include "Platform/FrameGenEngine.h"
#include "Platform/FsrFrameGen.h"
#include "Platform/NeuralPass.h"
#include "Platform/PresentProxy.h"
#include "Settings/MenuSettingsStore.h"
#include "Telemetry/Telemetry.h"
#include "UI/Menu.h"

#include <atomic>
#include <cstring>

namespace
{
    ImFont* g_customFont = nullptr;
    const char* g_fontStatus = "built-in font";
    std::atomic<bool> g_fontReloadPending{ true };
    Settings::FontPath g_loadedFontPath{};

    constexpr float kScreenPad = 12.0F;

    [[nodiscard]] ImVec4 ToVec4(float a_r, float a_g, float a_b, float a_a) noexcept
    {
        return ImVec4(a_r, a_g, a_b, a_a);
    }

    [[nodiscard]] double ToGiB(std::uint64_t a_bytes) noexcept
    {
        return static_cast<double>(a_bytes) / (1024.0 * 1024.0 * 1024.0);
    }

    void ResolveAnchor(
        std::uint32_t a_anchor, const ImVec2& a_display, ImVec2& a_pos, ImVec2& a_pivot) noexcept
    {
        switch (static_cast<Settings::OsdAnchor>(a_anchor)) {
        case Settings::OsdAnchor::kTopLeft:
            a_pos = ImVec2(kScreenPad, kScreenPad);
            a_pivot = ImVec2(0.0F, 0.0F);
            break;
        case Settings::OsdAnchor::kTopRight:
            a_pos = ImVec2(a_display.x - kScreenPad, kScreenPad);
            a_pivot = ImVec2(1.0F, 0.0F);
            break;
        case Settings::OsdAnchor::kBottomLeft:
            a_pos = ImVec2(kScreenPad, a_display.y - kScreenPad);
            a_pivot = ImVec2(0.0F, 1.0F);
            break;
        case Settings::OsdAnchor::kBottomRight:
            a_pos = ImVec2(a_display.x - kScreenPad, a_display.y - kScreenPad);
            a_pivot = ImVec2(1.0F, 1.0F);
            break;
        case Settings::OsdAnchor::kCustom:
        default:
            a_pos = ImVec2(0.0F, 0.0F);
            a_pivot = ImVec2(0.0F, 0.0F);
            break;
        }
    }
}

namespace UI::Osd
{
    bool Visible() noexcept
    {
        return Settings::OsdEnabledMirror().load(std::memory_order_acquire);
    }

    void Toggle()
    {
        bool nowEnabled = false;
        {
            std::scoped_lock lock(UI::Menu::InputMutex());
            auto& settings = Settings::Menu();
            settings.osdEnabled = !settings.osdEnabled;
            nowEnabled = settings.osdEnabled;
            Settings::OsdEnabledMirror().store(nowEnabled, std::memory_order_release);
            Settings::MarkDirty();
        }
        logger::info("[OSD] overlay {}", nowEnabled ? "shown" : "hidden");
    }

    void RequestFontReload() noexcept
    {
        g_fontReloadPending.store(true, std::memory_order_release);
    }

    void NotifyContextReset() noexcept
    {
        g_customFont = nullptr;
        g_fontStatus = "built-in font";
        g_loadedFontPath = Settings::FontPath{};
        g_fontReloadPending.store(true, std::memory_order_release);
    }

    const char* FontStatus() noexcept
    {
        return g_fontStatus;
    }

    void ServicePendingFontLoad()
    {
        if (!g_fontReloadPending.exchange(false, std::memory_order_acq_rel)) {
            return;
        }

        const char* path = Settings::Menu().osdFontPath.data();

        if (std::strncmp(path, g_loadedFontPath.data(), g_loadedFontPath.size()) == 0) {
            return;
        }
        const std::size_t pathLength = ::strnlen(path, g_loadedFontPath.size() - 1);
        std::memcpy(g_loadedFontPath.data(), path, pathLength);
        g_loadedFontPath[pathLength] = '\0';

        if (path[0] == '\0') {
            g_customFont = nullptr;
            g_fontStatus = "built-in font";
            return;
        }

        ImGuiIO& io = ImGui::GetIO();

        ImFontConfig config;
        config.Flags |= ImFontFlags_NoLoadError;
        ImFont* loaded = io.Fonts->AddFontFromFileTTF(path, 20.0F, &config);
        if (loaded) {
            g_customFont = loaded;
            g_fontStatus = "custom font loaded";
            logger::info("[OSD] custom font loaded: {}", path);
        } else {
            g_customFont = nullptr;
            g_fontStatus = "font failed to load — using the built-in font";
            logger::warn("[OSD] custom font could not be loaded ({}) — falling back to the built-in font", path);
        }
    }

    void Draw(bool a_menuOpen)
    {
        auto& settings = Settings::Menu();
        const ImVec2 display = ImGui::GetIO().DisplaySize;

        const bool editable = a_menuOpen;
        const bool custom =
            settings.osdAnchor == static_cast<std::uint32_t>(Settings::OsdAnchor::kCustom);
        const bool autoSize = !settings.HasOsdGeometry();

        ImGuiWindowFlags flags = ImGuiWindowFlags_NoSavedSettings |
                                 ImGuiWindowFlags_NoFocusOnAppearing |
                                 ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNav;
        if (!editable) {
            flags |= ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs;
        }
        if (autoSize) {
            flags |= ImGuiWindowFlags_AlwaysAutoResize;
        }

        if (custom) {
            ImGui::SetNextWindowPos(ImVec2(settings.osdX, settings.osdY), ImGuiCond_Once);
        } else {
            ImVec2 pos{};
            ImVec2 pivot{};
            ResolveAnchor(settings.osdAnchor, display, pos, pivot);
            ImGui::SetNextWindowPos(pos, ImGuiCond_Always, pivot);
        }
        if (!autoSize) {
            ImGui::SetNextWindowSize(ImVec2(settings.osdW, settings.osdH), ImGuiCond_Once);
        }

        ImGui::PushStyleColor(ImGuiCol_WindowBg,
            ToVec4(settings.osdBgR, settings.osdBgG, settings.osdBgB, 1.0F));
        ImGui::PushStyleColor(ImGuiCol_Text,
            ToVec4(settings.osdTextR, settings.osdTextG, settings.osdTextB, settings.osdTextA));
        ImGui::SetNextWindowBgAlpha(settings.osdBgA);

        ImGui::Begin("FO4GraphicsOverhaul OSD", nullptr, flags);

        ImGui::PushFont(g_customFont, settings.osdFontSize);

        static Telemetry::FrameStats s_frame{};
        static double s_lastCompute = -1.0;
        const double now = ImGui::GetTime();
        if (now - s_lastCompute > 0.25) {
            s_frame = Telemetry::CurrentFrameStats();
            s_lastCompute = now;
        }
        const Telemetry::Snapshot snap = Telemetry::Read();

        static double s_fgSampleTime = -1.0;
        static std::uint64_t s_fgLastReal = 0, s_fgLastDlss = 0, s_fgLastFsr = 0;
        static float s_realFps = 0.0F, s_generatedFps = 0.0F;
        static const char* s_fgBackend = "DLSS-G";
        if (s_fgSampleTime < 0.0 || now < s_fgSampleTime || now - s_fgSampleTime >= 1.0) {
            const auto proxy = Platform::PresentProxy::Snapshot();
            const std::uint64_t dlss = proxy.interpolated;
            const std::uint64_t fsr = Platform::FsrFrameGen::Generated();
            const bool monotonic = now > s_fgSampleTime && proxy.presents >= s_fgLastReal &&
                                   dlss >= s_fgLastDlss && fsr >= s_fgLastFsr;
            if (s_fgSampleTime >= 0.0 && monotonic) {
                const double seconds = now - s_fgSampleTime;
                const std::uint64_t dDlss = dlss - s_fgLastDlss;
                const std::uint64_t dFsr = fsr - s_fgLastFsr;
                s_realFps = static_cast<float>(static_cast<double>(proxy.presents - s_fgLastReal) / seconds);
                s_generatedFps = static_cast<float>(static_cast<double>(dDlss + dFsr) / seconds);
                s_fgBackend = dFsr > dDlss ? "AMD FSR" : "DLSS-G";
            } else {
                s_realFps = 0.0F;
                s_generatedFps = 0.0F;
            }
            s_fgLastReal = proxy.presents;
            s_fgLastDlss = dlss;
            s_fgLastFsr = fsr;
            s_fgSampleTime = now;
        }
        const bool generating = s_generatedFps > 0.0F &&
                                (Platform::FrameGenEngine::Interpolating() || Platform::FsrFrameGen::Selected());

        if (settings.osdShowFps) {
            if (generating) {
                ImGui::Text("%.0f FPS  (%.0f real + %.0f generated by %s)",
                    static_cast<double>(s_realFps + s_generatedFps), static_cast<double>(s_realFps),
                    static_cast<double>(s_generatedFps), s_fgBackend);
            } else if (s_frame.sampleCount > 0) {
                ImGui::Text("%.0f FPS", static_cast<double>(s_frame.averageFps));
            } else {
                ImGui::TextUnformatted("-- FPS");
            }
        }
        if (settings.osdShowFrametime) {
            ImGui::Text("%.2f ms", static_cast<double>(s_frame.averageMs));
        }
        if (settings.osdShowLows) {
            ImGui::Text("1%%: %.0f   0.1%%: %.0f", static_cast<double>(s_frame.onePercentLowFps),
                static_cast<double>(s_frame.pointOnePercentLowFps));
        }
        if (settings.osdShowCpu) {
            if (snap.cpuValid) {
                ImGui::Text("CPU  %.0f%%", static_cast<double>(snap.processCpuPercent));
            } else {
                ImGui::TextUnformatted("CPU  N/A");
            }
        }
        if (settings.osdShowRam) {
            if (snap.memValid) {
                ImGui::Text("RAM  %.1f GB (%.1f GB free)", ToGiB(snap.processPrivateBytes),
                    ToGiB(snap.systemAvailBytes));
            } else {
                ImGui::TextUnformatted("RAM  N/A");
            }
        }
        if (settings.osdShowVram) {
            if (snap.vramValid) {
                const std::uint64_t free = snap.vramBudgetBytes > snap.vramUsedBytes
                                               ? snap.vramBudgetBytes - snap.vramUsedBytes
                                               : 0;
                ImGui::Text("VRAM %.1f GB (%.1f GB free)", ToGiB(snap.vramUsedBytes), ToGiB(free));
            } else {
                ImGui::TextUnformatted("VRAM N/A");
            }
        }
        if (settings.osdShowGpu) {
            if (snap.gpuValid) {
                ImGui::Text("GPU  %u%%", snap.gpuUtilPercent);
            } else {
                ImGui::TextUnformatted("GPU  N/A");
            }
        }
        if (settings.osdShowGpuTemp) {
            if (snap.gpuValid) {
                ImGui::Text("GPU  %u C", snap.gpuTempC);
            } else {
                ImGui::TextUnformatted("GPU  -- C");
            }
        }
        if (settings.osdShowCpuTemp) {
            ImGui::TextUnformatted("CPU  N/A C");
        }
        if (settings.osdShowLoad) {
            if (snap.loadValid) {
                ImGui::Text("Load %.1f s", static_cast<double>(snap.lastLoadSeconds));
            } else {
                ImGui::TextUnformatted("Load --");
            }
        }
        if (settings.osdShowNeural) {
            const auto neural = Platform::NeuralPass::Snapshot();
            if (neural.enabled && neural.activePasses != 0U && neural.modelWidth != 0U) {
                ImGui::Text("NR %u pass%s  %u x %u", neural.activePasses, neural.activePasses == 1U ? "" : "es",
                    neural.modelWidth, neural.modelHeight);
            } else {
                ImGui::TextUnformatted(neural.enabled ? "NR --" : "NR off");
            }
        }
        if (editable) {
            ImGui::Separator();
            ImGui::TextUnformatted("(drag / resize while the menu is open)");
        }

        ImGui::PopFont();

        if (editable) {
            const ImVec2 pos = ImGui::GetWindowPos();
            const ImVec2 size = ImGui::GetWindowSize();
            bool changed = false;
            if (custom && (pos.x != settings.osdX || pos.y != settings.osdY)) {
                settings.osdX = pos.x;
                settings.osdY = pos.y;
                changed = true;
            }
            if (!autoSize && !ImGui::IsWindowCollapsed() &&
                (size.x != settings.osdW || size.y != settings.osdH)) {
                settings.osdW = size.x;
                settings.osdH = size.y;
                changed = true;
            }
            if (changed) {
                Settings::MarkDirty();
            }
        }

        ImGui::End();
        ImGui::PopStyleColor(2);
    }
}
