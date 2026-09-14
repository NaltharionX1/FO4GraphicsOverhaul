#include "Platform/FingerprintMath.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace
{
    int g_failures = 0;
    int g_checks = 0;

    void Check(bool a_ok, const char* a_what)
    {
        ++g_checks;
        if (!a_ok) {
            ++g_failures;
            std::printf("FAIL: %s\n", a_what);
        }
    }

    using namespace Platform::Fingerprint;

    void TestTileSplit()
    {
        for (const auto [w, h] : { std::pair{ 1920U, 1080U }, std::pair{ 1921U, 1081U }, std::pair{ 33U, 33U }, std::pair{ 3840U, 2160U } }) {
            std::uint64_t total = 0;
            for (std::uint32_t t = 0; t < kTiles; ++t) total += TilePixels(w, h, t);
            Check(total == static_cast<std::uint64_t>(w) * h, "the sixteen tiles cover the image exactly once");
        }
        Check(TilePixels(1920U, 1080U, 0) == 480U * 270U, "tile 0 of 1080p is 480x270");
        Check(TilePixels(1921U, 1081U, 15) == 478U * 268U, "the last tile takes the remainder (481-wide columns: the last is 478; 271-tall rows: the last is 268)");
        Check(TilePixels(1921U, 1081U, 0) == 481U * 271U, "the first tile is the ceiling split");
    }

    void TestFromSums()
    {
        std::uint32_t sums[kSumsPerPoint]{};
        const std::uint32_t w = 33, h = 33;
        double weighted = 0.0;
        std::uint64_t pixels = 0;
        for (std::uint32_t t = 0; t < kTiles; ++t) {
            const std::uint32_t n = TilePixels(w, h, t);
            sums[2U * t] = n * 100U;
            sums[2U * t + 1U] = n * (t + 1U);
            weighted += static_cast<double>(n) * (t + 1U);
            pixels += n;
        }
        const double expected = weighted / static_cast<double>(pixels);
        const PointStats s = FromSums(sums, w, h);
        Check(s.valid, "a full set of sums is valid");
        Check(s.meanLuma == 100.0F, "the mean luma over the frame");
        Check(s.tileDelta[0] == 1.0F && s.tileDelta[15] == 16.0F, "per-tile means");
        Check(s.maxTileDelta == 16.0F, "the largest tile residual");
        Check(std::abs(static_cast<double>(s.meanDelta) - expected) < 1e-4, "the mean delta over the frame is the PIXEL-weighted mean");
        Check(std::abs(expected - 8.5) > 0.05 && std::abs(static_cast<double>(s.meanDelta) - 8.5) > 0.05,
            "...and not the unweighted mean of the tile means (8.5), which the uneven tiles rule out");
        Check(!FromSums(nullptr, w, h).valid && !FromSums(sums, 0U, h).valid, "no sums or no size -> invalid");
    }

    FrameRecord Quiet(std::uint64_t a_frame, float a_a, float a_b, float a_c, float a_d)
    {
        FrameRecord r{};
        r.frame = a_frame;
        const float v[kPoints]{ a_a, a_b, a_c, a_d };
        for (std::uint32_t p = 0; p < kPoints; ++p) {
            r.points[p].valid = true;
            r.points[p].meanLuma = 100.0F;
            r.points[p].meanDelta = v[p];
            r.points[p].maxTileDelta = v[p];
        }
        return r;
    }

    void TestClassify()
    {
        FrameRecord window[40]{};
        for (std::uint32_t i = 0; i < 40; ++i) window[i] = Quiet(1000U + i, 1.0F, 1.0F, 1.0F, 1.0F);
        Check(Classify<40>(window, 0).verdict == Verdict::kNoData, "no records -> no data");
        Check(Classify<40>(window, 40).verdict == Verdict::kAllStable, "a quiet window -> all stable (the elimination branch)");

        window[30] = Quiet(1030U, 12.0F, 12.0F, 12.0F, 12.0F);
        auto a = Classify<40>(window, 40);
        Check(a.verdict == Verdict::kUpstream && a.spiked[0] && a.spikeFrame[0] == 1030U, "the input spiking first -> upstream, with its frame");
        Check(a.baseline[0] == 1.0F && a.spike[0] == 12.0F, "the baseline is the median, the spike the largest");

        window[30] = Quiet(1030U, 1.0F, 12.0F, 12.0F, 12.0F);
        a = Classify<40>(window, 40);
        Check(a.verdict == Verdict::kNeural && !a.spiked[0] && a.spiked[1], "input stable, output spiking -> the neural contract/model/history");

        window[30] = Quiet(1030U, 1.0F, 1.0F, 12.0F, 12.0F);
        a = Classify<40>(window, 40);
        Check(a.verdict == Verdict::kPresent && a.spiked[2] && a.spikeFrame[2] == 1030U, "the presented frame spiking first -> the HUD / D3D11 present path");

        window[30] = Quiet(1030U, 1.0F, 1.0F, 1.0F, 12.0F);
        a = Classify<40>(window, 40);
        Check(a.verdict == Verdict::kCanvas && !a.spiked[2] && a.spiked[3] && a.spikeFrame[3] == 1030U, "only the displayed frame spiking -> the canvas draw");

        for (std::uint32_t i = 0; i < 40; ++i) window[i] = Quiet(2000U + i, 0.0F, 0.0F, 0.0F, 0.0F);
        window[5] = Quiet(2005U, 1.5F, 1.5F, 1.5F, 1.5F);
        a = Classify<40>(window, 40);
        Check(a.verdict == Verdict::kAllStable, "the floor keeps a still scene's noise from counting as a spike");

        for (std::uint32_t i = 0; i < 40; ++i) window[i] = Quiet(3000U + i, 6.0F, 6.0F, 6.0F, 6.0F);
        window[7] = Quiet(3007U, 17.0F, 17.0F, 17.0F, 17.0F);
        a = Classify<40>(window, 40);
        Check(a.verdict == Verdict::kAllStable, "a busy but steady window is not a spike (the ratio is against the median)");

        window[7] = Quiet(3007U, 30.0F, 30.0F, 6.0F, 6.0F);
        Check(Classify<40>(window, 40).verdict == Verdict::kUpstream, "...and does, while it is sampled");
        window[7].points[0].valid = false;
        a = Classify<40>(window, 40);
        Check(a.verdict == Verdict::kNeural && !a.spiked[0] && a.spiked[1],
            "an unsampled input on the spike frame is excluded (review: the old fixture was below its threshold anyway)");
        Check(Classify<40>(window, 400).frames == 40, "the count is clamped to the window's capacity");

        for (std::uint32_t i = 0; i < 40; ++i) window[i] = Quiet(4000U + i, 0.5F, 0.5F, 0.5F, 0.5F);
        window[20].points[3].meanDelta = 0.9F;
        window[20].points[3].maxTileDelta = 7.0F;
        a = Classify<40>(window, 40);
        Check(a.verdict == Verdict::kCanvas && a.spiked[3] && a.spikeTile[3] == 7.0F && a.spikeTileFrame[3] == 4020U,
            "a spike in one tile of the displayed frame is caught by the tile series (the mean alone would miss it)");
        Check(!a.spiked[2] && a.baselineTile[3] == 0.5F, "the other points and the tile baseline are untouched");
        window[20].points[3].maxTileDelta = 1.8F;
        a = Classify<40>(window, 40);
        Check(a.verdict == Verdict::kAllStable, "the floor applies to the tile series too (review: 1.4 failed the ratio, not the floor)");
        window[20].points[3].maxTileDelta = 2.1F;
        Check(Classify<40>(window, 40).verdict == Verdict::kCanvas, "...and just above the floor the tile spike counts");

        for (auto& r : window) for (auto& p : r.points) p.valid = false;
        a = Classify<40>(window, 40);
        Check(a.verdict == Verdict::kNoData && a.frames == 0, "an all-invalid window is no data with zero frames analysed");
    }

    void TestFinalHandoff()
    {
        Check(kPoints == 5U, "the capture includes the actual proxy input after the limiter wait");
        Check(std::strcmp(PointName(static_cast<Point>(4U)), "game buffer entering proxy") == 0,
            "the final sample names the handoff it measures");
        if constexpr (kPoints >= 5U) {
            FrameRecord window[40]{};
            for (std::uint32_t i=0; i<40; ++i) {
                window[i] = Quiet(5000U+i, 0.5F, 0.5F, 0.5F, 0.5F);
                window[i].points[4].meanDelta = 0.5F;
                window[i].points[4].maxTileDelta = 0.5F;
            }
            window[20].points[4].meanLuma = 0.0F;
            window[20].points[4].meanDelta = 80.0F;
            window[20].points[4].maxTileDelta = 80.0F;
            const auto result = Classify<40>(window, 40);
            Check(static_cast<std::uint32_t>(result.verdict) == 6U && !result.spiked[3] && result.spiked[4],
                "a black flash appearing only after the canvas/limiter is attributed to the final handoff");
            for (std::uint32_t i=0; i<40; ++i) {
                window[i].points[4].meanLuma = (i%2U)==0U ? 0.0F : 100.0F;
                window[i].points[4].meanDelta = 100.0F;
                window[i].points[4].maxTileDelta = 100.0F;
            }
            Check(Classify<40>(window,40).verdict == Verdict::kHandoff && Classify<40>(window,40).handoffBlackFrames == 20U,
                "sustained black alternation at the handoff is not called stable just because its median is high");
            for (auto& row:window) row.points[3].meanLuma = 0.0F;
            Check(Classify<40>(window,40).handoffBlackFrames == 0U,
                "an image already black at the canvas is not a new black transition in the handoff");
            for (auto& row:window) {row.points[3].meanLuma=100.0F;row.points[3].valid=false;}
            Check(Classify<40>(window,40).handoffBlackFrames == 0U,
                "a missing canvas observation is not proof that the handoff made it black");
        }
    }
}

int main()
{
    TestTileSplit();
    TestFromSums();
    TestClassify();
    TestFinalHandoff();
    std::printf("FingerprintMathTests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
