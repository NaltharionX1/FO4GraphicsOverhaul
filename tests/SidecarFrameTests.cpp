#include "Platform/SidecarFrame.h"

#include <cstdint>
#include <cstdio>

namespace {

struct TestHarness {
    int checks = 0;
    int failures = 0;

    void Check(const bool condition, const char* const expression, const int line) noexcept
    {
        ++checks;
        if (!condition) {
            ++failures;
            std::printf("FAIL line %d: %s\n", line, expression);
        }
    }
};

#define CHECK(harness, expression) (harness).Check((expression), #expression, __LINE__)

ID3D12Resource* Fake(std::uintptr_t a_id) noexcept
{
    return reinterpret_cast<ID3D12Resource*>(a_id);
}

Platform::SidecarFrame::Guides SomeGuides(std::uintptr_t a_id) noexcept
{
    Platform::SidecarFrame::Guides guides{};
    guides.depth = Fake(a_id);
    guides.mv = Fake(a_id + 1);
    guides.depthWidth = guides.mvWidth = guides.renderWidth = 1920;
    guides.depthHeight = guides.mvHeight = guides.renderHeight = 1080;
    guides.depthFormat = DXGI_FORMAT_R32_FLOAT;
    guides.mvFormat = DXGI_FORMAT_R16G16_FLOAT;
    guides.jitterX = 0.25F;
    guides.jitterY = -0.125F;
    return guides;
}

Platform::SidecarFrame::Hudless SomeHudless(std::uintptr_t a_id) noexcept
{
    Platform::SidecarFrame::Hudless hudless{};
    hudless.colour = Fake(a_id);
    hudless.width = 1920;
    hudless.height = 1080;
    hudless.format = DXGI_FORMAT_R8G8B8A8_UNORM;
    return hudless;
}

}

int main()
{
    using namespace Platform::SidecarFrame;
    TestHarness h{};

    {
        Cursor c{};
        Guides g{};
        Hudless hl{};
        CHECK(h, !TakeGuides(c, g));
        CHECK(h, !TakeHudless(c, hl));
        CHECK(h, c.serial == 0);
        const State s = Snapshot();
        CHECK(h, s.guidesSerial == 0 && s.hudlessSerial == 0 && !s.guidesLive && !s.hudlessLive);
    }

    {
        PublishGuides(SomeGuides(0x100));
        Cursor fg{}, neural{};
        Guides g{};
        CHECK(h, TakeGuides(fg, g));
        CHECK(h, g.depth == Fake(0x100) && g.mv == Fake(0x101) && g.renderWidth == 1920 && g.jitterX == 0.25F);
        CHECK(h, !TakeGuides(fg, g));
        CHECK(h, g.depth == nullptr);
        CHECK(h, TakeGuides(neural, g));
        CHECK(h, g.depth == Fake(0x100));
        CHECK(h, !TakeGuides(neural, g));
        const State s = Snapshot();
        CHECK(h, s.guidesSerial == 1 && s.guidesLive && s.guidesTaken == 2);

        PublishGuides(SomeGuides(0x200));
        CHECK(h, TakeGuides(fg, g) && g.depth == Fake(0x200));
        CHECK(h, TakeGuides(neural, g) && g.depth == Fake(0x200));
        CHECK(h, Snapshot().guidesSerial == 2);
    }

    {
        Cursor c{};
        Guides g{};
        PublishGuides(SomeGuides(0x300));
        InvalidateGuides();
        CHECK(h, !Snapshot().guidesLive);
        CHECK(h, !TakeGuides(c, g));
        CHECK(h, c.serial == 0);
        PublishGuides(SomeGuides(0x400));
        CHECK(h, TakeGuides(c, g) && g.depth == Fake(0x400));
    }

    {
        Cursor c{};
        Guides g{};
        PublishGuides(SomeGuides(0x500));
        Guides bad = SomeGuides(0x600);
        bad.mv = nullptr;
        PublishGuides(bad);
        CHECK(h, !Snapshot().guidesLive);
        CHECK(h, !TakeGuides(c, g));
        Hudless badHl = SomeHudless(0x700);
        badHl.width = 0;
        PublishHudless(badHl);
        Hudless hl{};
        CHECK(h, !TakeHudless(c, hl));
    }

    {
        Cursor fg{};
        Hudless hl{};
        Guides g{};
        PublishHudless(SomeHudless(0x800));
        CHECK(h, TakeHudless(fg, hl) && hl.colour == Fake(0x800) && hl.width == 1920 && hl.format == DXGI_FORMAT_R8G8B8A8_UNORM);
        CHECK(h, !TakeHudless(fg, hl));
        CHECK(h, !TakeGuides(fg, g));
        Invalidate();
        CHECK(h, !Snapshot().hudlessLive && !Snapshot().guidesLive);
        CHECK(h, !TakeHudless(fg, hl));
        const State s = Snapshot();
        CHECK(h, s.hudlessSerial == 1 && s.hudlessTaken == 1);
    }

    {
        Cursor a{}, b{};
        Guides g{};
        BeginFrame(7);
        PublishGuides(SomeGuides(0xE00));
        CHECK(h, TakeGuides(a, g) && g.depth == Fake(0xE00));
        BeginFrame(8);
        CHECK(h, !TakeGuides(b, g));
        CHECK(h, b.serial == 0);
        CHECK(h, Snapshot().staleRefused == 1);
        Guides fresh = SomeGuides(0xE01);
        fresh.motion = GuideMotion::kDilated;
        PublishGuides(fresh);
        CHECK(h, TakeGuides(b, g) && g.depth == Fake(0xE01) && g.motion == GuideMotion::kDilated);
        Hudless hl{};
        PublishHudless(SomeHudless(0xE02));
        BeginFrame(9);
        CHECK(h, !TakeHudless(a, hl) && Snapshot().staleRefused == 2);
        BeginFrame(0);
        Invalidate();
    }

    {
        Cursor c{};
        Guides g{};
        PublishGuides(SomeGuides(0x900));
        PublishGuides(SomeGuides(0xA00));
        CHECK(h, TakeGuides(c, g) && g.depth == Fake(0xA00));
        CHECK(h, !TakeGuides(c, g));
    }

    {
        Cursor c{};
        Guides g{};
        for (int clause = 0; clause < 8; ++clause) {
            Guides bad = SomeGuides(0xB00);
            switch (clause) {
            case 0: bad.depth = nullptr; break;
            case 1: bad.mv = nullptr; break;
            case 2: bad.depthWidth = 0; break;
            case 3: bad.depthHeight = 0; break;
            case 4: bad.mvWidth = 0; break;
            case 5: bad.mvHeight = 0; break;
            case 6: bad.renderWidth = 0; break;
            default: bad.renderHeight = 0; break;
            }
            PublishGuides(SomeGuides(0xB10));
            PublishGuides(bad);
            CHECK(h, !Snapshot().guidesLive);
            CHECK(h, !TakeGuides(c, g));
        }
        Hudless hl{};
        for (int clause = 0; clause < 4; ++clause) {
            Hudless bad = SomeHudless(0xC00);
            switch (clause) {
            case 0: bad.colour = nullptr; break;
            case 1: bad.width = 0; break;
            case 2: bad.height = 0; break;
            default: bad.format = DXGI_FORMAT_UNKNOWN; break;
            }
            PublishHudless(SomeHudless(0xC10));
            PublishHudless(bad);
            CHECK(h, !Snapshot().hudlessLive);
            CHECK(h, !TakeHudless(c, hl));
        }
    }

    {
        Cursor a{}, b{};
        Hudless hl{};
        PublishHudless(SomeHudless(0xD00));
        CHECK(h, TakeHudless(a, hl) && hl.colour == Fake(0xD00));
        CHECK(h, !TakeHudless(a, hl));
        CHECK(h, TakeHudless(b, hl) && hl.colour == Fake(0xD00));
        CHECK(h, !TakeHudless(b, hl));
        PublishHudless(SomeHudless(0xD01));
        CHECK(h, TakeHudless(a, hl) && hl.colour == Fake(0xD01));
        CHECK(h, TakeHudless(b, hl) && hl.colour == Fake(0xD01));
    }

    std::printf("SidecarFrameTests: %d checks, %d failures\n", h.checks, h.failures);
    return h.failures == 0 ? 0 : 1;
}
