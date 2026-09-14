#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace Platform::Fingerprint
{
    inline constexpr std::uint32_t kTilesX = 4;
    inline constexpr std::uint32_t kTilesY = 4;
    inline constexpr std::uint32_t kTiles = kTilesX * kTilesY;
    static_assert(kTilesX == 4U && kTilesY == 4U && kTiles == 16U, "FingerprintCS.hlsl hard-codes a 4x4 tile grid");
    inline constexpr std::uint32_t kPoints = 5;
    inline constexpr std::uint32_t kSumsPerPoint = kTiles * 2;
    inline constexpr std::uint32_t kSumsTotal = kPoints * kSumsPerPoint;

    enum class Point : std::uint32_t
    {
        kNeuralInput = 0,
        kNeuralOutput = 1,
        kPresented = 2,
        kDisplayed = 3,
        kProxyInput = 4,
    };

    [[nodiscard]] inline const char* PointName(Point a_point) noexcept
    {
        switch (a_point) {
        case Point::kNeuralInput: return "neural input";
        case Point::kNeuralOutput: return "neural output";
        case Point::kPresented: return "game buffer at Present";
        case Point::kDisplayed: return "game buffer after the canvas";
        case Point::kProxyInput: return "game buffer entering proxy";
        default: return "unknown point";
        }
    }

    struct PointStats
    {
        float tileLuma[kTiles]{};
        float tileDelta[kTiles]{};
        float meanLuma{ 0.0F };
        float meanDelta{ 0.0F };
        float maxTileDelta{ 0.0F };
        bool valid{ false };
    };

    [[nodiscard]] inline std::uint32_t TilePixels(std::uint32_t a_width, std::uint32_t a_height, std::uint32_t a_tile) noexcept
    {
        const std::uint32_t tx = a_tile % kTilesX, ty = a_tile / kTilesX;
        const std::uint32_t tileW = (a_width + kTilesX - 1U) / kTilesX;
        const std::uint32_t tileH = (a_height + kTilesY - 1U) / kTilesY;
        const std::uint32_t x0 = tx * tileW, y0 = ty * tileH;
        if (x0 >= a_width || y0 >= a_height) return 0U;
        const std::uint32_t w = std::min(tileW, a_width - x0);
        const std::uint32_t h = std::min(tileH, a_height - y0);
        return w * h;
    }

    [[nodiscard]] inline PointStats FromSums(const std::uint32_t* a_sums, std::uint32_t a_width, std::uint32_t a_height) noexcept
    {
        PointStats out{};
        if (a_sums == nullptr || a_width == 0U || a_height == 0U) return out;
        double lumaTotal = 0.0, deltaTotal = 0.0;
        std::uint64_t pixels = 0;
        for (std::uint32_t t = 0; t < kTiles; ++t) {
            const std::uint32_t n = TilePixels(a_width, a_height, t);
            if (n == 0U) continue;
            out.tileLuma[t] = static_cast<float>(static_cast<double>(a_sums[2U * t]) / n);
            out.tileDelta[t] = static_cast<float>(static_cast<double>(a_sums[2U * t + 1U]) / n);
            out.maxTileDelta = std::max(out.maxTileDelta, out.tileDelta[t]);
            lumaTotal += a_sums[2U * t];
            deltaTotal += a_sums[2U * t + 1U];
            pixels += n;
        }
        if (pixels != 0U) {
            out.meanLuma = static_cast<float>(lumaTotal / static_cast<double>(pixels));
            out.meanDelta = static_cast<float>(deltaTotal / static_cast<double>(pixels));
            out.valid = true;
        }
        return out;
    }

    struct FrameRecord
    {
        std::uint64_t frame{ 0 };
        double timeMs{ 0.0 };
        PointStats points[kPoints]{};
        bool neuralOn{ false };
        std::uint32_t passesDelivered{ 0 };
        bool neuralReset{ false };
        std::uint64_t historyEpoch{ 0 };
        int guidesSource{ -1 };
        bool fgInterpolating{ false };
        bool menuOpen{ false };
        bool aaDrain{ false };
        float neuralCpuMs{ 0.0F };
    };

    inline constexpr float kSpikeRatio = 3.0F;
    inline constexpr float kSpikeFloor = 2.0F;

    enum class Verdict : std::uint32_t
    {
        kNoData = 0,
        kAllStable,
        kUpstream,
        kNeural,
        kPresent,
        kCanvas,
        kHandoff,
    };

    struct Analysis
    {
        Verdict verdict{ Verdict::kNoData };
        float baseline[kPoints]{};
        float spike[kPoints]{};
        std::uint64_t spikeFrame[kPoints]{};
        float baselineTile[kPoints]{};
        float spikeTile[kPoints]{};
        std::uint64_t spikeTileFrame[kPoints]{};
        bool spiked[kPoints]{};
        std::uint32_t frames{ 0 };
        std::uint32_t handoffBlackFrames{ 0 };
    };

    [[nodiscard]] inline const char* VerdictName(Verdict a_verdict) noexcept
    {
        switch (a_verdict) {
        case Verdict::kAllStable: return "no sampled point spiked: unobserved stages, the D3D12 copy, generated frames or scan-out remain to be checked";
        case Verdict::kUpstream:  return "the neural INPUT spiked first: upstream (super-resolution, exposure, grading, engine state)";
        case Verdict::kNeural:    return "the input was stable and the neural OUTPUT spiked: the neural contract, model or history";
        case Verdict::kPresent:   return "input and output stable, the GAME BUFFER AT PRESENT spiked: the HUD or the D3D11 path before the canvas";
        case Verdict::kCanvas:    return "input, output and the game buffer at Present stable, the buffer AFTER THE CANVAS spiked: our canvas draw (the menu / OSD)";
        case Verdict::kHandoff:   return "the canvas output was stable, but the actual PROXY INPUT spiked: the final handoff after the canvas / limiter";
        default:                  return "no data";
        }
    }

    namespace detail
    {
        [[nodiscard]] inline float Median(float* a_values, std::uint32_t a_count) noexcept
        {
            if (a_count == 0U) return 0.0F;
            std::sort(a_values, a_values + a_count);
            std::uint32_t rank = static_cast<std::uint32_t>(std::ceil(0.5F * static_cast<float>(a_count)));
            if (rank < 1U) rank = 1U;
            if (rank > a_count) rank = a_count;
            return a_values[rank - 1U];
        }

        [[nodiscard]] inline bool Spiked(float a_spike, float a_baseline) noexcept
        {
            return a_spike > kSpikeFloor && a_spike > a_baseline * kSpikeRatio;
        }
    }

    template <std::size_t N>
    [[nodiscard]] inline Analysis Classify(const FrameRecord* a_records, std::uint32_t a_count) noexcept
    {
        Analysis out{};
        if (a_records == nullptr || a_count == 0U) return out;
        if (a_count > N) a_count = static_cast<std::uint32_t>(N);
        out.frames = a_count;
        bool any = false;
        for (std::uint32_t p = 0; p < kPoints; ++p) {
            float means[N];
            float tiles[N];
            std::uint32_t n = 0;
            float spike = -1.0F, spikeTile = -1.0F;
            std::uint64_t spikeFrame = 0, spikeTileFrame = 0;
            for (std::uint32_t i = 0; i < a_count; ++i) {
                const PointStats& s = a_records[i].points[p];
                if (!s.valid) continue;
                means[n] = s.meanDelta;
                tiles[n] = s.maxTileDelta;
                ++n;
                if (s.meanDelta > spike) {
                    spike = s.meanDelta;
                    spikeFrame = a_records[i].frame;
                }
                if (s.maxTileDelta > spikeTile) {
                    spikeTile = s.maxTileDelta;
                    spikeTileFrame = a_records[i].frame;
                }
            }
            if (n == 0U) continue;
            any = true;
            out.baseline[p] = detail::Median(means, n);
            out.baselineTile[p] = detail::Median(tiles, n);
            out.spike[p] = spike;
            out.spikeFrame[p] = spikeFrame;
            out.spikeTile[p] = spikeTile;
            out.spikeTileFrame[p] = spikeTileFrame;
            out.spiked[p] = detail::Spiked(spike, out.baseline[p]) || detail::Spiked(spikeTile, out.baselineTile[p]);
        }
        if (!any) {
            out.frames = 0;
            return out;
        }
        for (std::uint32_t i = 0; i < a_count; ++i) {
            const auto& canvas = a_records[i].points[static_cast<std::uint32_t>(Point::kDisplayed)];
            const auto& handoff = a_records[i].points[static_cast<std::uint32_t>(Point::kProxyInput)];
            if (canvas.valid && handoff.valid && canvas.meanLuma >= 5.0F && handoff.meanLuma <= 1.0F) {
                ++out.handoffBlackFrames;
            }
        }
        if (out.handoffBlackFrames != 0U) out.spiked[static_cast<std::uint32_t>(Point::kProxyInput)] = true;
        if (out.spiked[static_cast<std::uint32_t>(Point::kNeuralInput)]) {
            out.verdict = Verdict::kUpstream;
        } else if (out.spiked[static_cast<std::uint32_t>(Point::kNeuralOutput)]) {
            out.verdict = Verdict::kNeural;
        } else if (out.spiked[static_cast<std::uint32_t>(Point::kPresented)]) {
            out.verdict = Verdict::kPresent;
        } else if (out.spiked[static_cast<std::uint32_t>(Point::kDisplayed)]) {
            out.verdict = Verdict::kCanvas;
        } else if (out.spiked[static_cast<std::uint32_t>(Point::kProxyInput)]) {
            out.verdict = Verdict::kHandoff;
        } else {
            out.verdict = Verdict::kAllStable;
        }
        return out;
    }
}
