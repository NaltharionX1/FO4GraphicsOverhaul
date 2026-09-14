#include "Platform/GtaoOcclusion.h"

#include <cstdio>
#include <vector>

namespace
{
    using namespace Platform::GtaoOcclusion;

    int g_failures = 0;

    void Check(bool a_condition, const char* a_what)
    {
        if (!a_condition) {
            std::printf("FAIL: %s\n", a_what);
            ++g_failures;
        }
    }

    void CheckNear(float a_actual, float a_expected, float a_tolerance, const char* a_what)
    {
        if (!(std::abs(a_actual - a_expected) <= a_tolerance)) {
            std::printf("FAIL: %s (got %.6f, expected %.6f +/- %.6f)\n", a_what,
                static_cast<double>(a_actual), static_cast<double>(a_expected),
                static_cast<double>(a_tolerance));
            ++g_failures;
        }
    }

    constexpr float kN = 0.0F;
    constexpr float kCosNorm = 1.0F;
    constexpr float kProjLen = 1.0F;
}

int main()
{

    CheckNear(Iarc(0.0F, kN, kCosNorm), 0.0F, 1e-6F,
        "Iarc(0) == 0 — the integral is measured FROM zero, which is why the two sides add");

    for (float n = -1.4F; n <= 1.4F; n += 0.2F) {
        CheckNear(Iarc(0.0F, n, std::cos(n)), 0.0F, 1e-6F,
            "Iarc(0) == 0 holds for every projected-normal angle, not just n = 0");
    }

    for (float n = -1.0F; n <= 1.0F; n += 0.5F) {
        const float cosNorm = std::cos(n);
        for (float h = -1.4F; h <= 1.4F; h += 0.1F) {
            const float eps = 1e-4F;
            const float numeric = (Iarc(h + eps, n, cosNorm) - Iarc(h - eps, n, cosNorm)) / (2 * eps);
            const float analytic = std::sin(h) * std::cos(h - n);
            CheckNear(numeric, analytic, 2e-3F, "d(Iarc)/dh == sin(h)*cos(h-n)");
        }
    }

    {
        const float stock = VisibilityStock(nullptr, 0, kN, kCosNorm, kProjLen, 0.0F);
        const float bitmask = VisibilityBitmask(nullptr, 0, kN, kCosNorm, kProjLen);
        CheckNear(stock, 1.0F, 1e-5F, "stock: an unoccluded slice is fully visible");
        CheckNear(bitmask, 1.0F, 1e-3F, "bitmask: an unoccluded slice is fully visible");
        CheckNear(bitmask, stock, 1e-3F, "the two models agree when nothing occludes");
    }

    for (float front = -1.2F; front <= 1.2F; front += 0.15F) {
        const Occluder slab{ front, (front >= 0.0F) ? DomainMax(kN) : DomainMin(kN) };
        const float stock = VisibilityStock(&slab, 1, kN, kCosNorm, kProjLen, 0.0F);
        const float bitmask = VisibilityBitmask(&slab, 1, kN, kCosNorm, kProjLen);

        CheckNear(bitmask, stock, 0.07F,
            "bitmask reproduces the analytic result for a single infinite-slab occluder");
    }

    {
        const Occluder thin{ 0.15F, 0.30F };

        const float stock = VisibilityStock(&thin, 1, kN, kCosNorm, kProjLen, 0.0F);
        const float bitmask = VisibilityBitmask(&thin, 1, kN, kCosNorm, kProjLen);

        const float truth = ArcMeasure(DomainMin(kN), thin.front, kN, kCosNorm) +
                            ArcMeasure(thin.back, DomainMax(kN), kN, kCosNorm);

        CheckNear(bitmask, truth, 0.07F, "HALO: the bitmask occludes only the band the object subtends");

        Check(stock < truth - 0.15F,
            "HALO: the stock running-maximum model over-occludes — this MUST fail for stock");
        Check(bitmask > stock + 0.15F,
            "HALO: the bitmask recovers visibility the stock model destroys");

        const float patched = VisibilityStock(&thin, 1, kN, kCosNorm, kProjLen, 0.05F);
        Check(patched < truth - 0.10F,
            "HALO: thinOccluderCompensation at 0.05 still over-occludes — it softens a latch it "
            "cannot remove");
        Check(bitmask > patched,
            "HALO: the bitmask beats the best the patched stock model can do");
    }

    {
        const Occluder a[3]{ { 0.2F, 0.4F }, { 0.6F, 0.8F }, { -0.5F, -0.3F } };
        const Occluder b[3]{ { -0.5F, -0.3F }, { 0.6F, 0.8F }, { 0.2F, 0.4F } };
        const float first = VisibilityBitmask(a, 3, kN, kCosNorm, kProjLen);
        const float second = VisibilityBitmask(b, 3, kN, kCosNorm, kProjLen);
        CheckNear(first, second, 1e-6F, "bitmask is order-invariant — it is an OR");
    }

    {
        std::vector<Occluder> occluders;
        float previous = VisibilityBitmask(nullptr, 0, kN, kCosNorm, kProjLen);
        for (int i = 0; i < 6; ++i) {
            const float lo = -1.2F + static_cast<float>(i) * 0.4F;
            occluders.push_back(Occluder{ lo, lo + 0.2F });
            const float current =
                VisibilityBitmask(occluders.data(), occluders.size(), kN, kCosNorm, kProjLen);
            Check(current <= previous + 1e-5F, "adding an occluder never increases visibility");
            Check(current >= -1e-5F, "visibility never goes negative");
            previous = current;
        }
    }

    {
        const Occluder everything{ DomainMin(kN), DomainMax(kN) };
        const float bitmask = VisibilityBitmask(&everything, 1, kN, kCosNorm, kProjLen);
        CheckNear(bitmask, 0.0F, 1e-5F, "an occluder spanning the domain leaves nothing visible");
    }

    {
        const Occluder degenerate{ 0.5F, 0.5F };
        const float v = VisibilityBitmask(&degenerate, 1, kN, kCosNorm, kProjLen);
        Check(v < 1.0F, "a zero-width occluder is not silently lost");

        const Occluder atEdge[2]{ { DomainMin(kN), DomainMin(kN) + 0.01F },
            { DomainMax(kN) - 0.01F, DomainMax(kN) } };
        const float edged = VisibilityBitmask(atEdge, 2, kN, kCosNorm, kProjLen);
        Check(edged >= 0.0F && edged <= 1.05F, "domain-edge occluders stay in range");

        const Occluder outside{ DomainMax(kN) + 1.0F, DomainMax(kN) + 2.0F };
        const float ignored = VisibilityBitmask(&outside, 1, kN, kCosNorm, kProjLen);
        CheckNear(ignored, VisibilityBitmask(nullptr, 0, kN, kCosNorm, kProjLen), 0.05F,
            "an occluder outside the domain does not occlude");
    }

    for (float n = -1.2F; n <= 1.2F; n += 0.3F) {
        const float cosNorm = std::cos(n);
        for (const float offset : { -0.4F, 0.4F }) {
            const float front = offset;
            const Occluder slab{ front, (front >= 0.0F) ? DomainMax(n) : DomainMin(n) };
            const float stock = VisibilityStock(&slab, 1, n, cosNorm, kProjLen, 0.0F);
            const float bitmask = VisibilityBitmask(&slab, 1, n, cosNorm, kProjLen);
            CheckNear(bitmask, stock, 0.09F, "equivalence holds for tilted projected normals too");
            Check(bitmask >= -1e-5F, "tilted slices stay non-negative");
        }
    }

    {
        const float straight = VisibilityBitmask(nullptr, 0, 0.0F, 1.0F, kProjLen);
        const float tilted = VisibilityBitmask(nullptr, 0, 1.0F, std::cos(1.0F), kProjLen);
        CheckNear(straight, 1.0F, 1e-3F, "an untilted unoccluded slice integrates to exactly 1");
        Check(tilted > 1.0F,
            "a tilted unoccluded slice overshoots 1 — this is why PATCH 12 saturates before storing");
    }

    {
        CheckNear(FinishSlices(2.0F, 2.0F, 1.0F), 1.0F, 1e-6F, "FinishSlices averages by slice count");
        Check(FinishSlices(0.0F, 1.0F, 2.2F) >= 0.03F, "total occlusion is disallowed");
        Check(FinishSlices(-0.5F, 1.0F, 2.2F) >= 0.0F, "PATCH 6's abs() keeps pow() finite");
    }

    if (g_failures == 0) {
        std::printf("GtaoOcclusionTests: all checks passed\n");
    }
    return g_failures == 0 ? 0 : 1;
}
