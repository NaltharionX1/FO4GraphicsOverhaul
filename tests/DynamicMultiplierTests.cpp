#include "Platform/DynamicMultiplier.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>

namespace
{
    unsigned g_checks = 0;
    unsigned g_failures = 0;

    void Check(bool a_ok, const char* a_what)
    {
        ++g_checks;
        if (!a_ok) {
            ++g_failures;
            std::cout << "  FAILED: " << a_what << '\n';
        }
    }

    double CappedPeriodMs(std::uint32_t a_generated, double a_refreshHz)
    {
        return (static_cast<double>(a_generated) + 1.0) * 1000.0 / a_refreshHz;
    }
}

int main()
{
    using namespace Platform::DynamicMultiplier;
    constexpr std::uint32_t kCap = 5;

    Check(IdealGeneratedFrames(300.0F, 11.4, kCap) == 3U, "300 Hz at an 11.4 ms period asks for 4x");
    Check(IdealGeneratedFrames(300.0F, 7.18, kCap) == 2U, "the wrong 7.18 ms input would still have asked for 3x, not held 2x");
    Check(IdealGeneratedFrames(300.0F, 13.8, kCap) == 3U, "after the step to 4x the longer period still asks for 4x (no hunting)");
    Check(IdealGeneratedFrames(300.0F, 9.5, kCap) == 2U, "a lighter load at 4x asks for 3x");
    Check(IdealGeneratedFrames(300.0F, 8.5, kCap) == 2U, "and 3x holds on its own shorter period");
    Check(std::fabs(PresentedHz(1U, 11.4) - 175.4) < 0.1, "2x at 11.4 ms presents 175 Hz (below the target, the reason to rise)");
    Check(std::fabs(PresentedHz(3U, 13.8) - 289.9) < 0.1, "4x at 13.8 ms presents 290 Hz (within 5 % of 300: reached)");

    Check(IdealGeneratedFrames(240.0F, 1000.0 / 60.0, kCap) == 3U, "60 real fps at 240 Hz: 4x");
    Check(IdealGeneratedFrames(120.0F, 1000.0 / 60.0, kCap) == 1U, "60 real fps at 120 Hz: 2x");
    Check(IdealGeneratedFrames(300.0F, 1000.0 / 60.0, kCap) == 4U, "60 real fps at 300 Hz: 5x");
    Check(IdealGeneratedFrames(60.0F, 1000.0 / 60.0, kCap) == 1U, "60 real fps at 60 Hz: the floor, 2x");

    for (std::uint32_t k = 1; k <= kCap; ++k) {
        Check(IdealGeneratedFrames(240.0F, CappedPeriodMs(k, 240.0), kCap) == k, "a display-capped period holds its count");
    }
    Check(IdealGeneratedFrames(240.0F, CappedPeriodMs(2U, 240.0) * 1.0000001, kCap) == 2U,
        "a hair over the capped period still holds (the tolerance keeps it off the integer)");

    Check(PresentedCapHz(240.0F, 0U) == 0.0F, "VSync off: no cap");
    Check(PresentedCapHz(0.0F, 1U) == 0.0F, "refresh unknown: no cap");
    Check(PresentedCapHz(240.0F, 1U) == 240.0F, "VSync on at interval 1: 240");
    Check(PresentedCapHz(240.0F, 2U) == 120.0F, "VSync on at interval 2: 120");
    Check(EffectiveTargetHz(300.0F, 0.0F) == 300.0F, "no cap: the target stands");
    Check(EffectiveTargetHz(300.0F, 240.0F) == 240.0F, "a target above the cap is the cap");
    Check(EffectiveTargetHz(120.0F, 240.0F) == 120.0F, "a target below the cap stands");
    {
        const float eff = EffectiveTargetHz(300.0F, PresentedCapHz(240.0F, 2U));
        Check(eff == 120.0F, "300 Hz asked, interval 2 on 240: 120 is what can be presented");
        Check(IdealGeneratedFrames(eff, CappedPeriodMs(1U, 120.0), kCap) == 1U, "and 2x holds at the synced period");
        Check(IdealGeneratedFrames(300.0F, CappedPeriodMs(1U, 120.0), kCap) == 4U, "(uncapped, the same period would have asked for 5x, a rate the chain cannot present)");
    }

    Check(IdealGeneratedFrames(240.0F, 2.0, kCap) == 1U, "a 2 ms period never asks for less than one generated frame");
    Check(IdealGeneratedFrames(240.0F, 100.0, kCap) == kCap, "a 100 ms period is clamped to the cap");
    Check(IdealGeneratedFrames(240.0F, 100.0, 3U) == 3U, "the cap is the session's (a runtime refusal lowers it)");
    Check(IdealGeneratedFrames(0.0F, 11.4, kCap) == 0U, "no target: no decision");
    Check(IdealGeneratedFrames(240.0F, 0.0, kCap) == 0U, "no period: no decision");
    Check(IdealGeneratedFrames(240.0F, 11.4, 0U) == 0U, "no cap: no decision");
    Check(PresentedHz(1U, 0.0) == 0.0, "no period: no presented rate");

    Check(OwnBackPressure(4400, 500) == 500, "GPU-bound: the render thread waited 4.4 ms, the worker slept 0.5 ms — ours is 0.5");
    Check(OwnBackPressure(2000, 9000) == 2000, "worker holding: the wait is smaller than the sleep — all of it is ours");
    Check(OwnBackPressure(0, 9000) == 0, "a limiter: no queue wait, nothing is ours");
    Check(OwnBackPressure(4400, 0) == 0, "no sleep at all: nothing is ours");
    Check(OwnBackPressure(-5, 100) == 0 && OwnBackPressure(100, -5) == 0, "a clock step backwards counts as nothing");

    if (g_failures != 0) {
        std::cout << "DynamicMultiplierTests: " << g_failures << " failure(s) of " << g_checks << '\n';
        return EXIT_FAILURE;
    }
    std::cout << "DynamicMultiplierTests: all checks passed (" << g_checks << ")\n";
    return EXIT_SUCCESS;
}
