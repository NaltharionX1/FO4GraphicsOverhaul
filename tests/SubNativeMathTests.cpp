#include "Platform/Matrix4.h"
#include "Platform/SubNativeMath.h"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <string_view>

namespace
{
    int g_failures = 0;
    int g_checks = 0;

    void Check(bool a_condition, std::string_view a_message)
    {
        ++g_checks;
        if (!a_condition) {
            ++g_failures;
            std::cout << "FAIL: " << a_message << '\n';
        }
    }
}

int main()
{
    using namespace Platform;

    Check(MaxMipLevelsForDims(1, 1) == 1, "1x1 supports exactly 1 mip");
    Check(MaxMipLevelsForDims(2, 1) == 2, "2x1 supports 2 mips");
    Check(MaxMipLevelsForDims(1920, 1080) == 11, "1920x1080 supports 11 mips (1+floor(log2(1920)))");
    Check(MaxMipLevelsForDims(640, 360) == 10, "640x360 supports 10 mips");
    Check(MaxMipLevelsForDims(1024, 1024) == 11, "1024 square supports 11 mips (exact power of two)");
    Check(MaxMipLevelsForDims(1023, 1023) == 10, "1023 square supports 10 mips (just below the power)");

    constexpr int kList[] = { 20, 25, 4 };
    Check(ShouldCopyOnOverride(kList, 3, 25), "override copies a listed index");
    Check(!ShouldCopyOnOverride(kList, 3, 57), "override does NOT copy an unlisted index");
    Check(!ShouldCopyOnOverride(nullptr, 0, 20), "override with an EMPTY list copies NONE (cheap default)");
    Check(ShouldCopyOnReset(kList, 3, 4), "reset copies a listed index");
    Check(!ShouldCopyOnReset(kList, 3, 57), "reset does NOT copy an unlisted index when a list is given");
    Check(ShouldCopyOnReset(nullptr, 0, 57), "reset with an EMPTY list copies ALL (correct default)");

    Check(ResolveTargetDim(1280, 1.0F, 1920) == 1920, "TSR off: the target IS the monitor");
    Check(ResolveTargetDim(1280, 1.001F, 1920) == 1920, "TSR at the epsilon boundary counts as off");
    Check(ResolveTargetDim(1280, 2.0F, 1920) == 2560, "Quality+TSR2 @1080p: 720p render -> 1440p target");
    Check(ResolveTargetDim(640, 2.0F, 1920) == 1280, "UP+TSR2: 640 -> 1280 (BELOW the monitor - unfloored)");
    Check(ResolveTargetDim(960, 2.0F, 1920) == 1920, "P+TSR2: 960 -> exactly the monitor (in-place case)");
    Check(ResolveTargetDim(1920, 2.0F, 1920) == 3840, "DLAA+TSR2: native -> 4K target");
    Check(ResolveTargetDim(1476, 1.5F, 1920) == 2214, "fractional scale rounds to nearest");
    Check(ResolveTargetDim(0, 2.0F, 1920) == 1, "degenerate render dim floors the target at 1");

    Check(EffectiveRatioBucket(1.0F) == 0, "1:1 buckets DLAA");
    Check(EffectiveRatioBucket(0.999F) == 0, "0.999 buckets DLAA (epsilon edge)");
    Check(EffectiveRatioBucket(0.99F) == 1, "0.99 buckets Quality - NGX DLAA requires a true 1:1");
    Check(EffectiveRatioBucket(0.769F) == 1, "the Ultra Quality ratio buckets Quality (crash policy)");
    Check(EffectiveRatioBucket(2.0F / 3.0F) == 1, "the published Quality ratio buckets Quality");
    Check(EffectiveRatioBucket(0.581F) == 2, "the published Balanced ratio buckets Balanced");
    Check(EffectiveRatioBucket(0.5F) == 3, "the published Performance ratio buckets Performance");
    Check(EffectiveRatioBucket(1.0F / 3.0F) == 4, "the published UP ratio buckets Ultra Performance");
    Check(EffectiveRatioBucket(0.05F) == 4, "extreme ratios stay Ultra Performance");
    Check(EffectiveRatioBucket(1.0F / 1.5F) == 1, "TSR 1.5 evaluates as Quality-class reconstruction");
    Check(EffectiveRatioBucket(1.0F / 2.0F) == 3, "TSR 2.0 evaluates as Performance-class reconstruction");

    {
        const float identity[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
        const float translate[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 2, 3, 4, 1 };
        const float scale[16] = { 2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 1 };
        float product[16]{};
        Matrix4::Multiply(product, translate, scale);
        Check(product[0] == 2.0F && product[5] == 2.0F && product[10] == 2.0F && product[15] == 1.0F,
            "Multiply: the diagonal of T*S is the scale");
        Check(product[12] == 4.0F && product[13] == 6.0F && product[14] == 8.0F,
            "Multiply: T*S scales the translation row (row-vector convention, a * b in that order)");
        float productRev[16]{};
        Matrix4::Multiply(productRev, scale, translate);
        Check(productRev[12] == 2.0F && productRev[13] == 3.0F && productRev[14] == 4.0F,
            "Multiply: S*T keeps the translation (the order matters, as in the SDK helper)");
        float viaIdentity[16]{};
        Matrix4::Multiply(viaIdentity, translate, identity);
        bool same = true;
        for (int i = 0; i < 16; ++i) { same = same && viaIdentity[i] == translate[i]; }
        Check(same, "Multiply: M * I == M");

        const float general[16] = { 2, 0, 1, 0, 1, 3, 0, 1, 0, 1, 4, 0, 5, 0, 2, 1 };
        float inverse[16]{};
        Check(Matrix4::Invert(inverse, general), "Invert: a regular matrix inverts");
        float check[16]{};
        Matrix4::Multiply(check, general, inverse);
        bool isIdentity = true;
        for (int i = 0; i < 16; ++i) {
            isIdentity = isIdentity && std::fabs(check[i] - identity[i]) < 1.0e-5F;
        }
        Check(isIdentity, "Invert: M * M^-1 == I (within 1e-5)");
        float inverseT[16]{};
        Check(Matrix4::Invert(inverseT, translate) && inverseT[12] == -2.0F && inverseT[13] == -3.0F && inverseT[14] == -4.0F,
            "Invert: a translation inverts to the negated translation row");
        const float singular[16] = { 1, 2, 3, 4, 2, 4, 6, 8, 0, 0, 1, 0, 0, 0, 0, 1 };
        float junk[16]{};
        Check(!Matrix4::Invert(junk, singular), "Invert: a singular matrix reports false (the SDK helper left an unscaled adjugate)");
    }

    std::cout << (g_failures == 0 ? "PASS" : "FAIL") << " (" << g_checks << " checks, "
              << g_failures << " failures)\n";
    return g_failures == 0 ? 0 : 1;
}
