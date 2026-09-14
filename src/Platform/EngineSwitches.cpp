#include "PCH.h"

#include "Platform/EngineSwitches.h"

#include "Platform/EngineMemory.h"

#include "RE/Bethesda/Script.h"

#include <atomic>
#include <cstring>

namespace
{

    constexpr std::size_t kMaxBytes = 8;

    struct Row
    {
        const char* command;
        const char* label;

        std::size_t siteOffset;
        std::uint8_t sitePrefix[kMaxBytes];
        std::size_t prefixLength;
        std::size_t siteLength;

        std::size_t deref1Offset;
        std::uint8_t deref1[kMaxBytes];
        std::size_t deref1Length;
        std::size_t deref2Offset;
        std::uint8_t deref2[kMaxBytes];
        std::size_t deref2Length;
        std::ptrdiff_t effectOffset;

        std::size_t storeOffset;
        std::uint8_t store[kMaxBytes];
        std::size_t storeLength;

        bool oneMeansOn;
    };

    constexpr Row kRows[]{
        { "gr", "godrays", 0x10F, { 0xC6, 0x05 }, 2, 7, 0, {}, 0, 0, {}, 0, 0, 0, {}, 0, false },
        { "cl", "character light", 0x105, { 0x88, 0x05 }, 2, 6, 0, {}, 0, 0, {}, 0, 0, 0, {}, 0, true },
        { "mb", "motion blur", 0x95, { 0x48, 0x8B, 0x05 }, 3, 7,
            0xAB, { 0x48, 0x8B, 0x48, 0x18 }, 4,
            0xAF, { 0x48, 0x8B, 0x59, 0x50 }, 4, 0x50,
            0xFD, { 0xC6, 0x43, 0x08, 0x01 }, 4, true },
    };

    constexpr std::size_t kRowCount = std::size(kRows);
    constexpr std::size_t kWindow = 0x180;

    consteval bool RowsAreWellFormed() noexcept
    {
        for (const Row& row : kRows) {
            if (row.siteOffset + row.siteLength > kWindow ||
                row.prefixLength > kMaxBytes || row.prefixLength == 0 ||
                row.deref1Offset + row.deref1Length > kWindow ||
                row.deref2Offset + row.deref2Length > kWindow ||
                row.storeOffset + row.storeLength > kWindow ||
                row.deref1Length > kMaxBytes || row.deref2Length > kMaxBytes ||
                row.storeLength > kMaxBytes) {
                return false;
            }
            if (row.prefixLength + sizeof(std::int32_t) > row.siteLength) {
                return false;
            }
            const bool chained = row.effectOffset != 0;
            if (chained != (row.deref1Length != 0) || chained != (row.deref2Length != 0) ||
                chained != (row.storeLength != 0)) {
                return false;
            }
        }
        return true;
    }
    static_assert(RowsAreWellFormed(),
        "an EngineSwitches row is malformed: an offset escapes the read window, a disp32 does not "
        "fit its instruction, or a chain/static row is missing (or wrongly carrying) its deref and "
        "store verification sites");

    std::atomic<std::uintptr_t> g_address[kRowCount]{};

    std::atomic<std::uint64_t> g_failures{ 0 };

    [[nodiscard]] bool ShouldLogFailure() noexcept
    {
        const auto n = g_failures.fetch_add(1, std::memory_order_relaxed) + 1U;
        return n <= 3U || (n % 500U) == 0U;
    }

    [[nodiscard]] bool Matches(const std::uint8_t* a_code, std::size_t a_offset,
        const std::uint8_t* a_expected, std::size_t a_length) noexcept
    {
        return a_length == 0 || std::memcmp(a_code + a_offset, a_expected, a_length) == 0;
    }

    [[nodiscard]] std::size_t IndexOf(const char* a_command) noexcept
    {
        if (a_command != nullptr) {
            for (std::size_t i = 0; i < kRowCount; ++i) {
                if (::_stricmp(kRows[i].command, a_command) == 0) {
                    return i;
                }
            }
        }
        return kRowCount;
    }

    [[nodiscard]] bool Resolve(std::size_t a_index, std::uintptr_t& a_out) noexcept
    {
        const std::uintptr_t base = g_address[a_index].load(std::memory_order_acquire);
        if (base == 0) {
            return false;
        }
        const Row& row = kRows[a_index];
        if (row.effectOffset == 0) {
            a_out = base;
            return true;
        }
        std::uintptr_t singleton = 0;
        std::uintptr_t first = 0;
        std::uintptr_t second = 0;
        const bool ok =
            Platform::EngineMemory::SafeRead(
                &singleton, reinterpret_cast<const void*>(base), sizeof(singleton)) &&
            singleton != 0 &&
            Platform::EngineMemory::SafeRead(
                &first, reinterpret_cast<const void*>(singleton + 0x18), sizeof(first)) &&
            first != 0 &&
            Platform::EngineMemory::SafeRead(
                &second, reinterpret_cast<const void*>(first + row.effectOffset), sizeof(second)) &&
            second != 0;
        if (!ok) {
            return false;
        }
        a_out = second + 0x8;
        return true;
    }
}

namespace Platform::EngineSwitches
{
    void BindAll(std::span<RE::SCRIPT_FUNCTION> a_functions) noexcept
    {
        std::size_t bound = 0;
        for (std::size_t i = 0; i < kRowCount; ++i) {
            const Row& row = kRows[i];

            const RE::SCRIPT_FUNCTION* found = nullptr;
            for (const RE::SCRIPT_FUNCTION& fn : a_functions) {
                if ((fn.shortName != nullptr && ::_stricmp(fn.shortName, row.command) == 0) ||
                    (fn.functionName != nullptr &&
                        ::_stricmp(fn.functionName, row.command) == 0)) {
                    found = &fn;
                    break;
                }
            }
            if (found == nullptr || found->executeFunction == nullptr) {
                logger::warn("[Switches] {:16} — '{}' not in the console table; it keeps its "
                             "console command",
                    row.label, row.command);
                continue;
            }

            std::uint8_t code[kWindow]{};
            if (!EngineMemory::SafeRead(code, found->executeFunction, sizeof(code))) {
                logger::warn("[Switches] {:16} — handler unreadable; it keeps its console command",
                    row.label);
                continue;
            }
            if (!Matches(code, row.siteOffset, row.sitePrefix, row.prefixLength) ||
                !Matches(code, row.deref1Offset, row.deref1, row.deref1Length) ||
                !Matches(code, row.deref2Offset, row.deref2, row.deref2Length) ||
                !Matches(code, row.storeOffset, row.store, row.storeLength)) {
                logger::warn("[Switches] {:16} — handler bytes DIFFER from the derivation at "
                             "+{:#x}; REFUSING to own it, so the console command stays live",
                    row.label, row.siteOffset);
                continue;
            }

            std::int32_t disp = 0;
            std::memcpy(&disp, &code[row.siteOffset + row.prefixLength], sizeof(disp));
            const auto handler = reinterpret_cast<std::uintptr_t>(found->executeFunction);
            const auto target = static_cast<std::uintptr_t>(
                static_cast<std::intptr_t>(handler + row.siteOffset + row.siteLength) + disp);
            if (!EngineMemory::WithinGameImage(target, sizeof(std::uintptr_t))) {
                logger::warn("[Switches] {:16} — derived address {:#x} is outside the game image; "
                             "REFUSING",
                    row.label, target);
                continue;
            }

            g_address[i].store(target, std::memory_order_release);
            ++bound;
            logger::info("[Switches] {:16} OWNED — {} @{:#x}, derived from '{}'s own bytes and "
                         "opcode-verified",
                row.label, row.effectOffset != 0 ? "chain slot" : "static byte", target,
                row.command);
        }
        logger::info("[Switches] {} of {} owned. An owned switch is written directly and its "
                     "console command is blocked; one that refused to bind keeps its command as "
                     "the manual fallback.",
            bound, kRowCount);
    }

    bool Owns(const char* a_command) noexcept
    {
        const std::size_t index = IndexOf(a_command);
        return index < kRowCount && g_address[index].load(std::memory_order_acquire) != 0;
    }

    bool Write(const char* a_command, bool a_on) noexcept
    {
        const std::size_t index = IndexOf(a_command);
        if (index >= kRowCount) {
            return false;
        }
        std::uintptr_t address = 0;
        if (!Resolve(index, address)) {
            if (ShouldLogFailure()) {
                logger::warn("[Switches] {:16} — could not resolve its address this time; falling "
                             "back to the console command",
                    kRows[index].label);
            }
            return false;
        }

        std::uint8_t current = 0;
        if (!EngineMemory::SafeRead(&current, reinterpret_cast<const void*>(address),
                sizeof(current))) {
            return false;
        }
        const std::uint8_t want = (a_on == kRows[index].oneMeansOn) ? 1U : 0U;
        if (current == want) {
            return true;
        }
        if (!EngineMemory::SafeWrite(reinterpret_cast<void*>(address), &want, sizeof(want))) {
            if (ShouldLogFailure()) {
                logger::warn("[Switches] {:16} — write FAILED @{:#x}; falling back to the console",
                    kRows[index].label, address);
            }
            return false;
        }
        logger::info("[Switches] {:16} {} -> {} @{:#x} — direct write, no console ({} means {})",
            kRows[index].label, current, want, address, want,
            a_on ? "ON to the user" : "OFF to the user");
        return true;
    }
}
