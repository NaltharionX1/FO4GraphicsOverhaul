#include "Platform/CommandCatalogue.h"

#include <cstdio>
#include <cstring>
#include <string_view>
#include <vector>

namespace
{
    int g_failures = 0;

    void Check(bool a_condition, const char* a_what)
    {
        if (!a_condition) {
            std::printf("FAIL: %s\n", a_what);
            ++g_failures;
        }
    }

    void CheckRow(bool a_condition, const char* a_rowId, const char* a_what)
    {
        if (!a_condition) {
            std::printf("FAIL: [%s] %s\n", a_rowId, a_what);
            ++g_failures;
        }
    }

    [[nodiscard]] bool GroupIsKnown(std::string_view a_group) noexcept
    {
        for (const char* const group : Platform::kCatalogueGroups) {
            if (a_group == group) {
                return true;
            }
        }
        return false;
    }

    void EveryGroupIsRenderable()
    {
        Platform::VisitCatalogue([](const Platform::CatalogueRow& a_row, std::size_t) {
            CheckRow(GroupIsKnown(a_row.group), a_row.id,
                "group is not in kCatalogueGroups - this row would never render");
        });

        for (const char* const group : Platform::kCatalogueGroups) {
            bool found = false;
            Platform::VisitCatalogue([&](const Platform::CatalogueRow& a_row, std::size_t) {
                if (std::string_view(a_row.group) == group) {
                    found = true;
                }
            });
            Check(found, "a declared group has no rows - it would render as an empty sub-tab");
        }
    }

    void RangeContainsDefault()
    {
        Platform::VisitCatalogue([](const Platform::CatalogueRow& a_row, std::size_t) {
            if (a_row.type == Platform::ValueType::kNone) {
                return;
            }
            CheckRow(a_row.vanillaDefault >= a_row.range.min, a_row.id,
                "vanilla default is BELOW its own range minimum");
            CheckRow(a_row.vanillaDefault <= a_row.range.max, a_row.id,
                "vanilla default is ABOVE its own range maximum");
        });
    }

    void RangesAreOrderedAndFinite()
    {
        Platform::VisitCatalogue([](const Platform::CatalogueRow& a_row, std::size_t) {
            if (a_row.type == Platform::ValueType::kNone) {
                CheckRow(a_row.range.min == 0.0 && a_row.range.max == 0.0, a_row.id,
                    "a valueless row must not declare a range");
                return;
            }
            CheckRow(a_row.range.min < a_row.range.max, a_row.id,
                "range is inverted or empty - clamping would collapse the control");
            if (a_row.type == Platform::ValueType::kBool) {
                CheckRow(a_row.range.min == 0.0 && a_row.range.max == 1.0, a_row.id,
                    "a bool row must have range 0..1");
            }
        });
    }

    void IdsAreUniqueAndStable()
    {
        std::vector<std::string_view> seen;
        Platform::VisitCatalogue([&](const Platform::CatalogueRow& a_row, std::size_t) {
            const std::string_view id{ a_row.id };
            CheckRow(!id.empty(), a_row.id, "row id is empty");
            for (const auto& other : seen) {
                CheckRow(other != id, a_row.id, "duplicate row id - persistence would collide");
            }
            seen.push_back(id);

            CheckRow(a_row.label != nullptr && a_row.label[0] != '\0', a_row.id, "row has no label");
            CheckRow(a_row.help != nullptr && a_row.help[0] != '\0', a_row.id, "row has no help text");
        });
    }

    void GlobalIndexRoundTrips()
    {
        Platform::VisitCatalogue([](const Platform::CatalogueRow& a_row, std::size_t a_index) {
            const auto* const byIndex = Platform::RowByGlobalIndex(a_index);
            CheckRow(byIndex == &a_row, a_row.id, "RowByGlobalIndex did not return this row");
            CheckRow(Platform::GlobalIndexForId(a_row.id) == a_index, a_row.id,
                "GlobalIndexForId disagrees with the visit order");
        });

        Check(Platform::RowByGlobalIndex(Platform::kCatalogueRowTotal) == nullptr,
            "RowByGlobalIndex must return nullptr past the end");
        Check(Platform::GlobalIndexForId("no.such.row") == Platform::kCatalogueRowTotal,
            "GlobalIndexForId must report not-found for an unknown id");
    }

    void TierInvariantsHold()
    {
        Platform::VisitCatalogue([](const Platform::CatalogueRow& a_row, std::size_t) {
            switch (a_row.tier) {
            case Platform::Tier::kOwnedAddress:
                CheckRow(a_row.relocationId != 0U, a_row.id,
                    "an owned row needs a relocation id - it is written by address, not by command");
                CheckRow(a_row.command == nullptr, a_row.id,
                    "an owned row must not also carry a console command");
                break;
            case Platform::Tier::kConsoleToggle:
                CheckRow(a_row.type == Platform::ValueType::kNone, a_row.id,
                    "a blind toggle carries no value - it flips, and we cannot read its state");
                CheckRow(!Platform::IsPersistable(a_row.tier), a_row.id,
                    "a blind toggle must never be persisted - we do not know what we would restore");
                [[fallthrough]];
            case Platform::Tier::kConsoleValue:
            case Platform::Tier::kConsoleSwitch:
            case Platform::Tier::kConsoleComposite:
                CheckRow(a_row.command != nullptr && a_row.command[0] != '\0', a_row.id,
                    "a console row needs a command word");
                CheckRow(a_row.relocationId == 0U, a_row.id,
                    "a console row must not carry a relocation id");
                break;
            case Platform::Tier::kOwnedSetting:
                CheckRow(a_row.command != nullptr && a_row.command[0] != '\0', a_row.id,
                    "a kOwnedSetting row needs its Setting key in `command`");
                CheckRow(std::strchr(a_row.command, ':') != nullptr, a_row.id,
                    "a kOwnedSetting row's `command` must be a Setting KEY (name:Section), not a "
                    "console command word");
                CheckRow(a_row.relocationId == 0U, a_row.id,
                    "a kOwnedSetting row is addressed by name - it must not carry a relocation id");
                break;
            case Platform::Tier::kOwnedModule:
                CheckRow(a_row.command != nullptr && std::strlen(a_row.command) > 0U,
                    a_row.id,
                    "a kOwnedModule row must name its owning module in `command`");
                CheckRow(std::strchr(a_row.command, ':') == nullptr, a_row.id,
                    "a kOwnedModule row names a MODULE, not a Setting key - use kOwnedSetting for "
                    "a name-addressed setting");
                CheckRow(a_row.relocationId == 0U, a_row.id,
                    "a kOwnedModule row is driven by code - it must not carry a relocation id");
                break;
            }
        });
    }

    void PolicyFlagsAreCoherent()
    {
        bool sawGrading = false;
        bool sawFov = false;
        Platform::VisitCatalogue([&](const Platform::CatalogueRow& a_row, std::size_t) {
            const bool sessionOnly =
                Platform::HasPolicy(a_row.policy, Platform::Policy::kSessionOnly);
            const bool noSweep =
                Platform::HasPolicy(a_row.policy, Platform::Policy::kNoFrameSweep);

            if (a_row.command != nullptr &&
                (std::strcmp(a_row.command, "scp") == 0 || std::strcmp(a_row.command, "stp") == 0 ||
                    std::strcmp(a_row.command, "shp") == 0 ||
                    std::strcmp(a_row.command, "blm") == 0 ||
                    std::strcmp(a_row.command, "ifx") == 0)) {
                sawGrading = true;
                CheckRow(!sessionOnly, a_row.id,
                    "every grading/effect row persists - kSessionOnly on one is a "
                    "regression to the destructive-write model");
                CheckRow(Platform::ShouldPersist(a_row), a_row.id,
                    "a grading/effect row must be accepted by BOTH the writer and the reader");
            }

            if (a_row.command != nullptr && std::strcmp(a_row.command, "fov") == 0) {
                sawFov = true;
                CheckRow(!sessionOnly, a_row.id,
                    "fov must keep persisting - it shares kConsoleComposite with grading, and that "
                    "is why the session-only rule is per row");
            }

            if (noSweep) {
                CheckRow(a_row.tier == Platform::Tier::kOwnedAddress, a_row.id,
                    "kNoFrameSweep only means something for a kOwnedAddress row - no other tier is "
                    "in EnforceFrame at all");
            }
        });
        Check(sawGrading, "the grading/effect families vanished - the persistence rule now checks "
                          "nothing");
        Check(sawFov, "the fov rows vanished - the must-keep-persisting half of the rule now "
                      "checks nothing");

        Platform::VisitCatalogue([](const Platform::CatalogueRow& a_row, std::size_t) {
            const bool persists = Platform::ShouldPersist(a_row);
            CheckRow(persists ==
                    (Platform::IsPersistable(a_row.tier) &&
                        !Platform::HasPolicy(a_row.policy, Platform::Policy::kSessionOnly)),
                a_row.id, "ShouldPersist must be the ONLY definition of the persistence rule");
        });
    }

    void CompositeArgumentsAreComplete()
    {
        Platform::VisitCatalogue([](const Platform::CatalogueRow& a_row, std::size_t) {
            if (a_row.tier != Platform::Tier::kConsoleComposite) {
                return;
            }
            CheckRow(a_row.argCount >= 2U, a_row.id, "a composite needs at least two arguments");
            CheckRow(a_row.argIndex < a_row.argCount, a_row.id, "argIndex is outside argCount");

            std::size_t siblings = 0;
            bool arityAgrees = true;
            std::vector<bool> slotSeen(a_row.argCount, false);
            Platform::VisitCatalogue([&](const Platform::CatalogueRow& a_other, std::size_t) {
                if (a_other.tier != Platform::Tier::kConsoleComposite ||
                    std::strcmp(a_other.command, a_row.command) != 0) {
                    return;
                }
                ++siblings;
                if (a_other.argCount != a_row.argCount) {
                    arityAgrees = false;
                    return;
                }
                if (a_other.argIndex < slotSeen.size()) {
                    if (slotSeen[a_other.argIndex]) {
                        arityAgrees = false;
                    }
                    slotSeen[a_other.argIndex] = true;
                }
            });

            CheckRow(arityAgrees, a_row.id,
                "composite siblings disagree on argument count, or two rows claim the same slot");
            CheckRow(siblings == a_row.argCount, a_row.id,
                "composite has the wrong number of argument rows for its arity");
            for (const bool seen : slotSeen) {
                CheckRow(seen, a_row.id, "composite is missing one of its argument slots");
            }
        });
    }

    void ShadowAndLodAddressesAreOwnedExactlyOnce()
    {
        static constexpr std::uint64_t kExpected[]{
            777729U,
            123089U, 1023729U, 342335U,
            646264U, 1304050U, 379949U,
            183225U,
            996623U,
        };

        for (const std::uint64_t want : kExpected) {
            std::size_t found = 0;
            Platform::VisitCatalogue([&](const Platform::CatalogueRow& a_row, std::size_t) {
                if (a_row.relocationId != want) {
                    return;
                }
                ++found;
                CheckRow(a_row.tier == Platform::Tier::kOwnedAddress, a_row.id,
                    "a shadow/LOD address row must be kOwnedAddress");
                CheckRow(Platform::HasPolicy(a_row.policy, Platform::Policy::kNoFrameSweep),
                    a_row.id,
                    "shadow/LOD rows stay OUT of the per-frame sweep - nothing in the engine "
                    "rewrites them, so a per-frame re-assert would be unearned cost");
            });
            Check(found == 1,
                "each shadow/LOD relocation id must appear exactly once - two rows on one address "
                "would be two sliders fighting over one value");
        }
    }

    void AoCommandsStayOutOfTheCatalogue()
    {
        Platform::VisitCatalogue([](const Platform::CatalogueRow& a_row, std::size_t) {
            if (a_row.command == nullptr) {
                return;
            }
            const std::string_view command{ a_row.command };
            const bool isBlockedFamily =
                command == "ao" || command == "sao" || command == "hbao";
            CheckRow(!isBlockedFamily, a_row.id,
                "the AO family is blocked at the console layer (the mod owns the switches) - a "
                "catalogue row sending one would make the mod refuse its own command");
        });
    }
}

int main()
{
    EveryGroupIsRenderable();
    RangeContainsDefault();
    RangesAreOrderedAndFinite();
    IdsAreUniqueAndStable();
    GlobalIndexRoundTrips();
    TierInvariantsHold();
    PolicyFlagsAreCoherent();
    CompositeArgumentsAreComplete();
    ShadowAndLodAddressesAreOwnedExactlyOnce();
    AoCommandsStayOutOfTheCatalogue();

    if (g_failures == 0) {
        std::printf("CommandCatalogueTests: all checks passed (%zu rows, %zu groups)\n",
            Platform::kCatalogueRowTotal, std::size(Platform::kCatalogueGroups));
        return 0;
    }
    std::printf("CommandCatalogueTests: %d check(s) FAILED\n", g_failures);
    return 1;
}
