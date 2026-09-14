#include "Platform/NeuralMailbox.h"

#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <vector>

namespace
{
    int g_failures = 0;

    void Check(bool a_condition, const char* a_label)
    {
        if (!a_condition) {
            ++g_failures;
            std::printf("FAIL: %s\n", a_label);
        }
    }

    using namespace Platform;

    struct Counters
    {
        int destroy{ 0 };
        int rearm{ 0 };
        int commits{ 0 };
        NeuralCommitInfo last{};
        NeuralTeardownResult destroyAnswer{ NeuralTeardownResult::kDestroyed };
    };

    NeuralEffects Wire(Counters& a_counters)
    {
        NeuralEffects effects;
        effects.destroyFeature = [&a_counters](std::uint32_t) {
            ++a_counters.destroy;
            return a_counters.destroyAnswer;
        };
        effects.rearmLatch = [&a_counters]() { ++a_counters.rearm; };
        effects.logCommit = [&a_counters](const NeuralCommitInfo& a_info) {
            ++a_counters.commits;
            a_counters.last = a_info;
        };
        return effects;
    }

    void TestIdleAndFirstArm()
    {
        Counters c;
        NeuralMailbox box;
        box.SetEffects(Wire(c));
        NeuralPumpResult r = box.Pump(1);
        Check(r.outcome == NeuralPumpOutcome::kIdle && r.appliedGeneration == 0, "idle before any publish");

        const std::uint64_t g = box.Publish([](NeuralRequest& q) { q.settings.enabled = true; });
        Check(g == 1, "first publish is generation 1");
        r = box.Pump(2);
        Check(r.outcome == NeuralPumpOutcome::kCommitted, "arm commits on the same pump");
        Check(r.scope == kNeuralScopeArm, "scope is arm only");
        Check(c.destroy == 0, "an arm tears nothing down");
        Check(c.commits == 1 && c.last.generation == 1 && c.last.settings.enabled, "logCommit saw the landed snapshot");
        Check(box.Applied().generation == 1 && box.Applied().settings.enabled, "Applied() reflects the commit");
        Check(box.Pump(3).outcome == NeuralPumpOutcome::kIdle, "idle after the commit");
    }

    void TestNoOpRepublishAndNormalisation()
    {
        Counters c;
        NeuralMailbox box;
        box.SetEffects(Wire(c));
        box.Publish([](NeuralRequest& q) { q.settings.enabled = true; });
        (void)box.Pump(1);
        const std::uint64_t g = box.Publish([](NeuralRequest& q) { q.settings.enabled = true; });
        Check(g == 1, "byte-identical republish keeps the generation");
        Check(box.Pump(2).outcome == NeuralPumpOutcome::kIdle, "...and pumps idle");
        const std::uint64_t g2 = box.Publish([](NeuralRequest& q) { q.settings.passes[0].style = 99; });
        Check(g2 == 1, "an out-of-range style normalises to the default and is a no-op");

        box.Publish([](NeuralRequest& q) {
            q.settings.passes[0].intensity = 9.0F;
            q.settings.passes[0].preset = 7;
            q.settings.passes[0].localTone = 0.75F;
        });
        const NeuralRequest p = box.PeekPublished();
        Check(p.settings.passes[0].intensity == 1.0F, "intensity normalised");
        Check(p.settings.passes[0].preset == 0, "preset normalised");
        Check(p.settings.passes[0].localTone == 0.75F, "the in-range value survives");
        Check(p.generation == 2, "one real change = one generation");
    }

    void TestScopesAndTeardown()
    {
        Counters c;
        NeuralMailbox box;
        box.SetEffects(Wire(c));
        box.Publish([](NeuralRequest& q) { q.settings.enabled = true; });
        (void)box.Pump(1);

        box.Publish([](NeuralRequest& q) { q.settings.passes[0].intensity = 0.5F; });
        NeuralPumpResult r = box.Pump(2);
        Check(r.outcome == NeuralPumpOutcome::kCommitted && r.scope == kNeuralScopeLive, "a strength change is live-only");
        Check(c.destroy == 0, "live change tears nothing down");

        box.Publish([](NeuralRequest& q) { q.settings.passes[0].style = 2; });
        r = box.Pump(3);
        Check(r.scope == kNeuralScopeLive, "style change is LIVE");
        Check(c.destroy == 0, "a style change tears nothing down");
        Check(box.TeardownSerial() == 0, "...and advances no teardown serial");

        box.Publish([](NeuralRequest& q) { q.settings.passes[0].preset = 2; });
        r = box.Pump(30);
        Check(r.scope == kNeuralScopeRecreate, "preset change is a recreate");
        Check(c.destroy == 1, "recreate invoked destroyFeature once");
        Check(box.TeardownSerial() == 1, "kDestroyed advanced the teardown serial");

        box.Publish([](NeuralRequest& q) { q.settings.passes[0].preset = 1; q.settings.passes[0].localTone = 0.5F; });
        r = box.Pump(4);
        Check(r.scope == (kNeuralScopeRecreate | kNeuralScopeLive), "preset + strength = recreate and live");
        Check(c.destroy == 2, "second recreate destroyed again");

        box.Publish([](NeuralRequest& q) { ++q.forceRecreateSerial; });
        r = box.Pump(5);
        Check(r.scope == kNeuralScopeRecreate && r.outcome == NeuralPumpOutcome::kCommitted,
            "the force ticket recreates with nothing else changed");
        Check(c.destroy == 3, "force ticket destroyed");
        Check(box.Applied().forceRecreateSerial == 1, "force serial consumed into Applied()");

        c.destroyAnswer = NeuralTeardownResult::kAlreadyAbsent;
        box.Publish([](NeuralRequest& q) { q.settings.enabled = false; });
        r = box.Pump(6);
        Check(r.scope == kNeuralScopeDisarm && r.outcome == NeuralPumpOutcome::kCommitted, "disarm commits");
        Check(c.destroy == 4 && box.TeardownSerial() == 3, "kAlreadyAbsent satisfied the step without a destruction event");

        box.Publish([](NeuralRequest& q) { q.settings.passes[0].intensity = 0.25F; });
        r = box.Pump(7);
        Check(r.outcome == NeuralPumpOutcome::kCommitted && r.scope == kNeuralScopeNone,
            "keys changed while off: scope none, still commits");
        Check(c.destroy == 4, "scope none invoked nothing");

        box.Publish([](NeuralRequest& q) { q.settings.enabled = true; });
        r = box.Pump(8);
        Check(r.scope == kNeuralScopeArm, "re-enable with the same keys is a plain arm");

        c.destroyAnswer = NeuralTeardownResult::kDestroyed;
        const auto destroysBefore = c.destroy;
        box.Publish([](NeuralRequest& q) { q.settings.modelPercent = 75; });
        r = box.Pump(9);
        Check(r.outcome == NeuralPumpOutcome::kCommitted && r.scope == kNeuralScopeRecreate, "a model-extent change is a recreate");
        Check(c.destroy == destroysBefore + 1, "a model-extent change invoked destroyFeature once");
        Check(box.Applied().settings.modelPercent == 75U, "the new extent is applied");
        const std::uint64_t generation = box.Publish([](NeuralRequest& q) { q.settings.modelPercent = 7; });
        r = box.Pump(10);
        Check(box.PeekPublished().settings.modelPercent == 100U, "an out-of-range percent is normalised to 100 at the door");
        Check(r.scope == kNeuralScopeRecreate && box.Applied().settings.modelPercent == 100U, "...and 100 from 75 is a recreate back to the exact path");
        (void)generation;
        const std::uint64_t again = box.Publish([](NeuralRequest& q) { q.settings.modelPercent = 0; });
        Check(again == box.PeekPublished().generation && box.PeekPublished().settings.modelPercent == 100U,
            "another out-of-range value normalises to the same 100: a no-op publish");
        box.Publish([](NeuralRequest& q) { q.settings.modelPercent = 300; });
        r = box.Pump(11);
        Check(r.scope == kNeuralScopeRecreate && box.Applied().settings.modelPercent == 150U,
            "300 with the gate closed lands on the play ceiling (150) at the door: a recreate from 100");
        const auto destroysAtCeiling = c.destroy;
        box.Publish([](NeuralRequest& q) { q.settings.modelBeyondPlay = true; });
        r = box.Pump(12);
        Check(box.Applied().settings.modelBeyondPlay && box.Applied().settings.modelPercent == 150U && c.destroy == destroysAtCeiling,
            "opening the gate at 150 changes nothing in effect: applied, no stage destroyed");
        box.Publish([](NeuralRequest& q) { q.settings.modelPercent = 300; });
        r = box.Pump(13);
        Check(r.scope == kNeuralScopeRecreate && box.Applied().settings.modelPercent == 300U, "with the gate open 300 stands: a recreate");
        box.Publish([](NeuralRequest& q) { q.settings.modelBeyondPlay = false; });
        r = box.Pump(14);
        Check(r.scope == kNeuralScopeRecreate && box.Applied().settings.modelPercent == 150U,
            "closing the gate at 300 is 150 at the door: a recreate");
    }

    void TestFailedTeardownRetriesOnlyTheFailedStep()
    {
        Counters c;
        NeuralMailbox box;
        box.SetEffects(Wire(c));
        box.Publish([](NeuralRequest& q) { q.settings.enabled = true; });
        (void)box.Pump(1);
        c.destroyAnswer = NeuralTeardownResult::kFailed;
        box.Publish([](NeuralRequest& q) { q.settings.passes[0].preset = 1; });
        NeuralPumpResult r = box.Pump(2);
        Check(r.outcome == NeuralPumpOutcome::kCommitFailed, "a failed teardown does not commit");
        Check(box.Applied().generation == 1 && box.Applied().settings.passes[0].preset == 0, "Applied() unchanged after the failure");
        Check(c.commits == 1, "no commit logged for the failure");
        r = box.Pump(3);
        Check(r.outcome == NeuralPumpOutcome::kCommitFailed && c.destroy == 2, "retried on the next pump");
        c.destroyAnswer = NeuralTeardownResult::kDestroyed;
        r = box.Pump(4);
        Check(r.outcome == NeuralPumpOutcome::kCommitted && c.destroy == 3, "the retry succeeds and commits");
        Check(box.Applied().settings.passes[0].preset == 1, "Applied() carries the preset now");
        Check(box.MakeDiagnosticSnapshot().completedTeardownSteps == 0, "step bookkeeping cleared after the commit");
    }

    void TestSupersededFromInsideAnEffect()
    {
        Counters c;
        NeuralMailbox box;
        NeuralEffects effects = Wire(c);
        NeuralMailbox* self = &box;
        effects.destroyFeature = [&c, self](std::uint32_t) {
            ++c.destroy;
            if (c.destroy == 1) {
                self->Publish([](NeuralRequest& q) { q.settings.passes[0].preset = 3; });
            }
            return NeuralTeardownResult::kDestroyed;
        };
        box.SetEffects(std::move(effects));
        box.Publish([](NeuralRequest& q) { q.settings.enabled = true; });
        (void)box.Pump(1);
        box.Publish([](NeuralRequest& q) { q.settings.passes[0].preset = 2; q.settings.passes[0].localTone = 0.5F; });
        NeuralPumpResult r = box.Pump(2);
        Check(r.outcome == NeuralPumpOutcome::kSuperseded, "a publish from inside the effect supersedes");
        Check(box.Applied().generation == 1, "the superseded target never committed");
        r = box.Pump(3);
        Check(r.outcome == NeuralPumpOutcome::kCommitted, "the newest target commits next pump");
        Check(c.destroy == 1, "the succeeded teardown step was carried forward, not re-invoked");
        Check(box.Applied().settings.passes[0].preset == 3 && box.Applied().settings.passes[0].localTone == 0.5F,
            "Applied() is the newest snapshot: the mid-teardown preset AND the outer publish's live value (the nested publish builds on the published snapshot)");
    }

    void TestRetrySerialDeliveredOnce()
    {
        Counters c;
        NeuralMailbox box;
        box.SetEffects(Wire(c));
        box.Publish([](NeuralRequest& q) { q.settings.enabled = true; });
        (void)box.Pump(1);
        box.Publish([](NeuralRequest& q) { ++q.retrySerial; });
        box.Publish([](NeuralRequest& q) { ++q.retrySerial; });
        NeuralPumpResult r = box.Pump(2);
        Check(r.outcome == NeuralPumpOutcome::kCommitted && r.scope == kNeuralScopeRetry, "retry scope");
        Check(c.rearm == 1, "two retry clicks before one pump rearm exactly once");
        Check(c.destroy == 1, "a retry tears the feature down first");
        Check(box.Applied().retrySerial == 2, "the applied serial is the newest");
        r = box.Pump(3);
        Check(r.outcome == NeuralPumpOutcome::kIdle && c.rearm == 1, "not re-delivered");

        c.destroyAnswer = NeuralTeardownResult::kFailed;
        box.Publish([](NeuralRequest& q) { ++q.retrySerial; });
        r = box.Pump(4);
        Check(r.outcome == NeuralPumpOutcome::kCommitFailed && c.rearm == 1, "rearm not delivered on a failed teardown");
        c.destroyAnswer = NeuralTeardownResult::kDestroyed;
        r = box.Pump(5);
        Check(r.outcome == NeuralPumpOutcome::kCommitted && c.rearm == 2, "rearm delivered once the teardown succeeds");
    }

    void TestThrowingEffects()
    {
        Counters c;
        NeuralMailbox box;
        NeuralEffects effects = Wire(c);
        bool throwDestroy = true;
        effects.destroyFeature = [&c, &throwDestroy](std::uint32_t) -> NeuralTeardownResult {
            ++c.destroy;
            if (throwDestroy) {
                throw std::runtime_error("boom");
            }
            return NeuralTeardownResult::kDestroyed;
        };
        effects.logCommit = [&c](const NeuralCommitInfo&) {
            ++c.commits;
            throw std::runtime_error("log boom");
        };
        box.SetEffects(std::move(effects));
        box.Publish([](NeuralRequest& q) { q.settings.enabled = true; });
        NeuralPumpResult r = box.Pump(1);
        Check(r.outcome == NeuralPumpOutcome::kCommitted && c.commits == 1, "a throwing logCommit is swallowed; the arm commits");
        box.Publish([](NeuralRequest& q) { q.settings.passes[0].preset = 1; });
        r = box.Pump(2);
        Check(r.outcome == NeuralPumpOutcome::kCommitFailed, "a throwing destroy is a failed step");
        throwDestroy = false;
        r = box.Pump(3);
        Check(r.outcome == NeuralPumpOutcome::kCommitted && c.destroy == 2, "retried and committed");
    }

    void TestFrameCounterAndSnapshot()
    {
        Counters c;
        NeuralMailbox box;
        box.SetEffects(Wire(c));
        box.Publish([](NeuralRequest& q) { q.settings.enabled = true; });
        NeuralPumpResult r = box.Pump(7);
        Check(r.outcome == NeuralPumpOutcome::kCommitted, "commit on frame 7");
        box.Publish([](NeuralRequest& q) { q.settings.passes[0].preset = 1; });
        r = box.Pump(7);
        Check(r.outcome == NeuralPumpOutcome::kIdle && box.Applied().settings.passes[0].preset == 0,
            "a second pump on the same frame is a no-op");
        NeuralDiagnosticSnapshot d = box.MakeDiagnosticSnapshot();
        Check(d.pending && d.latestGeneration == 2 && d.appliedGeneration == 1, "snapshot shows the pending generation");
        Check(d.pendingScope == kNeuralScopeRecreate, "snapshot names the pending scope");
        Check(d.requested.passes[0].preset == 1 && d.applied.passes[0].preset == 0, "snapshot carries both settings");
        r = box.Pump(8);
        Check(r.outcome == NeuralPumpOutcome::kCommitted, "the next frame commits");
        d = box.MakeDiagnosticSnapshot();
        Check(!d.pending && d.pendingScope == kNeuralScopeNone && d.teardownSerial == 1, "snapshot idle after the commit");

        char name[64]{};
        NeuralMailbox::ScopeName(kNeuralScopeArm | kNeuralScopeRecreate | kNeuralScopeLive, name, sizeof(name));
        Check(std::strcmp(name, "arm+recreate+live") == 0, "scope name composes in order");
        NeuralMailbox::ScopeName(kNeuralScopeNone, name, sizeof(name));
        Check(std::strcmp(name, "none") == 0, "scope none named");
    }

    void TestSupersededFromInsideRearmAndBeforeCommit()
    {
        Counters c;
        NeuralMailbox box;
        NeuralEffects effects = Wire(c);
        NeuralMailbox* self = &box;
        effects.rearmLatch = [&c, self]() {
            ++c.rearm;
            self->Publish([](NeuralRequest& q) { q.settings.passes[0].intensity = 0.25F; });
        };
        box.SetEffects(std::move(effects));
        box.Publish([](NeuralRequest& q) { q.settings.enabled = true; });
        (void)box.Pump(1);
        box.Publish([](NeuralRequest& q) { ++q.retrySerial; });
        NeuralPumpResult r = box.Pump(2);
        Check(r.outcome == NeuralPumpOutcome::kSuperseded, "a publish from inside rearmLatch supersedes (no stale commit)");
        Check(box.Applied().generation == 1 && box.Applied().settings.passes[0].intensity == 1.0F, "the stale target never landed");
        Check(c.commits == 1, "no commit logged for the superseded target");
        r = box.Pump(3);
        Check(r.outcome == NeuralPumpOutcome::kCommitted && box.Applied().settings.passes[0].intensity == 0.25F,
            "the newest target commits next pump");
        Check(c.destroy == 1 && c.rearm == 1, "teardown and rearm were not re-invoked for the carried target");
    }

    void TestFailingDestroyWithRacingPublishIsSuperseded()
    {
        Counters c;
        NeuralMailbox box;
        NeuralEffects effects = Wire(c);
        NeuralMailbox* self = &box;
        effects.destroyFeature = [&c, self](std::uint32_t) {
            ++c.destroy;
            if (c.destroy == 1) {
                self->Publish([](NeuralRequest& q) { q.settings.passes[0].preset = 2; });
                return NeuralTeardownResult::kFailed;
            }
            return NeuralTeardownResult::kDestroyed;
        };
        box.SetEffects(std::move(effects));
        box.Publish([](NeuralRequest& q) { q.settings.enabled = true; });
        (void)box.Pump(1);
        box.Publish([](NeuralRequest& q) { q.settings.passes[0].preset = 1; q.settings.passes[0].localTone = 0.5F; });
        NeuralPumpResult r = box.Pump(2);
        Check(r.outcome == NeuralPumpOutcome::kSuperseded, "a failing destroy with a racing publish reports superseded");
        Check(box.MakeDiagnosticSnapshot().completedTeardownSteps == 0, "a failed step is not carried as completed");
        r = box.Pump(3);
        Check(r.outcome == NeuralPumpOutcome::kCommitted && c.destroy == 2, "the newest target re-runs the (un-succeeded) teardown and commits");
        Check(box.Applied().settings.passes[0].preset == 2 && box.Applied().settings.passes[0].localTone == 0.5F,
            "newest values applied: the racing publish's preset AND the outer publish's live value both survive");
    }

    void TestDamageIsReconstructedNotCancelled()
    {
        Counters c;
        NeuralMailbox box;
        NeuralEffects effects = Wire(c);
        NeuralMailbox* self = &box;
        effects.destroyFeature = [&c, self](std::uint32_t) {
            ++c.destroy;
            if (c.destroy == 1) {
                self->Publish([](NeuralRequest& q) { q.settings.passes[0].preset = 0; });
            }
            return NeuralTeardownResult::kDestroyed;
        };
        box.SetEffects(std::move(effects));
        box.Publish([](NeuralRequest& q) { q.settings.enabled = true; });
        (void)box.Pump(1);
        box.Publish([](NeuralRequest& q) { q.settings.passes[0].preset = 1; });
        NeuralPumpResult r = box.Pump(2);
        Check(r.outcome == NeuralPumpOutcome::kSuperseded && box.Damaged(), "superseded after a real teardown: damaged");
        NeuralDiagnosticSnapshot d = box.MakeDiagnosticSnapshot();
        Check(d.damaged && (d.pendingScope & kNeuralScopeRecreate) != 0, "the snapshot shows the reconstruction pending");
        r = box.Pump(3);
        Check(r.outcome == NeuralPumpOutcome::kCommitted, "the reverted target commits");
        Check((r.scope & kNeuralScopeRecreate) != 0, "...as a RECREATE, not scope-none: the feature must be rebuilt");
        Check(c.destroy == 1, "the carried teardown was not re-invoked");
        Check(!box.Damaged(), "damage cleared by the reconstructing commit");
        Check(c.last.scope == r.scope, "logCommit saw the escalated scope");
    }

    void TestCombinedScopes()
    {
        Counters c;
        NeuralMailbox box;
        box.SetEffects(Wire(c));
        box.Publish([](NeuralRequest& q) { q.settings.enabled = true; q.settings.passes[0].preset = 2; });
        NeuralPumpResult r = box.Pump(1);
        Check(r.scope == (kNeuralScopeArm | kNeuralScopeRecreate), "arm with a different preset = arm + recreate");
        Check(c.destroy == 1, "one idempotent destroy call");
        box.Publish([](NeuralRequest& q) { ++q.forceRecreateSerial; q.settings.passes[0].localTone = 0.5F; });
        r = box.Pump(2);
        Check(r.scope == (kNeuralScopeRecreate | kNeuralScopeLive), "force + live = recreate + live");
        c.destroyAnswer = NeuralTeardownResult::kFailed;
        box.Publish([](NeuralRequest& q) { q.settings.passes[0].preset = 1; });
        r = box.Pump(3);
        Check(r.outcome == NeuralPumpOutcome::kCommitFailed, "recreate stuck");
        box.Publish([](NeuralRequest& q) { q.settings.enabled = false; });
        r = box.Pump(4);
        Check(r.scope == kNeuralScopeDisarm && r.outcome == NeuralPumpOutcome::kCommitFailed, "disarm while stuck still needs the teardown");
        c.destroyAnswer = NeuralTeardownResult::kDestroyed;
        r = box.Pump(5);
        Check(r.outcome == NeuralPumpOutcome::kCommitted && !box.Applied().settings.enabled, "disarm lands once the drain completes");
        box.Publish([](NeuralRequest& q) { q.settings.enabled = true; });
        (void)box.Pump(6);
        c.destroyAnswer = NeuralTeardownResult::kFailed;
        box.Publish([](NeuralRequest& q) { ++q.retrySerial; });
        r = box.Pump(7);
        Check(r.outcome == NeuralPumpOutcome::kCommitFailed && c.rearm == 0, "retry stuck: not rearmed yet");
        box.Publish([](NeuralRequest& q) { q.settings.passes[0].intensity = 0.5F; });
        c.destroyAnswer = NeuralTeardownResult::kDestroyed;
        r = box.Pump(8);
        Check(r.outcome == NeuralPumpOutcome::kCommitted && (r.scope & kNeuralScopeRetry) != 0 && c.rearm == 1,
            "the unconsumed retry rides the later commit and rearms once");
        Check(box.Applied().settings.passes[0].intensity == 0.5F, "the live change landed with it");
        Counters c2;
        NeuralMailbox box2;
        NeuralEffects effects = Wire(c2);
        NeuralMailbox* self = &box2;
        effects.destroyFeature = [&c2, self](std::uint32_t) {
            ++c2.destroy;
            if (c2.destroy == 1) {
                self->Publish([](NeuralRequest& q) { q.settings.passes[0].preset = 1; });
            }
            return NeuralTeardownResult::kDestroyed;
        };
        effects.rearmLatch = [&c2, self]() {
            ++c2.rearm;
            self->Publish([](NeuralRequest& q) { q.settings.passes[0].preset = 2; });
        };
        box2.SetEffects(std::move(effects));
        box2.Publish([](NeuralRequest& q) { q.settings.enabled = true; });
        (void)box2.Pump(1);
        box2.Publish([](NeuralRequest& q) { ++q.retrySerial; });
        r = box2.Pump(2);
        Check(r.outcome == NeuralPumpOutcome::kSuperseded, "first supersession (from destroy)");
        r = box2.Pump(3);
        Check(r.outcome == NeuralPumpOutcome::kSuperseded, "second supersession (from rearm)");
        r = box2.Pump(4);
        Check(r.outcome == NeuralPumpOutcome::kCommitted && box2.Applied().settings.passes[0].preset == 2, "third pump commits the newest");
        Check(c2.destroy == 1 && c2.rearm == 1, "each effect ran exactly once across the chain");
    }

    void TestRemainingClampsThroughPublish()
    {
        NeuralMailbox box;
        box.Publish([](NeuralRequest& q) {
            q.settings.enabled = true;
            q.settings.passes[0].localStructure = 8.0F;
            q.settings.passes[0].skinStructure = -9.0F;
            q.settings.passes[0].preset = 5;
        });
        const NeuralRequest p = box.PeekPublished();
        Check(p.settings.passes[0].localStructure == 1.0F, "localStructure clamped through Publish");
        Check(p.settings.passes[0].skinStructure == 1.0F, "skinStructure clamped through Publish");
        Check(p.settings.passes[0].preset == 0U, "preset clamped through Publish");
        (void)box.Pump(1);
        const std::uint64_t g = box.Publish([](NeuralRequest& q) { q.settings.passes[0].preset = 7; });
        Check(g == 1, "a value that clamps to the current one is a no-op (preset)");
    }

    void TestConcurrentProducersMonotonicGenerations()
    {
        NeuralMailbox box;
        constexpr int kThreads = 4;
        constexpr int kPerThread = 500;
        std::thread producers[kThreads];
        for (int t = 0; t < kThreads; ++t) {
            producers[t] = std::thread([&box, t]() {
                for (int i = 0; i < kPerThread; ++i) {
                    box.Publish([t, i](NeuralRequest& q) {
                        q.settings.enabled = true;
                        q.settings.passes[0].intensity = static_cast<float>((t * kPerThread + i) % 40) * 0.1F;
                    });
                }
            });
        }
        std::uint64_t lastSeen = 0;
        bool monotonic = true;
        for (int i = 0; i < 2000; ++i) {
            const NeuralRequest p = box.PeekPublished();
            if (p.generation < lastSeen) {
                monotonic = false;
            }
            lastSeen = p.generation;
            (void)box.Pump(static_cast<std::uint64_t>(i + 1));
        }
        for (auto& producer : producers) {
            producer.join();
        }
        Check(monotonic, "generations observed by a reader are monotonic under concurrent producers");
        (void)box.Pump(9999);
        const NeuralRequest applied = box.Applied();
        const NeuralRequest published = box.PeekPublished();
        Check(applied.generation == published.generation, "the render side converges to the newest publish");
        Check(applied.settings == published.settings, "Applied() equals the newest snapshot byte for byte");
        Check(published.generation >= 1 && published.generation <= kThreads * kPerThread, "generation count is bounded by the publishes");
    }

    void TestInitialSnapshotIsNormalisedAndAppliedFromTheStart()
    {
        NeuralRequest initial{};
        initial.settings.enabled = true;
        initial.settings.passes[0].style = 5;
        initial.generation = 42;
        NeuralMailbox box(initial);
        Check(box.Applied().generation == 0 && box.Applied().settings.enabled && box.Applied().settings.passes[0].style == 0,
            "the initial snapshot is applied at construction, normalised, at generation 0");
        Check(box.Pump(1).outcome == NeuralPumpOutcome::kIdle, "nothing pending after construction");
    }
}

void TestCascadeTeardownAndHistory()
{
    using namespace Platform;
    NeuralRequest initial{};
    initial.settings.enabled = true;
    initial.settings.passCount = 3;
    NeuralMailbox box(initial);
    std::vector<std::uint32_t> masks;
    NeuralCommitInfo last{};
    NeuralEffects effects{};
    effects.destroyFeature = [&](std::uint32_t mask) {
        masks.push_back(mask);
        return NeuralTeardownResult::kDestroyed;
    };
    effects.logCommit = [&](const NeuralCommitInfo& info) { last = info; };
    box.SetEffects(std::move(effects));
    box.Publish([](NeuralRequest& q) { q.settings.passes[1].style = 2; });
    Check(box.Pump(1).outcome == NeuralPumpOutcome::kCommitted && masks.empty(),
        "pass 2 style applies without any destructive callback");
    Check(last.historyResetMask == 0U, "a live commit carries no history restart (live values blend)");
    box.Publish([](NeuralRequest& q) { q.settings.passes[1].preset = 2; });
    Check(box.Pump(2).outcome == NeuralPumpOutcome::kCommitted,
        "pass 2 preset commits after its own teardown");
    Check(masks.size() == 1 && masks[0] == 2U, "pass 2 edit never destroys pass 1 or pass 3");
    box.Publish([](NeuralRequest& q) { q.settings.passCount = 1; });
    (void)box.Pump(3);
    Check(masks.size() == 2 && masks[1] == 6U, "count reduction retires precisely the removed stages");
    Check(box.Applied().settings.passes[1].preset == 2, "removed-stage configuration remains in the coherent snapshot");
}

void TestCascadeSupersessionRequiresNewStageWork()
{
    using namespace Platform;
    NeuralRequest initial{};
    initial.settings.enabled = true;
    initial.settings.passCount = 3;
    NeuralMailbox box(initial);
    std::vector<std::uint32_t> masks;
    NeuralEffects effects{};
    effects.destroyFeature = [&](std::uint32_t mask) {
        masks.push_back(mask);
        if (masks.size() == 1) {
            box.Publish([](NeuralRequest& q) { q.settings.passes[2].preset = 2; });
        }
        return NeuralTeardownResult::kDestroyed;
    };
    box.SetEffects(std::move(effects));
    box.Publish([](NeuralRequest& q) { q.settings.passes[0].preset = 1; });
    Check(box.Pump(1).outcome == NeuralPumpOutcome::kSuperseded, "stage edit inside teardown supersedes the old target");
    Check(box.Pump(2).outcome == NeuralPumpOutcome::kCommitted, "newest cascade commits after remaining stage work");
    Check(masks.size() == 2 && masks[0] == 1U && masks[1] == 4U,
        "completed pass 1 teardown cannot suppress a newly requested pass 3 teardown");
}

void TestCascadeFailureAndBetweenPumpSupersession()
{
    using namespace Platform;
    NeuralRequest initial{};
    initial.settings.enabled = true;
    initial.settings.passCount = 3;
    NeuralMailbox box(initial);
    bool fail = true;
    std::vector<std::uint32_t> masks;
    NeuralEffects effects{};
    effects.destroyFeature = [&](std::uint32_t mask) {
        masks.push_back(mask);
        return fail ? NeuralTeardownResult::kFailed : NeuralTeardownResult::kDestroyed;
    };
    box.SetEffects(std::move(effects));
    box.Publish([](NeuralRequest& q) { q.settings.passes[1].preset = 2; });
    Check(box.Pump(1).outcome == NeuralPumpOutcome::kCommitFailed &&
              box.Applied().settings.passes[1].preset == 0 &&
              box.MakeDiagnosticSnapshot().completedTeardownSteps == 0,
        "failed pass 2 teardown neither commits new values nor spends the stage step");
    fail = false;
    Check(box.Pump(2).outcome == NeuralPumpOutcome::kCommitted && masks.size() == 2 &&
              masks[0] == 2U && masks[1] == 2U, "only the failed stage is retried");

    NeuralMailbox superseded(initial);
    masks.clear();
    NeuralEffects second{};
    second.destroyFeature = [&](std::uint32_t mask) {
        masks.push_back(mask);
        if (masks.size() == 1) {
            superseded.Publish([](NeuralRequest& q) { q.settings.passes[1].preset = 2; });
        }
        return NeuralTeardownResult::kDestroyed;
    };
    superseded.SetEffects(std::move(second));
    superseded.Publish([](NeuralRequest& q) { q.settings.passes[0].preset = 1; });
    Check(superseded.Pump(1).outcome == NeuralPumpOutcome::kSuperseded, "first callback supersedes stage target");
    superseded.Publish([](NeuralRequest& q) { q.settings.passes[2].preset = 2; });
    Check(superseded.Pump(2).outcome == NeuralPumpOutcome::kCommitted && masks.size() == 2 &&
              masks[0] == 1U && masks[1] == 6U,
        "a publish between pumps preserves completed stages and performs both newly needed stages");
}

int main()
{
    TestIdleAndFirstArm();
    TestNoOpRepublishAndNormalisation();
    TestScopesAndTeardown();
    TestFailedTeardownRetriesOnlyTheFailedStep();
    TestSupersededFromInsideAnEffect();
    TestRetrySerialDeliveredOnce();
    TestThrowingEffects();
    TestFrameCounterAndSnapshot();
    TestInitialSnapshotIsNormalisedAndAppliedFromTheStart();
    TestSupersededFromInsideRearmAndBeforeCommit();
    TestFailingDestroyWithRacingPublishIsSuperseded();
    TestDamageIsReconstructedNotCancelled();
    TestCombinedScopes();
    TestRemainingClampsThroughPublish();
    TestConcurrentProducersMonotonicGenerations();
    TestCascadeTeardownAndHistory();
    TestCascadeSupersessionRequiresNewStageWork();
    TestCascadeFailureAndBetweenPumpSupersession();
    if (g_failures == 0) {
        std::printf("NeuralMailboxTests: all passed\n");
        return 0;
    }
    std::printf("NeuralMailboxTests: %d failure(s)\n", g_failures);
    return 1;
}
