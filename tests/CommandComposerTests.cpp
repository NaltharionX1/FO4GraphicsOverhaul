#include "Platform/CommandComposer.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string_view>

namespace
{
    int g_failures = 0;

    void Expect(std::string_view a_actual, std::string_view a_expected, const char* a_what)
    {
        if (a_actual != a_expected) {
            std::printf("FAIL: %s\n  expected: \"%.*s\"\n  actual:   \"%.*s\"\n", a_what,
                static_cast<int>(a_expected.size()), a_expected.data(),
                static_cast<int>(a_actual.size()), a_actual.data());
            ++g_failures;
        }
    }

    void Check(bool a_condition, const char* a_what)
    {
        if (!a_condition) {
            std::printf("FAIL: %s\n", a_what);
            ++g_failures;
        }
    }

    class Store
    {
    public:
        Store()
        {
            Platform::VisitCatalogue([this](const Platform::CatalogueRow& a_row, std::size_t a_index) {
                values_[a_index] = a_row.vanillaDefault;
            });
        }

        void Set(std::string_view a_id, double a_value)
        {
            values_[Platform::GlobalIndexForId(a_id)] = a_value;
        }

        [[nodiscard]] Platform::CommandComposer::ValueView View() const noexcept
        {
            return { values_.data(), values_.size() };
        }

    private:
        std::array<double, Platform::kCatalogueRowTotal> values_{};
    };

    [[nodiscard]] const Platform::CatalogueRow& Row(std::string_view a_id)
    {
        const auto* const row = Platform::RowByGlobalIndex(Platform::GlobalIndexForId(a_id));
        if (row == nullptr) {
            std::printf("FAIL: test asked for a row that does not exist: %.*s\n",
                static_cast<int>(a_id.size()), a_id.data());
            ++g_failures;
            static Platform::CatalogueRow dummy{};
            return dummy;
        }
        return *row;
    }

    void FormattingIsLocaleIndependentAndTidy()
    {
        char buf[64]{};
        using Platform::CommandComposer::FormatValue;
        using Platform::ValueType;

        Check(FormatValue(buf, sizeof(buf), ValueType::kFloat, 1.0), "format 1.0");
        Expect(buf, "1", "a whole float sheds its decimals");

        Check(FormatValue(buf, sizeof(buf), ValueType::kFloat, 0.3), "format 0.3");
        Expect(buf, "0.3", "trailing zeros are trimmed");

        Check(FormatValue(buf, sizeof(buf), ValueType::kFloat, 0.028672), "format bloom radius");
        Expect(buf, "0.028672", "small floats stay in fixed notation, never scientific");

        Check(FormatValue(buf, sizeof(buf), ValueType::kInt, 2.9999), "format 2.9999 as int");
        Expect(buf, "3", "an int rounds rather than truncates - a slider at 2.9999 means 3");

        Check(FormatValue(buf, sizeof(buf), ValueType::kFloat, 10000000.0), "format fog ceiling");
        Expect(buf, "10000000", "the fog ceiling renders as a plain integer");

        Check(!FormatValue(buf, sizeof(buf), ValueType::kFloat,
                  std::numeric_limits<double>::quiet_NaN()),
            "a non-finite value is refused, never sent as \"nan\"");

        char tiny[3]{};
        Check(!FormatValue(tiny, sizeof(tiny), ValueType::kFloat, 0.028672),
            "a too-small buffer refuses rather than truncating (a truncated float is a DIFFERENT "
            "number)");
        Expect(tiny, "", "a refused format leaves the buffer empty");
    }

    void CompositesSendEveryArgument()
    {
        const Store store;
        char buf[256]{};

        Check(Platform::CommandComposer::Compose(Row("Shp.BloomRadius"), store.View(), buf, sizeof(buf)),
            "compose shp");
        Expect(buf, "shp 3 1 0.104701 0.233586 0.3 1.899242 1.067172 1.671722 2.798483",
            "touching ONE shp slider must send all nine values in php's order");

        Check(Platform::CommandComposer::Compose(Row("Scp.Contrast"), store.View(), buf, sizeof(buf)),
            "compose scp");
        Expect(buf, "scp 1 1 1", "scp sends all three at the grading pass's identity defaults");

        Check(Platform::CommandComposer::Compose(Row("Stp.Red"), store.View(), buf, sizeof(buf)),
            "compose stp");
        Expect(buf, "stp 0 0 0 0", "stp sends all four, tint amount off by default");

        Check(Platform::CommandComposer::Compose(Row("Fov.World"), store.View(), buf, sizeof(buf)),
            "compose fov");
        Expect(buf, "fov 80 70", "fov sends third person then first person, never one alone");
    }

    void ChangedValuesPropagate()
    {
        Store store;
        store.Set("Stp.Strength", 2.0);
        char buf[256]{};

        Check(Platform::CommandComposer::Compose(Row("Stp.Strength"), store.View(), buf, sizeof(buf)),
            "compose stp after a change");
        Expect(buf, "stp 0 0 0 2",
            "the changed argument carries, the other three keep their default values");

        Store bw;
        bw.Set("Stp.Red", 1.0);
        bw.Set("Stp.Green", 1.0);
        bw.Set("Stp.Blue", 1.0);
        bw.Set("Stp.Strength", 1.0);
        Check(Platform::CommandComposer::Compose(Row("Stp.Red"), bw.View(), buf, sizeof(buf)),
            "compose the black-and-white preset");
        Expect(buf, "stp 1 1 1 1", "the black-and-white recipe composes exactly");

        Store black;
        black.Set("Stp.Red", 0.0);
        black.Set("Stp.Green", 0.0);
        black.Set("Stp.Blue", 0.0);
        black.Set("Stp.Strength", 1.0);
        Check(Platform::CommandComposer::Compose(Row("Stp.Blue"), black.View(), buf, sizeof(buf)),
            "compose the blackout");
        Expect(buf, "stp 0 0 0 1",
            "the full-black combination stays reachable - it is intentional, not a bug");
    }

    void FogModesAreNotDistances()
    {
        char buf[128]{};
        using Platform::CommandComposer::Compose;
        using Platform::CommandComposer::FogMode;

        Store dynamic;
        dynamic.Set("Fog.Mode", static_cast<double>(FogMode::kDynamic));
        Check(Compose(Row("Fog.Mode"), dynamic.View(), buf, sizeof(buf)), "compose fog dynamic");
        Expect(buf, "setfog 0 0", "Dynamic hands fog to the engine - it is NOT a distance of zero");

        Store off;
        off.Set("Fog.Mode", static_cast<double>(FogMode::kOff));
        Check(Compose(Row("Fog.Mode"), off.View(), buf, sizeof(buf)), "compose fog off");
        Expect(buf, "setfog 1 1", "two EQUAL values disable fog");

        Store custom;
        custom.Set("Fog.Mode", static_cast<double>(FogMode::kCustom));
        custom.Set("Fog.Distance", 5000.0);
        Check(Compose(Row("Fog.Distance"), custom.View(), buf, sizeof(buf)), "compose fog custom");
        Expect(buf, "setfog 0 5000", "Custom pins the distance in the second argument");

        char viaMode[128]{};
        Check(Compose(Row("Fog.Mode"), custom.View(), viaMode, sizeof(viaMode)), "compose via mode");
        Expect(viaMode, "setfog 0 5000", "both fog rows compose the identical command");

        Store degenerate;
        degenerate.Set("Fog.Mode", static_cast<double>(FogMode::kCustom));
        degenerate.Set("Fog.Distance", 0.0);
        Check(Compose(Row("Fog.Distance"), degenerate.View(), buf, sizeof(buf)), "compose fog 0");
        Expect(buf, "setfog 0 1",
            "a custom distance of 0 is lifted to the documented floor, never emitted as \"0 0\" "
            "(which would mean Dynamic)");
    }

    void SwitchesAndTogglesSpellThemselvesCorrectly()
    {
        Store store;
        char buf[128]{};
        using Platform::CommandComposer::Compose;

        store.Set("Godrays.Enable", 1.0);
        Check(Compose(Row("Godrays.Enable"), store.View(), buf, sizeof(buf)), "compose gr on");
        Expect(buf, "gr on", "a switch spells its state");

        store.Set("Godrays.Enable", 0.0);
        Check(Compose(Row("Godrays.Enable"), store.View(), buf, sizeof(buf)), "compose gr off");
        Expect(buf, "gr off", "and the other way");

        Check(Compose(Row("Scene.Grass"), store.View(), buf, sizeof(buf)), "compose tg");
        Expect(buf, "tg", "an argument-less toggle is just the command word");

        store.Set("Scene.LensFlare", 1.0);
        Check(Compose(Row("Scene.LensFlare"), store.View(), buf, sizeof(buf)), "compose lf on");
        Expect(buf, "lf on", "the promoted lens-flare row spells its state like any switch");

        store.Set("Ssr.Intensity", 2.5);
        Check(Compose(Row("Ssr.Intensity"), store.View(), buf, sizeof(buf)), "compose ssri");
        Expect(buf, "ssri 2.5", "a plain value command takes one argument");

        const Platform::CatalogueRow numericBoolOn{ "Test.NumericBool", "test", "test", nullptr, "",
            Platform::Tier::kConsoleValue, "hbao blurenable", 0U, 0U, 1U,
            Platform::ValueType::kBool, Platform::ValueRange{ 0.0, 1.0 }, 1.0,
            Platform::Evidence::kFieldVerified };
        Check(Compose(numericBoolOn, store.View(), buf, sizeof(buf)), "compose numeric bool on");
        Expect(buf, "hbao blurenable 1", "numeric bools are NUMERIC, never \"on\"");

        const Platform::CatalogueRow numericBoolOff{ "Test.NumericBool", "test", "test", nullptr, "",
            Platform::Tier::kConsoleValue, "hbao blurenable", 0U, 0U, 1U,
            Platform::ValueType::kBool, Platform::ValueRange{ 0.0, 1.0 }, 0.0,
            Platform::Evidence::kFieldVerified };
        Check(Compose(numericBoolOff, store.View(), buf, sizeof(buf)), "compose numeric bool off");
        Expect(buf, "hbao blurenable 0", "and numeric the other way, never \"off\"");
    }

    void OwnedRowsAndSmallBuffersRefuse()
    {
        const Store store;
        char buf[256]{};

        Check(!Platform::CommandComposer::Compose(
                  Row("Godrays.Scale"), store.View(), buf, sizeof(buf)),
            "an owned-address row has no command to send - it is written to memory");
        Expect(buf, "", "a refused compose leaves the buffer empty");

        Check(!Platform::CommandComposer::Compose(
                  Row("Scene.DepthOfField"), store.View(), buf, sizeof(buf)),
            "a kOwnedSetting row has no console command at all - it is written by name");
        Expect(buf, "", "and it leaves nothing behind that could be sent");

        Check(!Platform::CommandComposer::Compose(
                  Row("Fov.PipBoy"), store.View(), buf, sizeof(buf)),
            "a kOwnedModule row must never compose - its command field names its OWNER, not a "
            "command to send");
        Expect(buf, "", "and it leaves nothing behind that could be sent");

        char tiny[8]{};
        Check(!Platform::CommandComposer::Compose(Row("Shp.BloomRadius"), store.View(), tiny, sizeof(tiny)),
            "a buffer too small for all nine arguments refuses");
        Expect(tiny, "",
            "a refused composite leaves NOTHING behind - a partial shp would set eight wrong values");
    }

    void FogPairIsTheOneRuleBothConsumersUse()
    {
        using Platform::CommandComposer::FogMode;
        using Platform::CommandComposer::ResolveFogPair;

        Store dynamic;
        dynamic.Set("Fog.Mode", static_cast<double>(FogMode::kDynamic));
        auto pair = ResolveFogPair(dynamic.View());
        Check(pair.first == 0 && pair.second == 0,
            "Dynamic is the pair (0,0) - the ONLY pair the handler tests for, and the only one "
            "that clears its override byte");

        Store off;
        off.Set("Fog.Mode", static_cast<double>(FogMode::kOff));
        pair = ResolveFogPair(off.View());
        Check(pair.first == pair.second && pair.first != 0,
            "Off is any EQUAL non-zero pair - the static decode proved the handler has no off "
            "mode at all, so this must stay expressed as a pair rather than a flag");

        Store custom;
        custom.Set("Fog.Mode", static_cast<double>(FogMode::kCustom));
        custom.Set("Fog.Distance", 5000.0);
        pair = ResolveFogPair(custom.View());
        Check(pair.first == 0 && pair.second == 5000,
            "Custom pins the FAR distance and leaves near at 0");

        Store degenerate;
        degenerate.Set("Fog.Mode", static_cast<double>(FogMode::kCustom));
        degenerate.Set("Fog.Distance", 0.0);
        pair = ResolveFogPair(degenerate.View());
        Check(pair.first == 0 && pair.second == 1,
            "a custom distance of 0 is lifted to the floor - as a PAIR it would be Dynamic, and "
            "the mode the user picked must survive the numbers");

        char buf[128]{};
        Check(Platform::CommandComposer::Compose(
                  Row("Fog.Distance"), custom.View(), buf, sizeof(buf)),
            "compose custom fog");
        Expect(buf, "setfog 0 5000",
            "the composed command must be exactly the pair FogOwner writes to the engine");
    }

    void EveryCommandFitsTheConsoleCap()
    {
        constexpr std::size_t kMaxCommand = 128U;

        Store extremes;
        Platform::VisitCatalogue([&](const Platform::CatalogueRow& a_row, std::size_t) {
            if (a_row.type != Platform::ValueType::kNone) {
                extremes.Set(a_row.id, a_row.range.max);
            }
        });

        Platform::VisitCatalogue([&](const Platform::CatalogueRow& a_row, std::size_t) {
            if (a_row.tier == Platform::Tier::kOwnedAddress ||
                a_row.tier == Platform::Tier::kOwnedSetting ||
                a_row.tier == Platform::Tier::kOwnedModule) {
                return;
            }
            char buf[512]{};
            if (!Platform::CommandComposer::Compose(a_row, extremes.View(), buf, sizeof(buf))) {
                std::printf("FAIL: [%s] failed to compose at maximum values\n", a_row.id);
                ++g_failures;
                return;
            }
            if (std::strlen(buf) + 1U > kMaxCommand) {
                std::printf("FAIL: [%s] composes to %zu chars, over ConsoleCommand's %zu cap: %s\n",
                    a_row.id, std::strlen(buf), kMaxCommand, buf);
                ++g_failures;
            }
        });
    }
}

int main()
{
    FormattingIsLocaleIndependentAndTidy();
    CompositesSendEveryArgument();
    ChangedValuesPropagate();
    FogModesAreNotDistances();
    FogPairIsTheOneRuleBothConsumersUse();
    SwitchesAndTogglesSpellThemselvesCorrectly();
    OwnedRowsAndSmallBuffersRefuse();
    EveryCommandFitsTheConsoleCap();

    if (g_failures == 0) {
        std::printf("CommandComposerTests: all checks passed\n");
        return 0;
    }
    std::printf("CommandComposerTests: %d check(s) FAILED\n", g_failures);
    return 1;
}
