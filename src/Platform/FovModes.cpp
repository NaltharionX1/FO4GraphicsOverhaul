#include "PCH.h"

#include "Platform/FovModes.h"

#include "Platform/CommandCatalogue.h"
#include "Platform/FovOwner.h"
#include "Platform/OwnedSettings.h"

#include <atomic>
#include <cstring>

namespace
{
    enum class Mode : int
    {
        kNone = 0,
        kPipBoy,
        kTerminal,
    };

    std::atomic<int> g_mode{ static_cast<int>(Mode::kNone) };

    Mode g_appliedMode = Mode::kNone;
    float g_rampFrom = 0.0f;
    float g_rampTo = 0.0f;
    int g_rampFrame = 0;
    std::atomic<int> g_rampFrames{ 0 };

    std::atomic<float> g_preModeFirstPerson{ 0.0f };
    std::atomic<bool> g_havePreMode{ false };

    constexpr int kFramesIn = 6;
    constexpr int kFramesOut = 12;

    [[nodiscard]] bool RowValue(const char* a_id, float& a_out) noexcept
    {
        const std::size_t index = Platform::GlobalIndexForId(a_id);
        if (index >= Platform::kCatalogueRowTotal ||
            !Platform::OwnedSettings::IsSet(index)) {
            return false;
        }
        a_out = static_cast<float>(Platform::OwnedSettings::Value(index));
        return true;
    }

    void Resolve(Mode a_mode, float& a_firstPerson, float& a_thirdPerson, float& a_viewModel) noexcept
    {
        float engineFirst = 0.0f;
        float engineThird = 0.0f;
        (void)Platform::FovOwner::Current(engineFirst, engineThird);

        a_firstPerson = (g_havePreMode.load(std::memory_order_relaxed) && a_mode == Mode::kNone)
                            ? g_preModeFirstPerson.load(std::memory_order_relaxed)
                            : engineFirst;
        a_thirdPerson = engineThird;
        (void)RowValue("Fov.World", a_firstPerson);
        (void)RowValue("Fov.ThirdPerson", a_thirdPerson);

        a_viewModel = a_thirdPerson;

        const char* const modeRow = a_mode == Mode::kPipBoy     ? "Fov.PipBoy"
                                    : a_mode == Mode::kTerminal ? "Fov.Terminal"
                                                                : nullptr;
        if (modeRow != nullptr) {
            float modeValue = 0.0f;
            if (RowValue(modeRow, modeValue)) {
                a_firstPerson = modeValue;
                a_viewModel = modeValue;
            }
        }
    }

    void WriteNow(Mode a_mode, float a_firstPersonOverride, bool a_useOverride) noexcept
    {
        float firstPerson = 0.0f;
        float thirdPerson = 0.0f;
        float viewModel = 0.0f;
        Resolve(a_mode, firstPerson, thirdPerson, viewModel);
        if (a_useOverride) {
            firstPerson = a_firstPersonOverride;
        }
        (void)Platform::FovOwner::Write(firstPerson, thirdPerson, viewModel);
    }
}

namespace Platform::FovModes
{
    void Apply() noexcept
    {
        if (!Platform::FovOwner::Owns()) {
            return;
        }
        if (g_rampFrames.load(std::memory_order_relaxed) > 0) {
            return;
        }
        WriteNow(static_cast<Mode>(g_mode.load(std::memory_order_relaxed)), 0.0f, false);
    }

    void OnMenuBoundary(const char* a_menuName, bool a_opening) noexcept
    {
        if (a_menuName == nullptr) {
            return;
        }
        Mode which = Mode::kNone;
        if (std::strcmp(a_menuName, "PipboyMenu") == 0) {
            which = Mode::kPipBoy;
        } else if (std::strcmp(a_menuName, "TerminalMenu") == 0) {
            which = Mode::kTerminal;
        } else {
            return;
        }

        if (a_opening) {
            g_mode.store(static_cast<int>(which), std::memory_order_relaxed);
            return;
        }
        int expected = static_cast<int>(which);
        (void)g_mode.compare_exchange_strong(
            expected, static_cast<int>(Mode::kNone), std::memory_order_relaxed);
    }

    void OnLoadBoundary() noexcept
    {
        const int previous =
            g_mode.exchange(static_cast<int>(Mode::kNone), std::memory_order_relaxed);
        if (previous != static_cast<int>(Mode::kNone)) {
            logger::info("[FovModes] load boundary - a {} FOV mode was still recorded as open and "
                         "has been cleared",
                previous == static_cast<int>(Mode::kPipBoy) ? "Pip-Boy" : "terminal");
        }
    }

    void Tick() noexcept
    {
        const auto mode = static_cast<Mode>(g_mode.load(std::memory_order_relaxed));

        if (mode != g_appliedMode) {
            if (!Platform::FovOwner::Owns()) {
                g_appliedMode = mode;
                return;
            }

            float firstPerson = 0.0f;
            float thirdPerson = 0.0f;
            float viewModel = 0.0f;
            Resolve(mode, firstPerson, thirdPerson, viewModel);

            float currentFirst = 0.0f;
            float currentThird = 0.0f;
            if (!Platform::FovOwner::Current(currentFirst, currentThird)) {
                currentFirst = firstPerson;
            }

            if (g_appliedMode == Mode::kNone && mode != Mode::kNone) {
                g_preModeFirstPerson.store(currentFirst, std::memory_order_relaxed);
                g_havePreMode.store(true, std::memory_order_relaxed);
                Resolve(mode, firstPerson, thirdPerson, viewModel);
            }

            g_rampFrom = currentFirst;
            g_rampTo = firstPerson;
            g_rampFrame = 0;
            g_rampFrames.store((mode == Mode::kNone) ? kFramesOut : kFramesIn,
                std::memory_order_relaxed);
            g_appliedMode = mode;

            logger::info("[FovModes] {} — first person {:.1f} -> {:.1f} over {} frames, viewmodel "
                         "{:.1f} re-applied",
                mode == Mode::kPipBoy     ? "Pip-Boy opened"
                : mode == Mode::kTerminal ? "terminal opened"
                                          : "menu closed",
                g_rampFrom, g_rampTo, g_rampFrames.load(std::memory_order_relaxed), viewModel);

            if (g_rampFrom == g_rampTo) {
                g_rampFrames.store(0, std::memory_order_relaxed);
                WriteNow(mode, 0.0f, false);
                return;
            }
        }

        const int frames = g_rampFrames.load(std::memory_order_relaxed);
        if (frames <= 0) {
            return;
        }

        ++g_rampFrame;
        if (g_rampFrame >= frames) {
            g_rampFrames.store(0, std::memory_order_relaxed);
            WriteNow(g_appliedMode, 0.0f, false);
            return;
        }
        const float t = static_cast<float>(g_rampFrame) / static_cast<float>(frames);
        WriteNow(g_appliedMode, g_rampFrom + (g_rampTo - g_rampFrom) * t, true);
    }
}
