#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace Platform
{
    enum class Tier : std::uint8_t
    {
        kOwnedAddress = 0,

        kConsoleValue,

        kConsoleComposite,

        kConsoleSwitch,

        kConsoleToggle,

        kOwnedSetting,

        kOwnedModule,
    };

    [[nodiscard]] constexpr bool IsPersistable(Tier a_tier) noexcept
    {
        return a_tier != Tier::kConsoleToggle;
    }

    enum class Policy : std::uint8_t
    {
        kNone = 0,
        kSessionOnly = 1U << 0,
        kNoFrameSweep = 1U << 1,
    };

    [[nodiscard]] constexpr Policy operator|(Policy a_lhs, Policy a_rhs) noexcept
    {
        return static_cast<Policy>(
            static_cast<std::uint8_t>(a_lhs) | static_cast<std::uint8_t>(a_rhs));
    }

    [[nodiscard]] constexpr bool HasPolicy(Policy a_value, Policy a_flag) noexcept
    {
        return (static_cast<std::uint8_t>(a_value) & static_cast<std::uint8_t>(a_flag)) != 0U;
    }

    struct CatalogueRow;

    [[nodiscard]] constexpr bool ShouldPersist(const CatalogueRow& a_row) noexcept;

    enum class ValueType : std::uint8_t
    {
        kFloat = 0,
        kInt,
        kBool,
        kNone,
    };

    enum class Evidence : std::uint8_t
    {
        kFieldVerified = 0,
        kUncertain,
    };

    struct ValueRange
    {
        double min;
        double max;
    };

    enum class SliderScale : std::uint8_t
    {
        kLinear = 0,
        kLogarithmic,
    };

    struct GroupSectionOrder
    {
        const char* group;
        const char* sections[10];
    };

    inline constexpr GroupSectionOrder kGroupSectionOrder[]{
        { "Lighting",
            { "Godrays", "Character Lighting", "Bloom", "Scene Exposure", "Adaptation",
                "Luminance", "Sky", "Lens Flare", nullptr } },
        { "Camera", { "Field of View", "Blur", "Depth of Field", nullptr } },
        { "Weather", { "Fog", "Rain", nullptr } },
    };

    struct CatalogueRow
    {
        const char* id;
        const char* label;
        const char* group;

        const char* section;

        const char* help;

        Tier tier;

        const char* command;

        std::uint64_t relocationId;

        std::uint8_t argIndex;
        std::uint8_t argCount;

        ValueType type;
        ValueRange range;
        double vanillaDefault;
        Evidence evidence;

        SliderScale scale;

        Policy policy;

    };

    constexpr bool ShouldPersist(const CatalogueRow& a_row) noexcept
    {
        return IsPersistable(a_row.tier) && !HasPolicy(a_row.policy, Policy::kSessionOnly);
    }

    inline constexpr const char* kCatalogueGroups[]{
        "Lighting",
        "Reflections",
        "Colour grading",
        "Camera",
        "Weather",
        "Shadows",
        "LOD & distance",
        "Scene toggles",
        "Debug",
    };

    inline constexpr CatalogueRow kGodrayControls[]{
        { "Godrays.Enable", "Godrays", "Lighting", "Godrays",
            "Master switch for the volumetric lighting pass. Owned: the mod writes the engine's own "
            "switch directly and the console 'gr' command is blocked, so nothing else can move it.",
            Tier::kConsoleSwitch, "gr", 0U, 0U, 1U,
            ValueType::kBool, ValueRange{ 0.0, 1.0 }, 1.0, Evidence::kFieldVerified },

        { "Godrays.Scale", "Godray intensity", "Lighting", "Godrays",
            "Strength of the effect. Owned: your value is re-asserted every frame, so console and "
            "INI changes to it are overridden.",
            Tier::kOwnedAddress, nullptr, 70509U, 0U, 1U,
            ValueType::kFloat, ValueRange{ 0.1, 1.0 }, 1.0, Evidence::kFieldVerified },

        { "Godrays.Grid", "Godray grid density", "Lighting", "Godrays",
            "Tessellation density of the godray volume, and the main cost control. Whole numbers "
            "only. Below 12 nothing happens; past 256 the image stops changing. The cap is "
            "deliberate - far higher values nearly crash the game.",
            Tier::kOwnedAddress, nullptr, 289565U, 0U, 1U,
            ValueType::kInt, ValueRange{ 12.0, 256.0 }, 64.0, Evidence::kFieldVerified },

        { "Godrays.Quality", "Godray quality", "Lighting", "Godrays",
            "Sampling quality of the volumetric pass - four discrete steps, 0 to 3. The old build's "
            "0-10 slider was pushing it past what the engine understands.",
            Tier::kOwnedAddress, nullptr, 957649U, 0U, 1U,
            ValueType::kInt, ValueRange{ 0.0, 3.0 }, 2.0, Evidence::kFieldVerified },

        { "Godrays.Cascade", "Godray max cascade", "Lighting", "Godrays",
            "How many shadow cascades the volumetric pass samples. Fewer is cheaper.",
            Tier::kOwnedAddress, nullptr, 542491U, 0U, 1U,
            ValueType::kInt, ValueRange{ 1.0, 3.0 }, 2.0, Evidence::kFieldVerified },
    };

    inline constexpr CatalogueRow kReflectionControls[]{
        { "Ssr.Toggle", "Screen-space reflections", "Reflections", nullptr,
            "Screen-space reflections on or off. The engine has a second say: it can demand "
            "reflections for the scene regardless of this switch, and the menu shows when that "
            "is happening.",
            Tier::kConsoleSwitch, "ssr", 0U, 0U, 1U,
            ValueType::kBool, ValueRange{ 0.0, 1.0 }, 1.0, Evidence::kFieldVerified },

        { "Ssr.Intensity", "SSR intensity", "Reflections", nullptr,
            "Overall strength of screen-space reflections. The command is unbounded; the ceiling "
            "here is where the user found it already looking bad, not a target.",
            Tier::kConsoleValue, "ssri", 0U, 0U, 1U,
            ValueType::kFloat, ValueRange{ 0.0, 10.0 }, 1.0, Evidence::kFieldVerified },

        { "Ssr.BlendPower", "SSR blend power", "Reflections", nullptr,
            "How sharply reflections blend into the surface beneath them.",
            Tier::kConsoleValue, "ssrbp", 0U, 0U, 1U,
            ValueType::kFloat, ValueRange{ 0.0, 10.0 }, 1.0, Evidence::kFieldVerified },

        { "Ssr.AngleThreshold", "SSR angle threshold", "Reflections", nullptr,
            "Viewing angle past which reflections are rejected. Lower shows reflections on more "
            "surfaces, at the cost of more artefacts.",
            Tier::kConsoleValue, "ssrat", 0U, 0U, 1U,
            ValueType::kFloat, ValueRange{ 0.0, 1.0 }, 0.5, Evidence::kFieldVerified },

        { "Ssr.VerticalAlignPower", "SSR vertical alignment", "Reflections", nullptr,
            "How strongly reflections are aligned to vertical surfaces.",
            Tier::kConsoleValue, "ssrvap", 0U, 0U, 1U,
            ValueType::kFloat, ValueRange{ 0.0, 100.0 }, 1.0, Evidence::kFieldVerified },

    };

    inline constexpr CatalogueRow kCharacterControls[]{
        { "CharLight.Enable", "Character lighting", "Lighting", "Character Lighting",
            "Master switch for the character light that follows the player.",
            Tier::kConsoleSwitch, "cl", 0U, 0U, 1U,
            ValueType::kBool, ValueRange{ 0.0, 1.0 }, 1.0, Evidence::kFieldVerified },

        { "CharLight.Rim", "Rim light", "Lighting", "Character Lighting",
            "Edge light that separates characters from the background. The control that keeps faces "
            "readable in shadow.",
            Tier::kConsoleValue, "cl rim", 0U, 0U, 1U,
            ValueType::kFloat, ValueRange{ 0.0, 2.0 }, 1.0, Evidence::kFieldVerified },

        { "CharLight.Fill", "Fill light", "Lighting", "Character Lighting",
            "Soft frontal light lifting characters out of darkness.",
            Tier::kConsoleValue, "cl fill", 0U, 0U, 1U,
            ValueType::kFloat, ValueRange{ 0.0, 2.0 }, 1.0, Evidence::kFieldVerified },
    };

    inline constexpr CatalogueRow kColourControls[]{
        { "Scp.Saturation", "Saturation", "Colour grading", nullptr,
            "Colour intensity, applied by this mod's own pass over the game's per-weather "
            "grading. 1.0 leaves the picture exactly as the game drew it.",
            Tier::kConsoleComposite, "scp", 0U, 0U, 3U,
            ValueType::kFloat, ValueRange{ 0.0, 10.0 }, 1.000000, Evidence::kFieldVerified,
            SliderScale::kLogarithmic },
        { "Scp.Brightness", "Brightness", "Colour grading", nullptr,
            "Overall lightness, applied over the game's own grading. 1.0 changes nothing.",
            Tier::kConsoleComposite, "scp", 0U, 1U, 3U,
            ValueType::kFloat, ValueRange{ 0.0, 10.0 }, 1.000000, Evidence::kFieldVerified,
            SliderScale::kLogarithmic },
        { "Scp.Contrast", "Contrast", "Colour grading", nullptr,
            "Separation between darks and lights, about mid-grey. 1.0 changes nothing.",
            Tier::kConsoleComposite, "scp", 0U, 2U, 3U,
            ValueType::kFloat, ValueRange{ 0.0, 10.0 }, 1.000000, Evidence::kFieldVerified,
            SliderScale::kLogarithmic },

        { "Stp.Red", "Tint red", "Colour grading", nullptr,
            "Red channel of the colour tint, applied by this mod's own pass with the other three.",
            Tier::kConsoleComposite, "stp", 0U, 0U, 4U,
            ValueType::kFloat, ValueRange{ 0.0, 1.0 }, 0.0, Evidence::kFieldVerified,
            SliderScale::kLogarithmic },
        { "Stp.Green", "Tint green", "Colour grading", nullptr,
            "Green channel of the colour tint.",
            Tier::kConsoleComposite, "stp", 0U, 1U, 4U,
            ValueType::kFloat, ValueRange{ 0.0, 1.0 }, 0.0, Evidence::kFieldVerified,
            SliderScale::kLogarithmic },
        { "Stp.Blue", "Tint blue", "Colour grading", nullptr,
            "Blue channel of the colour tint.",
            Tier::kConsoleComposite, "stp", 0U, 2U, 4U,
            ValueType::kFloat, ValueRange{ 0.0, 1.0 }, 0.0, Evidence::kFieldVerified,
            SliderScale::kLogarithmic },
        { "Stp.Strength", "Tint amount", "Colour grading", nullptr,
            "How much of the tint above is applied. Vanilla is 0 - the colour is configured but "
            "inactive, so raising this alone reveals the tint the game already ships.",
            Tier::kConsoleComposite, "stp", 0U, 3U, 4U,
            ValueType::kFloat, ValueRange{ 0.0, 1.0 }, 0.000000, Evidence::kFieldVerified,
            SliderScale::kLogarithmic },

        { "Shp.EyeAdaptSpeed", "Eye adapt speed", "Lighting", "Adaptation",
            "How fast exposure adjusts on moving between bright and dark. One of nine values sent "
            "together as a single 'shp' command.",
            Tier::kConsoleComposite, "shp", 0U, 0U, 9U,
            ValueType::kFloat, ValueRange{ 0.0, 10.0 }, 3.000000, Evidence::kFieldVerified,
            SliderScale::kLinear },
        { "Shp.EyeAdaptStrength", "Eye adapt strength", "Lighting", "Adaptation",
            "How far exposure is allowed to travel when it adapts.",
            Tier::kConsoleComposite, "shp", 0U, 1U, 9U,
            ValueType::kFloat, ValueRange{ 0.0, 10.0 }, 1.000000, Evidence::kFieldVerified,
            SliderScale::kLinear },

        { "Shp.BloomRadius", "Scene exposure", "Lighting", "Scene Exposure",
            "An exposure-LIKE shading stage of the engine (its RESPONSE is fully measured; "
            "what the shader actually is remains underived - the record misnames it "
            "'bloomBlurRadius', it does not control bloom, and it is not the game's "
            "auto-exposure). A MEASURED perfect loop: 0 is the darkest boundary, 1 the "
            "brightest, the full cycle between. Vanilla sits near 0.105. At exactly the "
            "default the weather keeps its own value.",
            Tier::kConsoleComposite, "shp", 0U, 2U, 9U,
            ValueType::kFloat, ValueRange{ 0.0, 1.0 }, 0.1047006, Evidence::kFieldVerified,
            SliderScale::kLinear },
        { "Shp.BloomThreshold", "Engine bloom threshold", "Lighting", "Bloom",
            "How bright a pixel must be before it blooms at all. Lower makes more of the scene glow.",
            Tier::kConsoleComposite, "shp", 0U, 3U, 9U,
            ValueType::kFloat, ValueRange{ 0.0, 10.0 }, 0.233586, Evidence::kFieldVerified,
            SliderScale::kLogarithmic },
        { "Shp.BloomScale", "Engine bloom scale", "Lighting", "Bloom",
            "Overall strength of the bloom that survives the threshold.",
            Tier::kConsoleComposite, "shp", 0U, 4U, 9U,
            ValueType::kFloat, ValueRange{ 0.0, 10.0 }, 0.300000, Evidence::kFieldVerified,
            SliderScale::kLogarithmic },

        { "Shp.TargetLumMax", "Target luminance max", "Lighting", "Luminance",
            "The brightest ambient luminosity auto-exposure will allow. Lower it to stop bright "
            "areas blowing out.",
            Tier::kConsoleComposite, "shp", 0U, 5U, 9U,
            ValueType::kFloat, ValueRange{ 0.0, 10.0 }, 1.899242, Evidence::kFieldVerified,
            SliderScale::kLinear },
        { "Shp.TargetLumMin", "Target luminance min", "Lighting", "Luminance",
            "The darkest ambient luminosity allowed - the floor auto-exposure will not go below. "
            "Raise it to lift crushed shadows, lower it for darker interiors.",
            Tier::kConsoleComposite, "shp", 0U, 6U, 9U,
            ValueType::kFloat, ValueRange{ 0.0, 10.0 }, 1.067172, Evidence::kFieldVerified,
            SliderScale::kLinear },
        { "Shp.SunlightScale", "Sunlight scale", "Lighting", "Sky",
            "Multiplier on direct sunlight before tonemapping.",
            Tier::kConsoleComposite, "shp", 0U, 7U, 9U,
            ValueType::kFloat, ValueRange{ 0.0, 10.0 }, 1.671722, Evidence::kFieldVerified,
            SliderScale::kLinear },
        { "Shp.SkyScale", "Sky scale", "Lighting", "Sky",
            "Multiplier on sky brightness before tonemapping. Vanilla leans on this heavily (2.8).",
            Tier::kConsoleComposite, "shp", 0U, 8U, 9U,
            ValueType::kFloat, ValueRange{ 0.0, 10.0 }, 2.798483, Evidence::kFieldVerified,
            SliderScale::kLinear },

        { "Bloom.Threshold", "Bloom threshold (our pass)", "Lighting", "Bloom",
            "How bright a displayed pixel must be before our glow picks it up. Lower makes "
            "more of the scene bloom. This is this mod's own bloom, layered over the game's - "
            "the engine's bloom values are untouched (those return with the imagespace-modifier "
            "stage).",
            Tier::kConsoleComposite, "blm", 0U, 0U, 3U,
            ValueType::kFloat, ValueRange{ 0.0, 1.0 }, 0.75, Evidence::kFieldVerified,
            SliderScale::kLinear },
        { "Bloom.Strength", "Bloom strength (our pass)", "Lighting", "Bloom",
            "How much of our glow is added. 0 switches the pass off entirely - the frame is "
            "untouched by construction, not merely dimmed.",
            Tier::kConsoleComposite, "blm", 0U, 1U, 3U,
            ValueType::kFloat, ValueRange{ 0.0, 2.0 }, 0.0, Evidence::kFieldVerified,
            SliderScale::kLinear },
        { "Bloom.Radius", "Bloom radius (our pass)", "Lighting", "Bloom",
            "How far the glow spreads from bright sources.",
            Tier::kConsoleComposite, "blm", 0U, 2U, 3U,
            ValueType::kFloat, ValueRange{ 0.5, 2.0 }, 1.0, Evidence::kFieldVerified,
            SliderScale::kLinear },

    };

    inline constexpr CatalogueRow kCameraControls[]{
        { "Fov.ThirdPerson", "Third-person FOV", "Camera", "Field of View",
            "Field of view for the third-person camera. Independent of the first-person FOV below "
            "- the console's 'fov' command cannot separate them.",
            Tier::kConsoleComposite, "fov", 0U, 0U, 2U,
            ValueType::kFloat, ValueRange{ 10.0, 160.0 }, 80.0, Evidence::kFieldVerified },
        { "Fov.World", "First-person FOV", "Camera", "Field of View",
            "Field of view for the first-person view. (Internally this is the setting Bethesda "
            "named 'fDefault1stPersonFOV' - the engine's FOV names are cross-wired with what they "
            "actually do, and the labels here follow behaviour, not names.)",
            Tier::kConsoleComposite, "fov", 0U, 1U, 2U,
            ValueType::kFloat, ValueRange{ 10.0, 160.0 }, 70.0, Evidence::kFieldVerified },

        { "Fov.PipBoy", "Pip-Boy FOV", "Camera", "Field of View",
            "Field of view while the Pip-Boy is up. Applied when it opens and restored when it "
            "closes, ramped over a few frames. Set it to your first-person FOV to disable the "
            "effect.",
            Tier::kOwnedModule, "fov", 0U, 0U, 1U,
            ValueType::kFloat, ValueRange{ 10.0, 160.0 }, 80.0, Evidence::kFieldVerified },

        { "Fov.Terminal", "Terminal FOV", "Camera", "Field of View",
            "Field of view while a terminal is up. HIGHER SHOWS MORE: the terminal camera is "
            "positioned by the furniture and looks at the screen slightly off-centre, so lower "
            "values crop the screen rather than framing it. 100 shows the whole terminal; go lower "
            "only if you want it filling the view.",
            Tier::kOwnedModule, "fov", 0U, 0U, 1U,
            ValueType::kFloat, ValueRange{ 10.0, 160.0 }, 100.0, Evidence::kFieldVerified },

        { "MotionBlur.Enable", "Motion blur", "Camera", "Blur",
            "Global motion blur on or off.",
            Tier::kConsoleSwitch, "mb", 0U, 0U, 1U,
            ValueType::kBool, ValueRange{ 0.0, 1.0 }, 1.0, Evidence::kFieldVerified },

        { "Scene.LensFlare", "Lens flare", "Lighting", "Lens Flare",
            "Lens flare on or off.",
            Tier::kConsoleSwitch, "lf", 0U, 0U, 1U,
            ValueType::kBool, ValueRange{ 0.0, 1.0 }, 1.0, Evidence::kFieldVerified },

        { "Scene.DepthOfField", "Depth of field", "Camera", "Depth of Field",
            "The engine's depth-of-field blur. Most visible through a scope or in the Pip-Boy, "
            "which is where to check it.",
            Tier::kOwnedSetting, "bDoDepthOfField:Imagespace", 0U, 0U, 1U,
            ValueType::kBool, ValueRange{ 0.0, 1.0 }, 1.0, Evidence::kFieldVerified },

        { "Scene.Bokeh", "Bokeh depth of field", "Camera", "Depth of Field",
            "The bokeh (lens-blur) form of depth of field. Separate from the depth-of-field "
            "switch above - this one changes HOW the blur is drawn rather than whether it runs.",
            Tier::kConsoleSwitch, "bok", 0U, 0U, 1U,
            ValueType::kBool, ValueRange{ 0.0, 1.0 }, 1.0, Evidence::kFieldVerified },

        { "Dof.Strength", "DOF strength", "Camera", "Depth of Field",
            "Depth-of-field blur strength, layered through the engine's own modifier system. "
            "0 = no effect from this mod.",
            Tier::kConsoleComposite, "ifx", 0U, 0U, 5U,
            ValueType::kFloat, ValueRange{ 0.0, 1.0 }, 0.0, Evidence::kUncertain,
            SliderScale::kLinear },
        { "Dof.Distance", "DOF focus distance", "Camera", "Depth of Field",
            "Distance at which the blur begins, in game units.",
            Tier::kConsoleComposite, "ifx", 0U, 1U, 5U,
            ValueType::kFloat, ValueRange{ 0.0, 50000.0 }, 0.0, Evidence::kUncertain,
            SliderScale::kLogarithmic },
        { "Dof.Range", "DOF focus range", "Camera", "Depth of Field",
            "How deep the in-focus band is beyond the focus distance.",
            Tier::kConsoleComposite, "ifx", 0U, 2U, 5U,
            ValueType::kFloat, ValueRange{ 0.0, 50000.0 }, 0.0, Evidence::kUncertain,
            SliderScale::kLogarithmic },
        { "Dof.VignetteRadius", "DOF vignette radius", "Camera", "Depth of Field",
            "Radius of the screen-edge blur vignette.",
            Tier::kConsoleComposite, "ifx", 0U, 3U, 5U,
            ValueType::kFloat, ValueRange{ 0.0, 2.0 }, 0.0, Evidence::kUncertain,
            SliderScale::kLinear },
        { "Dof.VignetteStrength", "DOF vignette strength", "Camera", "Depth of Field",
            "Strength of the screen-edge blur vignette.",
            Tier::kConsoleComposite, "ifx", 0U, 4U, 5U,
            ValueType::kFloat, ValueRange{ 0.0, 1.0 }, 0.0, Evidence::kUncertain,
            SliderScale::kLinear },

        { "Wetness.DrySpeed", "Wetness dry speed", "Weather", "Rain",
            "How fast surfaces dry out once the rain stops. ★ SET IT TO 0 AND THEY NEVER DRY - the "
            "world stays wet until the weather wets it further, which is the whole point of this "
            "control. The base game uses 0.02, and the engine drains wetness continuously at that "
            "rate, which is why wet streets vanish moments after a storm passes.",
            Tier::kOwnedSetting, "fDrySpeed:LightingShader", 0U, 0U, 1U,
            ValueType::kFloat, ValueRange{ 0.0, 0.1 }, 0.02, Evidence::kFieldVerified },

        { "Wetness.WetSpeed", "Wetness build-up speed", "Weather", "Rain",
            "How fast surfaces get wet while it is actually raining. The base game uses 0.03. "
            "Raise it and a storm soaks the world quickly; lower it and wetness creeps in. Pair it "
            "with dry speed above - build-up fast and dry speed 0 keeps the world wet after rain.",
            Tier::kOwnedSetting, "fWetSpeed:LightingShader", 0U, 0U, 1U,
            ValueType::kFloat, ValueRange{ 0.0, 0.15 }, 0.03, Evidence::kFieldVerified },

        { "Wetness.Materials", "Wet surface materials", "Weather", "Rain",
            "The engine's wet-surface material system. Off means surfaces never go wet at all, and "
            "the two speed controls above have nothing to act on - check this first if they seem to "
            "do nothing.",
            Tier::kOwnedSetting, "bEnableWetnessMaterials:Display", 0U, 0U, 1U,
            ValueType::kBool, ValueRange{ 0.0, 1.0 }, 1.0, Evidence::kFieldVerified },
    };

    inline constexpr CatalogueRow kFogControls[]{
        { "Fog.Mode", "Fog", "Weather", "Fog",
            "Enabled hands fog back to the engine to drive per weather (setfog 0 0) - the "
            "recommended setting. Disabled switches fog off entirely (setfog 1 1). Custom pins it to "
            "the distance below (setfog 0 <n>).",
            Tier::kConsoleValue, "setfog", 0U, 0U, 1U,
            ValueType::kInt, ValueRange{ 0.0, 2.0 }, 0.0, Evidence::kFieldVerified },

        { "Fog.Distance", "Fog distance", "Weather", "Fog",
            "Whole numbers only. 1 puts fog right in your face; the ceiling is enormous because the "
            "command has no bound of its own. Only applies while the mode above is Custom.",
            Tier::kConsoleValue, "setfog", 0U, 0U, 1U,
            ValueType::kInt, ValueRange{ 1.0, 10000000.0 }, 10000.0, Evidence::kFieldVerified },
    };

    inline constexpr CatalogueRow kSceneToggles[]{
        { "Scene.Grass", "Toggle grass", "Scene toggles", nullptr,
            "Flips grass rendering. Also a button, for the same reason.",
            Tier::kConsoleToggle, "tg", 0U, 0U, 1U,
            ValueType::kNone, ValueRange{ 0.0, 0.0 }, 0.0, Evidence::kFieldVerified },

    };

    inline constexpr CatalogueRow kShadowControls[]{
        { "Shadow.Distance", "Shadow distance", "Shadows", nullptr,
            "How far from the camera the sun's shadows are drawn. The single biggest shadow "
            "quality/cost control, and the setting the game's own options menu cannot change while "
            "you play. The ceiling is where shadows start to glitch, not where they stop "
            "improving.",
            Tier::kOwnedAddress, nullptr, 777729U, 0U, 1U,
            ValueType::kFloat, ValueRange{ 500.0, 28000.0 }, 14000.0, Evidence::kFieldVerified,
            SliderScale::kLinear, Policy::kNoFrameSweep },

        { "Shadow.CascadeBlend", "Cascade blend width", "Shadows", nullptr,
            "How wide the blended band is where one shadow cascade hands over to the next - the "
            "control that hides the hard seam between them. In world distance, like shadow "
            "distance. ★ PAIR IT WITH CASCADE SPLITS BELOW: a wide blend with 3 splits is what "
            "makes even distant shadows look sharp. Past about 512 you stop seeing a difference; "
            "the engine will take more, there is just nothing left to gain.",
            Tier::kOwnedAddress, nullptr, 996623U, 0U, 1U,
            ValueType::kFloat, ValueRange{ 0.0, 512.0 }, 512.0, Evidence::kFieldVerified,
            SliderScale::kLinear, Policy::kNoFrameSweep },

        { "Shadow.NpcLightShadows", "Shadows from NPC-carried lights", "Shadows", nullptr,
            "Lets lights carried by NPCs cast shadows. Off in the base game; each one that "
            "qualifies is more shadow work, so watch the frame rate in crowded interiors.",
            Tier::kOwnedSetting, "bAllowShadowcasterNPCLights:Display", 0U, 0U, 1U,
            ValueType::kBool, ValueRange{ 0.0, 1.0 }, 0.0, Evidence::kUncertain },

        { "Shadow.Splits", "Shadow cascade splits", "Shadows", nullptr,
            "How many cascades the sun's shadow map is divided into - this is about how the "
            "shadow detail is DISTRIBUTED, not how far shadows reach. ★ PAIR IT WITH CASCADE BLEND "
            "ABOVE: 3 splits with a wide blend is the combination that makes even distant shadows "
            "look sharp, and it is why this control looked useless until the blend width existed.",
            Tier::kOwnedSetting, "iDirShadowSplits:Display", 0U, 0U, 1U,
            ValueType::kInt, ValueRange{ 1.0, 3.0 }, 2.0, Evidence::kFieldVerified },
    };

    inline constexpr CatalogueRow kLodControls[]{
        { "Lod.FadeObjects", "Object fade distance", "LOD & distance", nullptr,
            "How far away static objects stay drawn before fading out.",
            Tier::kOwnedAddress, nullptr, 123089U, 0U, 1U,
            ValueType::kFloat, ValueRange{ 0.0, 30.0 }, 15.0, Evidence::kFieldVerified,
            SliderScale::kLinear, Policy::kNoFrameSweep },

        { "Lod.FadeItems", "Item fade distance", "LOD & distance", nullptr,
            "How far away loose items and clutter stay drawn.",
            Tier::kOwnedAddress, nullptr, 1023729U, 0U, 1U,
            ValueType::kFloat, ValueRange{ 0.0, 30.0 }, 10.0, Evidence::kFieldVerified,
            SliderScale::kLinear, Policy::kNoFrameSweep },

        { "Lod.FadeActors", "Actor fade distance", "LOD & distance", nullptr,
            "How far away NPCs and creatures stay drawn.",
            Tier::kOwnedAddress, nullptr, 342335U, 0U, 1U,
            ValueType::kFloat, ValueRange{ 0.0, 30.0 }, 12.0, Evidence::kFieldVerified,
            SliderScale::kLinear, Policy::kNoFrameSweep },

        { "Lod.FadeGrass", "Grass fade start", "LOD & distance", nullptr,
            "How far out grass keeps being drawn at full strength before it starts fading. One of "
            "the easiest rows here to SEE - stand in a field and drag it. NOTE: this may or may "
            "not be the same value as the INI's fGrassStartFadeDistance - set it and the fade "
            "range below to obviously different numbers to find out which does what.",
            Tier::kOwnedAddress, nullptr, 183225U, 0U, 1U,
            ValueType::kFloat, ValueRange{ 0.0, 20000.0 }, 6144.0, Evidence::kFieldVerified,
            SliderScale::kLinear, Policy::kNoFrameSweep },

        { "Lod.BlockLevel0", "Object detail distance (near)", "LOD & distance", nullptr,
            "Distance for the finest level of the world's object grid. Needs a vista to judge.",
            Tier::kOwnedAddress, nullptr, 646264U, 0U, 1U,
            ValueType::kFloat, ValueRange{ 0.0, 102400.0 }, 102400.0, Evidence::kFieldVerified,
            SliderScale::kLogarithmic, Policy::kNoFrameSweep },

        { "Lod.BlockLevel1", "Object detail distance (mid)", "LOD & distance", nullptr,
            "Distance for the middle level of the world's object grid.",
            Tier::kOwnedAddress, nullptr, 1304050U, 0U, 1U,
            ValueType::kFloat, ValueRange{ 0.0, 327680.0 }, 327680.0, Evidence::kFieldVerified,
            SliderScale::kLogarithmic, Policy::kNoFrameSweep },

        { "Lod.BlockLevel2", "Object detail distance (far)", "LOD & distance", nullptr,
            "Distance for the coarsest level of the world's object grid.",
            Tier::kOwnedAddress, nullptr, 379949U, 0U, 1U,
            ValueType::kFloat, ValueRange{ 0.0, 389120.0 }, 389120.0, Evidence::kFieldVerified,
            SliderScale::kLogarithmic, Policy::kNoFrameSweep },

        { "Lod.BlockMaximum", "Object detail distance (maximum)", "LOD & distance", nullptr,
            "The outer limit for the world's object grid - the far edge of the three levels above.",
            Tier::kOwnedSetting, "fBlockMaximumDistance:TerrainManager", 0U, 0U, 1U,
            ValueType::kFloat, ValueRange{ 0.0, 389120.0 }, 389120.0, Evidence::kFieldVerified,
            SliderScale::kLogarithmic },

        { "Lod.SplitDistanceMult", "Terrain split distance", "LOD & distance", nullptr,
            "Multiplier on how close terrain has to be before it splits into finer detail. Higher "
            "keeps finer terrain further out.",
            Tier::kOwnedSetting, "fSplitDistanceMult:TerrainManager", 0U, 0U, 1U,
            ValueType::kFloat, ValueRange{ 0.0, 4.0 }, 1.0, Evidence::kFieldVerified },

        { "Lod.MeshFadeLevel1", "Mesh LOD fade (level 1)", "LOD & distance", nullptr,
            "Distance at which the first mesh LOD level fades over to the next.",
            Tier::kOwnedSetting, "fMeshLODLevel1FadeDist:Display", 0U, 0U, 1U,
            ValueType::kFloat, ValueRange{ 0.0, 40000.0 }, 18432.0, Evidence::kFieldVerified },

        { "Lod.MeshFadeLevel2", "Mesh LOD fade (level 2)", "LOD & distance", nullptr,
            "Distance at which the second mesh LOD level fades over to the next.",
            Tier::kOwnedSetting, "fMeshLODLevel2FadeDist:Display", 0U, 0U, 1U,
            ValueType::kFloat, ValueRange{ 0.0, 40000.0 }, 18432.0, Evidence::kFieldVerified },

        { "ShaderFade.DecalStart", "Decal fade start", "LOD & distance", nullptr,
            "Where decals - blood, scorch marks, posters - begin fading out with distance.",
            Tier::kOwnedAddress, nullptr, 785654U, 0U, 1U,
            ValueType::kFloat, ValueRange{ 0.0, 1.0 }, 1.0, Evidence::kFieldVerified,
            SliderScale::kLinear, Policy::kNoFrameSweep },

        { "ShaderFade.DecalEnd", "Decal fade end", "LOD & distance", nullptr,
            "Where decals are gone entirely. Raise it with the start above to keep blood and "
            "scorch marks visible further out.",
            Tier::kOwnedAddress, nullptr, 103975U, 0U, 1U,
            ValueType::kFloat, ValueRange{ 0.0, 1.1 }, 1.1, Evidence::kFieldVerified,
            SliderScale::kLinear, Policy::kNoFrameSweep },

        { "Lod.MeshRenderAllLevels", "Render all mesh LOD levels", "LOD & distance", nullptr,
            "Draws every mesh LOD level rather than only the one chosen for the distance. Heavier, "
            "and it removes the visible pop as levels swap.",
            Tier::kOwnedSetting, "bMeshLODRenderAllLevels:LOD", 0U, 0U, 1U,
            ValueType::kBool, ValueRange{ 0.0, 1.0 }, 0.0, Evidence::kFieldVerified },
    };

    inline constexpr CatalogueRow kDebugControls[]{

        { "Debug.ToggleAnimations", "Toggle actor animations", "Debug", nullptr,
            "Freezes or resumes every actor animation. A screenshot tool.",
            Tier::kConsoleToggle, "tanim", 0U, 0U, 1U,
            ValueType::kNone, ValueRange{ 0.0, 0.0 }, 0.0, Evidence::kFieldVerified },

        { "Debug.ToggleMovement", "Toggle actor movement", "Debug", nullptr,
            "Freezes or resumes every actor's movement. A screenshot tool.",
            Tier::kConsoleToggle, "tmove", 0U, 0U, 1U,
            ValueType::kNone, ValueRange{ 0.0, 0.0 }, 0.0, Evidence::kFieldVerified },
    };

    struct CatalogueTable
    {
        const CatalogueRow* rows;
        std::size_t count;
    };

    inline constexpr CatalogueTable kCatalogueTables[]{
        { kGodrayControls, std::size(kGodrayControls) },
        { kReflectionControls, std::size(kReflectionControls) },
        { kCharacterControls, std::size(kCharacterControls) },
        { kColourControls, std::size(kColourControls) },
        { kCameraControls, std::size(kCameraControls) },
        { kFogControls, std::size(kFogControls) },
        { kShadowControls, std::size(kShadowControls) },
        { kLodControls, std::size(kLodControls) },
        { kSceneToggles, std::size(kSceneToggles) },
        { kDebugControls, std::size(kDebugControls) },
    };

    [[nodiscard]] constexpr std::size_t CatalogueRowTotal() noexcept
    {
        std::size_t total = 0;
        for (const auto& table : kCatalogueTables) {
            total += table.count;
        }
        return total;
    }

    inline constexpr std::size_t kCatalogueRowTotal = CatalogueRowTotal();

    template <class Fn>
    constexpr void VisitCatalogue(Fn&& a_fn)
    {
        std::size_t globalIndex = 0;
        for (const auto& table : kCatalogueTables) {
            for (std::size_t i = 0; i < table.count; ++i) {
                a_fn(table.rows[i], globalIndex);
                ++globalIndex;
            }
        }
    }

    [[nodiscard]] constexpr const CatalogueRow* RowByGlobalIndex(std::size_t a_index) noexcept
    {
        std::size_t globalIndex = 0;
        for (const auto& table : kCatalogueTables) {
            if (a_index < globalIndex + table.count) {
                return &table.rows[a_index - globalIndex];
            }
            globalIndex += table.count;
        }
        return nullptr;
    }

    [[nodiscard]] constexpr std::size_t GlobalIndexForId(std::string_view a_id) noexcept
    {
        std::size_t globalIndex = 0;
        for (const auto& table : kCatalogueTables) {
            for (std::size_t i = 0; i < table.count; ++i) {
                if (a_id == table.rows[i].id) {
                    return globalIndex;
                }
                ++globalIndex;
            }
        }
        return kCatalogueRowTotal;
    }
}
