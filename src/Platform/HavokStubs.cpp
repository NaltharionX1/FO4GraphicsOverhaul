// SPDX-License-Identifier: GPL-3.0-or-later
// Portions adapted from High FPS Physics Fix, Copyright (c) 2025 AntoniX35, MIT License.

#include "PCH.h"

#include "Platform/HavokFixes.h"

#include "Platform/EnginePatch.h"
#include "Platform/EngineStub.h"

#include <xbyak/xbyak.h>

namespace
{
    constexpr std::uint32_t kOneFloat = 0x3f800000;
    constexpr std::uint32_t kTimestep60 = 0x3c88888a;

    constexpr std::uint64_t kStutterFnA = 12890;
    constexpr std::ptrdiff_t kStutter1Offset = 0x196;
    constexpr std::ptrdiff_t kStutter3Offset = 0x145;
    constexpr std::uint64_t kStutterFnB = 1395106;
    constexpr std::ptrdiff_t kStutter2Offset = 0x1A1;
    constexpr std::uint64_t kObjectsTransferFn = 754666;
    constexpr std::ptrdiff_t kObjectsTransferOffset = 0;

    constexpr std::uint8_t kNop = 0x90;
    constexpr std::uint8_t kNop4[]{ 0x0F, 0x1F, 0x40, 0x00 };
    constexpr std::uint8_t kNop8[]{ 0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00 };

    struct StutterConstantToInt : Xbyak::CodeGenerator
    {
        explicit StutterConstantToInt(std::uintptr_t a_returnTo)
        {
            Xbyak::Label retn;
            Xbyak::Label magic;
            movss(xmm3, dword[rip + magic]);
            cvttss2si(rcx, xmm3);
            jmp(ptr[rip + retn]);
            L(retn);
            dq(a_returnTo + 0x5);
            L(magic);
            dd(kOneFloat);
        }
    };

    struct StutterUseExisting : Xbyak::CodeGenerator
    {
        explicit StutterUseExisting(std::uintptr_t a_returnTo)
        {
            Xbyak::Label retn;
            movss(xmm2, xmm6);
            jmp(ptr[rip + retn]);
            L(retn);
            dq(a_returnTo + 0x6);
        }
    };

    struct StutterFixedTimestep : Xbyak::CodeGenerator
    {
        explicit StutterFixedTimestep(std::uintptr_t a_returnTo)
        {
            Xbyak::Label retn;
            Xbyak::Label magic;
            movss(xmm1, dword[rip + magic]);
            jmp(ptr[rip + retn]);
            L(retn);
            dq(a_returnTo + 0x5);
            L(magic);
            dd(kTimestep60);
        }
    };

    constexpr std::uint64_t kFrameTimerId = 922988;
    constexpr std::ptrdiff_t kFrameTimerSlowOffset = 0x218;

    constexpr std::uint64_t kWind1Fn = 1469635;
    constexpr std::ptrdiff_t kWind1Offset = 0x21;
    constexpr std::uint64_t kWind234Fn = 1164603;
    constexpr std::ptrdiff_t kWind2Offset = 0x9E;
    constexpr std::ptrdiff_t kWind3Offset = 0x147;
    constexpr std::ptrdiff_t kWind4Offset = 0x32B;

    constexpr std::uint8_t kNop3[]{ 0x0F, 0x1F, 0x00 };

    struct WindFixedTimestep : Xbyak::CodeGenerator
    {
        explicit WindFixedTimestep(std::uintptr_t a_returnTo)
        {
            Xbyak::Label retn;
            Xbyak::Label magic;
            movaps(ptr[rsp + 0x30], xmm6);
            movss(xmm6, dword[rip + magic]);
            jmp(ptr[rip + retn]);
            L(retn);
            dq(a_returnTo + 0x8);
            L(magic);
            dd(kTimestep60);
        }
    };

    struct WindSlowTimerRcx : Xbyak::CodeGenerator
    {
        WindSlowTimerRcx(std::uintptr_t a_returnTo, std::uintptr_t a_frameTimer)
        {
            Xbyak::Label retn;
            Xbyak::Label timer;
            mov(rcx, ptr[rip + timer]);
            movss(xmm9, dword[rcx]);
            jmp(ptr[rip + retn]);
            L(retn);
            dq(a_returnTo + 0x9);
            L(timer);
            dq(a_frameTimer);
        }
    };

    struct WindSlowTimerR8 : Xbyak::CodeGenerator
    {
        WindSlowTimerR8(std::uintptr_t a_returnTo, std::uintptr_t a_frameTimer)
        {
            Xbyak::Label retn;
            Xbyak::Label timer;
            mov(r8, ptr[rip + timer]);
            movss(xmm0, dword[r8]);
            jmp(ptr[rip + retn]);
            L(retn);
            dq(a_returnTo + 0x8);
            L(timer);
            dq(a_frameTimer);
        }
    };

    constexpr std::uint64_t kMagicRateForward = 0x426b4b44;
    constexpr std::uint64_t kMagicRateReverse = 0xc26b4b44;

    constexpr std::uint64_t kRotationFn = 457276;
    constexpr std::ptrdiff_t kRotationOffset = 0xE1;
    constexpr std::uint64_t kLockpickFn = 676000;
    constexpr std::ptrdiff_t kLockpickOffset = 0x42;
    constexpr std::uint64_t kWorkshopRotFn = 1144472;
    constexpr std::ptrdiff_t kWorkshopRotOffset = 0xA2;
    constexpr std::uint64_t kSittingRotFn = 533372;
    constexpr std::ptrdiff_t kSittingRotXOffset = 0xC0;
    constexpr std::ptrdiff_t kSittingRotYOffset = 0xD7;

    constexpr std::ptrdiff_t kRotationFastTimerOffset = 0x21C;

    struct RotationRate : Xbyak::CodeGenerator
    {
        RotationRate(std::uintptr_t a_returnTo, std::uintptr_t a_frameTimer)
        {
            Xbyak::Label retn;
            Xbyak::Label timer;
            Xbyak::Label reverseBranch;
            Xbyak::Label join;
            Xbyak::Label forwardRate;
            Xbyak::Label reverseRate;

            jne(reverseBranch);
            movss(xmm2, dword[rip + forwardRate]);
            mov(rcx, ptr[rip + timer]);
            mulss(xmm2, dword[rcx]);
            jmp(join);
            L(reverseBranch);
            movss(xmm2, dword[rip + reverseRate]);
            mov(rcx, ptr[rip + timer]);
            mulss(xmm2, dword[rcx]);
            L(join);
            mulss(xmm2, ptr[rbx + 0x38]);
            jmp(ptr[rip + retn]);
            L(retn);
            dq(a_returnTo + 0x19);
            L(timer);
            dq(a_frameTimer);
            L(forwardRate);
            dq(kMagicRateForward);
            L(reverseRate);
            dq(kMagicRateReverse);
        }
    };

    struct LockpickRotationTimestep : Xbyak::CodeGenerator
    {
        explicit LockpickRotationTimestep(std::uintptr_t a_returnTo)
        {
            Xbyak::Label retn;
            Xbyak::Label magic;
            mulss(xmm1, dword[rip + magic]);
            jmp(ptr[rip + retn]);
            L(retn);
            dq(a_returnTo + 0x8);
            L(magic);
            dd(kTimestep60);
        }
    };

    struct WorkshopRotationTimer : Xbyak::CodeGenerator
    {
        WorkshopRotationTimer(std::uintptr_t a_returnTo, std::uintptr_t a_frameTimer)
        {
            Xbyak::Label retn;
            Xbyak::Label timer;
            mov(rax, ptr[rip + timer]);
            mulss(xmm1, dword[rax]);
            jmp(ptr[rip + retn]);
            L(retn);
            dq(a_returnTo + 0x8);
            L(timer);
            dq(a_frameTimer);
        }
    };

    struct SittingRotationX : Xbyak::CodeGenerator
    {
        SittingRotationX(std::uintptr_t a_returnTo, std::uintptr_t a_frameTimer)
        {
            Xbyak::Label retn;
            Xbyak::Label rate;
            Xbyak::Label timer;
            mulss(xmm0, dword[rip + rate]);
            mov(r9, ptr[rip + timer]);
            mulss(xmm0, dword[r9]);
            mulss(xmm0, ptr[rax + 0x4C]);
            jmp(ptr[rip + retn]);
            L(retn);
            dq(a_returnTo + 0x5);
            L(rate);
            dq(kMagicRateForward);
            L(timer);
            dq(a_frameTimer);
        }
    };

    struct SittingRotationY : Xbyak::CodeGenerator
    {
        SittingRotationY(std::uintptr_t a_returnTo, std::uintptr_t a_frameTimer)
        {
            Xbyak::Label retn;
            Xbyak::Label rate;
            Xbyak::Label timer;
            mulss(xmm1, dword[rip + rate]);
            mov(r9, ptr[rip + timer]);
            mulss(xmm1, dword[r9]);
            movss(xmm0, ptr[rbx + 0x64]);
            jmp(ptr[rip + retn]);
            L(retn);
            dq(a_returnTo + 0x5);
            L(rate);
            dq(kMagicRateForward);
            L(timer);
            dq(a_frameTimer);
        }
    };

    constexpr std::uint32_t kTimestep017 = 0x3c8b4396;
    constexpr std::uint64_t kStuckAnimFn = 463133;
    constexpr std::ptrdiff_t kStuckAnimOffset = 0xA9;

    constexpr std::uint64_t kMotionFeedbackFn = 1201084;
    constexpr std::ptrdiff_t kMotionFeedbackOffset = 0x9F7;
    constexpr std::uint8_t kJmpShortByte = 0xEB;

    struct StuckAnimTimestep : Xbyak::CodeGenerator
    {
        explicit StuckAnimTimestep(std::uintptr_t a_returnTo)
        {
            Xbyak::Label retn;
            Xbyak::Label magic;
            movss(xmm3, dword[rip + magic]);
            jmp(ptr[rip + retn]);
            L(retn);
            dq(a_returnTo + 0x5);
            L(magic);
            dd(kTimestep017);
        }
    };

    constexpr std::uint64_t kZoomFn = 629736;
    constexpr std::ptrdiff_t kZoomLeftOffset = 0xFF;
    constexpr std::ptrdiff_t kZoomRightOffset = 0x163;
    constexpr std::uint64_t kLoadScreenFn = 618896;
    constexpr std::ptrdiff_t kRepeatRateOffset = 0x354;
    constexpr std::ptrdiff_t kPanUpOffset = 0x46C;
    constexpr std::ptrdiff_t kPanDownOffset = 0x4C6;
    constexpr std::ptrdiff_t kPanLeftOffset = 0x51F;
    constexpr std::ptrdiff_t kPanRightOffset = 0x584;
    constexpr std::uint64_t kLoadScreenRotFn = 22234;
    constexpr std::ptrdiff_t kLoadScreenRotOffset = 0xBF;
    constexpr std::uint8_t kNop6[]{ 0x66, 0x0F, 0x1F, 0x44, 0x00, 0x00 };

    struct LoadModelZoomSpeed : Xbyak::CodeGenerator
    {
        LoadModelZoomSpeed(std::uintptr_t a_returnTo, std::uintptr_t a_value,
            std::uintptr_t a_frameTimer)
        {
            Xbyak::Label retn;
            Xbyak::Label value;
            Xbyak::Label rate;
            Xbyak::Label timer;
            mov(rcx, ptr[rip + value]);
            movss(xmm1, ptr[rcx]);
            mulss(xmm1, dword[rip + rate]);
            mov(rcx, ptr[rip + timer]);
            mulss(xmm1, dword[rcx]);
            jmp(ptr[rip + retn]);
            L(retn);
            dq(a_returnTo + 0x8);
            L(value);
            dq(a_value);
            L(rate);
            dq(kMagicRateForward);
            L(timer);
            dq(a_frameTimer);
        }
    };

    struct LoadModelPanSpeed : Xbyak::CodeGenerator
    {
        LoadModelPanSpeed(std::uintptr_t a_returnTo, std::uintptr_t a_value,
            std::uintptr_t a_frameTimer)
        {
            Xbyak::Label retn;
            Xbyak::Label value;
            Xbyak::Label rate;
            Xbyak::Label timer;
            mov(rcx, ptr[rip + value]);
            movss(xmm0, ptr[rcx]);
            mulss(xmm0, dword[rip + rate]);
            mov(rcx, ptr[rip + timer]);
            mulss(xmm0, dword[rcx]);
            jmp(ptr[rip + retn]);
            L(retn);
            dq(a_returnTo + 0x8);
            L(value);
            dq(a_value);
            L(rate);
            dq(kMagicRateForward);
            L(timer);
            dq(a_frameTimer);
        }
    };

    struct LoadModelRepeatRate : Xbyak::CodeGenerator
    {
        explicit LoadModelRepeatRate(std::uintptr_t a_returnTo)
        {
            Xbyak::Label retn;
            Xbyak::Label magic;
            mov(ecx, ptr[rbx + 0x26C]);
            movss(xmm8, dword[rip + magic]);
            jmp(ptr[rip + retn]);
            L(retn);
            dq(a_returnTo + 0x6);
            L(magic);
            dd(kTimestep60);
        }
    };

    template <class Generator>
    [[nodiscard]] bool InstallStub(std::uint64_t a_id, std::ptrdiff_t a_offset, const char* a_name) noexcept
    {
        const std::uintptr_t site = Platform::EngineStub::ResolveSite(a_id, a_offset, a_name);
        if (site == 0U) {
            return false;
        }
        try {
            Generator code{ site };
            code.ready();
            return Platform::EngineStub::InstallBranch(a_id, a_offset, code.getCode(),
                code.getSize(), a_name);
        } catch (...) {
            logger::warn("[Stub] {}: code generation threw — skipped (fail-open)", a_name);
            return false;
        }
    }
}

namespace
{
    template <class Generator>
    [[nodiscard]] bool InstallTimerStub(std::uint64_t a_id, std::ptrdiff_t a_offset,
        std::uintptr_t a_frameTimer, const char* a_name) noexcept
    {
        const std::uintptr_t site = Platform::EngineStub::ResolveSite(a_id, a_offset, a_name);
        if (site == 0U) {
            return false;
        }
        try {
            Generator code{ site, a_frameTimer };
            code.ready();
            return Platform::EngineStub::InstallBranch(a_id, a_offset, code.getCode(),
                code.getSize(), a_name);
        } catch (...) {
            logger::warn("[Stub] {}: code generation threw — skipped (fail-open)", a_name);
            return false;
        }
    }

    template <class Generator>
    [[nodiscard]] bool InstallValueStub(std::uint64_t a_id, std::ptrdiff_t a_offset,
        std::uintptr_t a_value, std::uintptr_t a_frameTimer, const char* a_name) noexcept
    {
        const std::uintptr_t site = Platform::EngineStub::ResolveSite(a_id, a_offset, a_name);
        if (site == 0U) {
            return false;
        }
        try {
            Generator code{ site, a_value, a_frameTimer };
            code.ready();
            return Platform::EngineStub::InstallBranch(a_id, a_offset, code.getCode(),
                code.getSize(), a_name);
        } catch (...) {
            logger::warn("[Stub] {}: code generation threw — skipped (fail-open)", a_name);
            return false;
        }
    }
}

namespace Platform
{
    void HavokFixes::ApplyWindSpeedFixes() noexcept
    {
        const std::uintptr_t frameTimer =
            EngineStub::ResolveSite(kFrameTimerId, kFrameTimerSlowOffset, "Havok/FrameTimerSlow");

        const bool w1 = InstallStub<WindFixedTimestep>(kWind1Fn, kWind1Offset, "Havok/FixWindSpeed-1");
        const bool w1Tail = w1 && EnginePatch::WriteBytes(kWind1Fn, kWind1Offset + 0x5, kNop3, {},
            "Havok/FixWindSpeed-1-tail");

        bool site2 = false;
        bool site3 = false;
        bool site4 = false;
        if (frameTimer != 0U) {
            const bool w2 = InstallTimerStub<WindSlowTimerRcx>(kWind234Fn, kWind2Offset, frameTimer,
                "Havok/FixWindSpeed-2");
            const bool w3 = InstallTimerStub<WindSlowTimerRcx>(kWind234Fn, kWind3Offset, frameTimer,
                "Havok/FixWindSpeed-3");
            const bool w4 = InstallTimerStub<WindSlowTimerR8>(kWind234Fn, kWind4Offset, frameTimer,
                "Havok/FixWindSpeed-4");
            site2 = w2 && EnginePatch::WriteBytes(kWind234Fn, kWind2Offset + 0x5, kNop4, {},
                "Havok/FixWindSpeed-2-tail");
            site3 = w3 && EnginePatch::WriteBytes(kWind234Fn, kWind3Offset + 0x5, kNop4, {},
                "Havok/FixWindSpeed-3-tail");
            site4 = w4 && EnginePatch::WriteBytes(kWind234Fn, kWind4Offset + 0x5, kNop3, {},
                "Havok/FixWindSpeed-4-tail");
        } else {
            logger::warn("[Havok] FixWindSpeed: the slow frame timer did not resolve — the three timer-based sites are skipped (fail-open)");
        }

        const bool site1 = w1 && w1Tail;
        if (site1 && site2 && site3 && site4) {
            logger::info("[Havok] FixWindSpeed: all 4 sites applied — wind no longer scales with framerate");
        } else {
            logger::warn("[Havok] FixWindSpeed: partial ({}{}{}{}) — wind speed may still be wrong on the paths that failed",
                site1 ? "1" : "-", site2 ? "2" : "-", site3 ? "3" : "-", site4 ? "4" : "-");
        }
    }

    void HavokFixes::ApplyMotionFixes() noexcept
    {
        const bool stuck = InstallStub<StuckAnimTimestep>(kStuckAnimFn, kStuckAnimOffset,
            "Havok/FixStuckAnimation");
        logger::info("[Havok] FixStuckAnimation {} — animations no longer wedge at high framerate",
            stuck ? "applied" : "NOT applied");

        const bool responsive = EnginePatch::WriteByte(kMotionFeedbackFn, kMotionFeedbackOffset,
            kJmpShortByte, "Havok/FixMotionResponsive");
        logger::info("[Havok] FixMotionResponsive {} — movement input feedback",
            responsive ? "applied" : "NOT applied");
    }

    void HavokFixes::ApplyRotationFixes() noexcept
    {
        const std::uintptr_t fastTimer =
            EngineStub::ResolveSite(kFrameTimerId, kRotationFastTimerOffset, "Havok/FrameTimerFast");

        bool main = false;
        bool mainTail = false;
        if (fastTimer != 0U) {
            main = InstallTimerStub<RotationRate>(kRotationFn, kRotationOffset, fastTimer,
                "Havok/FixRotationSpeed");
            mainTail = main && EnginePatch::FillBytes(kRotationFn, kRotationOffset + 0x5, kNop, 0x14,
                "Havok/FixRotationSpeed-tail");
        } else {
            logger::warn("[Havok] FixRotationSpeed: the fast frame timer did not resolve — main look rotation is skipped (fail-open)");
        }

        const bool lockpick = InstallStub<LockpickRotationTimestep>(kLockpickFn, kLockpickOffset,
            "Havok/FixLockpickRotation");
        const bool lockpickTail = !lockpick || EnginePatch::WriteBytes(kLockpickFn,
            kLockpickOffset + 0x5, kNop3, {}, "Havok/FixLockpickRotation-tail");

        const bool workshop = fastTimer != 0U &&
            InstallTimerStub<WorkshopRotationTimer>(kWorkshopRotFn, kWorkshopRotOffset, fastTimer,
                "Havok/FixWorkshopRotationSpeed");
        const bool workshopTail = !workshop || EnginePatch::WriteBytes(kWorkshopRotFn,
            kWorkshopRotOffset + 0x5, kNop3, {}, "Havok/FixWorkshopRotationSpeed-tail");

        if (main && mainTail && lockpick && lockpickTail && workshop && workshopTail) {
            logger::info("[Havok] rotation family: all sites applied — look/lockpick/workshop rotation no longer scale with framerate");
        } else {
            logger::warn("[Havok] rotation family: partial (look={} lockpick={} workshop={}) — the failed paths still scale with framerate",
                main && mainTail, lockpick && lockpickTail, workshop && workshopTail);
        }
    }

    void HavokFixes::ApplySittingRotationFixes() noexcept
    {
        const std::uintptr_t fastTimer =
            EngineStub::ResolveSite(kFrameTimerId, kRotationFastTimerOffset, "Havok/FrameTimerFast");
        if (fastTimer == 0U) {
            logger::warn("[Havok] FixSittingRotationSpeed: the fast frame timer did not resolve — both axes skipped (fail-open)");
            return;
        }

        const bool x = InstallTimerStub<SittingRotationX>(kSittingRotFn, kSittingRotXOffset,
            fastTimer, "Havok/FixSittingRotationX");
        const bool y = InstallTimerStub<SittingRotationY>(kSittingRotFn, kSittingRotYOffset,
            fastTimer, "Havok/FixSittingRotationY");

        if (x && y) {
            logger::info("[Havok] FixSittingRotationSpeed: both axes applied — seated first-person camera no longer scales with framerate");
        } else {
            logger::warn("[Havok] FixSittingRotationSpeed: partial (x={} y={}) — the failed axis still scales with framerate",
                x, y);
        }
    }

    bool HavokFixes::ApplyLoadingModelFixes(std::uintptr_t a_zoomSpeedValue,
        std::uintptr_t a_rotateSpeedValue) noexcept
    {
        const std::uintptr_t fastTimer =
            EngineStub::ResolveSite(kFrameTimerId, kRotationFastTimerOffset, "Havok/FrameTimerFast");
        if (fastTimer == 0U || a_zoomSpeedValue == 0U || a_rotateSpeedValue == 0U) {
            logger::warn("[Havok] FixLoadingModel: prerequisites missing (timer={:#x} zoom={:#x} rotate={:#x}) — the whole family is skipped (fail-open)",
                fastTimer, a_zoomSpeedValue, a_rotateSpeedValue);
            return false;
        }

        const bool zoomL = InstallValueStub<LoadModelZoomSpeed>(kZoomFn, kZoomLeftOffset,
            a_zoomSpeedValue, fastTimer, "Havok/FixLoadingModel-zoom-left");
        const bool zoomLTail = !zoomL || EnginePatch::WriteBytes(kZoomFn, kZoomLeftOffset + 0x5,
            kNop3, {}, "Havok/FixLoadingModel-zoom-left-tail");
        const bool zoomR = InstallValueStub<LoadModelZoomSpeed>(kZoomFn, kZoomRightOffset,
            a_zoomSpeedValue, fastTimer, "Havok/FixLoadingModel-zoom-right");
        const bool zoomRTail = !zoomR || EnginePatch::WriteBytes(kZoomFn, kZoomRightOffset + 0x5,
            kNop3, {}, "Havok/FixLoadingModel-zoom-right-tail");

        const bool repeat = InstallStub<LoadModelRepeatRate>(kLoadScreenFn, kRepeatRateOffset,
            "Havok/FixLoadingModel-repeat-rate");
        const bool repeatTail = !repeat || EnginePatch::WriteByte(kLoadScreenFn,
            kRepeatRateOffset + 0x5, kNop, "Havok/FixLoadingModel-repeat-rate-tail");

        const bool rotation = EnginePatch::WriteBytes(kLoadScreenRotFn, kLoadScreenRotOffset,
            kNop6, {}, "Havok/FixLoadingModel-rotation");

        struct PanSite
        {
            std::ptrdiff_t offset;
            const char* name;
            const char* tailName;
        };
        constexpr PanSite kPanSites[]{
            { kPanUpOffset, "Havok/FixLoadingModel-pan-up", "Havok/FixLoadingModel-pan-up-tail" },
            { kPanDownOffset, "Havok/FixLoadingModel-pan-down", "Havok/FixLoadingModel-pan-down-tail" },
            { kPanLeftOffset, "Havok/FixLoadingModel-pan-left", "Havok/FixLoadingModel-pan-left-tail" },
            { kPanRightOffset, "Havok/FixLoadingModel-pan-right", "Havok/FixLoadingModel-pan-right-tail" },
        };
        int panApplied = 0;
        for (const PanSite& site : kPanSites) {
            const bool ok = InstallValueStub<LoadModelPanSpeed>(kLoadScreenFn, site.offset,
                a_rotateSpeedValue, fastTimer, site.name);
            const bool tail = !ok || EnginePatch::WriteBytes(kLoadScreenFn, site.offset + 0x5,
                kNop3, {}, site.tailName);
            if (ok && tail) {
                ++panApplied;
            }
        }

        const bool all = zoomL && zoomLTail && zoomR && zoomRTail && repeat && repeatTail &&
                         rotation && panApplied == 4;
        if (all) {
            logger::info("[Havok] FixLoadingModel: all 8 sub-fixes applied — loading-screen model zoom/rotate no longer scale with framerate");
        } else {
            logger::warn("[Havok] FixLoadingModel: partial (zoom {}/{}, repeat {}, rotation {}, pan {}/4) — the failed paths still scale with framerate",
                zoomL && zoomLTail, zoomR && zoomRTail, repeat && repeatTail, rotation, panApplied);
        }
        return all;
    }

    void HavokFixes::ApplyStutterFixes() noexcept
    {
        const bool a = InstallStub<StutterConstantToInt>(kStutterFnA, kStutter1Offset,
            "Havok/FixStuttering-1");

        const bool b = EnginePatch::WriteBytes(kStutterFnB, kStutter2Offset, kNop8, {},
            "Havok/FixStuttering-2-nop");

        const bool c = InstallStub<StutterUseExisting>(kStutterFnA, kStutter3Offset,
            "Havok/FixStuttering-3");
        const bool cTail = c && EnginePatch::WriteByte(kStutterFnA, kStutter3Offset + 0x5, kNop,
            "Havok/FixStuttering-3-tail");
        const bool cNop = EnginePatch::WriteBytes(kStutterFnA, kStutter3Offset + 0xE, kNop4, {},
            "Havok/FixStuttering-3-nop4");

        const bool d = InstallStub<StutterFixedTimestep>(kObjectsTransferFn, kObjectsTransferOffset,
            "Havok/FixStuttering-objects");

        const int applied = (a ? 1 : 0) + (b ? 1 : 0) + (c ? 1 : 0) + (cTail ? 1 : 0) +
                            (cNop ? 1 : 0) + (d ? 1 : 0);
        if (applied == 6) {
            logger::info("[Havok] FixStuttering: all 6 sub-patches applied (frame-pacing hitches at high FPS)");
        } else {
            logger::warn("[Havok] FixStuttering: only {}/6 sub-patches applied — stuttering may persist on the paths that failed (see the lines above)",
                applied);
        }
    }
}
