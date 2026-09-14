#include "PCH.h"

#include "Platform/EngineImod.h"

#include "Platform/EngineMemory.h"

#include "RE/Bethesda/FormFactory.h"
#include "RE/Bethesda/ImageSpaceManager.h"
#include "RE/Bethesda/ImageSpaceModifier.h"
#include "RE/Bethesda/MemoryManager.h"
#include "RE/Bethesda/TESDataHandler.h"
#include "RE/Bethesda/TESForms.h"

#include <atomic>
#include <cmath>
#include <cstring>

namespace
{

    constexpr REL::ID kTriggerByForm{ 179769 };
    constexpr REL::ID kStopByForm{ 217873 };
    constexpr REL::ID kNiFloatDataVtable{ 1417447 };
    constexpr std::uintptr_t kSismeRva = 0x36E95E8;

    using TriggerFn = RE::ImageSpaceModifierInstanceForm* (*)(RE::TESImageSpaceModifier*, float, void*);
    using StopFn = void (*)(RE::TESImageSpaceModifier*);

    constexpr std::uint32_t kFlatSlots = 42;
    constexpr std::uintptr_t kKeySizeOffset = 0x028;
    constexpr std::uintptr_t kInterpolatorOffset = 0x120;
    constexpr std::uintptr_t kAnimatableOffset = 0x020;
    constexpr std::uintptr_t kDurationOffset = 0x024;

    constexpr std::uint32_t kSemantics = 12;
    constexpr std::uint32_t kCinSaturation = 9;
    constexpr std::uint32_t kCinBrightness = 10;
    constexpr std::uint32_t kCinContrast = 11;

    constexpr std::uint32_t kShpToHdr[9]{ 0, 8, 1, 2, 3, 4, 5, 6, 7 };

    constexpr std::uint32_t kSemTintAmount = 12;
    constexpr std::uint32_t kSemFadeAmount = 13;
    constexpr std::uint32_t kSemanticSlots = 14;

    struct NamedChannel
    {
        std::uintptr_t interpOffset;
        std::uintptr_t keySizeOffset;
    };
    constexpr NamedChannel kNamed[]{
        { 0x2B8, 0x0F4 },
        { 0x2C0, 0x0F8 },
        { 0x2C8, 0x0FC },
        { 0x2D0, 0x114 },
        { 0x2D8, 0x118 },
        { 0x290, 0x0DC },
        { 0x298, 0x0E0 },
        { 0x2A0, 0x0E4 },
        { 0x2A8, 0x104 },
        { 0x2B0, 0x108 },
        { 0x270, 0x0D4 },
        { 0x278, 0x0D8 },
        { 0x2E0, 0x110 },
    };
    constexpr std::uint32_t kNamedCount =
        static_cast<std::uint32_t>(std::size(kNamed));
    constexpr NamedChannel kTintColor{ 0x280, 0x0D0 };
    constexpr NamedChannel kFadeColor{ 0x288, 0x10C };
    constexpr std::int32_t kIdNamedBase = 100;

    std::atomic<bool> g_lightShapeSpecialRequested{ false };
    std::atomic<bool> g_adaptationOffRequested{ false };
    std::atomic<bool> g_gradingNeutralRequested{ false };
    std::atomic<bool> g_controlsDirty{ false };
    struct HdrRequest
    {
        std::atomic<bool> set{ false };
        std::atomic<float> value{ 0.0F };
    };
    HdrRequest g_hdrRequests[9]{};
    HdrRequest g_effectRequests[5]{};
    std::atomic<bool> g_radialOffRequested{ false };
    std::atomic<bool> g_doubleOffRequested{ false };

    std::atomic<std::uint8_t> g_phase{
        static_cast<std::uint8_t>(Platform::EngineImod::Phase::kIdle) };
    std::atomic<bool> g_sismeOn{ true };
    std::atomic<std::uint32_t> g_armedCount{ 0 };

    bool g_bindAttempted = false;
    std::atomic<bool> g_latched{ false };
    TriggerFn g_trigger = nullptr;
    StopFn g_stop = nullptr;
    RE::TESImageSpaceModifier* g_form = nullptr;
    std::atomic<bool> g_applied{ false };

    std::ptrdiff_t g_interpDataOffset = -1;
    std::ptrdiff_t g_dataNumKeysOffset = -1;
    std::ptrdiff_t g_dataKeysOffset = -1;
    std::uint32_t g_cloneBytes = 0x30;
    const void* g_donorInterp = nullptr;
    const void* g_donorData = nullptr;

    std::int32_t g_multSlot[kSemanticSlots];
    std::int32_t g_addSlot[kSemanticSlots];

    struct Clone
    {
        void* interpolator = nullptr;
        void* data = nullptr;
        float* keys = nullptr;
        std::int32_t armedSlot = -1;
    };
    constexpr std::uint32_t kClonePool = 40;
    Clone g_clones[kClonePool]{};

    void Latch(const char* a_what) noexcept
    {
        if (!g_latched) {
            g_latched = true;
            g_phase.store(static_cast<std::uint8_t>(Platform::EngineImod::Phase::kUnavailable),
                std::memory_order_relaxed);
            logger::warn("[EngineImod] {} — the engine-modifier layer is OFF for the session "
                         "(fail-open; nothing was written to any engine record).",
                a_what);
        }
    }

    [[nodiscard]] void* FlatInterpolatorSlot(RE::TESImageSpaceModifier* a_form,
        std::uint32_t a_slot) noexcept
    {
        return reinterpret_cast<std::uint8_t*>(a_form) + kInterpolatorOffset +
               static_cast<std::uintptr_t>(a_slot) * sizeof(void*);
    }

    [[nodiscard]] void* FlatKeySizeSlot(RE::TESImageSpaceModifier* a_form,
        std::uint32_t a_slot) noexcept
    {
        return reinterpret_cast<std::uint8_t*>(a_form) + kKeySizeOffset +
               static_cast<std::uintptr_t>(a_slot) * sizeof(std::uint32_t);
    }

    [[nodiscard]] bool DiscoverNiLayouts() noexcept
    {
        auto* const dataHandler = RE::TESDataHandler::GetSingleton();
        if (dataHandler == nullptr) {
            Latch("TESDataHandler is not up yet");
            return false;
        }
        const auto& imads = dataHandler->GetFormArray<RE::TESImageSpaceModifier>();
        const void* const dataVtable = reinterpret_cast<const void*>(
            REL::Relocation<std::uintptr_t>{ kNiFloatDataVtable }.address());

        constexpr std::uint32_t kMaxSamples = 24;
        void* samples[kMaxSamples]{};
        std::uint32_t sampleCount = 0;
        for (auto* const form : imads) {
            if (form == nullptr || sampleCount >= kMaxSamples) {
                break;
            }
            for (std::uint32_t slot = 0; slot < kFlatSlots && sampleCount < kMaxSamples; ++slot) {
                void* interp = nullptr;
                if (Platform::EngineMemory::SafeRead(
                        &interp, FlatInterpolatorSlot(form, slot), sizeof(interp)) &&
                    interp != nullptr) {
                    samples[sampleCount++] = interp;
                }
            }
        }
        if (sampleCount < 4) {
            Latch("too few vanilla IMAD interpolators to verify the Ni layout against");
            return false;
        }

        for (std::ptrdiff_t offset = 0x10; offset <= 0x28; offset += 8) {
            std::uint32_t hits = 0;
            for (std::uint32_t i = 0; i < sampleCount; ++i) {
                void* candidate = nullptr;
                const void* candidateVtable = nullptr;
                if (Platform::EngineMemory::SafeRead(&candidate,
                        static_cast<std::uint8_t*>(samples[i]) + offset, sizeof(candidate)) &&
                    candidate != nullptr &&
                    Platform::EngineMemory::SafeRead(
                        &candidateVtable, candidate, sizeof(candidateVtable)) &&
                    candidateVtable == dataVtable) {
                    ++hits;
                }
            }
            if (hits == sampleCount) {
                if (g_interpDataOffset >= 0) {
                    Latch("ambiguous NiFloatInterpolator data slot (two offsets verified)");
                    return false;
                }
                g_interpDataOffset = offset;
            }
        }
        if (g_interpDataOffset < 0) {
            Latch("no NiFloatInterpolator offset consistently carries a NiFloatData");
            return false;
        }

        struct Candidate
        {
            std::ptrdiff_t numKeys;
            std::ptrdiff_t keys;
        };
        constexpr Candidate kCandidates[]{ { 0x10, 0x18 }, { 0x18, 0x10 }, { 0x14, 0x18 } };
        for (const auto& candidate : kCandidates) {
            bool allValid = true;
            for (std::uint32_t i = 0; i < sampleCount && allValid; ++i) {
                void* data = nullptr;
                if (!Platform::EngineMemory::SafeRead(&data,
                        static_cast<std::uint8_t*>(samples[i]) + g_interpDataOffset,
                        sizeof(data)) ||
                    data == nullptr) {
                    allValid = false;
                    break;
                }
                std::uint32_t numKeys = 0;
                float* keys = nullptr;
                if (!Platform::EngineMemory::SafeRead(&numKeys,
                        static_cast<std::uint8_t*>(data) + candidate.numKeys, sizeof(numKeys)) ||
                    !Platform::EngineMemory::SafeRead(&keys,
                        static_cast<std::uint8_t*>(data) + candidate.keys, sizeof(keys))) {
                    allValid = false;
                    break;
                }
                if (numKeys == 0 || numKeys > 4096 || keys == nullptr) {
                    allValid = false;
                    break;
                }
                float firstPair[2]{};
                if (!Platform::EngineMemory::SafeRead(&firstPair, keys, sizeof(firstPair)) ||
                    !std::isfinite(firstPair[0]) || !std::isfinite(firstPair[1])) {
                    allValid = false;
                    break;
                }
                if (numKeys > 1) {
                    float secondPair[2]{};
                    if (!Platform::EngineMemory::SafeRead(&secondPair,
                            keys + 2, sizeof(secondPair)) ||
                        !(secondPair[0] >= firstPair[0])) {
                        allValid = false;
                        break;
                    }
                }
            }
            if (allValid) {
                if (g_dataNumKeysOffset >= 0) {
                    Latch("ambiguous NiFloatData layout (two candidates verified)");
                    return false;
                }
                g_dataNumKeysOffset = candidate.numKeys;
                g_dataKeysOffset = candidate.keys;
            }
        }
        if (g_dataNumKeysOffset < 0) {
            Latch("no NiFloatData layout candidate verified on every sampled vanilla object");
            return false;
        }

        g_donorInterp = samples[0];
        void* donorData = nullptr;
        static_cast<void>(Platform::EngineMemory::SafeRead(&donorData,
            static_cast<const std::uint8_t*>(g_donorInterp) + g_interpDataOffset,
            sizeof(donorData)));
        g_donorData = donorData;

        std::uint8_t scratch[0x60]{};
        g_cloneBytes =
            Platform::EngineMemory::SafeRead(&scratch, g_donorInterp, 0x60) ? 0x60 : 0x30;

        logger::info("[EngineImod] Ni layouts verified on {} vanilla interpolators: "
                     "interp->data at +{:#x}, data->numKeys at +{:#x}, data->keys at +{:#x}, "
                     "clone width {:#x}",
            sampleCount, g_interpDataOffset, g_dataNumKeysOffset, g_dataKeysOffset, g_cloneBytes);
        return true;
    }

    [[nodiscard]] Clone* RebuildClone(std::uint32_t a_index, float a_value) noexcept
    {
        if (a_index >= kClonePool) {
            return nullptr;
        }
        auto& clone = g_clones[a_index];
        if (clone.interpolator == nullptr) {
            clone.interpolator = RE::malloc(0x60);
            clone.data = RE::malloc(0x60);
            clone.keys = static_cast<float*>(RE::malloc(sizeof(float) * 4));
            if (clone.interpolator == nullptr || clone.data == nullptr ||
                clone.keys == nullptr) {
                return nullptr;
            }
            std::memset(clone.interpolator, 0, 0x60);
            std::memset(clone.data, 0, 0x60);
        }
        if (!Platform::EngineMemory::SafeRead(clone.interpolator, g_donorInterp, g_cloneBytes) ||
            !Platform::EngineMemory::SafeRead(clone.data, g_donorData, g_cloneBytes)) {
            return nullptr;
        }
        clone.keys[0] = 0.0F;
        clone.keys[1] = a_value;
        clone.keys[2] = 0.0F;
        clone.keys[3] = 0.0F;
        *reinterpret_cast<volatile long*>(static_cast<std::uint8_t*>(clone.interpolator) + 8) = 1;
        *reinterpret_cast<volatile long*>(static_cast<std::uint8_t*>(clone.data) + 8) = 1;
        *reinterpret_cast<std::uint32_t*>(
            static_cast<std::uint8_t*>(clone.data) + g_dataNumKeysOffset) = 1;
        *reinterpret_cast<float**>(static_cast<std::uint8_t*>(clone.data) + g_dataKeysOffset) =
            clone.keys;
        *reinterpret_cast<void**>(
            static_cast<std::uint8_t*>(clone.interpolator) + g_interpDataOffset) = clone.data;
        {
            auto* const interp = static_cast<std::uint8_t*>(clone.interpolator);
            constexpr std::uint32_t kNeverEvaluated = 0xFF7FFFFFU;
            *reinterpret_cast<std::uint32_t*>(interp + 0x10) = kNeverEvaluated;
            if (g_interpDataOffset == 0x20) {
                *reinterpret_cast<std::uint32_t*>(interp + 0x18) = kNeverEvaluated;
                *reinterpret_cast<std::uint32_t*>(interp + 0x28) = 0U;
            }
        }
        return &clone;
    }

    [[nodiscard]] bool Bind() noexcept
    {
        if (g_latched) {
            return false;
        }
        if (g_bindAttempted) {
            return g_form != nullptr;
        }
        if (RE::TESDataHandler::GetSingleton() == nullptr) {
            return false;
        }
        g_bindAttempted = true;
        for (std::uint32_t i = 0; i < kSemanticSlots; ++i) {
            if (i < 9) {
                g_multSlot[i] = static_cast<std::int32_t>(2U * i);
                g_addSlot[i] = static_cast<std::int32_t>(2U * i + 1U);
            } else if (i < 12) {
                g_multSlot[i] = static_cast<std::int32_t>(34U + 2U * (i - 9U));
                g_addSlot[i] = g_multSlot[i] + 1;
            } else {
                g_multSlot[i] = -1;
                g_addSlot[i] = -1;
            }
        }

        g_trigger = reinterpret_cast<TriggerFn>(
            REL::Relocation<std::uintptr_t>{ kTriggerByForm }.address());
        g_stop = reinterpret_cast<StopFn>(
            REL::Relocation<std::uintptr_t>{ kStopByForm }.address());
        if (!Platform::EngineMemory::WithinGameImage(
                reinterpret_cast<std::uintptr_t>(g_trigger), 16) ||
            !Platform::EngineMemory::WithinGameImage(
                reinterpret_cast<std::uintptr_t>(g_stop), 16)) {
            Latch("Trigger/Stop relocations resolve outside the game image");
            return false;
        }
        if (!DiscoverNiLayouts()) {
            return false;
        }
        auto* const factory = RE::ConcreteFormFactory<RE::TESImageSpaceModifier>::GetFormFactory();
        if (factory == nullptr) {
            Latch("the TESImageSpaceModifier form factory is unavailable");
            return false;
        }
        g_form = factory->Create();
        if (g_form == nullptr) {
            Latch("the form factory returned no TESImageSpaceModifier");
            return false;
        }
        auto* const formBytes = reinterpret_cast<std::uint8_t*>(g_form);
        *reinterpret_cast<bool*>(formBytes + kAnimatableOffset) = false;
        *reinterpret_cast<float*>(formBytes + kDurationOffset) = 0.0F;

        logger::info("[EngineImod] bound: runtime IMOD form constructed, Trigger/Stop resolved");
        g_phase.store(static_cast<std::uint8_t>(Platform::EngineImod::Phase::kReady),
            std::memory_order_relaxed);
        return true;
    }

    void ReadSisme() noexcept
    {
        std::uint8_t sisme = 1;
        const auto address = REL::Offset{ kSismeRva }.address();
        if (Platform::EngineMemory::SafeRead(&sisme, reinterpret_cast<void*>(address), 1)) {
            g_sismeOn.store(sisme != 0, std::memory_order_relaxed);
        }
    }

    void ClearChannel(std::uintptr_t a_interpOffset, std::uintptr_t a_keySizeOffset) noexcept
    {
        void* null = nullptr;
        std::uint32_t zero = 0;
        static_cast<void>(Platform::EngineMemory::SafeWrite(
            reinterpret_cast<std::uint8_t*>(g_form) + a_interpOffset, &null, sizeof(null)));
        static_cast<void>(Platform::EngineMemory::SafeWrite(
            reinterpret_cast<std::uint8_t*>(g_form) + a_keySizeOffset, &zero, sizeof(zero)));
    }

    void ClearAllSlots() noexcept
    {
        for (std::uint32_t slot = 0; slot < kFlatSlots; ++slot) {
            void* null = nullptr;
            std::uint32_t zero = 0;
            static_cast<void>(Platform::EngineMemory::SafeWrite(
                FlatInterpolatorSlot(g_form, slot), &null, sizeof(null)));
            static_cast<void>(Platform::EngineMemory::SafeWrite(
                FlatKeySizeSlot(g_form, slot), &zero, sizeof(zero)));
        }
        for (const auto& named : kNamed) {
            ClearChannel(named.interpOffset, named.keySizeOffset);
        }
        ClearChannel(kTintColor.interpOffset, kTintColor.keySizeOffset);
        ClearChannel(kFadeColor.interpOffset, kFadeColor.keySizeOffset);
        for (auto& clone : g_clones) {
            clone.armedSlot = -1;
        }
    }

    [[nodiscard]] bool ArmChannel(std::uintptr_t a_interpOffset, std::uintptr_t a_keySizeOffset,
        void* a_interpolator) noexcept
    {
        std::uint32_t one = 1;
        return Platform::EngineMemory::SafeWrite(
                   reinterpret_cast<std::uint8_t*>(g_form) + a_keySizeOffset, &one,
                   sizeof(one)) &&
               Platform::EngineMemory::SafeWrite(
                   reinterpret_cast<std::uint8_t*>(g_form) + a_interpOffset, &a_interpolator,
                   sizeof(a_interpolator));
    }

    [[nodiscard]] bool ArmSlot(std::uint32_t a_cloneIndex, std::int32_t a_slot,
        float a_value) noexcept
    {
        if (a_slot < 0) {
            return false;
        }
        auto* const clone = RebuildClone(a_cloneIndex, a_value);
        if (clone == nullptr) {
            return false;
        }
        clone->armedSlot = a_slot;
        std::uint32_t one = 1;
        const auto slot = static_cast<std::uint32_t>(a_slot);
        return Platform::EngineMemory::SafeWrite(
                   FlatKeySizeSlot(g_form, slot), &one, sizeof(one)) &&
               Platform::EngineMemory::SafeWrite(
                   FlatInterpolatorSlot(g_form, slot), &clone->interpolator,
                   sizeof(clone->interpolator));
    }

    [[nodiscard]] bool ArmDesired(std::uint32_t a_cloneIndex, std::int32_t a_id,
        float a_value) noexcept
    {
        if (a_id >= kIdNamedBase) {
            const auto namedIndex = static_cast<std::uint32_t>(a_id - kIdNamedBase);
            if (namedIndex >= kNamedCount) {
                return false;
            }
            auto* const clone = RebuildClone(a_cloneIndex, a_value);
            if (clone == nullptr) {
                return false;
            }
            clone->armedSlot = a_id;
            return ArmChannel(kNamed[namedIndex].interpOffset,
                kNamed[namedIndex].keySizeOffset, clone->interpolator);
        }
        return ArmSlot(a_cloneIndex, a_id, a_value);
    }

    void Remove() noexcept
    {
        if (g_applied) {
            g_stop(g_form);
            g_applied = false;
        }
    }

    void Apply() noexcept
    {
        g_trigger(g_form, 1.0F, nullptr);
        g_applied = true;
    }

    [[nodiscard]] bool ReadEof(float (&a_out)[44]) noexcept;
    std::int32_t g_currentSlots[kClonePool]{};
    float g_currentValues[kClonePool]{};
    std::uint32_t g_currentCount = 0;
    std::atomic<bool> g_forceRearm{ false };

    void ApplyControls() noexcept
    {
        struct Desired
        {
            std::int32_t slot;
            float value;
        };
        Desired desired[kClonePool]{};
        std::uint32_t count = 0;
        const auto add = [&](std::int32_t a_slot, float a_value) {
            if (a_slot < 0) {
                return;
            }
            for (std::uint32_t i = 0; i < count; ++i) {
                if (desired[i].slot == a_slot) {
                    desired[i].value = a_value;
                    return;
                }
            }
            if (count < kClonePool) {
                desired[count++] = Desired{ a_slot, a_value };
            }
        };

        if (g_adaptationOffRequested.load(std::memory_order_relaxed)) {
            add(g_multSlot[0], 0.0F);
            add(g_multSlot[8], 0.0F);
        }
        if (g_gradingNeutralRequested.load(std::memory_order_relaxed)) {
            add(g_multSlot[kCinSaturation], 0.0F);
            add(g_addSlot[kCinSaturation], 1.0F);
            add(g_multSlot[kCinBrightness], 0.0F);
            add(g_addSlot[kCinBrightness], 1.0F);
            add(g_multSlot[kCinContrast], 0.0F);
            add(g_addSlot[kCinContrast], 1.0F);
        }
        const bool lightShapeSpecial =
            g_lightShapeSpecialRequested.load(std::memory_order_relaxed);
        if (lightShapeSpecial) {
            add(g_multSlot[1], 0.0F);
            add(g_addSlot[1], 1.000001F);
        }
        for (std::uint32_t i = 0; i < 9; ++i) {
            if (i == 2U && lightShapeSpecial) {
                continue;
            }
            if (g_hdrRequests[i].set.load(std::memory_order_relaxed)) {
                const auto semantic = kShpToHdr[i];
                add(g_multSlot[semantic], 0.0F);
                add(g_addSlot[semantic],
                    g_hdrRequests[i].value.load(std::memory_order_relaxed));
            }
        }
        {
            bool anyDof = false;
            for (std::uint32_t i = 0; i < 5; ++i) {
                anyDof = anyDof || g_effectRequests[i].set.load(std::memory_order_relaxed);
            }
            if (anyDof) {
                float eof[44]{};
                const bool haveEof = ReadEof(eof);
                constexpr std::uint32_t kDofModData[5]{ 35, 36, 37, 40, 41 };
                for (std::uint32_t i = 0; i < 5; ++i) {
                    const bool set = g_effectRequests[i].set.load(std::memory_order_relaxed);
                    const float value =
                        set ? g_effectRequests[i].value.load(std::memory_order_relaxed)
                        : (haveEof ? eof[kDofModData[i]] : 0.0F);
                    add(kIdNamedBase + static_cast<std::int32_t>(i), value);
                }
            }
        }
        if (g_radialOffRequested.load(std::memory_order_relaxed)) {
            for (std::uint32_t i = 5; i <= 9; ++i) {
                add(kIdNamedBase + static_cast<std::int32_t>(i), 0.0F);
            }
        }
        if (g_doubleOffRequested.load(std::memory_order_relaxed)) {
            add(kIdNamedBase + 11, 0.0F);
        }

        bool sameTopology = g_applied && count == g_currentCount && count > 0;
        bool sameMap = sameTopology;
        for (std::uint32_t i = 0; sameTopology && i < count; ++i) {
            sameTopology = desired[i].slot == g_currentSlots[i];
            sameMap = sameTopology && sameMap && desired[i].value == g_currentValues[i];
        }
        const bool forced = g_forceRearm.exchange(false, std::memory_order_relaxed);
        if (sameMap && !forced) {
            return;
        }
        if (sameMap && forced) {
            logger::info("[EngineImod] re-armed on request with an unchanged map ({} slot(s)): a load boundary or a settings replay", count);
        }

        Remove();
        ClearAllSlots();
        std::uint32_t armed = 0;
        for (std::uint32_t i = 0; i < count; ++i) {
            if (ArmDesired(i, desired[i].slot, desired[i].value)) {
                ++armed;
            }
            g_currentSlots[i] = desired[i].slot;
            g_currentValues[i] = desired[i].value;
        }
        g_currentCount = count;
        g_armedCount.store(armed, std::memory_order_relaxed);
        if (armed > 0) {
            Apply();
            g_phase.store(static_cast<std::uint8_t>(Platform::EngineImod::Phase::kApplied),
                std::memory_order_relaxed);
        } else {
            g_phase.store(static_cast<std::uint8_t>(Platform::EngineImod::Phase::kReady),
                std::memory_order_relaxed);
        }
        if (!sameTopology) {
            logger::info("[EngineImod] control topology changed: {} slot(s) armed", armed);
        }
    }

    [[nodiscard]] bool ReadEof(float (&a_out)[44]) noexcept
    {
        auto* const manager = RE::ImageSpaceManager::GetSingleton();
        if (manager == nullptr) {
            return false;
        }
        return Platform::EngineMemory::SafeRead(&a_out, &manager->currentEOFData, sizeof(a_out));
    }

}

namespace Platform::EngineImod
{
    std::atomic<bool> g_tickQueued{ false };
    std::atomic<bool> g_taskRefusalLogged{ false };
    void GameThreadTick() noexcept;

    void FrameTick() noexcept
    {
        if (g_latched) {
            return;
        }
        const bool anyRequest = g_adaptationOffRequested.load(std::memory_order_relaxed) ||
                                g_gradingNeutralRequested.load(std::memory_order_relaxed);
        if (!g_controlsDirty.load(std::memory_order_relaxed) && !(anyRequest && !g_applied)) {
            return;
        }
        if (g_tickQueued.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        const auto* const tasks = F4SE::GetTaskInterface();
        if (tasks == nullptr) {
            g_tickQueued.store(false, std::memory_order_release);
            if (!g_taskRefusalLogged.exchange(true, std::memory_order_relaxed)) {
                logger::warn("[EngineImod] the F4SE task interface is unavailable — the engine-modifier controls stay "
                             "unapplied (they run only on the game thread, never from the render thread)");
            }
            return;
        }
        try {
            tasks->AddTask([]() noexcept {
                GameThreadTick();
                g_tickQueued.store(false, std::memory_order_release);
            });
        } catch (...) {
            g_tickQueued.store(false, std::memory_order_release);
            Latch("C++ exception queueing the engine-modifier task");
        }
    }

    void GameThreadTick() noexcept
    {
        try {
            const bool dirty = g_controlsDirty.exchange(false, std::memory_order_relaxed);
            const bool anyRequest = g_adaptationOffRequested.load(std::memory_order_relaxed) ||
                                    g_gradingNeutralRequested.load(std::memory_order_relaxed);
            if (g_latched || (!dirty && !anyRequest && !g_applied)) {
                return;
            }
            if (!Bind()) {
                if (dirty && !g_latched) {
                    g_controlsDirty.store(true, std::memory_order_relaxed);
                }
                return;
            }
            ReadSisme();
            if (!g_sismeOn.load(std::memory_order_relaxed)) {
                static std::atomic<bool> s_loggedSisme{ false };
                if (!s_loggedSisme.exchange(true, std::memory_order_relaxed)) {
                    logger::warn("[EngineImod] the game's IMOD system is disabled (sisme 0) — "
                                 "engine-modifier controls have no effect until sisme 1");
                }
            }

            if (dirty) {
                ApplyControls();
            }
        } catch (...) {
            Remove();
            Latch("C++ exception in the engine-modifier layer");
        }
    }

    void RequestRearm() noexcept
    {
        g_forceRearm.store(true, std::memory_order_relaxed);
        g_controlsDirty.store(true, std::memory_order_relaxed);
    }

    void SetLightShapeSpecialTest(bool a_on) noexcept
    {
        g_lightShapeSpecialRequested.store(a_on, std::memory_order_relaxed);
        g_controlsDirty.store(true, std::memory_order_relaxed);
    }

    bool LightShapeSpecialTestRequested() noexcept
    {
        return g_lightShapeSpecialRequested.load(std::memory_order_relaxed);
    }

    void SetAdaptationOff(bool a_off) noexcept
    {
        g_adaptationOffRequested.store(a_off, std::memory_order_relaxed);
        g_controlsDirty.store(true, std::memory_order_relaxed);
    }

    bool AdaptationOffRequested() noexcept
    {
        return g_adaptationOffRequested.load(std::memory_order_relaxed);
    }

    void SetEngineGradingNeutral(bool a_neutral) noexcept
    {
        g_gradingNeutralRequested.store(a_neutral, std::memory_order_relaxed);
        g_controlsDirty.store(true, std::memory_order_relaxed);
    }

    bool EngineGradingNeutralRequested() noexcept
    {
        return g_gradingNeutralRequested.load(std::memory_order_relaxed);
    }

    void SetHdrParam(std::uint32_t a_argIndex, float a_value, bool a_set) noexcept
    {
        if (a_argIndex >= 9) {
            return;
        }
        g_hdrRequests[a_argIndex].value.store(a_value, std::memory_order_relaxed);
        g_hdrRequests[a_argIndex].set.store(a_set, std::memory_order_relaxed);
        g_controlsDirty.store(true, std::memory_order_relaxed);
    }

    void SetEffectParam(std::uint32_t a_argIndex, float a_value, bool a_set) noexcept
    {
        if (a_argIndex >= 5) {
            return;
        }
        g_effectRequests[a_argIndex].value.store(a_value, std::memory_order_relaxed);
        g_effectRequests[a_argIndex].set.store(a_set, std::memory_order_relaxed);
        g_controlsDirty.store(true, std::memory_order_relaxed);
    }

    void SetRadialBlurOff(bool a_off) noexcept
    {
        g_radialOffRequested.store(a_off, std::memory_order_relaxed);
        g_controlsDirty.store(true, std::memory_order_relaxed);
    }

    bool RadialBlurOffRequested() noexcept
    {
        return g_radialOffRequested.load(std::memory_order_relaxed);
    }

    void SetDoubleVisionOff(bool a_off) noexcept
    {
        g_doubleOffRequested.store(a_off, std::memory_order_relaxed);
        g_controlsDirty.store(true, std::memory_order_relaxed);
    }

    bool DoubleVisionOffRequested() noexcept
    {
        return g_doubleOffRequested.load(std::memory_order_relaxed);
    }

    State Snapshot() noexcept
    {
        return State{
            static_cast<Phase>(g_phase.load(std::memory_order_relaxed)),
            g_sismeOn.load(std::memory_order_relaxed),
            g_armedCount.load(std::memory_order_relaxed),
        };
    }
}
