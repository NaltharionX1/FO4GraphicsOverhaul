#include "Platform/AaMailbox.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace
{
    using Platform::AaCommitInfo;
    using Platform::AaAvailability;
    using Platform::AaCapabilityState;
    using Platform::AaDiagnosticSnapshot;
    using Platform::AaEffectiveEngine;
    using Platform::AaEngineCapabilities;
    using Platform::AaEngineRequest;
    using Platform::AaEffects;
    using Platform::AaMailbox;
    using Platform::AaMailboxAllocators;
    using Platform::AaPumpOutcome;
    using Platform::AaRequest;
    using Platform::AaStartupResolutionOutcome;
    using Platform::AaTeardownResult;
    using Platform::kAaDrainFrames;
    using Platform::kAaRecreateDlss;
    using Platform::kAaRecreateBoth;
    using Platform::kAaRecreateFsr;
    using Platform::kAaRecreateNone;

    struct CaseResult
    {
        std::string name;
        int checks{ 0 };
        int failures{ 0 };
        std::vector<std::string> messages;
    };

    CaseResult* g_active{ nullptr };

    void Check(bool condition, std::string_view message)
    {
        if (g_active == nullptr) {
            return;
        }
        ++g_active->checks;
        if (!condition) {
            ++g_active->failures;
            g_active->messages.emplace_back(message);
        }
    }

    struct FakeEffects
    {
        std::atomic<int> freeDlssCalls{ 0 };
        std::atomic<int> destroyFsrContextCalls{ 0 };
        std::atomic<int> rearmFsrLatchCalls{ 0 };
        std::atomic<int> rearmExposureCalls{ 0 };
        std::atomic<int> requestHistoryResetCalls{ 0 };
        std::atomic<int> logCommitCalls{ 0 };
        std::atomic<bool> freeDlssSucceeds{ true };
        std::atomic<bool> destroyFsrSucceeds{ true };

        std::atomic<bool> freeDlssAlreadyAbsent{ false };

        std::function<void()> onFreeDlssBeforeReturn;
        std::function<void()> onRearmFsrLatchBeforeReturn;
        std::function<void()> onRequestHistoryResetBeforeReturn;

        std::atomic<bool> freeDlssThrows{ false };
        std::atomic<bool> destroyFsrContextThrows{ false };
        std::atomic<bool> requestHistoryResetThrows{ false };
        std::atomic<bool> logCommitThrows{ false };

        std::mutex lastCommitMutex;
        std::optional<AaCommitInfo> lastCommit;

        AaEffects Bind()
        {
            AaEffects e;
            e.freeDlss = [this]() -> AaTeardownResult {
                ++freeDlssCalls;
                if (freeDlssThrows.load(std::memory_order_relaxed)) {
                    throw std::runtime_error("FakeEffects: injected freeDlss failure (throw)");
                }
                if (onFreeDlssBeforeReturn) {
                    onFreeDlssBeforeReturn();
                }
                if (freeDlssAlreadyAbsent.load(std::memory_order_relaxed)) {
                    return AaTeardownResult::kAlreadyAbsent;
                }
                return freeDlssSucceeds.load(std::memory_order_relaxed) ? AaTeardownResult::kDestroyed
                                                                         : AaTeardownResult::kFailed;
            };
            e.destroyFsrContext = [this]() -> bool {
                ++destroyFsrContextCalls;
                if (destroyFsrContextThrows.load(std::memory_order_relaxed)) {
                    throw std::runtime_error("FakeEffects: injected destroyFsrContext failure (throw)");
                }
                return destroyFsrSucceeds.load(std::memory_order_relaxed);
            };
            e.rearmFsrLatch = [this]() {
                ++rearmFsrLatchCalls;
                if (onRearmFsrLatchBeforeReturn) {
                    onRearmFsrLatchBeforeReturn();
                }
            };
            e.rearmExposure = [this]() { ++rearmExposureCalls; };
            e.requestHistoryReset = [this]() {
                ++requestHistoryResetCalls;
                if (requestHistoryResetThrows.load(std::memory_order_relaxed)) {
                    throw std::runtime_error("FakeEffects: injected requestHistoryReset failure (throw)");
                }
                if (onRequestHistoryResetBeforeReturn) {
                    onRequestHistoryResetBeforeReturn();
                }
            };
            e.logCommit = [this](const AaCommitInfo& info) {
                ++logCommitCalls;
                if (logCommitThrows.load(std::memory_order_relaxed)) {
                    throw std::runtime_error("FakeEffects: injected logCommit failure (throw)");
                }
                std::lock_guard<std::mutex> lock(lastCommitMutex);
                lastCommit = info;
            };
            return e;
        }
    };

    Platform::PumpResult PumpTimes(AaMailbox& mailbox, std::uint64_t start, int count)
    {
        Platform::PumpResult last{};
        for (int i = 0; i < count; ++i) {
            last = mailbox.Pump(start + static_cast<std::uint64_t>(i));
        }
        return last;
    }

    AaEngineCapabilities ExactCapabilities(AaAvailability dlss, AaAvailability fsr)
    {
        return { { 0xA11U, 1U }, AaCapabilityState::kResolved, dlss, fsr };
    }

    void Case01_RequestedUnappliedThroughDrain()
    {
        AaRequest initial;
        initial.preset = 11;
        AaMailbox mailbox{ initial };
        FakeEffects fake;
        mailbox.SetEffects(fake.Bind());

        mailbox.Publish([](AaRequest& r) { r.preset = 13; });

        PumpTimes(mailbox, 0, kAaDrainFrames - 1);
        Check(mailbox.Applied().preset == 11,
            "mid-drain, eval must still observe the OLD preset (requested value not yet applied)");

        PumpTimes(mailbox, 100, kAaDrainFrames + 4);
        Check(mailbox.Applied().preset == 13,
            "after the drain completes, eval must observe the NEW (published) preset");
        Check(mailbox.Applied().generation == 1,
            "applied generation must reach the published generation once the drain commits");
    }

    void Case02_FinalTickSupersedeZeroFrees()
    {
        AaRequest initial;
        initial.preset = 100;
        AaMailbox mailbox{ initial };
        FakeEffects fake;
        mailbox.SetEffects(fake.Bind());

        mailbox.Publish([](AaRequest& r) { r.preset = 200; });
        PumpTimes(mailbox, 0, kAaDrainFrames - 1);

        mailbox.Publish([](AaRequest& r) { r.preset = 300; });

        bool sawSuperseded = false;
        bool everAppliedB = false;
        for (int i = 0; i < kAaDrainFrames + 2; ++i) {
            const auto result = mailbox.Pump(1000 + static_cast<std::uint64_t>(i));
            if (result.outcome == AaPumpOutcome::kSuperseded) {
                sawSuperseded = true;
            }
            if (mailbox.Applied().preset == 200) {
                everAppliedB = true;
            }
        }
        Check(sawSuperseded,
            "a newer generation arriving before the final tick must report kSuperseded exactly once");
        Check(!everAppliedB, "a superseded request (B) must never be observed as applied");

        PumpTimes(mailbox, 2000, kAaDrainFrames + 2);
        Check(mailbox.Applied().preset == 300,
            "after a supersede, the drain must restart and eventually commit the NEWEST request (C)");
        Check(fake.freeDlssCalls.load() <= 1,
            "supersede-then-commit must free at most once (never once for the aborted B and again for C)");
    }

    void Case03_ABACancelsPremutation()
    {
        AaRequest initial;
        initial.preset = 11;
        AaMailbox mailbox{ initial };
        FakeEffects fake;
        mailbox.SetEffects(fake.Bind());

        mailbox.Publish([](AaRequest& r) { r.preset = 22; });
        PumpTimes(mailbox, 0, 1);

        mailbox.Publish([](AaRequest& r) { r.preset = 11; });

        bool everAppliedB = false;
        for (int i = 0; i < kAaDrainFrames + 4; ++i) {
            (void)mailbox.Pump(10 + static_cast<std::uint64_t>(i));
            if (mailbox.Applied().preset == 22) {
                everAppliedB = true;
            }
        }

        Check(!everAppliedB, "A->B->A must never transiently apply B");
        Check(mailbox.Applied().preset == 11, "A->B->A must settle back on A");
        Check(mailbox.Applied().generation == 2,
            "applied generation must still advance to the LATEST publish (2), even though the net "
            "payload change is zero");
        Check(fake.freeDlssCalls.load() == 0,
            "A->B->A must perform NET ZERO teardowns (the settled value never actually changed)");
    }

    void Case04_DlssOnlyNeverDestroysActiveFsr()
    {
        AaRequest initial;
        initial.effectiveEngine = AaEffectiveEngine::kFsr;
        initial.preset = 11;
        AaMailbox mailbox{ initial };
        FakeEffects fake;
        mailbox.SetEffects(fake.Bind());

        mailbox.Publish([](AaRequest& r) { r.preset = 13; });

        PumpTimes(mailbox, 0, kAaDrainFrames + 4);

        Check(mailbox.Applied().preset == 13, "a DLSS-only preset change must still commit while FSR is active");
        Check(mailbox.Applied().effectiveEngine == AaEffectiveEngine::kFsr,
            "FSR must remain the selected/active engine throughout");
        Check(mailbox.Applied().generation == 1, "the DLSS-only change must reach generation 1");
        Check(fake.destroyFsrContextCalls.load() == 0,
            "a DLSS-only change must NEVER destroy the active FSR context");
    }

    void Case05_SameSelectorOffOnRearmsExactlyOnce()
    {
        AaRequest initial;
        initial.effectiveEngine = AaEffectiveEngine::kFsr;
        initial.dlaaEnabled = true;
        AaMailbox mailbox{ initial };
        FakeEffects fake;
        mailbox.SetEffects(fake.Bind());

        mailbox.Publish([](AaRequest& r) { r.dlaaEnabled = false; });
        PumpTimes(mailbox, 0, kAaDrainFrames + 4);

        mailbox.Publish([](AaRequest& r) { r.dlaaEnabled = true; });
        PumpTimes(mailbox, 100, kAaDrainFrames + 4);

        Check(mailbox.Applied().dlaaEnabled, "the master switch must settle back ON");
        Check(mailbox.Applied().effectiveEngine == AaEffectiveEngine::kFsr,
            "the effective FSR engine must be unchanged by an enable/disable cycle");
        Check(mailbox.Applied().generation == 2, "both the off and the on publish must be applied in turn");
        Check(fake.rearmFsrLatchCalls.load() == 1,
            "a same-selector off->on cycle must re-arm the FSR failure latch EXACTLY ONCE");
    }

    void Case06_SwitchThenOffNeverPublishesStaleActive()
    {
        AaRequest initial;
        initial.effectiveEngine = AaEffectiveEngine::kDlss;
        initial.dlaaEnabled = true;
        AaMailbox mailbox{ initial };
        FakeEffects fake;
        mailbox.SetEffects(fake.Bind());

        mailbox.Publish([](AaRequest& r) { r.effectiveEngine = AaEffectiveEngine::kFsr; });
        PumpTimes(mailbox, 0, 1);

        mailbox.Publish([](AaRequest& r) {
            r.effectiveEngine = AaEffectiveEngine::kFsr;
            r.dlaaEnabled = false;
        });

        PumpTimes(mailbox, 10, kAaDrainFrames + 4);

        Check(fake.logCommitCalls.load() >= 1, "a commit must eventually be logged");
        {
            std::lock_guard<std::mutex> lock(fake.lastCommitMutex);
            Check(fake.lastCommit.has_value(), "logCommit must have been invoked with a snapshot");
            if (fake.lastCommit.has_value()) {
                Check(fake.lastCommit->generation == 2,
                    "logCommit must report the FINAL (latest) generation, never a stale superseded one");
                Check(!fake.lastCommit->dlaaEnabled,
                    "logCommit must reflect the committed dlaaEnabled=false, never a stale 'active' claim");
            }
        }
        Check(mailbox.Applied().generation == 2, "applied must reach the final generation");
        Check(!mailbox.Applied().dlaaEnabled, "applied must reflect the final disabled state");
    }

    void Case07_PumpProgressesWhileDisabled()
    {
        AaRequest initial;
        initial.dlaaEnabled = false;
        initial.effectiveEngine = AaEffectiveEngine::kDlss;
        initial.preset = 11;
        AaMailbox mailbox{ initial };
        FakeEffects fake;
        mailbox.SetEffects(fake.Bind());

        mailbox.Publish([](AaRequest& r) {
            r.preset = 13;
            r.dlaaEnabled = false;
        });

        PumpTimes(mailbox, 0, kAaDrainFrames + 4);

        Check(mailbox.Applied().preset == 13,
            "the drain must still progress to a commit even while both engines are disabled");
        Check(mailbox.Applied().generation == 1, "the drain must commit (generation 1) despite being disabled");
    }

    void Case08_FailedTeardownLeavesAppliedUnchangedAndRetries()
    {
        AaRequest initial;
        initial.preset = 11;
        AaMailbox mailbox{ initial };
        FakeEffects fake;
        fake.freeDlssSucceeds.store(false, std::memory_order_relaxed);
        mailbox.SetEffects(fake.Bind());

        mailbox.Publish([](AaRequest& r) { r.preset = 13; });
        PumpTimes(mailbox, 0, kAaDrainFrames + 4);

        Check(mailbox.Applied().preset == 11, "a failed teardown must leave applied UNCHANGED");
        Check(mailbox.Applied().generation == 0, "a failed commit must not advance the applied generation");
        Check(fake.freeDlssCalls.load() >= 1,
            "the failing teardown effect must actually have been invoked (a retry was attempted)");

        fake.freeDlssSucceeds.store(true, std::memory_order_relaxed);
        PumpTimes(mailbox, 100, kAaDrainFrames + 4);

        Check(mailbox.Applied().preset == 13,
            "once the teardown succeeds, a RETRIED commit must apply the new request");
        Check(mailbox.Applied().generation == 1, "the retried commit must advance the applied generation");
    }

    void Case09_IdlePumpNoUnderflow()
    {
        AaRequest initial;
        initial.preset = 42;
        AaMailbox mailbox{ initial };
        FakeEffects fake;
        mailbox.SetEffects(fake.Bind());

        for (int i = 0; i < 50; ++i) {
            const auto result = mailbox.Pump(static_cast<std::uint64_t>(i));
            Check(result.outcome == AaPumpOutcome::kIdle, "an idle mailbox must report kIdle on every pump");
            Check(mailbox.MakeDiagnosticSnapshot().drainCountdown == 0,
                "an idle drain countdown must never go negative or spuriously arm");
        }

        mailbox.Publish([](AaRequest& r) { r.preset = 99; });
        PumpTimes(mailbox, 1000, kAaDrainFrames + 4);
        Check(mailbox.Applied().preset == 99, "a genuinely staged drain must still commit");
        Check(mailbox.Applied().generation == 1, "...and reach generation 1");

        for (int i = 0; i < 500; ++i) {
            const auto result = mailbox.Pump(2000 + static_cast<std::uint64_t>(i));
            Check(result.outcome == AaPumpOutcome::kIdle,
                "post-commit idle pumps must stay idle, never drift into a phantom re-drain");
            Check(mailbox.MakeDiagnosticSnapshot().drainCountdown == 0,
                "post-commit idle countdown must never underflow negative");
        }
    }

    void Case10_ConcurrentProducersMonotonicGenerations()
    {
        constexpr int kPerThread = 500;
        AaRequest initial;
        AaMailbox mailbox{ initial };
        FakeEffects fake;
        mailbox.SetEffects(fake.Bind());

        std::atomic<bool> stop{ false };
        std::vector<std::uint64_t> samples;
        samples.reserve(1U << 16);

        std::thread producer1([&mailbox]() {
            for (int i = 0; i < kPerThread; ++i) {
                mailbox.Publish([](AaRequest& r) { ++r.fsrRetrySerial; });
            }
        });
        std::thread producer2([&mailbox]() {
            for (int i = 0; i < kPerThread; ++i) {
                mailbox.Publish([](AaRequest& r) { ++r.fsrRetrySerial; });
            }
        });
        std::thread pumper([&mailbox, &stop, &samples]() {
            std::uint64_t frame = 0;
            while (!stop.load(std::memory_order_acquire)) {
                (void)mailbox.Pump(frame++);
                samples.push_back(mailbox.MakeDiagnosticSnapshot().latestGeneration);
            }
        });

        producer1.join();
        producer2.join();
        stop.store(true, std::memory_order_release);
        pumper.join();

        constexpr std::uint64_t kExpectedGeneration = static_cast<std::uint64_t>(2 * kPerThread);
        const auto finalSnapshot = mailbox.MakeDiagnosticSnapshot();

        Check(finalSnapshot.latestGeneration == kExpectedGeneration,
            "every concurrent Publish() must be reflected exactly once in the generation counter "
            "(no lost/duplicate generations under concurrent producers)");
        Check(finalSnapshot.fsrRetrySerial == kExpectedGeneration,
            "concurrent producers incrementing the same serial must never lose an update "
            "(read-modify-publish must be race-free)");

        bool monotonic = true;
        for (std::size_t i = 1; i < samples.size(); ++i) {
            if (samples[i] < samples[i - 1]) {
                monotonic = false;
                break;
            }
        }
        Check(monotonic, "the generation observed by the pumping thread must never appear to go backwards");

        PumpTimes(mailbox, 1000000, kAaDrainFrames + 4);
        Check(mailbox.Applied().generation == finalSnapshot.latestGeneration,
            "after the storm settles, the coordinator must eventually APPLY the last published "
            "generation (no lost update end-to-end, publish -> commit)");
    }

    void Case11_ArchitectureMustUseAtomicSharedPtrPublication()
    {
        static_assert(std::is_same_v<decltype(std::declval<const AaMailbox&>().PeekPublishedNode()),
                          std::shared_ptr<const AaRequest>>,
            "the producer-published request node must be exposed as "
            "std::shared_ptr<const AaRequest> -- see AaMailbox::PeekPublishedNode()");
        static_assert(
            std::is_same_v<decltype(std::declval<const AaMailbox&>().PeekDiagnosticIdentityNode()),
                std::shared_ptr<const AaDiagnosticSnapshot>>,
            "the diagnostic snapshot node must be exposed as "
            "std::shared_ptr<const AaDiagnosticSnapshot> -- see AaMailbox::PeekDiagnosticIdentityNode()");

        Check(Platform::kAaRequestPublicationIsAtomicSharedPtr,
            "request publication must be std::atomic<std::shared_ptr<const AaRequest>> "
            "(CAS/store by producers, acquire-load by the render thread) -- NOT a mutex-serialized "
            "writer over a plain struct read by a seqlock");
        Check(Platform::kAaDiagnosticPublicationIsAtomicSharedPtr,
            "the diagnostic snapshot must use the SAME immutable-node/atomic<shared_ptr> "
            "principle, with the render thread as WRITER and the UI thread as acquire-load READER");

        AaRequest initial;
        initial.preset = 123;
        AaMailbox mailbox{ initial };
        FakeEffects fake;
        mailbox.SetEffects(fake.Bind());

        const std::shared_ptr<const AaRequest> nodeA1 = mailbox.PeekPublishedNode();
        const std::shared_ptr<const AaRequest> nodeA2 = mailbox.PeekPublishedNode();
        Check(nodeA1.get() == nodeA2.get(),
            "behavioral probe: two reads with NO intervening publish must return the SAME node "
            "(pointer identity) -- a fresh allocation on every read would not be an immutable "
            "published-node design even if it happened to return a shared_ptr");

        mailbox.Publish([](AaRequest& r) { r.preset = 456; });
        const std::shared_ptr<const AaRequest> nodeB = mailbox.PeekPublishedNode();
        Check(nodeB.get() != nodeA1.get(),
            "behavioral probe: a publish that actually changes the request must swap in a "
            "genuinely NEW node (different pointer), never mutate the old one in place");
        Check(nodeA1->preset == 123,
            "behavioral probe: the node a reader already held onto must NEVER mutate in place once "
            "a newer node is published -- true immutable-node semantics, not a mutable struct behind "
            "a lock (Case13 exercises this same leg under concurrency)");
        Check(nodeB->preset == 456, "sanity: the newly-published node must reflect the new value");

        const std::shared_ptr<const AaDiagnosticSnapshot> diagA1 = mailbox.PeekDiagnosticIdentityNode();
        const std::shared_ptr<const AaDiagnosticSnapshot> diagA2 = mailbox.PeekDiagnosticIdentityNode();
        Check(diagA1.get() == diagA2.get(),
            "behavioral probe: two diagnostic reads with no intervening render-owned state change "
            "must return the SAME node");

        mailbox.Publish([](AaRequest& r) { r.preset = 789; });
        (void)mailbox.Pump(0);
        const std::shared_ptr<const AaDiagnosticSnapshot> diagB = mailbox.PeekDiagnosticIdentityNode();
        Check(diagB.get() != diagA1.get(),
            "behavioral probe: a real render-owned state change must swap in a genuinely NEW "
            "diagnostic node");
        Check(diagA1->preset == 123,
            "behavioral probe: an old diagnostic node a reader already held must never mutate in "
            "place once a newer one publishes");
    }

    void Case12_ConcurrentMultiFieldPublishInternalConsistency()
    {
        AaRequest initial;
        AaMailbox mailbox{ initial };
        FakeEffects fake;
        mailbox.SetEffects(fake.Bind());

        constexpr int kPublishes = 2000;
        std::atomic<bool> inconsistencyObserved{ false };
        std::atomic<bool> stop{ false };

        std::thread producer([&mailbox]() {
            for (int i = 0; i < kPublishes; ++i) {
                mailbox.Publish([](AaRequest& r) {
                    const auto g = r.generation + 1;
                    r.preset = static_cast<std::uint32_t>(1000 + (g % 50));
                    r.resolutionScale = static_cast<float>(g % 7);
                    r.autoExposure = (g % 2 == 0);
                    r.effectiveEngine = (g % 3 == 0) ? AaEffectiveEngine::kFsr
                                                      : AaEffectiveEngine::kDlss;
                    r.historyResetSerial = g;
                    r.fsrRetrySerial = g;
                    r.exposureRetrySerial = g;
                });
            }
        });

        auto readerFn = [&mailbox, &inconsistencyObserved, &stop]() {
            while (!stop.load(std::memory_order_acquire)) {
                const AaRequest snap = mailbox.PeekPublished();
                const auto g = snap.generation;
                if (g == 0) {
                    continue;
                }
                const bool consistent =
                    snap.preset == static_cast<std::uint32_t>(1000 + (g % 50)) &&
                    snap.resolutionScale == static_cast<float>(g % 7) &&
                    snap.autoExposure == (g % 2 == 0) &&
                    snap.effectiveEngine == ((g % 3 == 0) ? AaEffectiveEngine::kFsr
                                                           : AaEffectiveEngine::kDlss) &&
                    snap.historyResetSerial == g &&
                    snap.fsrRetrySerial == g &&
                    snap.exposureRetrySerial == g;
                if (!consistent) {
                    inconsistencyObserved.store(true, std::memory_order_relaxed);
                }
            }
        };

        std::thread reader1(readerFn);
        std::thread reader2(readerFn);

        producer.join();
        stop.store(true, std::memory_order_release);
        reader1.join();
        reader2.join();

        Check(!inconsistencyObserved.load(),
            "every field of a concurrently-read published snapshot must agree with that snapshot's "
            "OWN generation (no torn cross-field reads) -- see the file banner: a pass here does not "
            "excuse the rejected seqlock/plain-struct design (Case11 is the real RED signal)");
    }

    void Case13_OldSnapshotStaysValidWhileWriterPublishesMany()
    {
        AaRequest initial;
        initial.preset = 7;
        AaMailbox mailbox{ initial };
        FakeEffects fake;
        mailbox.SetEffects(fake.Bind());

        const std::shared_ptr<const AaRequest> oldNode = mailbox.PeekPublishedNode();

        constexpr int kPublishes = 5000;
        std::thread writer([&mailbox]() {
            for (int i = 0; i < kPublishes; ++i) {
                mailbox.Publish([i](AaRequest& r) { r.preset = static_cast<std::uint32_t>(100 + i); });
            }
        });
        writer.join();

        Check(oldNode->preset == 7 && oldNode->generation == 0,
            "a NODE already held by a reader (the actual shared_ptr<const AaRequest> loaded from "
            "the atomic, not a disconnected by-value copy) must stay exactly as it was read -- "
            "immutable, never mutated in place by a later writer");

        const std::shared_ptr<const AaRequest> newNode = mailbox.PeekPublishedNode();
        Check(newNode->generation == static_cast<std::uint64_t>(kPublishes),
            "the writer must have completed every publish (no lost update) despite a reader holding "
            "an old node the whole time -- the reader never blocked the writer's progress");
        Check(newNode.get() != oldNode.get(),
            "the writer's publishes must have produced a genuinely NEW node -- never reused or "
            "mutated the reader's old one in place");
    }

    void Case14_ScopeNoneEnableDisableCommitsWithoutArmingDrain()
    {
        AaRequest initial;
        initial.effectiveEngine = AaEffectiveEngine::kDlss;
        initial.dlaaEnabled = true;
        initial.preset = 11;
        AaMailbox mailbox{ initial };
        FakeEffects fake;
        mailbox.SetEffects(fake.Bind());

        mailbox.Publish([](AaRequest& r) { r.dlaaEnabled = false; });

        const auto result = mailbox.Pump(0);
        Check(result.outcome == AaPumpOutcome::kCommitted,
            "a scope-none (enable/disable) publish must commit on the VERY FIRST pump, never arm the "
            "multi-frame recreate drain");
        Check(!mailbox.Applied().dlaaEnabled,
            "the disable must be visible immediately (same pump it was published on)");
        Check(mailbox.MakeDiagnosticSnapshot().drainCountdown == 0,
            "a scope-none publish must never arm a nonzero drain countdown");
    }

    void Case15_EventOnlyPublishCommitsWithoutArmingDrain()
    {
        AaRequest initial;
        AaMailbox mailbox{ initial };
        FakeEffects fake;
        mailbox.SetEffects(fake.Bind());

        mailbox.Publish([](AaRequest& r) { ++r.historyResetSerial; });

        const auto result = mailbox.Pump(0);
        Check(result.outcome == AaPumpOutcome::kCommitted,
            "an event-only publish (serial bump, no config change) must commit on the very first "
            "pump, never arm the multi-frame drain");
        Check(fake.requestHistoryResetCalls.load() == 1,
            "the history-reset event must have already fired by that immediate commit");
        Check(mailbox.MakeDiagnosticSnapshot().drainCountdown == 0,
            "an event-only publish must never arm a nonzero drain countdown");
    }

    void Case16_NoOpPublishDoesNotAdvanceGeneration()
    {
        AaRequest initial;
        initial.preset = 11;
        AaMailbox mailbox{ initial };
        FakeEffects fake;
        mailbox.SetEffects(fake.Bind());

        const auto genBefore = mailbox.PeekPublished().generation;
        const auto ret = mailbox.Publish([](AaRequest&) {   });

        Check(ret == genBefore,
            "a no-op Publish (builder touches nothing, request stays byte-identical) must NOT "
            "advance the generation");
        Check(mailbox.PeekPublished().generation == genBefore,
            "...and the generation visible to any reader must stay unchanged too");
    }

    void Case17_ForcedRecreateTicketForcesTeardownDespiteIdenticalConfig()
    {
        AaRequest initial;
        initial.effectiveEngine = AaEffectiveEngine::kFsr;
        initial.dlaaEnabled = true;
        initial.preset = 11;
        AaMailbox mailbox{ initial };
        FakeEffects fake;
        mailbox.SetEffects(fake.Bind());

        mailbox.Publish([](AaRequest& r) { ++r.forceRecreateSerial; });
        PumpTimes(mailbox, 0, kAaDrainFrames + 4);

        Check(fake.freeDlssCalls.load() + fake.destroyFsrContextCalls.load() >= 1,
            "a forced-recreate ticket must force at least one REAL teardown call even though the "
            "visible config is byte-identical to what is already applied");
        Check(mailbox.Applied().forceRecreateSerial == 1,
            "the ticket must still be reflected in applied once the (forced) drain commits");
    }

    void Case18_BothEngineTeardownPerStepRetryAndVisibility()
    {
        AaRequest initial;
        initial.effectiveEngine = AaEffectiveEngine::kDlss;
        AaMailbox mailbox{ initial };
        FakeEffects fake;
        fake.destroyFsrSucceeds.store(false, std::memory_order_relaxed);
        mailbox.SetEffects(fake.Bind());

        mailbox.Publish([](AaRequest& r) { r.effectiveEngine = AaEffectiveEngine::kFsr; });
        PumpTimes(mailbox, 0, kAaDrainFrames - 1);

        const auto result1 = mailbox.Pump(1000);
        Check(result1.outcome == AaPumpOutcome::kCommitFailed,
            "a both-engine teardown with a failing second step must report kCommitFailed");
        Check(fake.freeDlssCalls.load() == 1, "the DLSS-free step must succeed on its first attempt");
        Check(fake.destroyFsrContextCalls.load() == 1,
            "the FSR-destroy step must be attempted (and fail) exactly once so far");

        const auto snapAfterFirstFailure = mailbox.MakeDiagnosticSnapshot();
        Check((snapAfterFirstFailure.completedTeardownSteps & kAaRecreateDlss) != 0U,
            "per-step teardown visibility must show the DLSS-free step as ALREADY COMPLETED after a "
            "partial both-engine teardown failure (per-step completion tracking)");

        const auto result2 = mailbox.Pump(1001);
        Check(result2.outcome == AaPumpOutcome::kCommitFailed, "the retry must still fail");
        Check(fake.freeDlssCalls.load() == 1,
            "a retried both-engine teardown must NOT re-invoke the already-SUCCEEDED DLSS-free step");
        Check(fake.destroyFsrContextCalls.load() == 2, "the still-failing FSR-destroy step must retry");

        fake.destroyFsrSucceeds.store(true, std::memory_order_relaxed);
        const auto result3 = mailbox.Pump(1002);
        Check(result3.outcome == AaPumpOutcome::kCommitted,
            "once the remaining step succeeds, the retried commit must complete");
        Check(fake.freeDlssCalls.load() == 1,
            "the final successful retry must still never have re-invoked the completed DLSS-free step");
        Check(fake.destroyFsrContextCalls.load() == 3,
            "the FSR-destroy step's eventually-successful attempt is its third invocation");
        Check(mailbox.Applied().effectiveEngine == AaEffectiveEngine::kFsr,
            "the engine switch must eventually commit");
    }

    void Case19_SecondEngineRepeatedFailurePersistsFirstEngineCompletion()
    {
        AaRequest initial;
        initial.effectiveEngine = AaEffectiveEngine::kDlss;
        AaMailbox mailbox{ initial };
        FakeEffects fake;
        fake.freeDlssSucceeds.store(false, std::memory_order_relaxed);
        fake.destroyFsrSucceeds.store(false, std::memory_order_relaxed);
        mailbox.SetEffects(fake.Bind());

        mailbox.Publish([](AaRequest& r) { r.effectiveEngine = AaEffectiveEngine::kFsr; });
        PumpTimes(mailbox, 0, kAaDrainFrames - 1);

        (void)mailbox.Pump(3000);
        Check(fake.freeDlssCalls.load() == 1 && fake.destroyFsrContextCalls.load() == 0,
            "sanity: while the FIRST step keeps failing, the second step must not be attempted yet");

        (void)mailbox.Pump(3001);
        Check(fake.freeDlssCalls.load() == 2 && fake.destroyFsrContextCalls.load() == 0,
            "sanity: the still-failing first step is correctly retried; second step still unreached");

        fake.freeDlssSucceeds.store(true, std::memory_order_relaxed);
        (void)mailbox.Pump(3002);
        Check(fake.freeDlssCalls.load() == 3 && fake.destroyFsrContextCalls.load() == 1,
            "once the first step succeeds, the second step must be attempted for the first time (and "
            "here fails)");

        (void)mailbox.Pump(3003);
        Check(fake.destroyFsrContextCalls.load() == 2,
            "the still-failing SECOND step must be retried (this is the 'second-engine-failure' case)");
        Check(fake.freeDlssCalls.load() == 3,
            "the ALREADY-SUCCEEDED first step must NOT be re-invoked while only the second step is "
            "still failing (per-step completion tracking)");

        fake.destroyFsrSucceeds.store(true, std::memory_order_relaxed);
        const auto result = mailbox.Pump(3004);
        Check(result.outcome == AaPumpOutcome::kCommitted, "the fifth attempt must finally commit");
        Check(fake.destroyFsrContextCalls.load() == 3, "the second step's eventually-successful attempt");
        Check(fake.freeDlssCalls.load() == 3,
            "the final successful commit must still never have re-invoked the already-completed "
            "first step, across FOUR total retry attempts");
        Check(mailbox.Applied().effectiveEngine == AaEffectiveEngine::kFsr,
            "the engine switch must eventually commit");
    }

    void Case20_TeardownCallbackIdempotencyDefenseInDepth()
    {
        struct FakeResource
        {
            bool freed = false;
            int redundantFreeAttempts = 0;

            bool Free()
            {
                if (freed) {
                    ++redundantFreeAttempts;
                    return true;
                }
                freed = true;
                return true;
            }
        };

        FakeResource dlss;
        FakeResource fsr;
        AaEffects effects;
        effects.freeDlss = [&dlss]() -> AaTeardownResult {
            const bool wasAlreadyFreed = dlss.freed;
            if (!dlss.Free()) {
                return AaTeardownResult::kFailed;
            }
            return wasAlreadyFreed ? AaTeardownResult::kAlreadyAbsent : AaTeardownResult::kDestroyed;
        };
        effects.destroyFsrContext = [&fsr]() { return fsr.Free(); };

        Check(effects.freeDlss() == AaTeardownResult::kDestroyed,
            "first freeDlss call must report a genuine kDestroyed");
        Check(effects.freeDlss() == AaTeardownResult::kAlreadyAbsent,
            "a SECOND freeDlss call (double-invocation) must report kAlreadyAbsent -- a non-failure "
            "result, but NEVER a second kDestroyed (that would masquerade a repeat as a real event)");
        Check(effects.destroyFsrContext(), "first destroyFsrContext call must succeed");
        Check(effects.destroyFsrContext(),
            "a SECOND destroyFsrContext call (double-invocation) must still report success");

        Check(dlss.freed && fsr.freed, "both resources end up freed exactly once logically");
        Check(dlss.redundantFreeAttempts == 1 && fsr.redundantFreeAttempts == 1,
            "the redundant call is observable (for logging/metrics) but never fatal -- idempotency as "
            "defense-in-depth, independent of whether AaMailbox's own per-step tracking is correct");
    }

    void Case21_DuplicateFramePumpDoesNotDoubleAdvanceDrain()
    {
        AaRequest initial;
        initial.preset = 11;
        AaMailbox mailbox{ initial };
        FakeEffects fake;
        mailbox.SetEffects(fake.Bind());

        mailbox.Publish([](AaRequest& r) { r.preset = 13; });

        (void)mailbox.Pump(50);
        const int countdownAfterFrame50 = mailbox.MakeDiagnosticSnapshot().drainCountdown;
        Check(countdownAfterFrame50 == kAaDrainFrames - 1,
            "sanity: exactly one tick of progress after the first pump");

        (void)mailbox.Pump(50);
        const int countdownAfterDuplicate50 = mailbox.MakeDiagnosticSnapshot().drainCountdown;
        Check(countdownAfterDuplicate50 == countdownAfterFrame50,
            "Pump() called twice with the SAME frame id must consume the frame ONCE: the countdown "
            "must not advance a second time for a duplicate frame id");

        (void)mailbox.Pump(51);
        const int countdownAfterFrame51 = mailbox.MakeDiagnosticSnapshot().drainCountdown;
        Check(countdownAfterFrame51 == countdownAfterFrame50 - 1,
            "a subsequent DISTINCT frame id must resume progressing the drain by exactly one more tick");

        Check(mailbox.Applied().preset == 11,
            "the drain must not have committed yet at this point (still mid-drain)");
    }

    void Case22_AutomaticFsrSelectionOffOnRearmsExactlyOnce()
    {
        AaRequest initial;
        initial.effectiveEngine = AaEffectiveEngine::kDlss;
        initial.dlaaEnabled = true;
        AaMailbox mailbox{ initial };
        FakeEffects fake;
        mailbox.SetEffects(fake.Bind());

        mailbox.Publish([](AaRequest& r) { r.effectiveEngine = AaEffectiveEngine::kFsr; });
        PumpTimes(mailbox, 0, kAaDrainFrames + 4);
        Check(mailbox.Applied().effectiveEngine == AaEffectiveEngine::kFsr,
            "the automatic fallback into FSR must actually commit");
        const int rearmsBeforeCycle = fake.rearmFsrLatchCalls.load();

        mailbox.Publish([](AaRequest& r) { r.dlaaEnabled = false; });
        PumpTimes(mailbox, 100, kAaDrainFrames + 4);
        mailbox.Publish([](AaRequest& r) { r.dlaaEnabled = true; });
        PumpTimes(mailbox, 200, kAaDrainFrames + 4);

        Check(mailbox.Applied().dlaaEnabled, "the master switch must settle back ON");
        Check(mailbox.Applied().effectiveEngine == AaEffectiveEngine::kFsr,
            "the automatically selected FSR effective engine must be unchanged");
        const int rearmsAfterCycle = fake.rearmFsrLatchCalls.load();
        Check(rearmsAfterCycle - rearmsBeforeCycle == 1,
            "an off/on cycle on an AUTOMATICALLY-selected (not constructor-preset) FSR engine must "
            "re-arm the failure latch exactly once, isolated from whatever fired on the initial "
            "fallback commit");
    }

    void Case23_FinalTickSupersessionExactZeroAttributableCalls()
    {
        AaRequest initial;
        initial.effectiveEngine = AaEffectiveEngine::kDlss;
        AaMailbox mailbox{ initial };
        FakeEffects fake;
        mailbox.SetEffects(fake.Bind());

        mailbox.Publish([](AaRequest& r) { r.effectiveEngine = AaEffectiveEngine::kFsr; });
        PumpTimes(mailbox, 0, kAaDrainFrames - 1);

        const int freeDlssBefore = fake.freeDlssCalls.load();
        const int destroyFsrBefore = fake.destroyFsrContextCalls.load();

        mailbox.Publish([](AaRequest& r) { r.effectiveEngine = AaEffectiveEngine::kDlss; });

        const auto result = mailbox.Pump(9000);
        Check(result.outcome == AaPumpOutcome::kSuperseded,
            "the immediate next pump after a last-moment supersede must report kSuperseded");
        Check(fake.freeDlssCalls.load() == freeDlssBefore,
            "EXACTLY ZERO additional freeDlss calls may be attributable to the superseded request at "
            "the stale boundary (not merely '<=1 by the end' -- checked at the exact moment)");
        Check(fake.destroyFsrContextCalls.load() == destroyFsrBefore,
            "EXACTLY ZERO additional destroyFsrContext calls may be attributable to the superseded "
            "request at the stale boundary");

        PumpTimes(mailbox, 9500, kAaDrainFrames + 4);
        Check(mailbox.Applied().effectiveEngine == AaEffectiveEngine::kDlss,
            "after the supersede, the drain must restart and eventually commit request C (back to "
            "DLSS)");
    }

    void Case24_SwitchThenOffAppliedSampledEveryPumpNeverStale()
    {
        AaRequest initial;
        initial.effectiveEngine = AaEffectiveEngine::kDlss;
        initial.dlaaEnabled = true;
        AaMailbox mailbox{ initial };
        FakeEffects fake;
        mailbox.SetEffects(fake.Bind());

        mailbox.Publish([](AaRequest& r) { r.effectiveEngine = AaEffectiveEngine::kFsr; });
        PumpTimes(mailbox, 0, 1);

        mailbox.Publish([](AaRequest& r) {
            r.effectiveEngine = AaEffectiveEngine::kFsr;
            r.dlaaEnabled = false;
        });

        bool everStale = false;
        for (int i = 0; i < kAaDrainFrames + 4; ++i) {
            (void)mailbox.Pump(10 + static_cast<std::uint64_t>(i));

            const AaRequest applied = mailbox.Applied();
            const bool isOriginal = applied.effectiveEngine == AaEffectiveEngine::kDlss && applied.dlaaEnabled;
            const bool isFinal = applied.effectiveEngine == AaEffectiveEngine::kFsr && !applied.dlaaEnabled;
            if (!isOriginal && !isFinal) {
                everStale = true;
            }

            const auto diag = mailbox.MakeDiagnosticSnapshot();
            const bool diagOriginal = diag.effectiveEngine == AaEffectiveEngine::kDlss && diag.dlaaEnabled;
            const bool diagFinal = diag.effectiveEngine == AaEffectiveEngine::kFsr && !diag.dlaaEnabled;
            if (!diagOriginal && !diagFinal) {
                everStale = true;
            }
        }

        Check(!everStale,
            "Applied() and the diagnostic snapshot must NEVER show the stale intermediate "
            "'effective FSR, enabled' target at ANY intermediate pump -- only the original "
            "pre-switch state or the eventual final committed state");
        Check(mailbox.Applied().effectiveEngine == AaEffectiveEngine::kFsr && !mailbox.Applied().dlaaEnabled,
            "the drain must eventually settle on the FINAL (latest) request");
    }

    void Case25a_EventSerialCoherenceOverConcurrentMixedPublishes()
    {
        AaRequest initial;
        initial.effectiveEngine = AaEffectiveEngine::kFsr;
        AaMailbox mailbox{ initial };
        FakeEffects fake;
        mailbox.SetEffects(fake.Bind());

        constexpr int kPerProducer = 1500;
        std::atomic<bool> inconsistencyObserved{ false };
        std::atomic<bool> stop{ false };

        std::thread producerA([&mailbox]() {
            for (int i = 0; i < kPerProducer; ++i) {
                mailbox.Publish([](AaRequest& r) {
                    const auto g = r.generation + 1;
                    r.preset = static_cast<std::uint32_t>(3000 + (g % 20));
                    r.resolutionScale = static_cast<float>(3000 + (g % 20));
                });
            }
        });
        std::thread producerB([&mailbox]() {
            for (int i = 0; i < kPerProducer; ++i) {
                mailbox.Publish([](AaRequest& r) { ++r.fsrRetrySerial; });
            }
        });

        auto readerFn = [&mailbox, &inconsistencyObserved, &stop]() {
            while (!stop.load(std::memory_order_acquire)) {
                const AaRequest snap = mailbox.PeekPublished();
                if (snap.preset != 11 &&
                    static_cast<float>(snap.preset) != snap.resolutionScale) {
                    inconsistencyObserved.store(true, std::memory_order_relaxed);
                }
            }
        };
        std::thread reader1(readerFn);
        std::thread reader2(readerFn);

        producerA.join();
        producerB.join();
        stop.store(true, std::memory_order_release);
        reader1.join();
        reader2.join();

        Check(!inconsistencyObserved.load(),
            "preset and resolutionScale are published TOGETHER by the same builder call and must "
            "never be observed torn relative to each other, even while a second producer concurrently "
            "bumps an unrelated serial on every publish");

        const auto finalSerial = mailbox.PeekPublished().fsrRetrySerial;
        Check(finalSerial == static_cast<std::uint64_t>(kPerProducer),
            "the FINAL published fsrRetrySerial must equal the total number of concurrent "
            "increments -- no lost updates across producerB's CAS-retry publish loop");

        PumpTimes(mailbox, 5000000, kAaDrainFrames + 4);
        Check(fake.rearmFsrLatchCalls.load() == 1,
            "settling after the storm must consume the final fsrRetrySerial and fire its "
            "associated notification (rearmFsrLatch) EXACTLY ONCE -- never once per intermediate bump "
            "(there were 1500 of them before this settle), never lost, never double-fired");
    }

    void Case25b_FailedCommitRetainsEventForLaterSuccessfulRetry()
    {
        AaRequest initial;
        initial.effectiveEngine = AaEffectiveEngine::kFsr;
        initial.dlaaEnabled = true;
        initial.preset = 11;
        AaMailbox mailbox{ initial };
        FakeEffects fake;
        fake.freeDlssSucceeds.store(false, std::memory_order_relaxed);
        mailbox.SetEffects(fake.Bind());

        mailbox.Publish([](AaRequest& r) {
            r.preset = 13;
            ++r.fsrRetrySerial;
        });
        PumpTimes(mailbox, 0, kAaDrainFrames + 4);

        Check(mailbox.Applied().preset == 11, "the failed commit must leave applied unchanged");
        Check(fake.rearmFsrLatchCalls.load() == 0,
            "the retry-event effect must NOT have fired yet -- the commit that would consume it never "
            "succeeded");

        fake.freeDlssSucceeds.store(true, std::memory_order_relaxed);
        PumpTimes(mailbox, 100, kAaDrainFrames + 4);

        Check(mailbox.Applied().preset == 13, "the retried commit must now apply the new preset");
        Check(fake.rearmFsrLatchCalls.load() == 1,
            "the retained retry-event must fire EXACTLY ONCE on the eventually-successful retry -- "
            "never lost by the earlier failure(s)");
    }

    void Case27_ConcurrentDiagnosticWriterReadersConsistency()
    {
        AaRequest initial;
        AaMailbox mailbox{ initial };
        FakeEffects fake;
        mailbox.SetEffects(fake.Bind());

        constexpr int kPerProducer = 800;
        std::atomic<bool> stopRender{ false };
        std::atomic<bool> inconsistencyObserved{ false };

        std::thread producer1([&mailbox]() {
            for (int i = 0; i < kPerProducer; ++i) {
                mailbox.Publish([](AaRequest& r) {
                    const auto g = r.generation + 1;
                    r.preset = static_cast<std::uint32_t>(2000 + (g % 40));
                    r.resolutionScale = static_cast<float>(g % 5);
                });
            }
        });
        std::thread producer2([&mailbox]() {
            for (int i = 0; i < kPerProducer; ++i) {
                mailbox.Publish([](AaRequest& r) { ++r.historyResetSerial; });
            }
        });

        std::thread renderThread([&mailbox, &stopRender]() {
            std::uint64_t frame = 0;
            while (!stopRender.load(std::memory_order_acquire)) {
                (void)mailbox.Pump(frame++);
            }
        });

        auto readerFn = [&mailbox, &stopRender, &inconsistencyObserved]() {
            while (!stopRender.load(std::memory_order_acquire)) {
                const auto snap = mailbox.MakeDiagnosticSnapshot();
                if (snap.latestGeneration < snap.appliedGeneration) {
                    inconsistencyObserved.store(true, std::memory_order_relaxed);
                }
                const bool presetPlausible = snap.preset == 11 ||
                    (snap.preset >= 2000 && snap.preset < 2040);
                if (!presetPlausible) {
                    inconsistencyObserved.store(true, std::memory_order_relaxed);
                }
            }
        };
        std::thread reader1(readerFn);
        std::thread reader2(readerFn);

        producer1.join();
        producer2.join();
        stopRender.store(true, std::memory_order_release);
        renderThread.join();
        reader1.join();
        reader2.join();

        Check(!inconsistencyObserved.load(),
            "concurrent diagnostic writer (render thread's Pump()) and readers "
            "(MakeDiagnosticSnapshot()) must never observe latestGeneration < appliedGeneration, nor "
            "a torn/implausible config field -- blockers [A]/[F5]: same nondeterministic-RED caveat "
            "as Case11/12 applies (a pass here does not excuse the seqlock/plain-struct diagnostic "
            "design; Case11 is the real RED signal)");

        PumpTimes(mailbox, 1000000, kAaDrainFrames + 4);
        Check(mailbox.Applied().generation == mailbox.PeekPublished().generation,
            "after the storm settles, the coordinator must have applied the last published generation");
    }

    void Case28_PartialTeardownRetargetToOldAppliedStaysDamaged()
    {
        AaRequest initial;
        initial.effectiveEngine = AaEffectiveEngine::kDlss;
        AaMailbox mailbox{ initial };
        FakeEffects fake;
        fake.destroyFsrSucceeds.store(false, std::memory_order_relaxed);
        mailbox.SetEffects(fake.Bind());

        mailbox.Publish([](AaRequest& r) { r.effectiveEngine = AaEffectiveEngine::kFsr; });
        PumpTimes(mailbox, 0, kAaDrainFrames - 1);

        (void)mailbox.Pump(2000);
        Check(fake.freeDlssCalls.load() == 1 && fake.destroyFsrContextCalls.load() == 1,
            "sanity: the DLSS-free step succeeded once and the FSR-destroy step failed once -- the "
            "same partial-teardown-failure precondition Case18 already covers");
        Check(mailbox.Applied().effectiveEngine == AaEffectiveEngine::kDlss,
            "sanity: the failed commit leaves applied at A (DLSS)");

        mailbox.Publish([](AaRequest& r) { r.effectiveEngine = AaEffectiveEngine::kDlss; });

        PumpTimes(mailbox, 3000, kAaDrainFrames + 4);

        Check(mailbox.Applied().effectiveEngine == AaEffectiveEngine::kDlss,
            "the cancel-back-to-A request still eventually settles on A");
        Check(fake.freeDlssCalls.load() == 1,
            "the already-succeeded DLSS-free step must never be re-invoked by the cancel-back-to-A "
            "retarget (a repeated successful free is exactly what the teardown contract forbids)");
        Check(fake.destroyFsrContextCalls.load() == 1,
            "a scope-none cancel must not itself force an extra retry of the still-failed FSR-destroy "
            "step (that retry cadence is a separate concern -- the point here is the DAMAGED "
            "bookkeeping, not an immediate re-attempt)");

        Check((mailbox.MakeDiagnosticSnapshot().damagedScope & kAaRecreateDlss) != 0U,
            "once the DLSS-free step ACTUALLY SUCCEEDED as part of a both-engine "
            "teardown attempt that was later abandoned (retargeted back to the OLD applied "
            "configuration before its OWN commit ever landed), the coordinator must retain that as a "
            "DAMAGED/teardown-completed scope -- 'cancel back to A' is NOT a free no-op once real "
            "destructive work already ran; A's DLSS resources are already gone and evaluation must "
            "stay fail-closed for that scope until an explicit reconstruction");

        PumpTimes(mailbox, 9000, 20);
        Check((mailbox.MakeDiagnosticSnapshot().damagedScope & kAaRecreateDlss) != 0U,
            "the damaged-scope flag must persist across ordinary idle pumps -- only an explicit "
            "reconstruction of that scope may clear it, never the mere passage of idle frames");
    }

    void Case29_DamagedScopeClearsOnlyAfterGenuineReconstruction()
    {
        AaRequest initial;
        initial.effectiveEngine = AaEffectiveEngine::kDlss;
        AaMailbox mailbox{ initial };
        FakeEffects fake;
        fake.destroyFsrSucceeds.store(false, std::memory_order_relaxed);
        mailbox.SetEffects(fake.Bind());

        mailbox.Publish([](AaRequest& r) { r.effectiveEngine = AaEffectiveEngine::kFsr; });
        PumpTimes(mailbox, 0, kAaDrainFrames - 1);
        (void)mailbox.Pump(4000);

        mailbox.Publish([](AaRequest& r) { r.effectiveEngine = AaEffectiveEngine::kDlss; });
        PumpTimes(mailbox, 5000, kAaDrainFrames + 4);

        Check((mailbox.MakeDiagnosticSnapshot().damagedScope & kAaRecreateDlss) != 0U,
            "precondition (same scenario as Case28): the cancel-back-to-A retarget must leave the "
            "DLSS scope flagged damaged");

        mailbox.Publish([](AaRequest& r) { r.preset = 77; });
        PumpTimes(mailbox, 6000, kAaDrainFrames + 4);

        Check(mailbox.Applied().preset == 77, "the ordinary reconstruction commit must succeed");
        Check(fake.destroyFsrContextCalls.load() == 1,
            "sanity: the FSR-destroy step is still never re-invoked by this DLSS-only reconstruction");
        Check((mailbox.MakeDiagnosticSnapshot().damagedScope & kAaRecreateDlss) == 0U,
            "once the DLSS scope's teardown/recreate genuinely runs to a real, "
            "non-abandoned commit, the damaged flag for that scope must clear -- reconstruction, not "
            "merely the passage of time, is what restores fail-closed evaluation to normal");
    }

    void Case30_PublishDuringTeardownCallbackRereadsBeforePublishingApplied()
    {
        AaRequest initial;
        initial.preset = 11;
        AaMailbox mailbox{ initial };
        FakeEffects fake;

        bool injected = false;
        fake.onFreeDlssBeforeReturn = [&mailbox, &injected]() {
            if (!injected) {
                injected = true;
                mailbox.Publish([](AaRequest& r) {
                    r.preset = 15;
                    ++r.historyResetSerial;
                });
            }
        };
        mailbox.SetEffects(fake.Bind());

        mailbox.Publish([](AaRequest& r) { r.preset = 13; });
        PumpTimes(mailbox, 0, kAaDrainFrames - 1);

        (void)mailbox.Pump(500);

        Check(mailbox.Applied().preset == 11,
            "a publish landing DURING the teardown callback makes the target STALE -- "
            "Applied() must NOT show B's stale preset (13); this commit must be withheld exactly like "
            "a supersede (applied stays at the pre-drain value) until the NEWEST generation (C) "
            "actually commits");
        Check(fake.logCommitCalls.load() == 0,
            "the stale target (B) must NOT be logged via logCommit just because its own "
            "teardown succeeded -- REREAD-BEFORE-PUBLISH must catch the publish-during-callback and "
            "withhold the stale commit");

        PumpTimes(mailbox, 1000, kAaDrainFrames + 4);

        Check(mailbox.Applied().preset == 15 && mailbox.Applied().generation == 2,
            "the coordinator must eventually retarget to and commit the NEWEST generation (C), never "
            "getting stuck on the withheld stale one");
        Check(fake.freeDlssCalls.load() == 1,
            "the DLSS-free step already ACTUALLY SUCCEEDED servicing this logical "
            "attempt -- completed-step accounting must carry over into the retargeted drain so it is "
            "NEVER re-invoked, even though the request that consumes it changed identity (B -> C) "
            "before publish");
        Check(fake.logCommitCalls.load() == 1,
            "exactly ONE commit may ever be logged for this whole sequence -- the real one (C) -- "
            "never the withheld stale one (B)");
        Check(fake.requestHistoryResetCalls.load() == 1,
            "the history-reset event bundled into the publish-during-callback (C) must still fire "
            "exactly once on the eventual real commit -- retained, never silently dropped by the "
            "withheld stale intermediate attempt");
    }

    void Case31_PublishDuringFirstBothTeardownSkipsStaleSecond()
    {
        AaRequest initial;
        initial.effectiveEngine = AaEffectiveEngine::kDlss;
        AaMailbox mailbox{ initial };
        FakeEffects fake;

        bool injected = false;
        fake.onFreeDlssBeforeReturn = [&mailbox, &injected]() {
            if (!injected) {
                injected = true;
                mailbox.Publish([](AaRequest& r) { r.effectiveEngine = AaEffectiveEngine::kDlss; });
            }
        };
        mailbox.SetEffects(fake.Bind());

        mailbox.Publish([](AaRequest& r) { r.effectiveEngine = AaEffectiveEngine::kFsr; });
        PumpTimes(mailbox, 0, kAaDrainFrames - 1);
        const auto result = mailbox.Pump(700);

        Check(result.outcome == AaPumpOutcome::kSuperseded,
            "publish from the first BOTH teardown callback must supersede B before another stale "
            "destructive callback runs");
        Check(fake.freeDlssCalls.load() == 1,
            "the first destructive step must run exactly once before its callback publishes C");
        Check(fake.destroyFsrContextCalls.load() == 0,
            "after the first callback publishes a scope-none target, the now-unneeded stale FSR "
            "teardown must be skipped (re-read after each external destructive callback)");
        Check(mailbox.Applied().generation == 0 &&
                mailbox.Applied().effectiveEngine == AaEffectiveEngine::kDlss,
            "the stale switch target B must never become Applied");
        const auto diag = mailbox.MakeDiagnosticSnapshot();
        Check((diag.damagedScope & kAaRecreateDlss) != 0U,
            "the DLSS step that really completed before retarget must remain damaged/fail-closed");
        Check((diag.damagedScope & kAaRecreateFsr) == 0U,
            "the FSR scope must not be marked damaged when its now-unauthorized callback never ran");
    }

    void Case32_FsrToDlssRearmsExposureExactlyOnce()
    {
        AaRequest initial;
        initial.effectiveEngine = AaEffectiveEngine::kFsr;
        initial.dlaaEnabled = true;
        initial.autoExposure = true;
        AaMailbox mailbox{ initial };
        FakeEffects fake;
        mailbox.SetEffects(fake.Bind());

        mailbox.Publish([](AaRequest& r) { r.effectiveEngine = AaEffectiveEngine::kDlss; });
        PumpTimes(mailbox, 0, kAaDrainFrames + 4);

        Check(mailbox.Applied().effectiveEngine == AaEffectiveEngine::kDlss &&
                mailbox.Applied().generation == 1,
            "the FSR-to-DLSS switch must commit");
        Check(fake.rearmExposureCalls.load() == 1,
            "FSR-to-DLSS makes auto exposure newly active and must re-arm its latch exactly once");
    }

    void Case33_PublishDuringNotificationRereadsBeforePublishingApplied()
    {
        AaRequest initial;
        initial.dlaaEnabled = false;
        AaMailbox mailbox{ initial };
        FakeEffects fake;

        bool injected = false;
        fake.onRequestHistoryResetBeforeReturn = [&mailbox, &injected]() {
            if (!injected) {
                injected = true;
                mailbox.Publish([](AaRequest& r) { r.dlaaEnabled = false; });
            }
        };
        mailbox.SetEffects(fake.Bind());

        mailbox.Publish([](AaRequest& r) { r.dlaaEnabled = true; });
        const auto result = mailbox.Pump(0);

        Check(result.outcome == AaPumpOutcome::kSuperseded,
            "a publish from a pre-commit notification must supersede the stale target in the same Pump");
        Check(mailbox.Applied().generation == 0 && !mailbox.Applied().dlaaEnabled,
            "the stale enable target must not become Applied after its notification publishes C");
        Check(fake.logCommitCalls.load() == 0,
            "the stale enable target must not be logged after a notification publishes C");
        Check(fake.requestHistoryResetCalls.load() == 1,
            "the notification callback must have run exactly once to create the deterministic race");

        (void)mailbox.Pump(1);
        Check(mailbox.Applied().generation == 2 && !mailbox.Applied().dlaaEnabled,
            "the newest cancel target C must commit on the next scope-none Pump without exposing B");
    }

    void Case34_ThrowingDestructiveCallbackTreatedAsStepFailureNoTerminate()
    {
        AaRequest initial;
        initial.preset = 11;
        AaMailbox mailbox{ initial };
        FakeEffects fake;
        fake.freeDlssThrows.store(true, std::memory_order_relaxed);
        mailbox.SetEffects(fake.Bind());

        mailbox.Publish([](AaRequest& r) { r.preset = 13; });
        const auto result = PumpTimes(mailbox, 0, kAaDrainFrames + 4);

        Check(result.outcome == AaPumpOutcome::kCommitFailed,
            "a throwing destructive callback must be treated exactly like a `false` return: the step "
            "FAILS (kCommitFailed) -- Pump() must never std::terminate/crash despite being noexcept");
        Check(mailbox.Applied().preset == 11, "a thrown-during-teardown commit must leave applied UNCHANGED");
        Check(mailbox.Applied().generation == 0, "a thrown commit must not advance the applied generation");
        Check(fake.freeDlssCalls.load() >= 1,
            "the throwing step must actually have been invoked (a retry was attempted)");

        fake.freeDlssThrows.store(false, std::memory_order_relaxed);
        PumpTimes(mailbox, 100, kAaDrainFrames + 4);

        Check(mailbox.Applied().preset == 13,
            "once the callback stops throwing, a RETRIED commit must apply the new request -- "
            "identical retry semantics to Case08's ordinary `false`-return failure");
        Check(mailbox.Applied().generation == 1, "the retried commit must advance the applied generation");
    }

    void Case35_ThrowingNoncriticalCallbackIsFailSafeCommitProceeds()
    {
        AaRequest initial;
        AaMailbox mailbox{ initial };
        FakeEffects fake;
        fake.requestHistoryResetThrows.store(true, std::memory_order_relaxed);
        mailbox.SetEffects(fake.Bind());

        mailbox.Publish([](AaRequest& r) { ++r.historyResetSerial; });
        const auto result = mailbox.Pump(0);

        Check(result.outcome == AaPumpOutcome::kCommitted,
            "a throwing NON-CRITICAL notification callback must be fail-safe: the commit proceeds "
            "(kCommitted), never aborted/retried on account of the notification alone, and Pump() "
            "never terminates despite being noexcept");
        Check(fake.requestHistoryResetCalls.load() == 1,
            "the throwing notification must still have been invoked exactly once (observable), even "
            "though it threw");
        Check(mailbox.Applied().historyResetSerial == 1,
            "the commit itself must have gone through unaffected by the thrown notification");
    }

    void Case38_ThrowingSecondDestructiveStepInBothEngineDrainRetriesOnlyThatStep()
    {
        AaRequest initial;
        initial.effectiveEngine = AaEffectiveEngine::kDlss;
        AaMailbox mailbox{ initial };
        FakeEffects fake;
        fake.destroyFsrContextThrows.store(true, std::memory_order_relaxed);
        mailbox.SetEffects(fake.Bind());

        mailbox.Publish([](AaRequest& r) { r.effectiveEngine = AaEffectiveEngine::kFsr; });
        PumpTimes(mailbox, 0, kAaDrainFrames - 1);

        const auto result1 = mailbox.Pump(2000);
        Check(result1.outcome == AaPumpOutcome::kCommitFailed,
            "a throwing second destructive step must be treated exactly like a `false` return: "
            "kCommitFailed, no terminate despite Pump() being noexcept");
        Check(fake.freeDlssCalls.load() == 1, "the DLSS-free step must have succeeded once");
        Check(fake.destroyFsrContextCalls.load() == 1,
            "the throwing FSR-destroy step must actually have been invoked (and be retried)");
        Check(mailbox.Applied().generation == 0,
            "the thrown-during-teardown commit must leave applied UNCHANGED");

        fake.destroyFsrContextThrows.store(false, std::memory_order_relaxed);
        const auto result2 = mailbox.Pump(2001);
        Check(result2.outcome == AaPumpOutcome::kCommitted,
            "once the callback stops throwing and succeeds, the retried commit must complete");
        Check(fake.freeDlssCalls.load() == 1,
            "the already-succeeded DLSS-free step must NEVER have been re-invoked across the "
            "throw-then-retry sequence -- identical per-step-completion semantics to an ordinary "
            "`false`-return retry (Case18/19)");
        Check(fake.destroyFsrContextCalls.load() == 2, "the FSR-destroy step's eventually-successful retry");
        Check(mailbox.Applied().effectiveEngine == AaEffectiveEngine::kFsr,
            "the engine switch must eventually commit");
    }

    void Case39_ThrowingLogCommitIsFailSafeCommitAlreadyLanded()
    {
        AaRequest initial;
        initial.preset = 11;
        AaMailbox mailbox{ initial };
        FakeEffects fake;
        fake.logCommitThrows.store(true, std::memory_order_relaxed);
        mailbox.SetEffects(fake.Bind());

        mailbox.Publish([](AaRequest& r) { r.preset = 13; });
        PumpTimes(mailbox, 0, kAaDrainFrames + 4);

        Check(mailbox.Applied().preset == 13 && mailbox.Applied().generation == 1,
            "a throwing logCommit must be fail-safe: applied_ is stored BEFORE logCommit runs, so the "
            "commit must have landed regardless of logCommit throwing afterward -- never undone, never "
            "retried on the notification's account, and Pump() never terminates despite being noexcept");
        Check(fake.logCommitCalls.load() == 1,
            "the throwing logCommit must still have been invoked exactly once (observable) even though "
            "it threw");
        Check(!fake.lastCommit.has_value(),
            "sanity: logCommit threw BEFORE recording lastCommit -- proving the throw happened inside "
            "the real callback body (not merely that the callback was skipped)");
    }

    void Case40_PublishDuringFirstNotificationSkipsStaleLaterNotifications()
    {
        AaRequest initial;
        initial.dlaaEnabled = false;
        initial.effectiveEngine = AaEffectiveEngine::kFsr;
        AaMailbox mailbox{ initial };
        FakeEffects fake;

        bool injected = false;
        fake.onRearmFsrLatchBeforeReturn = [&mailbox, &injected]() {
            if (!injected) {
                injected = true;
                mailbox.Publish([](AaRequest& r) { r.mipBias = -0.5F; });
            }
        };
        mailbox.SetEffects(fake.Bind());

        mailbox.Publish([](AaRequest& r) {
            ++r.fsrRetrySerial;
            ++r.exposureRetrySerial;
            ++r.historyResetSerial;
        });

        const auto superseded = mailbox.Pump(0);
        Check(superseded.outcome == AaPumpOutcome::kSuperseded,
            "a publish from the first notification must supersede the stale target in the same Pump");
        Check(fake.rearmFsrLatchCalls.load() == 1,
            "the first notification must run once before its deterministic supersede");
        Check(fake.rearmExposureCalls.load() == 0 && fake.requestHistoryResetCalls.load() == 0,
            "later notifications computed for the stale target must not run after the first callback publishes C");

        const auto committed = mailbox.Pump(1);
        Check(committed.outcome == AaPumpOutcome::kCommitted,
            "the newest scope-none request must commit on the next Pump");
        Check(fake.rearmFsrLatchCalls.load() == 1,
            "a notification already delivered before retarget must carry forward and not fire twice");
        Check(fake.rearmExposureCalls.load() == 0 && fake.requestHistoryResetCalls.load() == 1,
            "history skipped for stale B must be delivered once for retained FSR target C, while the "
            "DLSS-only exposure retry remains suppressed and pending");
    }

    void Case41_EventSerialDeliveredExactlyOnceAcrossNotificationSupersede()
    {
        AaRequest initial;
        AaMailbox mailbox{ initial };
        FakeEffects fake;

        bool injected = false;
        fake.onRequestHistoryResetBeforeReturn = [&mailbox, &injected]() {
            if (!injected) {
                injected = true;
                mailbox.Publish([](AaRequest& r) { ++r.exposureRetrySerial; });
            }
        };
        mailbox.SetEffects(fake.Bind());

        mailbox.Publish([](AaRequest& r) { ++r.historyResetSerial; });
        const auto superseded = mailbox.Pump(0);
        Check(superseded.outcome == AaPumpOutcome::kSuperseded,
            "history callback publication must supersede the withheld event-only target");
        Check(fake.requestHistoryResetCalls.load() == 1,
            "the retained history serial must be delivered once before retarget");

        const auto committed = mailbox.Pump(1);
        Check(committed.outcome == AaPumpOutcome::kCommitted,
            "the retarget retaining the delivered history serial must commit immediately");
        Check(fake.rearmExposureCalls.load() == 1,
            "the genuinely new exposure serial on C must still be delivered");
        Check(fake.requestHistoryResetCalls.load() == 1,
            "the already-delivered history serial must not fire again when C commits");
    }

    void Case36_RenderOnlyDamagedScopeAccessorIndependentOfDiagnosticSnapshot()
    {
        AaRequest initial;
        initial.effectiveEngine = AaEffectiveEngine::kDlss;
        AaMailbox mailbox{ initial };
        FakeEffects fake;
        fake.destroyFsrSucceeds.store(false, std::memory_order_relaxed);
        mailbox.SetEffects(fake.Bind());

        Check(mailbox.DamagedScope() == kAaRecreateNone,
            "sanity: a freshly-constructed mailbox has no damaged scope");
        Check((mailbox.DamagedScope() & (kAaRecreateDlss | kAaRecreateFsr)) == 0U,
            "sanity: neither scope is damaged before any teardown ever runs");

        mailbox.Publish([](AaRequest& r) { r.effectiveEngine = AaEffectiveEngine::kFsr; });
        PumpTimes(mailbox, 0, kAaDrainFrames - 1);
        (void)mailbox.Pump(8000);

        mailbox.Publish([](AaRequest& r) { r.effectiveEngine = AaEffectiveEngine::kDlss; });
        PumpTimes(mailbox, 8500, kAaDrainFrames + 4);

        Check((mailbox.DamagedScope() & kAaRecreateDlss) != 0U,
            "the render-only accessor must reflect the damaged DLSS scope IMMEDIATELY -- reading "
            "damagedScope_ directly, with zero dependency on a diagnostic-snapshot allocation/publish "
            "ever having succeeded");
        Check((mailbox.DamagedScope() & kAaRecreateFsr) == 0U,
            "the FSR scope must not be reported damaged -- its destroy genuinely failed, but a plain "
            "failure (never having SUCCEEDED) is not the same as 'succeeded then abandoned'");
    }

    void Case37_DiagnosticSnapshotPublishesOnStateChangeOnlyNotEveryIdleFrame()
    {
        AaRequest initial;
        initial.preset = 11;
        AaMailbox mailbox{ initial };
        FakeEffects fake;
        mailbox.SetEffects(fake.Bind());

        const auto nodeAfterConstruction = mailbox.PeekDiagnosticIdentityNode();
        Check(nodeAfterConstruction != nullptr, "sanity: construction publishes an initial diagnostic node");

        for (int i = 0; i < 20; ++i) {
            const auto result = mailbox.Pump(static_cast<std::uint64_t>(i));
            Check(result.outcome == AaPumpOutcome::kIdle, "sanity: every one of these pumps is genuinely idle");
            const auto nodeThisTick = mailbox.PeekDiagnosticIdentityNode();
            Check(nodeThisTick == nodeAfterConstruction,
                "an idle Pump() (no render-owned state change) must NOT allocate/publish a new "
                "diagnostic node -- the SAME node (pointer identity) must still be published after any "
                "number of consecutive idle pumps");
        }

        mailbox.Publish([](AaRequest& r) { r.preset = 99; });
        PumpTimes(mailbox, 100, kAaDrainFrames + 4);

        const auto nodeAfterRealChange = mailbox.PeekDiagnosticIdentityNode();
        Check(nodeAfterRealChange != nodeAfterConstruction,
            "once a real state change actually commits, a NEW diagnostic node (different pointer/"
            "identity) must be published -- never coalesced away");
        Check(mailbox.Applied().preset == 99, "sanity: the real change actually committed");

        for (int i = 0; i < 20; ++i) {
            (void)mailbox.Pump(1000 + static_cast<std::uint64_t>(i));
            Check(mailbox.PeekDiagnosticIdentityNode() == nodeAfterRealChange,
                "idle pumps after a real change must keep publishing the SAME node -- no further "
                "allocation until the NEXT real state change");
        }
    }

    void Case42_ConstructorMustNotBeNoexcept()
    {
        Check(!noexcept(AaMailbox(AaRequest{})),
            "AaMailbox's constructor must NOT be declared noexcept -- the contract requires a "
            "construction-time allocation failure to be able to propagate out (abort construction "
            "cleanly) instead of being forced to swallow it internally and silently complete with "
            "broken state");
    }

    void Case43_DiagnosticAllocationFailureAbortsConstructionEntirely()
    {
        int requestAllocatorCalls = 0;
        long requestOwnersDuringDiagnosticAllocator = 0;
        std::shared_ptr<const AaRequest> capturedRequestNode;
        AaMailboxAllocators allocators;
        allocators.makeRequestNode = [&requestAllocatorCalls,
                                          &capturedRequestNode](const AaRequest& r) {
            ++requestAllocatorCalls;
            capturedRequestNode = std::make_shared<const AaRequest>(r);
            return capturedRequestNode;
        };
        allocators.makeDiagnosticNode = [&capturedRequestNode,
                                            &requestOwnersDuringDiagnosticAllocator](
                                            const AaDiagnosticSnapshot&)
            -> std::shared_ptr<const AaDiagnosticSnapshot> {
            requestOwnersDuringDiagnosticAllocator = capturedRequestNode.use_count();
            throw std::runtime_error("test-injected diagnostic-node allocation failure");
        };

        AaRequest initial;
        initial.preset = 42;

        bool threw = false;
        std::string whatMessage;
        try {
            [[maybe_unused]] AaMailbox shouldNotConstruct{ initial, allocators };
        } catch (const std::exception& ex) {
            threw = true;
            whatMessage = ex.what();
        }

        Check(threw,
            "an allocation failure on EITHER initial node must abort construction "
            "entirely (propagate an exception out of the constructor) -- never complete "
            "construction with the OTHER node published successfully while this one is silently "
            "left null, and never std::terminate despite the throw (the constructor is no longer "
            "noexcept)");
        Check(whatMessage.find("diagnostic-node") != std::string::npos,
            "sanity: the propagated exception must be the one thrown by the rigged diagnostic-node "
            "allocator, not some unrelated failure masking the real one");

        Check(requestAllocatorCalls == 1,
            "sanity: the request-node allocator (which succeeds) must have run exactly once before "
            "the diagnostic-node allocator's throw aborted construction");
        Check(requestOwnersDuringDiagnosticAllocator == 2,
            "while the diagnostic allocator is running, exactly the constructor-local request node "
            "and this test's capture must own the request object; an extra copied publication owner "
            "would make this count larger");
        Check(capturedRequestNode != nullptr && capturedRequestNode.use_count() == 1,
            "after constructor unwind, only this test's capture may remain; this proves cleanup/no "
            "leaked mailbox ownership, while the constructor's audited source order proves neither "
            "atomic store is reached before both validated locals exist");

        AaRequest secondInitial;
        secondInitial.preset = 77;
        AaMailbox recovered{ secondInitial };
        Check(recovered.HasPublishedRequestNode() && recovered.PeekPublishedNode() != nullptr,
            "N1(i): a subsequent, ordinary construction after the earlier aborted attempt must "
            "succeed cleanly and expose a non-null published request node -- the earlier abort must "
            "leave no corrupting state behind");
        Check(recovered.PeekDiagnosticIdentityNode() != nullptr,
            "N1(i): ...and a non-null diagnostic node too (this construction uses the real default "
            "diagnostic allocator, never rigged in this second attempt)");
        Check(recovered.Applied().preset == 77,
            "sanity: the recovered mailbox reflects its own initial request");
    }

    void Case44_RequestAllocationFailureAbortsConstructionEntirely()
    {
        int diagnosticAllocatorCalls = 0;
        AaMailboxAllocators allocators;
        allocators.makeRequestNode = [](const AaRequest&) -> std::shared_ptr<const AaRequest> {
            throw std::runtime_error("test-injected request-node allocation failure");
        };
        allocators.makeDiagnosticNode = [&diagnosticAllocatorCalls](const AaDiagnosticSnapshot& s)
            -> std::shared_ptr<const AaDiagnosticSnapshot> {
            ++diagnosticAllocatorCalls;
            return std::make_shared<const AaDiagnosticSnapshot>(s);
        };

        AaRequest initial;
        initial.preset = 42;

        bool threw = false;
        std::string whatMessage;
        try {
            [[maybe_unused]] AaMailbox shouldNotConstruct{ initial, allocators };
        } catch (const std::exception& ex) {
            threw = true;
            whatMessage = ex.what();
        }

        Check(threw,
            "an allocation failure on EITHER initial node must abort construction "
            "entirely -- never complete construction with the OTHER node published successfully "
            "while this one is silently left null, and never std::terminate despite the throw");
        Check(whatMessage.find("request-node") != std::string::npos,
            "sanity: the propagated exception must be the one thrown by the rigged request-node "
            "allocator, not some unrelated failure masking the real one");

        Check(diagnosticAllocatorCalls == 0,
            "N1(i): the diagnostic-node allocator must NEVER be invoked when the (textually-first) "
            "request-node allocator throws -- construction must abort immediately, never partially "
            "proceeding to build (let alone store/publish) the other node");

        AaRequest secondInitial;
        secondInitial.preset = 88;
        AaMailbox recovered{ secondInitial };
        Check(recovered.HasPublishedRequestNode() && recovered.PeekPublishedNode() != nullptr,
            "N1(i): a subsequent, ordinary construction after the earlier aborted attempt must "
            "succeed cleanly and expose a non-null published request node");
        Check(recovered.PeekDiagnosticIdentityNode() != nullptr,
            "N1(i): ...and expose a non-null diagnostic node too");
        Check(recovered.Applied().preset == 88,
            "sanity: the recovered mailbox reflects its own initial request");
    }

    void Case45_BiasFieldsAreNormalizedAndClampedInAppliedState()
    {
        AaRequest initial;
        AaMailbox mailbox{ initial };
        FakeEffects fake;
        mailbox.SetEffects(fake.Bind());

        mailbox.Publish([](AaRequest& r) {
            r.mipBias = -50.0F;
            r.fsrMipBias = 25.0F;
        });
        const auto result = mailbox.Pump(0);

        Check(result.outcome == AaPumpOutcome::kCommitted,
            "sanity: a bias-only change must commit on the very first Pump (no create-time config "
            "touched)");
        Check(mailbox.Applied().mipBias >= -3.0F && mailbox.Applied().mipBias <= 0.0F,
            "mipBias must be normalized/clamped to [-3, 0] in the applied state -- an "
            "out-of-range published value must never reach Applied() verbatim");
        Check(mailbox.Applied().fsrMipBias >= -3.0F && mailbox.Applied().fsrMipBias <= 0.0F,
            "fsrMipBias must be normalized/clamped to [-3, 0] in the applied state -- "
            "an out-of-range published value must never reach Applied() verbatim");

        mailbox.Publish([](AaRequest& r) { r.mipBias = std::numeric_limits<float>::quiet_NaN(); });
        (void)mailbox.Pump(1);
        Check(std::isfinite(mailbox.Applied().mipBias),
            "a non-finite mipBias publish must normalize to a finite value (mirrors "
            "DlaaSettings::ClampMipBias treating non-finite input as 0), never propagate NaN into "
            "the applied state");
    }

    void Case46_BiasOnlyChangeCommitsAsScopeNoneWithoutArmingDrain()
    {
        AaRequest initial;
        initial.effectiveEngine = AaEffectiveEngine::kFsr;
        AaMailbox mailbox{ initial };
        FakeEffects fake;
        mailbox.SetEffects(fake.Bind());

        mailbox.Publish([](AaRequest& r) { r.mipBias = -10.0F; });
        const auto result = mailbox.Pump(0);

        Check(result.outcome == AaPumpOutcome::kCommitted,
            "a bias-only change must commit on the SAME Pump it was published on -- it must never "
            "arm the multi-frame recreate drain (the scope-none contract extended to "
            "bias)");
        Check(fake.freeDlssCalls.load() == 0 && fake.destroyFsrContextCalls.load() == 0,
            "a bias-only change must never invoke either teardown callback -- it is a live, "
            "create-time-resource-free tweak, not a recreate");
        Check(mailbox.Applied().generation == 1,
            "sanity: the bias publish must still be recognized as a real change and reach "
            "generation 1");
        Check(mailbox.Applied().effectiveEngine == AaEffectiveEngine::kFsr,
            "sanity: the unrelated effective engine must be carried forward untouched");

        Check(mailbox.Applied().mipBias == -3.0F,
            "the committed/applied mipBias must be the CLAMPED value (-3.0, the edge "
            "of [-3, 0]) -- not the out-of-range raw published value (-10.0) stored verbatim");
    }

    void Case47_IdenticalBiasAfterClampingIsAGenerationNoOp()
    {
        AaRequest initial;
        AaMailbox mailbox{ initial };
        FakeEffects fake;
        mailbox.SetEffects(fake.Bind());

        mailbox.Publish([](AaRequest& r) { r.mipBias = -10.0F; });
        (void)mailbox.Pump(0);
        Check(mailbox.Applied().generation == 1,
            "sanity: the first (out-of-range) bias publish is a real change");

        const std::uint64_t generationBeforeSecondPublish = mailbox.PeekPublished().generation;
        mailbox.Publish([](AaRequest& r) { r.mipBias = -3.0F; });
        const auto result = mailbox.Pump(1);

        Check(mailbox.PeekPublished().generation == generationBeforeSecondPublish,
            "republishing the value the prior publish should have clamped down to "
            "must be recognized as a byte-identical no-op (generation must not advance) -- "
            "comparison must happen AFTER normalization, not against the raw unclamped value");
        Check(result.outcome == AaPumpOutcome::kIdle,
            "a true no-op publish must leave the coordinator idle on the next Pump -- nothing to "
            "drain or commit");
        Check(fake.logCommitCalls.load() == 1,
            "logCommit must have fired exactly once total (for the first, genuine bias change) -- "
            "a true no-op publish must never trigger a second commit/log");
    }

    void Case48_BiasChangeDuringActiveDrainRetargetsWithoutDisturbingDrainScope()
    {
        AaRequest initial;
        initial.preset = 11;
        AaMailbox mailbox{ initial };
        FakeEffects fake;
        mailbox.SetEffects(fake.Bind());

        mailbox.Publish([](AaRequest& r) { r.preset = 13; });
        PumpTimes(mailbox, 0, kAaDrainFrames - 1);
        Check(mailbox.Applied().preset == 11, "sanity: mid-drain, the preset change is not yet applied");

        mailbox.Publish([](AaRequest& r) { r.mipBias = -10.0F; });

        bool sawSuperseded = false;
        for (int i = kAaDrainFrames - 1; i < 2 * kAaDrainFrames; ++i) {
            const auto result = mailbox.Pump(static_cast<std::uint64_t>(i));
            if (result.outcome == AaPumpOutcome::kSuperseded) {
                sawSuperseded = true;
            }
        }

        Check(sawSuperseded,
            "a bias change published mid-drain must retarget the drain (a newer generation exists)");
        Check(mailbox.Applied().preset == 13,
            "the drain must still commit the ORIGINAL scope-driving field (preset 13) once "
            "retargeted");
        Check(fake.freeDlssCalls.load() == 1,
            "layering a bias-only change onto an in-flight DLSS-scope drain must NOT "
            "disturb that drain's scope -- freeDlss must fire exactly once (never twice; the next "
            "check confirms destroyFsrContext never fires at all)");
        Check(fake.destroyFsrContextCalls.load() == 0,
            "the bias-only addition must never widen the in-flight scope to include "
            "FSR");

        Check(mailbox.Applied().mipBias == -3.0F,
            "the committed/applied mipBias must be the CLAMPED value (-3.0) even when "
            "it rode along inside a retargeted, non-scope-none drain -- not the out-of-range raw "
            "published value (-10.0) stored verbatim");
    }

    void Case49_FreeDlssDestroyedThenSupersedingPublishAdvancesSerialOnceFsrNotInvoked()
    {
        AaRequest initial;
        initial.effectiveEngine = AaEffectiveEngine::kDlss;
        AaMailbox mailbox{ initial };
        FakeEffects fake;

        bool injected = false;
        fake.onFreeDlssBeforeReturn = [&mailbox, &injected]() {
            if (!injected) {
                injected = true;
                mailbox.Publish([](AaRequest& r) { r.effectiveEngine = AaEffectiveEngine::kDlss; });
            }
        };
        mailbox.SetEffects(fake.Bind());

        mailbox.Publish([](AaRequest& r) { r.effectiveEngine = AaEffectiveEngine::kFsr; });
        PumpTimes(mailbox, 0, kAaDrainFrames - 1);
        const auto result = mailbox.Pump(700);

        Check(result.outcome == AaPumpOutcome::kSuperseded,
            "sanity (mirrors Case31): publish from the first BOTH teardown callback must supersede B");
        Check(fake.freeDlssCalls.load() == 1, "sanity: the DLSS step ran exactly once before the supersede");
        Check(fake.destroyFsrContextCalls.load() == 0,
            "L4(a): destroyFsrContext for the stale (superseded) target must NEVER be invoked -- the "
            "post-callback reread discovers staleness before the FSR step is ever reached");
        Check(mailbox.DlssTeardownSerial() == 1,
            "L4(a): freeDlss reporting kDestroyed must advance the serial EXACTLY ONCE, even though a "
            "producer published a superseding request from inside that very callback -- the increment "
            "happens BEFORE the post-callback reread, so the real, physical destruction that already "
            "happened is correctly counted regardless of what the reread later discovers");

        (void)mailbox.Pump(701);
        Check(mailbox.Applied().generation == 2 &&
                mailbox.Applied().effectiveEngine == AaEffectiveEngine::kDlss,
            "sanity: the retargeted scope-none commit (back to A) must land");
        Check(mailbox.DlssTeardownSerial() == 1,
            "L4(a): the serial must NOT advance again merely because the retargeted (unrelated, "
            "scope-none) commit landed afterward");
    }

    void Case50_BothScopeOnlyDlssDestroysSerialOnceFsrStepAloneRetries()
    {
        AaRequest initial;
        initial.effectiveEngine = AaEffectiveEngine::kDlss;
        AaMailbox mailbox{ initial };
        FakeEffects fake;
        fake.destroyFsrSucceeds.store(false, std::memory_order_relaxed);
        mailbox.SetEffects(fake.Bind());

        mailbox.Publish([](AaRequest& r) { r.effectiveEngine = AaEffectiveEngine::kFsr; });
        PumpTimes(mailbox, 0, kAaDrainFrames - 1);

        const auto result1 = mailbox.Pump(1000);
        Check(result1.outcome == AaPumpOutcome::kCommitFailed,
            "sanity (mirrors Case18): a both-engine teardown with a failing second step must report "
            "kCommitFailed");
        Check(fake.freeDlssCalls.load() == 1 && fake.destroyFsrContextCalls.load() == 1,
            "sanity: DLSS succeeded once, FSR failed once");
        Check(mailbox.DlssTeardownSerial() == 1,
            "L4(b): the serial must advance EXACTLY ONCE at the moment DLSS genuinely destroys, even "
            "though the OVERALL commit this tick fails on the sibling FSR step");
        Check((mailbox.MakeDiagnosticSnapshot().completedTeardownSteps & kAaRecreateDlss) != 0U,
            "sanity: the DLSS step must be tracked as already-completed");

        const auto result2 = mailbox.Pump(1001);
        Check(result2.outcome == AaPumpOutcome::kCommitFailed, "the retry must still fail");
        Check(fake.freeDlssCalls.load() == 1,
            "L4(b): a retry of a both-engine teardown must NEVER re-invoke the already-succeeded DLSS "
            "step -- only the still-failing FSR step retries");
        Check(fake.destroyFsrContextCalls.load() == 2, "the still-failing FSR step must have retried");
        Check(mailbox.DlssTeardownSerial() == 1,
            "L4(b): the serial must stay at exactly 1 across the FSR-only retry -- no re-invocation of "
            "freeDlss means no possibility of a re-increment");

        fake.destroyFsrSucceeds.store(true, std::memory_order_relaxed);
        const auto result3 = mailbox.Pump(1002);
        Check(result3.outcome == AaPumpOutcome::kCommitted,
            "once the remaining step succeeds, the retried commit must complete");
        Check(fake.freeDlssCalls.load() == 1,
            "sanity: the eventual successful commit still never re-invoked the completed DLSS step");
        Check(mailbox.DlssTeardownSerial() == 1,
            "L4(b)/(e): the serial must remain exactly 1 through the eventual successful commit -- the "
            "commit event itself is never what drives the serial, only the genuine freeDlss callback "
            "result is");
    }

    void Case51_FreeDlssFailedAndThrownBothLeaveSerialZeroStepPending()
    {
        AaRequest initial;
        initial.preset = 11;
        AaMailbox mailbox{ initial };
        FakeEffects fake;
        fake.freeDlssSucceeds.store(false, std::memory_order_relaxed);
        mailbox.SetEffects(fake.Bind());

        mailbox.Publish([](AaRequest& r) { r.preset = 13; });
        PumpTimes(mailbox, 0, kAaDrainFrames + 4);

        Check(mailbox.Applied().preset == 11, "sanity: a failed teardown must leave applied unchanged");
        Check(fake.freeDlssCalls.load() >= 1, "sanity: the failing step was actually attempted");
        Check(mailbox.DlssTeardownSerial() == 0,
            "L4(c): a reported kFailed must NEVER advance the serial -- the step never destroyed "
            "anything, it merely failed");

        fake.freeDlssSucceeds.store(true, std::memory_order_relaxed);
        fake.freeDlssThrows.store(true, std::memory_order_relaxed);
        PumpTimes(mailbox, 100, kAaDrainFrames + 4);

        Check(mailbox.Applied().preset == 11,
            "sanity: a throwing teardown must likewise leave applied unchanged (caught, mapped to "
            "kFailed, retried)");
        Check(mailbox.DlssTeardownSerial() == 0,
            "L4(c): a caught THROW (mapped to kFailed by InvokeDlssTeardownStep) must ALSO never "
            "advance the serial -- identical to an explicit kFailed return");

        fake.freeDlssThrows.store(false, std::memory_order_relaxed);
        PumpTimes(mailbox, 1000, kAaDrainFrames + 4);

        Check(mailbox.Applied().preset == 13 && mailbox.Applied().generation == 1,
            "sanity: once the callback stops failing/throwing, the retried commit must finally land");
        Check(mailbox.DlssTeardownSerial() == 1,
            "L4(c): the serial must advance to exactly 1 once a GENUINE destruction finally happens -- "
            "proving the zero readings above reflect a working counter correctly withholding credit, "
            "not a stub that never increments at all");
    }

    void Case52_FreeDlssAlreadyAbsentAndIdempotentRetrySatisfiesStepWithoutSerialIncrement()
    {
        AaRequest initial;
        initial.effectiveEngine = AaEffectiveEngine::kDlss;
        AaMailbox mailbox{ initial };
        FakeEffects fake;
        mailbox.SetEffects(fake.Bind());

        mailbox.Publish([](AaRequest& r) { ++r.forceRecreateSerial; });
        PumpTimes(mailbox, 0, kAaDrainFrames + 4);
        Check(mailbox.Applied().forceRecreateSerial == 1, "sanity: the first forced drain must commit");
        Check(mailbox.DlssTeardownSerial() == 1, "sanity: the first genuine destruction advances the serial once");
        const int freeDlssCallsAfterFirst = fake.freeDlssCalls.load();

        fake.freeDlssAlreadyAbsent.store(true, std::memory_order_relaxed);
        mailbox.Publish([](AaRequest& r) { ++r.forceRecreateSerial; });
        PumpTimes(mailbox, 5000, kAaDrainFrames + 4);

        Check(mailbox.Applied().forceRecreateSerial == 2,
            "L4(d): a step reporting kAlreadyAbsent must still SATISFY the step -- the commit must "
            "succeed exactly as it would for a real kDestroyed");
        Check(fake.freeDlssCalls.load() == freeDlssCallsAfterFirst + 1,
            "sanity: the callback WAS invoked again for this second, independent drain");
        Check(mailbox.DlssTeardownSerial() == 1,
            "L4(d): kAlreadyAbsent (including an idempotent repeat call finding the resource already "
            "gone) must add ZERO to the serial -- the step is satisfied, but nothing was destroyed by "
            "THIS call, so the serial must stay at its prior value");
    }

    void Case53_EventualCommitAfterDestructiveSuccessDoesNotReincrementSerial()
    {
        AaRequest initial;
        initial.effectiveEngine = AaEffectiveEngine::kDlss;
        AaMailbox mailbox{ initial };
        FakeEffects fake;
        mailbox.SetEffects(fake.Bind());

        mailbox.Publish([](AaRequest& r) { r.effectiveEngine = AaEffectiveEngine::kFsr; });
        PumpTimes(mailbox, 0, kAaDrainFrames + 4);
        Check(mailbox.Applied().generation == 1 &&
                mailbox.Applied().effectiveEngine == AaEffectiveEngine::kFsr,
            "sanity: the engine switch must commit");
        Check(mailbox.DlssTeardownSerial() == 1, "sanity: the genuine destruction advances the serial once");

        mailbox.Publish([](AaRequest& r) { r.dlaaEnabled = false; });
        const auto result = mailbox.Pump(100);
        Check(result.outcome == AaPumpOutcome::kCommitted,
            "sanity: the unrelated scope-none commit must land on its own single Pump");
        Check(!mailbox.Applied().dlaaEnabled && mailbox.Applied().generation == 2, "sanity: it committed");
        Check(fake.freeDlssCalls.load() == 1 && fake.destroyFsrContextCalls.load() == 1,
            "sanity: the unrelated scope-none commit invoked NEITHER teardown callback");
        Check(mailbox.DlssTeardownSerial() == 1,
            "L4(e): an eventual, wholly UNRELATED commit must NEVER re-increment the serial -- it is "
            "not derived from 'a commit happened', only from a genuine freeDlss kDestroyed report");

        mailbox.Publish([](AaRequest& r) { r.effectiveEngine = AaEffectiveEngine::kDlss; });
        PumpTimes(mailbox, 9000, kAaDrainFrames + 4);
        Check(mailbox.Applied().effectiveEngine == AaEffectiveEngine::kDlss &&
                mailbox.Applied().generation == 3,
            "sanity: the second engine switch (back to DLSS) must commit");
        Check(mailbox.DlssTeardownSerial() == 2,
            "L4(e): a genuinely NEW, independent real teardown must advance the serial again -- "
            "confirming the earlier '== 1' readings reflect correct withholding, not a stuck counter");
    }

    void Case54_UnsetFreeDlssCallbackSatisfiesStepWithZeroSerialNoMasquerade()
    {
        AaRequest initial;
        initial.preset = 11;
        AaMailbox mailbox{ initial };
        FakeEffects fake;
        AaEffects effects = fake.Bind();
        effects.freeDlss = nullptr;
        mailbox.SetEffects(std::move(effects));

        mailbox.Publish([](AaRequest& r) { r.preset = 13; });
        PumpTimes(mailbox, 0, kAaDrainFrames + 4);

        Check(mailbox.Applied().preset == 13,
            "L4(f): an UNSET freeDlss callback must still SATISFY the DLSS teardown step (defaults to "
            "kAlreadyAbsent -- see AaMailbox.h's AaEffects contract) so the commit proceeds normally");
        Check(mailbox.Applied().generation == 1, "sanity: the commit reached generation 1");
        Check(mailbox.DlssTeardownSerial() == 0,
            "L4(f): an unset callback must NEVER masquerade as a destruction event -- the serial must "
            "stay at exactly zero even though the step was satisfied and the commit succeeded");
    }

    void Case56_SuccessfulConstructionAlwaysExposesBothNodesNonNullFromBirth()
    {
        AaMailbox plain;
        Check(plain.HasPublishedRequestNode(),
            "N1(ii): a freshly-constructed (default allocators) mailbox must expose a non-null "
            "published request node immediately, from birth");
        Check(plain.PeekPublishedNode() != nullptr,
            "N1(ii): ...redundant direct check via the node-identity accessor");
        Check(plain.PeekDiagnosticIdentityNode() != nullptr,
            "N1(ii): a freshly-constructed mailbox must expose a non-null diagnostic node immediately, "
            "from birth");

        int requestCalls = 0;
        int diagnosticCalls = 0;
        AaMailboxAllocators passthroughAllocators;
        passthroughAllocators.makeRequestNode = [&requestCalls](const AaRequest& r) {
            ++requestCalls;
            return std::make_shared<const AaRequest>(r);
        };
        passthroughAllocators.makeDiagnosticNode = [&diagnosticCalls](const AaDiagnosticSnapshot& s) {
            ++diagnosticCalls;
            return std::make_shared<const AaDiagnosticSnapshot>(s);
        };
        AaRequest initial;
        initial.preset = 55;
        AaMailbox viaSeam{ initial, passthroughAllocators };

        Check(viaSeam.HasPublishedRequestNode() && viaSeam.PeekPublishedNode() != nullptr,
            "N1(ii): a mailbox constructed via the injectable seam (non-throwing custom allocators) "
            "must also expose a non-null published request node immediately");
        Check(viaSeam.PeekDiagnosticIdentityNode() != nullptr,
            "N1(ii): ...and a non-null diagnostic node immediately, even though the seam was actually "
            "exercised (not merely defaulted)");
        Check(requestCalls == 1 && diagnosticCalls == 1,
            "sanity: each allocator hook must be invoked exactly once during construction");
        Check(viaSeam.Applied().preset == 55, "sanity: the supplied initial request is honored end-to-end");
    }

    void Case57_NullAllocatorReturnAbortsConstructionBeforePublication()
    {
        AaRequest initial;
        initial.preset = 66;

        int requestCalls = 0;
        int diagnosticCalls = 0;
        AaMailboxAllocators nullRequestAllocators;
        nullRequestAllocators.makeRequestNode = [&requestCalls](const AaRequest&)
            -> std::shared_ptr<const AaRequest> {
            ++requestCalls;
            return {};
        };
        nullRequestAllocators.makeDiagnosticNode = [&diagnosticCalls](const AaDiagnosticSnapshot& s) {
            ++diagnosticCalls;
            return std::make_shared<const AaDiagnosticSnapshot>(s);
        };

        bool nullRequestThrew = false;
        bool nullRequestConstructed = false;
        try {
            AaMailbox mailbox{ initial, nullRequestAllocators };
            nullRequestConstructed = true;
        } catch (...) {
            nullRequestThrew = true;
        }
        Check(nullRequestThrew && !nullRequestConstructed,
            "a request-node allocator returning nullptr must abort construction, never produce a "
            "live mailbox whose published-request read would dereference null");
        Check(requestCalls == 1 && diagnosticCalls == 0,
            "a null first-node result must abort immediately, before invoking the diagnostic allocator "
            "or reaching either publication store");

        requestCalls = 0;
        diagnosticCalls = 0;
        std::shared_ptr<const AaRequest> capturedRequestNode;
        AaMailboxAllocators nullDiagnosticAllocators;
        nullDiagnosticAllocators.makeRequestNode = [&requestCalls,
                                                        &capturedRequestNode](const AaRequest& r) {
            ++requestCalls;
            capturedRequestNode = std::make_shared<const AaRequest>(r);
            return capturedRequestNode;
        };
        nullDiagnosticAllocators.makeDiagnosticNode = [&diagnosticCalls](const AaDiagnosticSnapshot&)
            -> std::shared_ptr<const AaDiagnosticSnapshot> {
            ++diagnosticCalls;
            return {};
        };

        bool nullDiagnosticThrew = false;
        bool nullDiagnosticConstructed = false;
        try {
            AaMailbox mailbox{ initial, nullDiagnosticAllocators };
            nullDiagnosticConstructed = true;
        } catch (...) {
            nullDiagnosticThrew = true;
        }
        Check(nullDiagnosticThrew && !nullDiagnosticConstructed,
            "a diagnostic-node allocator returning nullptr must abort construction, never produce a "
            "live mailbox that violates the both-nodes-non-null-from-birth invariant");
        Check(requestCalls == 1 && diagnosticCalls == 1,
            "the diagnostic-null path must build each local exactly once before rejecting the second "
            "result");
        Check(capturedRequestNode != nullptr && capturedRequestNode.use_count() == 1,
            "after diagnostic-null construction aborts, the previously-built request node must have "
            "no surviving mailbox ownership");
    }

    void Case58_StartupResolutionFallsBackWithoutDrainOrEffects()
    {
        AaRequest initial;
        initial.requestedEngine = AaEngineRequest::kDlss;
        initial.effectiveEngine = AaEffectiveEngine::kUnresolved;
        AaMailbox mailbox{ initial };
        FakeEffects fake;
        mailbox.SetEffects(fake.Bind());

        const auto result = mailbox.InitializeEffectiveEngine(
            ExactCapabilities(AaAvailability::kUnavailable, AaAvailability::kAvailable));

        Check(result.outcome == AaStartupResolutionOutcome::kInitialized,
            "startup resolution must publish a newly resolved effective engine");
        Check(result.resolution.requested == AaEngineRequest::kDlss &&
                result.resolution.effective == AaEffectiveEngine::kFsr,
            "a retained DLSS request must fall back one-way to FSR when exact capabilities require it");
        Check(mailbox.PeekPublished().requestedEngine == AaEngineRequest::kDlss &&
                mailbox.PeekPublished().effectiveEngine == AaEffectiveEngine::kFsr,
            "startup resolution must preserve requested intent while publishing the resolved effective engine");
        Check(mailbox.Applied().generation == mailbox.PeekPublished().generation &&
                mailbox.Applied().effectiveEngine == AaEffectiveEngine::kFsr,
            "startup resolution must rebase published and applied state coherently");
        const auto diagnostic = mailbox.MakeDiagnosticSnapshot();
        Check(diagnostic.requestedEngine == AaEngineRequest::kDlss &&
                diagnostic.effectiveEngine == AaEffectiveEngine::kFsr &&
                diagnostic.drainScope == kAaRecreateNone && diagnostic.drainCountdown == 0,
            "startup resolution must update diagnostics without staging a recreate drain");
        Check(fake.freeDlssCalls == 0 && fake.destroyFsrContextCalls == 0 &&
                fake.rearmFsrLatchCalls == 0 && fake.rearmExposureCalls == 0 &&
                fake.requestHistoryResetCalls == 0 && fake.logCommitCalls == 0,
            "startup resolution must run no teardown, rearm, history, or commit-log effects");

        const auto pump = mailbox.Pump(1);
        Check(pump.outcome == AaPumpOutcome::kIdle,
            "the first Pump after a coherent startup rebase must remain idle");
    }

    void Case59_StartupResolutionIsIdempotentAndRejectsAfterPump()
    {
        AaRequest initial;
        initial.requestedEngine = AaEngineRequest::kDlss;
        initial.effectiveEngine = AaEffectiveEngine::kUnresolved;
        AaMailbox mailbox{ initial };
        const auto capabilities =
            ExactCapabilities(AaAvailability::kAvailable, AaAvailability::kAvailable);

        const auto first = mailbox.InitializeEffectiveEngine(capabilities);
        const auto firstNode = mailbox.PeekPublishedNode();
        const auto second = mailbox.InitializeEffectiveEngine(capabilities);
        Check(first.outcome == AaStartupResolutionOutcome::kInitialized &&
                second.outcome == AaStartupResolutionOutcome::kUnchanged,
            "repeating identical startup resolution before Pump must be idempotent");
        Check(mailbox.PeekPublishedNode() == firstNode && second.generation == first.generation,
            "idempotent startup resolution must not allocate or advance generation");

        (void)mailbox.Pump(10);
        const auto rejected = mailbox.InitializeEffectiveEngine(
            ExactCapabilities(AaAvailability::kUnavailable, AaAvailability::kAvailable));
        Check(rejected.outcome == AaStartupResolutionOutcome::kRejectedAfterPump,
            "startup rebase must reject every call after any Pump has begun render ownership");
        Check(mailbox.PeekPublishedNode() == firstNode,
            "a rejected post-Pump startup rebase must not mutate published state");
    }

    void Case60_StartupResolutionRetriesAllocationFailureWithoutPoisoning()
    {
        int requestCalls = 0;
        AaMailboxAllocators allocators;
        allocators.makeRequestNode = [&requestCalls](const AaRequest& request)
            -> std::shared_ptr<const AaRequest> {
            ++requestCalls;
            if (requestCalls == 2) {
                return {};
            }
            return std::make_shared<const AaRequest>(request);
        };

        AaRequest initial;
        initial.effectiveEngine = AaEffectiveEngine::kUnresolved;
        AaMailbox mailbox{ initial, allocators };
        const auto capabilities =
            ExactCapabilities(AaAvailability::kUnavailable, AaAvailability::kAvailable);

        const auto failed = mailbox.InitializeEffectiveEngine(capabilities);
        Check(failed.outcome == AaStartupResolutionOutcome::kAllocationFailed &&
                mailbox.PeekPublished().effectiveEngine == AaEffectiveEngine::kUnresolved &&
                mailbox.Applied().effectiveEngine == AaEffectiveEngine::kUnresolved,
            "a startup allocation failure must retain the last coherent unresolved state");

        const auto retried = mailbox.InitializeEffectiveEngine(capabilities);
        Check(retried.outcome == AaStartupResolutionOutcome::kInitialized &&
                mailbox.PeekPublished().effectiveEngine == AaEffectiveEngine::kFsr &&
                mailbox.Applied().effectiveEngine == AaEffectiveEngine::kFsr,
            "the identical startup resolution must retry and succeed after transient allocation failure");
        Check(requestCalls == 3,
            "failed startup resolution must not poison the identical retry or suppress allocation");
    }

    void Case61_StartupResolutionPreservesConcurrentLatestRequest()
    {
        int requestCalls = 0;
        AaMailbox* mailboxPtr = nullptr;
        AaMailboxAllocators allocators;
        allocators.makeRequestNode = [&requestCalls, &mailboxPtr](const AaRequest& request)
            -> std::shared_ptr<const AaRequest> {
            ++requestCalls;
            if (requestCalls == 2 && mailboxPtr != nullptr) {
                mailboxPtr->Publish([](AaRequest& current) {
                    current.requestedEngine = AaEngineRequest::kFsr;
                    current.preset = 99;
                });
            }
            return std::make_shared<const AaRequest>(request);
        };

        AaRequest initial;
        initial.requestedEngine = AaEngineRequest::kDlss;
        initial.effectiveEngine = AaEffectiveEngine::kUnresolved;
        AaMailbox mailbox{ initial, allocators };
        mailboxPtr = &mailbox;

        const auto result = mailbox.InitializeEffectiveEngine(
            ExactCapabilities(AaAvailability::kAvailable, AaAvailability::kAvailable));
        const AaRequest published = mailbox.PeekPublished();
        Check(result.outcome == AaStartupResolutionOutcome::kInitialized,
            "startup resolution must retry its CAS after a racing request publication");
        Check(published.requestedEngine == AaEngineRequest::kFsr &&
                published.effectiveEngine == AaEffectiveEngine::kFsr && published.preset == 99,
            "startup resolution must recompute from and preserve the latest immutable request after a race");
        Check(mailbox.Applied().generation == published.generation &&
                mailbox.Applied().requestedEngine == published.requestedEngine &&
                mailbox.Applied().effectiveEngine == published.effectiveEngine,
            "the successful retry must rebase applied state to the same latest published request");
    }

    void Case62_RequestedOnlyChangeCommitsScopeNone()
    {
        AaRequest initial;
        initial.requestedEngine = AaEngineRequest::kDlss;
        initial.effectiveEngine = AaEffectiveEngine::kFsr;
        AaMailbox mailbox{ initial };
        FakeEffects fake;
        mailbox.SetEffects(fake.Bind());

        mailbox.Publish([](AaRequest& request) { request.requestedEngine = AaEngineRequest::kFsr; });
        const auto result = mailbox.Pump(1);
        Check(result.outcome == AaPumpOutcome::kCommitted &&
                mailbox.Applied().requestedEngine == AaEngineRequest::kFsr &&
                mailbox.Applied().effectiveEngine == AaEffectiveEngine::kFsr,
            "requested-only change with unchanged effective engine must commit on the same Pump");
        Check(fake.freeDlssCalls == 0 && fake.destroyFsrContextCalls == 0,
            "requested-only change with unchanged effective engine must have recreate scope none");
        Check(fake.lastCommit.has_value() && fake.lastCommit->scope == kAaRecreateNone,
            "requested-only commit log must truthfully report scope none");
    }

    void Case63_ActiveToNoneTearsDownOnlyTheActiveEngine()
    {
        {
            AaRequest initial;
            initial.effectiveEngine = AaEffectiveEngine::kDlss;
            AaMailbox mailbox{ initial };
            FakeEffects fake;
            mailbox.SetEffects(fake.Bind());
            mailbox.Publish([](AaRequest& request) { request.effectiveEngine = AaEffectiveEngine::kNone; });
            PumpTimes(mailbox, 1, kAaDrainFrames);
            Check(fake.freeDlssCalls == 1 && fake.destroyFsrContextCalls == 0,
                "active DLSS to no engine must tear down DLSS only");
        }
        {
            AaRequest initial;
            initial.effectiveEngine = AaEffectiveEngine::kFsr;
            AaMailbox mailbox{ initial };
            FakeEffects fake;
            mailbox.SetEffects(fake.Bind());
            mailbox.Publish([](AaRequest& request) { request.effectiveEngine = AaEffectiveEngine::kNone; });
            PumpTimes(mailbox, 100, kAaDrainFrames);
            Check(fake.freeDlssCalls == 0 && fake.destroyFsrContextCalls == 1,
                "active FSR to no engine must tear down FSR only");
        }
    }

    void Case64_NoReverseFallbackAndInvalidCapabilitiesFailClosed()
    {
        AaRequest fsrRequested;
        fsrRequested.requestedEngine = AaEngineRequest::kFsr;
        fsrRequested.effectiveEngine = AaEffectiveEngine::kUnresolved;
        AaMailbox noReverse{ fsrRequested };
        const auto noReverseResult = noReverse.InitializeEffectiveEngine(
            ExactCapabilities(AaAvailability::kAvailable, AaAvailability::kUnavailable));
        Check(noReverseResult.resolution.effective == AaEffectiveEngine::kNone &&
                noReverse.Applied().effectiveEngine == AaEffectiveEngine::kNone,
            "an unavailable requested FSR engine must never reverse-infer or fall back to DLSS");

        AaRequest invalidInitial;
        invalidInitial.effectiveEngine = AaEffectiveEngine::kUnresolved;
        AaMailbox invalid{ invalidInitial };
        const AaEngineCapabilities invalidCapabilities{
            {}, AaCapabilityState::kResolved, AaAvailability::kAvailable, AaAvailability::kAvailable };
        const auto invalidResult = invalid.InitializeEffectiveEngine(invalidCapabilities);
        Check(invalidResult.resolution.effective == AaEffectiveEngine::kNone &&
                invalid.Applied().effectiveEngine == AaEffectiveEngine::kNone,
            "invalid capability tuples must fail closed to no effective engine");

        AaRequest indeterminateInitial;
        indeterminateInitial.effectiveEngine = AaEffectiveEngine::kUnresolved;
        AaMailbox indeterminate{ indeterminateInitial };
        const AaEngineCapabilities indeterminateCapabilities{
            { 0xA11U, 2U }, AaCapabilityState::kIndeterminate,
            AaAvailability::kUnknown, AaAvailability::kUnknown };
        const auto indeterminateResult = indeterminate.InitializeEffectiveEngine(indeterminateCapabilities);
        Check(indeterminateResult.resolution.effective == AaEffectiveEngine::kNone &&
                indeterminate.Applied().effectiveEngine == AaEffectiveEngine::kNone,
            "indeterminate capability observations must fail closed to no effective engine");
    }

    void Case65_DisabledEffectiveChangeStillAccountsForResidentResources()
    {
        AaRequest initial;
        initial.dlaaEnabled = false;
        initial.effectiveEngine = AaEffectiveEngine::kDlss;
        AaMailbox mailbox{ initial };
        FakeEffects fake;
        mailbox.SetEffects(fake.Bind());

        mailbox.Publish([](AaRequest& request) { request.effectiveEngine = AaEffectiveEngine::kNone; });
        PumpTimes(mailbox, 1, kAaDrainFrames);
        Check(fake.freeDlssCalls == 1 && fake.destroyFsrContextCalls == 0,
            "disabled effective DLSS -> none must still retire possibly resident DLSS resources");
        Check(mailbox.Applied().effectiveEngine == AaEffectiveEngine::kNone,
            "disabled effective-engine retirement must commit the no-engine state");
    }

    void Case66_NoEngineSuppressesAndRetainsRetryNotifications()
    {
        AaRequest initial;
        initial.effectiveEngine = AaEffectiveEngine::kNone;
        AaMailbox mailbox{ initial };
        FakeEffects fake;
        mailbox.SetEffects(fake.Bind());

        mailbox.Publish([](AaRequest& request) {
            ++request.fsrRetrySerial;
            ++request.exposureRetrySerial;
            ++request.historyResetSerial;
        });
        const auto noEngineCommit = mailbox.Pump(1);
        Check(noEngineCommit.outcome == AaPumpOutcome::kCommitted,
            "retry serials may commit as intent while no engine is effective");
        Check(fake.rearmFsrLatchCalls == 0 && fake.rearmExposureCalls == 0 &&
                fake.requestHistoryResetCalls == 0,
            "None must authorize no retry, exposure, or history callback");

        mailbox.Publish([](AaRequest& request) {
            request.effectiveEngine = AaEffectiveEngine::kFsr;
        });
        PumpTimes(mailbox, 10, kAaDrainFrames);
        Check(fake.rearmFsrLatchCalls == 1 && fake.requestHistoryResetCalls == 1,
            "retry/history serials suppressed by no-engine state must remain pending for later FSR activation");
        Check(fake.rearmExposureCalls == 0,
            "FSR activation must not consume the retained DLSS-only exposure retry");

        mailbox.Publish([](AaRequest& request) {
            request.effectiveEngine = AaEffectiveEngine::kDlss;
        });
        PumpTimes(mailbox, 20, kAaDrainFrames);
        Check(fake.rearmExposureCalls == 1,
            "the retained exposure retry must fire exactly once when DLSS later becomes effective");
    }

    void Case67_AlreadyResolvedStartupFastPathPreservesConcurrentLatest()
    {
        AaMailbox* mailboxPtr = nullptr;
        bool injected = false;
        AaMailboxAllocators allocators;
        allocators.beforeStartupIdentityCheck = [&]() {
            if (!injected && mailboxPtr != nullptr) {
                injected = true;
                mailboxPtr->Publish([](AaRequest& request) {
                    request.requestedEngine = AaEngineRequest::kFsr;
                    request.preset = 123;
                });
            }
        };

        AaRequest initial;
        initial.requestedEngine = AaEngineRequest::kDlss;
        initial.effectiveEngine = AaEffectiveEngine::kDlss;
        AaMailbox mailbox{ initial, allocators };
        mailboxPtr = &mailbox;

        const auto result = mailbox.InitializeEffectiveEngine(
            ExactCapabilities(AaAvailability::kAvailable, AaAvailability::kAvailable));
        const AaRequest published = mailbox.PeekPublished();
        Check(injected, "the deterministic fast-path race seam must publish the newer request");
        Check(result.outcome == AaStartupResolutionOutcome::kInitialized,
            "already-resolved startup must retry when its identity check discovers a newer request");
        Check(published.requestedEngine == AaEngineRequest::kFsr &&
                published.effectiveEngine == AaEffectiveEngine::kFsr && published.preset == 123,
            "fast-path startup retry must resolve and preserve every field of the newer immutable request");
        Check(mailbox.Applied().generation == published.generation &&
                mailbox.Applied().requestedEngine == published.requestedEngine &&
                mailbox.Applied().effectiveEngine == published.effectiveEngine &&
                mailbox.Applied().preset == published.preset,
            "fast-path startup retry must rebase applied state to the same latest published node");
    }

    void Case68_StartupAllocationReentryPumpForcesRejection()
    {
        enum class AllocatorExit
        {
            kSuccess,
            kNull,
            kThrow,
        };

        const auto verify = [](AllocatorExit allocatorExit) {
            AaMailbox* mailboxPtr = nullptr;
            int requestCalls = 0;
            AaMailboxAllocators allocators;
            allocators.makeRequestNode = [&requestCalls, &mailboxPtr, allocatorExit](const AaRequest& request)
                -> std::shared_ptr<const AaRequest> {
                ++requestCalls;
                if (requestCalls == 2 && mailboxPtr != nullptr) {
                    (void)mailboxPtr->Pump(77);
                    if (allocatorExit == AllocatorExit::kNull) {
                        return {};
                    }
                    if (allocatorExit == AllocatorExit::kThrow) {
                        throw std::bad_alloc{};
                    }
                }
                return std::make_shared<const AaRequest>(request);
            };

            AaRequest initial;
            initial.effectiveEngine = AaEffectiveEngine::kUnresolved;
            AaMailbox mailbox{ initial, allocators };
            mailboxPtr = &mailbox;
            const auto originalNode = mailbox.PeekPublishedNode();

            const auto result = mailbox.InitializeEffectiveEngine(
                ExactCapabilities(AaAvailability::kUnavailable, AaAvailability::kAvailable));
            Check(result.outcome == AaStartupResolutionOutcome::kRejectedAfterPump,
                "a Pump reentered by the startup allocator must outrank every allocator exit");
            Check(mailbox.PeekPublishedNode() == originalNode &&
                    mailbox.PeekPublished().effectiveEngine == AaEffectiveEngine::kUnresolved,
                "allocator-reentrant Pump rejection must publish no resolved candidate");
            Check(mailbox.Applied().effectiveEngine == AaEffectiveEngine::kUnresolved,
                "allocator-reentrant Pump rejection must not rebase after Pump began ownership");
        };

        verify(AllocatorExit::kSuccess);
        verify(AllocatorExit::kNull);
        verify(AllocatorExit::kThrow);
    }

    void Case69_StartupAllocationFailureRefreshesConcurrentLatestRequest()
    {
        enum class AllocatorExit
        {
            kNull,
            kThrow,
        };

        const auto verify = [](AllocatorExit allocatorExit) {
            AaMailbox* mailboxPtr = nullptr;
            int requestCalls = 0;
            AaMailboxAllocators allocators;
            allocators.makeRequestNode = [&requestCalls, &mailboxPtr, allocatorExit](const AaRequest& request)
                -> std::shared_ptr<const AaRequest> {
                ++requestCalls;
                if (requestCalls == 2 && mailboxPtr != nullptr) {
                    mailboxPtr->Publish([](AaRequest& current) {
                        current.requestedEngine = AaEngineRequest::kFsr;
                        current.preset = 246;
                    });
                    if (allocatorExit == AllocatorExit::kNull) {
                        return {};
                    }
                    throw std::bad_alloc{};
                }
                return std::make_shared<const AaRequest>(request);
            };

            AaRequest initial;
            initial.requestedEngine = AaEngineRequest::kDlss;
            initial.effectiveEngine = AaEffectiveEngine::kUnresolved;
            AaMailbox mailbox{ initial, allocators };
            mailboxPtr = &mailbox;

            const auto result = mailbox.InitializeEffectiveEngine(
                ExactCapabilities(AaAvailability::kAvailable, AaAvailability::kAvailable));
            const AaRequest published = mailbox.PeekPublished();
            Check(result.outcome == AaStartupResolutionOutcome::kAllocationFailed,
                "allocation failure without Pump must remain retryable allocation failure");
            Check(published.requestedEngine == AaEngineRequest::kFsr &&
                    published.effectiveEngine == AaEffectiveEngine::kUnresolved && published.preset == 246,
                "allocation failure must preserve the concurrently published latest request");
            Check(result.generation == published.generation &&
                    result.resolution.requested == AaEngineRequest::kFsr &&
                    result.resolution.effective == AaEffectiveEngine::kFsr,
                "allocation-failure result must describe the latest immutable request, never stale input");
            Check(mailbox.Applied().requestedEngine == AaEngineRequest::kDlss &&
                    mailbox.Applied().effectiveEngine == AaEffectiveEngine::kUnresolved,
                "allocation failure must not rebase applied state to an unpublished resolution");
        };

        verify(AllocatorExit::kNull);
        verify(AllocatorExit::kThrow);
    }

    void Case70_QualityModeStagesDlssDrainAndNormalizesFailClosed()
    {
        AaRequest initial;
        AaMailbox mailbox{ initial };
        FakeEffects fake;
        mailbox.SetEffects(fake.Bind());

        mailbox.Publish([](AaRequest& r) { r.qualityMode = 4; });

        PumpTimes(mailbox, 0, kAaDrainFrames - 1);
        Check(mailbox.Applied().qualityMode == 0,
            "mid-drain, eval must still observe the OLD quality mode (never a torn/early switch)");

        PumpTimes(mailbox, 100, kAaDrainFrames + 4);
        Check(mailbox.Applied().qualityMode == 4,
            "after the drain commits, eval must observe the NEW quality mode (live switch)");
        Check(fake.freeDlssCalls.load() >= 1,
            "a quality-mode change must stage a REAL DLSS-scope teardown (create-time render extent)");

        mailbox.Publish([](AaRequest& r) { r.qualityMode = 99; });
        Check(mailbox.PeekPublished().qualityMode == 0,
            "Publish must normalize an out-of-range quality mode to 0 (DLAA) fail-closed");

        AaRequest garbageInitial;
        garbageInitial.qualityMode = 77;
        const AaMailbox constructed{ garbageInitial };
        Check(constructed.Applied().qualityMode == 0,
            "the constructor must normalize an out-of-range initial quality mode to 0 (DLAA)");

        AaRequest noneInitial;
        noneInitial.effectiveEngine = AaEffectiveEngine::kNone;
        AaMailbox noneMailbox{ noneInitial };
        FakeEffects noneFake;
        noneMailbox.SetEffects(noneFake.Bind());
        noneMailbox.Publish([](AaRequest& r) { r.qualityMode = 5; });
        PumpTimes(noneMailbox, 5000, 2);
        Check(noneMailbox.Applied().qualityMode == 5,
            "with effectiveEngine kNone, a quality-mode change must commit scope-none (same tick)");
        Check(noneFake.freeDlssCalls.load() == 0,
            "with effectiveEngine kNone, a quality-mode change must invoke ZERO teardowns");
    }

    void Case72_CreateTimeChangeUnderActiveDlssAffectsActiveEngine()
    {
        AaRequest initial;
        initial.effectiveEngine = AaEffectiveEngine::kDlss;
        initial.dlaaEnabled = true;
        initial.preset = 11;
        AaMailbox mailbox{ initial };
        FakeEffects fake;
        mailbox.SetEffects(fake.Bind());

        mailbox.Publish([](AaRequest& r) { r.preset = 13; });
        const auto first = mailbox.Pump(1);
        Check(first.outcome == AaPumpOutcome::kDraining, "a preset change under an active DLSS engine is a REAL drain");
        Check(first.affectsActiveEngine,
            "the DLSS-bit drain must read as affecting the active engine (the passthrough gate)");
        Check(mailbox.MakeDiagnosticSnapshot().drainScope == kAaRecreateDlss,
            "the create-time change under DLSS must stage the DLSS bit ONLY");
        PumpTimes(mailbox, 100, kAaDrainFrames + 4);

        Check(mailbox.Applied().preset == 13, "the preset change must commit");
        Check(fake.freeDlssCalls.load() == 1, "the DLSS feature must be freed exactly once");
        Check(fake.destroyFsrContextCalls.load() == 0,
            "a create-time DLSS change must never touch the FSR context");

        mailbox.Publish([](AaRequest& r) { r.autoExposure = false; });
        PumpTimes(mailbox, 1000, kAaDrainFrames + 4);
        mailbox.Publish([](AaRequest& r) { r.qualityMode = 2; });
        PumpTimes(mailbox, 2000, kAaDrainFrames + 4);
        mailbox.Publish([](AaRequest& r) { r.resolutionScale = 1.5F; });
        PumpTimes(mailbox, 3000, kAaDrainFrames + 4);
        Check(fake.freeDlssCalls.load() == 4,
            "autoExposure / qualityMode / resolutionScale each free the DLSS feature once, and only once");
    }
}

int main()
{
    struct NamedCase
    {
        const char* name;
        void (*fn)();
    };

    const NamedCase cases[] = {
        { "requested-unapplied-through-drain", &Case01_RequestedUnappliedThroughDrain },
        { "final-tick-supersede-zero-frees", &Case02_FinalTickSupersedeZeroFrees },
        { "A-B-A-cancels-premutation", &Case03_ABACancelsPremutation },
        { "dlss-only-never-destroys-active-fsr", &Case04_DlssOnlyNeverDestroysActiveFsr },
        { "same-selector-off-on-rearms-exactly-once", &Case05_SameSelectorOffOnRearmsExactlyOnce },
        { "switch-then-off-never-publishes-stale-active", &Case06_SwitchThenOffNeverPublishesStaleActive },
        { "pump-progresses-while-disabled", &Case07_PumpProgressesWhileDisabled },
        { "failed-teardown-leaves-applied-unchanged-and-retries",
            &Case08_FailedTeardownLeavesAppliedUnchangedAndRetries },
        { "idle-pump-no-underflow", &Case09_IdlePumpNoUnderflow },
        { "concurrent-producers-monotonic-generations", &Case10_ConcurrentProducersMonotonicGenerations },

        { "A-architecture-must-use-atomic-shared-ptr-publication",
            &Case11_ArchitectureMustUseAtomicSharedPtrPublication },
        { "A-concurrent-multi-field-publish-internal-consistency",
            &Case12_ConcurrentMultiFieldPublishInternalConsistency },
        { "A-old-snapshot-stays-valid-while-writer-publishes-many",
            &Case13_OldSnapshotStaysValidWhileWriterPublishesMany },
        { "B-scope-none-enable-disable-commits-without-arming-drain",
            &Case14_ScopeNoneEnableDisableCommitsWithoutArmingDrain },
        { "B-event-only-publish-commits-without-arming-drain",
            &Case15_EventOnlyPublishCommitsWithoutArmingDrain },
        { "C-no-op-publish-does-not-advance-generation", &Case16_NoOpPublishDoesNotAdvanceGeneration },
        { "C-forced-recreate-ticket-forces-teardown-despite-identical-config",
            &Case17_ForcedRecreateTicketForcesTeardownDespiteIdenticalConfig },
        { "D-both-engine-teardown-per-step-retry-and-visibility",
            &Case18_BothEngineTeardownPerStepRetryAndVisibility },
        { "D-second-engine-repeated-failure-persists-first-engine-completion",
            &Case19_SecondEngineRepeatedFailurePersistsFirstEngineCompletion },
        { "D-teardown-callback-idempotency-defense-in-depth",
            &Case20_TeardownCallbackIdempotencyDefenseInDepth },
        { "E-duplicate-frame-pump-does-not-double-advance-drain",
            &Case21_DuplicateFramePumpDoesNotDoubleAdvanceDrain },
        { "F-automatic-fsr-selection-off-on-rearms-exactly-once",
            &Case22_AutomaticFsrSelectionOffOnRearmsExactlyOnce },
        { "F-final-tick-supersession-exact-zero-attributable-calls",
            &Case23_FinalTickSupersessionExactZeroAttributableCalls },
        { "F-switch-then-off-applied-sampled-every-pump-never-stale",
            &Case24_SwitchThenOffAppliedSampledEveryPumpNeverStale },
        { "F-event-serial-coherence-over-concurrent-mixed-publishes",
            &Case25a_EventSerialCoherenceOverConcurrentMixedPublishes },
        { "F-failed-commit-retains-event-for-later-successful-retry",
            &Case25b_FailedCommitRetainsEventForLaterSuccessfulRetry },
        { "A-F-concurrent-diagnostic-writer-readers-consistency",
            &Case27_ConcurrentDiagnosticWriterReadersConsistency },

        { "G-partial-teardown-retarget-to-old-applied-stays-damaged",
            &Case28_PartialTeardownRetargetToOldAppliedStaysDamaged },
        { "G-damaged-scope-clears-only-after-genuine-reconstruction",
            &Case29_DamagedScopeClearsOnlyAfterGenuineReconstruction },
        { "H-publish-during-teardown-callback-rereads-before-publishing-applied",
            &Case30_PublishDuringTeardownCallbackRereadsBeforePublishingApplied },
        { "H-publish-during-first-both-teardown-skips-stale-second",
            &Case31_PublishDuringFirstBothTeardownSkipsStaleSecond },
        { "F-fsr-to-dlss-rearms-exposure-exactly-once",
            &Case32_FsrToDlssRearmsExposureExactlyOnce },
        { "H-publish-during-notification-rereads-before-publishing-applied",
            &Case33_PublishDuringNotificationRereadsBeforePublishingApplied },

        { "exception-throwing-destructive-callback-treated-as-step-failure-no-terminate",
            &Case34_ThrowingDestructiveCallbackTreatedAsStepFailureNoTerminate },
        { "exception-throwing-noncritical-callback-is-fail-safe-commit-proceeds",
            &Case35_ThrowingNoncriticalCallbackIsFailSafeCommitProceeds },
        { "accessor-render-only-damaged-scope-independent-of-diagnostic-snapshot",
            &Case36_RenderOnlyDamagedScopeAccessorIndependentOfDiagnosticSnapshot },
        { "diagnostic-snapshot-publishes-on-state-change-only-not-every-idle-frame",
            &Case37_DiagnosticSnapshotPublishesOnStateChangeOnlyNotEveryIdleFrame },
        { "exception-throwing-second-destructive-step-in-both-engine-drain-retries-only-that-step",
            &Case38_ThrowingSecondDestructiveStepInBothEngineDrainRetriesOnlyThatStep },
        { "exception-throwing-logcommit-is-fail-safe-commit-already-landed",
            &Case39_ThrowingLogCommitIsFailSafeCommitAlreadyLanded },

        { "H-publish-during-first-notification-skips-stale-later-notifications",
            &Case40_PublishDuringFirstNotificationSkipsStaleLaterNotifications },
        { "F-event-serial-delivered-exactly-once-across-notification-supersede",
            &Case41_EventSerialDeliveredExactlyOnceAcrossNotificationSupersede },

        { "I-constructor-must-not-be-noexcept", &Case42_ConstructorMustNotBeNoexcept },
        { "I-diagnostic-node-allocation-failure-aborts-construction-entirely",
            &Case43_DiagnosticAllocationFailureAbortsConstructionEntirely },
        { "I-request-node-allocation-failure-aborts-construction-entirely",
            &Case44_RequestAllocationFailureAbortsConstructionEntirely },
        { "K-bias-fields-are-normalized-and-clamped-in-applied-state",
            &Case45_BiasFieldsAreNormalizedAndClampedInAppliedState },
        { "K-bias-only-change-commits-as-scope-none-without-arming-drain",
            &Case46_BiasOnlyChangeCommitsAsScopeNoneWithoutArmingDrain },
        { "K-identical-bias-after-clamping-is-a-generation-no-op",
            &Case47_IdenticalBiasAfterClampingIsAGenerationNoOp },
        { "K-bias-change-during-active-drain-retargets-without-disturbing-drain-scope",
            &Case48_BiasChangeDuringActiveDrainRetargetsWithoutDisturbingDrainScope },

        { "L-a-free-dlss-destroyed-then-superseding-publish-advances-serial-once-fsr-not-invoked",
            &Case49_FreeDlssDestroyedThenSupersedingPublishAdvancesSerialOnceFsrNotInvoked },
        { "L-b-both-scope-only-dlss-destroys-serial-once-fsr-step-alone-retries",
            &Case50_BothScopeOnlyDlssDestroysSerialOnceFsrStepAloneRetries },
        { "L-c-free-dlss-failed-and-thrown-both-leave-serial-zero-step-pending",
            &Case51_FreeDlssFailedAndThrownBothLeaveSerialZeroStepPending },
        { "L-d-free-dlss-already-absent-and-idempotent-retry-satisfies-step-without-serial-increment",
            &Case52_FreeDlssAlreadyAbsentAndIdempotentRetrySatisfiesStepWithoutSerialIncrement },
        { "L-e-eventual-commit-after-destructive-success-does-not-reincrement-serial",
            &Case53_EventualCommitAfterDestructiveSuccessDoesNotReincrementSerial },
        { "L-f-unset-free-dlss-callback-satisfies-step-with-zero-serial-no-masquerade",
            &Case54_UnsetFreeDlssCallbackSatisfiesStepWithZeroSerialNoMasquerade },

        { "N1ii-successful-construction-always-exposes-both-nodes-non-null-from-birth",
            &Case56_SuccessfulConstructionAlwaysExposesBothNodesNonNullFromBirth },
        { "N1iii-null-allocator-return-aborts-construction-before-publication",
            &Case57_NullAllocatorReturnAbortsConstructionBeforePublication },

        { "startup-resolution-fallback-without-drain-or-effects",
            &Case58_StartupResolutionFallsBackWithoutDrainOrEffects },
        { "startup-resolution-idempotent-and-rejected-after-pump",
            &Case59_StartupResolutionIsIdempotentAndRejectsAfterPump },
        { "startup-resolution-retries-allocation-failure",
            &Case60_StartupResolutionRetriesAllocationFailureWithoutPoisoning },
        { "startup-resolution-preserves-concurrent-latest-request",
            &Case61_StartupResolutionPreservesConcurrentLatestRequest },
        { "requested-only-change-commits-scope-none",
            &Case62_RequestedOnlyChangeCommitsScopeNone },
        { "active-to-none-tears-down-only-active-engine",
            &Case63_ActiveToNoneTearsDownOnlyTheActiveEngine },
        { "no-reverse-fallback-invalid-and-indeterminate-fail-closed",
            &Case64_NoReverseFallbackAndInvalidCapabilitiesFailClosed },
        { "disabled-effective-change-accounts-for-resident-resources",
            &Case65_DisabledEffectiveChangeStillAccountsForResidentResources },
        { "no-engine-suppresses-and-retains-retry-notifications",
            &Case66_NoEngineSuppressesAndRetainsRetryNotifications },
        { "already-resolved-startup-fast-path-preserves-concurrent-latest",
            &Case67_AlreadyResolvedStartupFastPathPreservesConcurrentLatest },
        { "startup-allocation-reentry-pump-forces-rejection",
            &Case68_StartupAllocationReentryPumpForcesRejection },
        { "startup-allocation-failure-refreshes-concurrent-latest-request",
            &Case69_StartupAllocationFailureRefreshesConcurrentLatestRequest },

        { "quality-mode-stages-dlss-drain-and-normalizes-fail-closed",
            &Case70_QualityModeStagesDlssDrainAndNormalizesFailClosed },

        { "create-time-change-under-active-dlss-affects-active-engine",
            &Case72_CreateTimeChangeUnderActiveDlssAffectsActiveEngine },
    };

    std::vector<CaseResult> results;
    results.reserve(std::size(cases));

    for (const auto& c : cases) {
        CaseResult result;
        result.name = c.name;
        g_active = &result;
        try {
            c.fn();
        } catch (const std::exception& ex) {
            ++result.failures;
            result.messages.emplace_back(std::string("unhandled exception: ") + ex.what());
        } catch (...) {
            ++result.failures;
            result.messages.emplace_back("unhandled non-standard exception");
        }
        g_active = nullptr;
        results.push_back(std::move(result));
    }

    int failedCases = 0;
    for (const auto& result : results) {
        const bool pass = result.failures == 0;
        std::cout << (pass ? "[PASS] " : "[FAIL] ") << result.name << " (" << (result.checks - result.failures)
                   << "/" << result.checks << " checks)\n";
        for (const auto& message : result.messages) {
            std::cout << "    - " << message << '\n';
        }
        if (!pass) {
            ++failedCases;
        }
    }

    std::cout << failedCases << " of " << results.size() << " AaMailbox coordinator cases failed\n";

    return failedCases > 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
