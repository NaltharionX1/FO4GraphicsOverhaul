#include "Platform/PresentMath.h"

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace
{
    int g_checks = 0;
    int g_failures = 0;

    void Check(bool a_condition, std::string_view a_message)
    {
        ++g_checks;
        if (!a_condition) {
            ++g_failures;
            std::cout << "  - FAIL: " << a_message << '\n';
        }
    }

    using Platform::IsFlipEffect;
    using Platform::kPresentAllowTearing;
    using Platform::ResolveBufferCount;
    using Platform::ResolvePresentParams;
    using Platform::ResolveSwapEffect;
    using Platform::SwapChainCaps;
    using Platform::SwapEffectChoice;

    constexpr SwapChainCaps kModern{ true, true, true };
    constexpr SwapChainCaps kFlipSeqOnly{ true, false, false };
    constexpr SwapChainCaps kNoFlip{ false, false, false };

    void SwapEffectResolution()
    {
        Check(ResolveSwapEffect(SwapEffectChoice::kAuto, kModern, true) == SwapEffectChoice::kFlipDiscard,
            "auto + modern caps -> FLIP_DISCARD");
        Check(ResolveSwapEffect(SwapEffectChoice::kAuto, kFlipSeqOnly, true) ==
                SwapEffectChoice::kFlipSequential,
            "auto + flip-sequential-only caps -> FLIP_SEQUENTIAL");
        Check(ResolveSwapEffect(SwapEffectChoice::kAuto, kNoFlip, true) == SwapEffectChoice::kDiscard,
            "auto + no flip support -> DISCARD");

        Check(ResolveSwapEffect(SwapEffectChoice::kFlipDiscard, kModern, false) ==
                SwapEffectChoice::kDiscard,
            "explicit FLIP_DISCARD in exclusive fullscreen must be refused (HFPF only warns; freezes on start)");
        Check(ResolveSwapEffect(SwapEffectChoice::kFlipSequential, kModern, false) ==
                SwapEffectChoice::kDiscard,
            "explicit FLIP_SEQUENTIAL in exclusive fullscreen must be refused");
        Check(ResolveSwapEffect(SwapEffectChoice::kAuto, kModern, false) == SwapEffectChoice::kDiscard,
            "auto in exclusive fullscreen must stay on the unconditionally safe bitblt path");

        Check(ResolveSwapEffect(SwapEffectChoice::kFlipDiscard, kFlipSeqOnly, true) ==
                SwapEffectChoice::kFlipSequential,
            "FLIP_DISCARD without support downgrades to FLIP_SEQUENTIAL");
        Check(ResolveSwapEffect(SwapEffectChoice::kFlipDiscard, kNoFlip, true) ==
                SwapEffectChoice::kDiscard,
            "FLIP_DISCARD with no flip support at all downgrades to DISCARD");
        Check(ResolveSwapEffect(SwapEffectChoice::kFlipSequential, kNoFlip, true) ==
                SwapEffectChoice::kDiscard,
            "FLIP_SEQUENTIAL without support downgrades to DISCARD");

        Check(ResolveSwapEffect(SwapEffectChoice::kSequential, kModern, true) ==
                SwapEffectChoice::kSequential,
            "an explicit bitblt choice is honoured even when flip is available");

        Check(IsFlipEffect(SwapEffectChoice::kFlipDiscard) && IsFlipEffect(SwapEffectChoice::kFlipSequential),
            "flip predicate covers both flip modes");
        Check(!IsFlipEffect(SwapEffectChoice::kDiscard) && !IsFlipEffect(SwapEffectChoice::kAuto),
            "flip predicate excludes bitblt and auto");
    }

    void BufferCountResolution()
    {
        Check(ResolveBufferCount(0U, SwapEffectChoice::kFlipDiscard, 1U) == 3U,
            "auto + flip -> 3 buffers");
        Check(ResolveBufferCount(0U, SwapEffectChoice::kDiscard, 1U) == 1U,
            "auto + bitblt leaves the engine's own value alone");
        Check(ResolveBufferCount(1U, SwapEffectChoice::kFlipDiscard, 1U) == 2U,
            "flip floors at 2 buffers — DXGI rejects fewer");
        Check(ResolveBufferCount(1U, SwapEffectChoice::kDiscard, 1U) == 1U,
            "bitblt may legitimately use 1 buffer");
        Check(ResolveBufferCount(4U, SwapEffectChoice::kFlipDiscard, 1U) == 4U,
            "an in-range explicit count is honoured");
        Check(ResolveBufferCount(99U, SwapEffectChoice::kFlipDiscard, 1U) == 3U,
            "out-of-range count falls back to the auto decision rather than passing through (HFPF passes it)");
        Check(ResolveBufferCount(99U, SwapEffectChoice::kDiscard, 2U) == 2U,
            "out-of-range count on bitblt falls back to the engine value");
    }

    void PresentParameterCoupling()
    {
        const auto vsyncOff = ResolvePresentParams(false, 1U, true);
        Check(vsyncOff.syncInterval == 0U && vsyncOff.flags == kPresentAllowTearing,
            "vsync off + tearing chain -> interval 0 WITH the tearing flag");

        const auto vsyncOffNoTear = ResolvePresentParams(false, 1U, false);
        Check(vsyncOffNoTear.syncInterval == 0U && vsyncOffNoTear.flags == 0U,
            "vsync off without a tearing chain -> interval 0, NO flag (the flag would fail the call)");

        const auto vsyncOn = ResolvePresentParams(true, 1U, true);
        Check(vsyncOn.syncInterval == 1U && vsyncOn.flags == 0U,
            "vsync on must never carry the tearing flag, even on a tearing-capable chain");

        Check(ResolvePresentParams(true, 3U, true).syncInterval == 3U, "vsync honours a half/third-rate interval");
        Check(ResolvePresentParams(true, 0U, false).syncInterval == 1U,
            "vsync on with interval 0 is nonsense -> clamped to 1 rather than silently disabling vsync");
        Check(ResolvePresentParams(true, 99U, false).syncInterval == 4U, "interval clamps to the DXGI maximum of 4");
    }

    void PaceArithmetic()
    {
        using Platform::PaceSpinTicks;
        using Platform::PaceTimerDue100ns;
        constexpr long long kTenMHz = 10'000'000;
        Check(PaceSpinTicks(kTenMHz) == 2'500, "0.25 ms of spin at the 10 MHz QPC");
        Check(PaceTimerDue100ns(100'000, 2'500, kTenMHz) == -97'500,
            "10 ms remaining -> 9.75 ms to the timer: negative (relative), 100 ns units");
        Check(PaceTimerDue100ns(2'500, 2'500, kTenMHz) == 0, "inside the spin window -> nothing for the timer");
        Check(PaceTimerDue100ns(0, 2'500, kTenMHz) == 0 && PaceTimerDue100ns(-5, 2'500, kTenMHz) == 0,
            "at or past the deadline -> nothing for the timer");
        Check(PaceTimerDue100ns(100'000, 2'500, 0) == 0, "a zero frequency never divides");
        Check(PaceTimerDue100ns(16'667, 0, 1'000'000) == -166'670, "a 1 MHz clock: 16.667 ms -> 166,670 units of 100 ns");
        Check(PaceTimerDue100ns(10'000'000, 2'500, kTenMHz) == -9'997'500, "a full second does not overflow");
    }

}

int main()
{
    struct Case
    {
        const char* name;
        void (*fn)();
    };
    constexpr Case cases[]{
        { "SwapEffectResolution", &SwapEffectResolution },
        { "BufferCountResolution", &BufferCountResolution },
        { "PresentParameterCoupling", &PresentParameterCoupling },
        { "PaceArithmetic", &PaceArithmetic },
    };

    for (const auto& c : cases) {
        const int before = g_failures;
        c.fn();
        std::cout << ((g_failures == before) ? "[PASS] " : "[FAIL] ") << c.name << '\n';
    }
    std::cout << "PresentMathTests: " << g_checks << " checks, " << g_failures << " failures\n";
    return (g_failures == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
