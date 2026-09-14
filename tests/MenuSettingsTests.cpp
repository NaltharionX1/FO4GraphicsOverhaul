#include "Settings/MenuSettingsIO.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>

namespace
{
    int g_failures = 0;

    void Check(bool a_condition, const char* a_label)
    {
        if (!a_condition) {
            ++g_failures;
            std::printf("FAIL: %s\n", a_label);
        }
    }

    [[nodiscard]] std::string ReadAll(const char* a_path)
    {
        std::ifstream in(a_path, std::ios::binary);
        std::ostringstream out;
        out << in.rdbuf();
        return out.str();
    }

    void WriteFile(const char* a_path, const char* a_contents)
    {
        std::ofstream out(a_path, std::ios::binary);
        out << a_contents;
    }

    [[nodiscard]] Settings::FontPath MakePath(const char* a_text)
    {
        Settings::FontPath path{};
        const std::size_t length = std::strlen(a_text);
        if (length < path.size()) {
            std::memcpy(path.data(), a_text, length);
        }
        return path;
    }

    [[nodiscard]] bool PathIs(const Settings::FontPath& a_path, const char* a_expected)
    {
        return std::strncmp(a_path.data(), a_expected, a_path.size()) == 0;
    }
}

int main(int a_argc, char** a_argv)
{
    if (a_argc == 3 && std::strcmp(a_argv[1], "--emit") == 0) {
        const Settings::MenuSettings defaults{};
        if (!Settings::IO::Save(a_argv[2], defaults)) {
            std::printf("emit failed: %s\n", a_argv[2]);
            return 1;
        }
        std::printf("canonical INI emitted: %s\n", a_argv[2]);
        return 0;
    }

    const char* path = "MenuSettingsTests.roundtrip.ini";
    constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
    constexpr float kInf = std::numeric_limits<float>::infinity();

    const Settings::MenuSettings defaults{};
    Check(Settings::IO::Save(path, defaults), "save defaults");
    Settings::MenuSettings loaded{};
    Check(Settings::IO::Load(path, loaded), "load defaults");
    Check(loaded == defaults, "round-trip == defaults");

    const std::string firstBytes = ReadAll(path);
    Check(Settings::IO::Save(path, loaded), "re-save loaded");
    Check(ReadAll(path) == firstBytes, "byte-exact re-save");
    Check(!firstBytes.empty(), "file non-empty");

    Settings::MenuSettings custom{};
    custom.openHotkey = 45;
    custom.menuStyle = 0;
    custom.fontScale = 1.25F;
    custom.transparentMenu = true;
    custom.windowX = -1280.50F;
    custom.windowY = 64.00F;
    custom.windowW = 640.00F;
    custom.windowH = 480.00F;
    custom.aoMode = 2;
    custom.aoEnabled = true;
    custom.aoQuality = 3;
    custom.aoDenoisePasses = 0;
    custom.aoRadius = 140.00F;
    custom.aoRadiusMultiplier = 1.25F;
    custom.aoFalloffRange = 0.50F;
    custom.aoSampleDistributionPower = 1.75F;
    custom.aoOccluderThickness = 12.50F;
    custom.aoFinalValuePower = 3.25F;
    custom.aoMinScreenRadius = 6.00F;
    custom.aoDepthFadeEnabled = false;
    custom.aoDepthFadeStart = 30000.00F;
    custom.aoDepthFadeEnd = 60000.00F;
    custom.gradingEnabled = true;
    custom.engineGradingNeutral = true;
    custom.sceneExposureDisabled = true;
    custom.gameAutoExposureDisabled = true;
    custom.radialBlurSuppressed = true;
    custom.doubleVisionSuppressed = true;
    custom.fingerprintToggleHotkey = 109;
    custom.fingerprintMarkHotkey = 107;
    custom.neural.enabled = true;
    custom.neural.passes[0].style = 2;
    custom.neural.passes[0].intensity = 0.5F;
    custom.neural.passes[0].localTone = 0.75F;
    custom.neural.passes[0].localStructure = 0.25F;
    custom.neural.passes[0].skinStructure = 0.5F;
    custom.neural.passes[0].autoMask = false;
    custom.neural.modelPercent = 75;
    custom.neural.modelBeyondPlay = true;
    custom.osdShowNeural = true;
    Check(Settings::IO::Save(path, custom), "save custom");
    Settings::MenuSettings customLoaded{};
    Check(Settings::IO::Load(path, customLoaded), "load custom");
    Check(customLoaded == custom, "round-trip == custom");
    Check(customLoaded.HasWindowGeometry(), "custom geometry is recognized as stored");
    {
        const std::string written = ReadAll(path);
        Check(written.find("AutoSkinMask=0") != std::string::npos, "the writer emits AutoSkinMask (the skin slider's gate)");
        Check(written.find("AutoMask=") == std::string::npos, "the writer never emits the old AutoMask key");
        WriteFile(path, "[NeuralRendering]\nIntensity=0.5\nAutoMask=0\nSkinStructure=-1\n");
        Settings::MenuSettings old{};
        (void)Settings::IO::Load(path, old);
        Check(old.neural.passes[0].intensity == 0.5F, "the old file was parsed (so the two checks below are not vacuous)");
        Check(old.neural.passes[0].autoMask, "an old AutoMask = false is ignored: the mask stays on");
        Check(old.neural.passes[0].skinStructure == 1.0F, "an old SkinStructure = -1 (the retired 'auto') lands on 1.0");
        Check(old.neural.modelPercent == 100U, "a legacy file without ModelPercent loads the exact path (100)");
        Check(written.find("ModelPercent=75") != std::string::npos, "the writer emits ModelPercent in [NeuralRendering]");
        WriteFile(path, "[NeuralRendering]\nModelPercent=400\n");
        Settings::MenuSettings outOfRange{};
        (void)Settings::IO::Load(path, outOfRange);
        Check(outOfRange.neural.modelPercent == 100U, "an out-of-range ModelPercent (400) lands on 100, never on a clamp edge");
        Check(written.find("ModelBeyondPlay=1") != std::string::npos, "the writer emits ModelBeyondPlay in [NeuralRendering]");
        WriteFile(path, "[NeuralRendering]\nModelPercent=300\n");
        Settings::MenuSettings gated{};
        (void)Settings::IO::Load(path, gated);
        Check(gated.neural.modelPercent == 150U && !gated.neural.modelBeyondPlay,
            "300 without the gate loads as the play ceiling (150): the slider, the door and the pass agree");
        WriteFile(path, "[NeuralRendering]\nModelPercent=300\nModelBeyondPlay=1\n");
        Settings::MenuSettings opened{};
        (void)Settings::IO::Load(path, opened);
        Check(opened.neural.modelPercent == 300U && opened.neural.modelBeyondPlay, "300 with the gate open loads as 300");
        Check(old.neural.modelBeyondPlay == false, "a legacy file loads the gate closed");
    }

    Check(Settings::ClampHotkey(0) == 45, "hotkey 0 -> default (45 = VK_INSERT)");
    Check(Settings::ClampHotkey(255) == 45, "hotkey 255 -> default (45 = VK_INSERT)");
    Check(Settings::ClampHotkey(45) == 45, "hotkey valid passes");
    Check(Settings::ClampMenuStyle(3) == 2, "style 3 -> default");
    Check(Settings::ClampMenuStyle(1) == 1, "style valid passes");
    Check(Settings::ClampFontScale(0.0F) == 1.0F, "scale 0 -> default");
    Check(Settings::ClampFontScale(99.0F) == 1.0F, "scale 99 -> default");
    Check(Settings::ClampFontScale(0.75F) == 0.75F, "scale valid passes");

    Check(Settings::ClampAoMode(0) == 0, "ao mode Off passes");
    Check(Settings::ClampAoMode(2) == 2, "ao mode GTAO passes");
    Check(Settings::ClampAoMode(1) == 0, "legacy HBAO mode -> Off");
    Check(Settings::ClampAoMode(7) == 0, "unknown ao mode -> Off");
    Check(Settings::ClampAoQuality(3) == 3, "ao quality 3 passes");
    Check(Settings::ClampAoQuality(4) == 4, "ao quality 4 (Extreme, our tier) passes");
    Check(Settings::ClampAoQuality(9) == 2, "ao quality out of range -> default");
    Check(Settings::ClampAoDenoisePasses(0) == 0, "ao denoise 0 passes");
    Check(Settings::ClampAoDenoisePasses(4) == 1, "ao denoise out of range -> default");
    Check(Settings::ClampAoRadius(140.0F) == 140.0F, "ao radius valid passes");
    Check(Settings::ClampAoRadius(kNaN) == 70.0F, "ao radius NaN -> default");
    Check(Settings::ClampAoRadius(5000.0F) == 70.0F, "ao radius out of range -> default");
    Check(Settings::ClampAoDepthFadeEnd(60000.0F) == 60000.0F, "ao fade end valid passes");
    Check(Settings::ClampAoDepthFadeEnd(kInf) == 50000.0F, "ao fade end inf -> default");

    Check(Settings::ClampWindowSize(0.0F) == 0.0F, "size 0 stays unset");
    Check(Settings::ClampWindowSize(50.0F) == 0.0F, "size below minimum -> unset");
    Check(Settings::ClampWindowSize(kNaN) == 0.0F, "size NaN -> unset");
    Check(Settings::ClampWindowSize(640.0F) == 640.0F, "size valid passes");
    Check(Settings::ClampWindowSize(99999.0F) == 0.0F, "size beyond maximum -> unset");
    Check(Settings::ClampWindowSize(Settings::kMaxWindowExtent) == Settings::kMaxWindowExtent,
        "size at the maximum still passes");
    Check(Settings::ClampWindowCoord(-2000.0F) == -2000.0F, "negative coord passes (multi-monitor)");
    Check(Settings::ClampWindowCoord(kInf) == 0.0F, "coord inf -> 0");
    Check(Settings::ClampWindowCoord(kNaN) == 0.0F, "coord NaN -> 0");
    Check(Settings::ClampWindowCoord(99999.0F) == 0.0F, "coord beyond bound -> 0");

    {
        Settings::MenuSettings geom{};
        Check(!geom.HasWindowGeometry(), "fresh defaults have no stored geometry");
        geom.windowW = 640.0F;
        geom.windowH = 480.0F;
        Check(geom.HasWindowGeometry(), "stored size counts as geometry");
        geom.windowH = 0.0F;
        Check(!geom.HasWindowGeometry(), "half-stored geometry does not count");
    }

    WriteFile(path,
        "[Other]\nOpenHotkey=13\n[Menu]\nOpenHotkey=banana\nMenuStyle=7\nFontScale=nan\n"
        "TransparentMenu=maybe\nUnknownKey=5\nWindowX=banana\nWindowW=-5\nWindowH=1e9\n"
        "[Display]\nD3D12PresentProxy=0\n");
    Settings::MenuSettings hostile{};
    Check(Settings::IO::Load(path, hostile), "load hostile");
    Check(hostile == Settings::MenuSettings{}, "hostile input clamps to defaults");

    WriteFile(path, "[Menu]\nOpenHotkey=45\nMenuStyle=1\nFontScale=1.25\nTransparentMenu=1\n");
    Settings::MenuSettings legacy{};
    Check(Settings::IO::Load(path, legacy), "load legacy (pre-geometry) INI");
    Check(legacy.openHotkey == 45, "legacy hotkey preserved");
    Check(legacy.menuStyle == 1, "legacy style preserved");
    Check(legacy.fontScale == 1.25F, "legacy font scale preserved");
    Check(legacy.transparentMenu, "legacy transparency preserved");
    Check(!legacy.HasWindowGeometry(), "legacy INI leaves geometry unset");

    WriteFile(path, "[OSD]\nOpenHotkey=45\nWindowW=640\n[Menu]\nMenuStyle=1\n");
    Settings::MenuSettings scoped{};
    Check(Settings::IO::Load(path, scoped), "load section-scoped");
    Check(scoped.openHotkey == Settings::MenuSettings{}.openHotkey, "[OSD]OpenHotkey ignored for [Menu]");
    Check(!scoped.HasWindowGeometry(), "[OSD]WindowW ignored for [Menu]");
    Check(scoped.menuStyle == 1, "[Menu] key still applied");

    {
        Settings::MenuSettings osd{};
        osd.osdEnabled = true;
        osd.osdHotkey = 118;
        osd.osdAnchor = static_cast<std::uint32_t>(Settings::OsdAnchor::kBottomLeft);
        osd.osdX = -640.00F;
        osd.osdY = 32.00F;
        osd.osdW = 220.00F;
        osd.osdH = 140.00F;
        osd.osdFontSize = 22.00F;
        osd.osdFontPath = MakePath("C:\\Windows\\Fonts\\consola.ttf");
        osd.osdTextR = 0.90F;
        osd.osdBgA = 0.25F;
        osd.osdShowLows = true;
        osd.osdShowGpuTemp = false;
        Check(Settings::IO::Save(path, osd), "save OSD block");
        Settings::MenuSettings osdLoaded{};
        Check(Settings::IO::Load(path, osdLoaded), "load OSD block");
        Check(osdLoaded == osd, "round-trip == OSD block");
        Check(PathIs(osdLoaded.osdFontPath, "C:\\Windows\\Fonts\\consola.ttf"),
            "font path survives a round trip verbatim (backslashes intact)");
        Check(osdLoaded.HasOsdGeometry(), "stored OSD size counts as geometry");

        const std::string bytes = ReadAll(path);
        Check(Settings::IO::Save(path, osdLoaded), "re-save OSD block");
        Check(ReadAll(path) == bytes, "byte-exact re-save with a string field present");
    }

    {
        Settings::FontPath dirty = MakePath("C:\\fonts\\a.ttf");
        dirty[7] = '\n';
        const Settings::FontPath clean = Settings::ClampFontPath(dirty);
        Check(PathIs(clean, "C:\\font"), "font path truncates at the first control character");

        Settings::FontPath unterminated{};
        unterminated.fill('A');
        const Settings::FontPath fixed = Settings::ClampFontPath(unterminated);
        Check(fixed[Settings::kFontPathCapacity - 1] == '\0', "font path is always NUL-terminated");
    }

    {
        std::string huge = "[OSD]\nFontPath=";
        huge.append(Settings::kFontPathCapacity + 50, 'x');
        huge += "\n";
        WriteFile(path, huge.c_str());
        Settings::MenuSettings loadedHuge{};
        Check(Settings::IO::Load(path, loadedHuge), "load over-long font path");
        Check(loadedHuge.osdFontPath[0] == '\0', "over-long font path is rejected, not truncated");
    }

    Check(Settings::ClampOsdAnchor(9) == Settings::MenuSettings{}.osdAnchor, "bad anchor -> default");
    Check(Settings::ClampOsdAnchor(4) == 4, "Custom anchor is valid");
    Check(Settings::ClampOsdFontSize(1.0F) == Settings::kOsdFontSizeMin, "tiny font size clamps up");
    Check(Settings::ClampOsdFontSize(900.0F) == Settings::kOsdFontSizeMax, "huge font size clamps down");
    Check(Settings::ClampOsdFontSize(kNaN) == Settings::MenuSettings{}.osdFontSize, "NaN font size -> default");
    Check(Settings::ClampColorChannel(-1.0F) == 0.0F, "negative colour channel clamps to 0");
    Check(Settings::ClampColorChannel(5.0F) == 1.0F, "over-bright colour channel clamps to 1");
    Check(Settings::ClampColorChannel(kNaN) == 0.0F, "NaN colour channel -> 0");
    Check(Settings::ClampOsdSize(10.0F) == 0.0F, "OSD size below the floor -> unset");

    WriteFile(path, "[Menu]\nHotkey=45\nAnchor=3\n[OSD]\nOpenHotkey=45\nMenuStyle=0\n");
    {
        Settings::MenuSettings scopedBoth{};
        Check(Settings::IO::Load(path, scopedBoth), "load cross-section file");
        Check(scopedBoth.osdHotkey == Settings::MenuSettings{}.osdHotkey,
            "[Menu]Hotkey does not reach the OSD hotkey");
        Check(scopedBoth.osdAnchor == Settings::MenuSettings{}.osdAnchor,
            "[Menu]Anchor does not reach the OSD anchor");
        Check(scopedBoth.openHotkey == Settings::MenuSettings{}.openHotkey,
            "[OSD]OpenHotkey does not reach the menu hotkey");
        Check(scopedBoth.menuStyle == Settings::MenuSettings{}.menuStyle,
            "[OSD]MenuStyle does not reach the menu style");
    }

    {
        Settings::MenuSettings display{};
        display.vsyncEnabled = true;
        display.vsyncInterval = 2;
        display.fpsUnlimited = false;
        display.fpsLimit = 144;
        display.loadingScreenUnlimited = false;
        display.loadingScreenFpsLimit = 90;
        display.lockCursor = true;
        display.gsyncFlickerFix = true;
        display.disableActorFade = true;
        display.disablePlayerFade = true;
        Check(Settings::IO::Save(path, display), "save Display block");
        Settings::MenuSettings displayLoaded{};
        Check(Settings::IO::Load(path, displayLoaded), "load Display block");
        Check(displayLoaded == display, "round-trip == Display block");
    }

    Check(Settings::ClampVSyncInterval(0) == 1, "vsync interval 0 -> default (off is vsyncEnabled)");
    Check(Settings::ClampVSyncInterval(5) == 1, "vsync interval beyond max -> default");
    Check(Settings::ClampVSyncInterval(4) == 4, "vsync interval at the max passes");
    Check(Settings::ClampFpsLimit(0) == Settings::kFpsLimitMin,
        "fps limit 0 -> floor (off is the Unlimited flag, not a zero value)");
    Check(Settings::ClampFpsLimit(1) == Settings::kFpsLimitMin,
        "fps limit 1 -> floor (a 1 fps cap looks like a frozen game)");
    Check(Settings::ClampFpsLimit(59) == Settings::kFpsLimitMin, "fps limit below the floor -> floor");
    Check(Settings::ClampFpsLimit(Settings::kFpsLimitMin) == Settings::kFpsLimitMin,
        "fps limit at the floor passes");
    Check(Settings::ClampFpsLimit(144) == 144, "fps limit valid passes");
    Check(Settings::ClampFpsLimit(Settings::kFpsLimitMax) == Settings::kFpsLimitMax,
        "fps limit at the max passes");
    Check(Settings::ClampFpsLimit(Settings::kFpsLimitMax + 1) == Settings::kFpsLimitMin,
        "fps limit beyond the max -> floor");

    {
        const Settings::MenuSettings shipped{};
        Check(shipped.fpsUnlimited, "general limiter ships unlimited");
        Check(shipped.loadingScreenUnlimited, "loading-screen limiter ships unlimited");
        Check(shipped.fpsLimit == Settings::kFpsLimitMin, "general limiter's stored value is the floor");
        Check(shipped.loadingScreenFpsLimit == Settings::kFpsLimitMin,
            "loading limiter's stored value is the floor");
    }

    WriteFile(path,
        "[Display]\nVSync=maybe\nVSyncInterval=99\nFpsUnlimited=banana\nFpsLimit=999999\n"
        "LoadingScreenFpsLimit=1\nLockCursor=banana\nGSyncFlickerFix=maybe\n");
    {
        Settings::MenuSettings hostileDisplay{};
        Check(Settings::IO::Load(path, hostileDisplay), "load hostile Display block");
        Check(hostileDisplay == Settings::MenuSettings{}, "hostile Display input clamps to defaults");
    }

    {
        WriteFile(path, "[NeuralRendering]\nEnabled=true\nPassCount=3\nStyle=1\nIntensity=0.5\n"
                        "[NeuralRendering.Pass2]\nStyle=2\nIntensity=0.25\nLocalTone=0.5\n"
                        "[NeuralRendering.Pass3]\nStyle=0\nIntensity=0.75\nLocalTone=0.75\n");
        Settings::MenuSettings cascade{};
        Check(Settings::IO::Load(path, cascade), "load three independently configured neural passes");
        Check(Settings::IO::Save(path, cascade), "save three independently configured neural passes");
        const std::string saved = ReadAll(path);
        Check(saved.find("PassCount=3") != std::string::npos, "neural pass count survives INI read/write");
        Check(saved.find("[NeuralRendering.Pass2]") != std::string::npos,
            "neural pass 2 is preserved instead of discarded as an unknown section");
        Check(saved.find("[NeuralRendering.Pass3]") != std::string::npos,
            "neural pass 3 is preserved instead of discarded as an unknown section");
        Check(cascade.neural.passCount == 3 && cascade.neural.passes[0].style == 1 &&
                  cascade.neural.passes[1].style == 2 && cascade.neural.passes[2].style == 0,
            "all three neural styles are independent");
        Check(cascade.neural.passes[0].intensity == 0.5F && cascade.neural.passes[1].intensity == 0.25F &&
                  cascade.neural.passes[2].intensity == 0.75F && cascade.neural.passes[1].localTone == 0.5F,
            "strength and local tone belong to their configured neural stage");
        cascade.neural.passCount = 1;
        Check(Settings::IO::Save(path, cascade), "save Standard with dormant stage values");
        Settings::MenuSettings dormant{};
        Check(Settings::IO::Load(path, dormant) && dormant == cascade,
            "reducing count preserves dormant pass 2 and pass 3 settings");
        WriteFile(path, "[NeuralRendering]\nStyle=2\nIntensity=0.25\n");
        Settings::MenuSettings legacyCascade{};
        Check(Settings::IO::Load(path, legacyCascade) && legacyCascade.neural.passCount == 1 &&
                  legacyCascade.neural.passes[0].style == 2 && legacyCascade.neural.passes[0].intensity == 0.25F &&
                  legacyCascade.neural.passes[1].style == 0 && legacyCascade.neural.passes[1].intensity == 1.0F,
            "legacy tuned settings become pass 1 without activating or copying into extra stages");
        WriteFile(path, "[NeuralRendering]\nPassCount=4\n[NeuralRendering.Pass3]\nStyle=99\nIntensity=8\nLocalTone=8\n");
        Settings::MenuSettings invalid{};
        Check(Settings::IO::Load(path, invalid) && invalid.neural.passCount == 1 &&
                  invalid.neural.passes[2].style == 0 && invalid.neural.passes[2].intensity == 1.0F &&
                  invalid.neural.passes[2].localTone == 1.0F,
            "out-of-range count and dormant-stage fields fail closed to their defaults");
    }

    {
        WriteFile(path, "[Diagnostics]\nFingerprintToggleHotkey=999\nFingerprintMarkHotkey=0\n");
        Settings::MenuSettings keys{};
        keys.fingerprintToggleHotkey = 109;
        keys.fingerprintMarkHotkey = 107;
        Check(Settings::IO::Load(path, keys) && keys.fingerprintToggleHotkey == 0 && keys.fingerprintMarkHotkey == 0,
            "optional keys: out of range -> unset, 0 stays unset (the rows are visited: the pre-set values were replaced)");
        WriteFile(path, "[Diagnostics]\nFingerprintToggleHotkey=109\nFingerprintMarkHotkey=107\n");
        Check(Settings::IO::Load(path, keys) && keys.fingerprintToggleHotkey == 109 && keys.fingerprintMarkHotkey == 107,
            "optional keys are read");
        Check(Settings::MenuSettings{}.fingerprintToggleHotkey == 0 && Settings::MenuSettings{}.fingerprintMarkHotkey == 0,
            "both default to unset");
        Check(Settings::ClampOptionalHotkey(0) == 0 && Settings::ClampOptionalHotkey(1) == 1 && Settings::ClampOptionalHotkey(254) == 254 &&
                  Settings::ClampOptionalHotkey(255) == 0 && Settings::ClampOptionalHotkey(0xFFFFFFFFU) == 0,
            "the optional clamp's boundaries: 0 legal, [1, 254] legal, 255 and a wrapped negative -> unset");
    }

    std::remove(path);

    if (g_failures == 0) {
        std::printf("MenuSettingsTests: all checks passed\n");
        return 0;
    }
    std::printf("MenuSettingsTests: %d failure(s)\n", g_failures);
    return 1;
}
