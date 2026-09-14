#include "PCH.h"

#include "Platform/EnginePatch.h"

#include <atomic>
#include <string>
#include <vector>

namespace
{
    std::atomic<std::uint32_t> g_applied{ 0 };
    std::atomic<std::uint32_t> g_refused{ 0 };

    [[nodiscard]] std::string HexOf(const std::uint8_t* a_bytes, std::size_t a_count) noexcept
    {
        try {
            static constexpr char kDigits[] = "0123456789ABCDEF";
            std::string out;
            out.reserve(a_count * 3U);
            for (std::size_t i = 0; i < a_count; ++i) {
                if (i != 0U) {
                    out.push_back(' ');
                }
                out.push_back(kDigits[(a_bytes[i] >> 4) & 0x0F]);
                out.push_back(kDigits[a_bytes[i] & 0x0F]);
            }
            return out;
        } catch (...) {
            return "<unavailable>";
        }
    }

    [[nodiscard]] bool SehRead(void* a_dst, const void* a_src, std::size_t a_count) noexcept
    {
        __try {
            std::memcpy(a_dst, a_src, a_count);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    [[nodiscard]] bool SehWrite(void* a_dst, const void* a_src, std::size_t a_count) noexcept
    {
        __try {
            std::memcpy(a_dst, a_src, a_count);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    [[nodiscard]] bool WithinGameImage(std::uintptr_t a_address, std::size_t a_length) noexcept
    {
        const HMODULE base = ::GetModuleHandleW(nullptr);
        if (!base) {
            return false;
        }
        const auto* const dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
            return false;
        }
        const auto* const nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
            reinterpret_cast<const std::uint8_t*>(base) + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) {
            return false;
        }
        const auto start = reinterpret_cast<std::uintptr_t>(base);
        const auto end = start + nt->OptionalHeader.SizeOfImage;
        return a_address >= start && (a_address + a_length) <= end;
    }
}

namespace Platform
{
    bool EnginePatch::WriteBytes(std::uint64_t a_id, std::ptrdiff_t a_offset,
        std::span<const std::uint8_t> a_patch, std::span<const std::uint8_t> a_expected,
        const char* a_name) noexcept
    {
        if (a_patch.empty()) {
            return false;
        }
        std::uintptr_t address = 0;
        try {
            const REL::Relocation<std::uintptr_t> reloc{ REL::ID(static_cast<std::uint32_t>(a_id)),
                a_offset };
            address = reloc.address();
        } catch (...) {
            g_refused.fetch_add(1, std::memory_order_relaxed);
            logger::warn("[Patch] {}: REL({}) did not resolve — patch skipped (fail-open)", a_name, a_id);
            return false;
        }
        if (address == 0U || !WithinGameImage(address, a_patch.size())) {
            g_refused.fetch_add(1, std::memory_order_relaxed);
            logger::warn("[Patch] {}: REL({})+{:#x} resolved to {:#x}, which is outside the game image — patch skipped (fail-open)",
                a_name, a_id, a_offset, address);
            return false;
        }

        std::uint8_t current[32]{};
        const std::size_t count = a_patch.size() <= sizeof(current) ? a_patch.size() : sizeof(current);
        if (!SehRead(current, reinterpret_cast<const void*>(address), count)) {
            g_refused.fetch_add(1, std::memory_order_relaxed);
            logger::warn("[Patch] {}: reading {:#x} faulted — patch skipped (fail-open)", a_name, address);
            return false;
        }

        if (!a_expected.empty()) {
            const std::size_t expectCount =
                a_expected.size() <= count ? a_expected.size() : count;
            if (std::memcmp(current, a_expected.data(), expectCount) != 0) {
                g_refused.fetch_add(1, std::memory_order_relaxed);
                logger::warn("[Patch] {}: REL({})+{:#x} holds [{}] but [{}] was expected — patch REFUSED (fail-open)",
                    a_name, a_id, a_offset, HexOf(current, expectCount),
                    HexOf(a_expected.data(), expectCount));
                return false;
            }
        }

        DWORD oldProtect = 0;
        if (!::VirtualProtect(reinterpret_cast<void*>(address), a_patch.size(),
                PAGE_EXECUTE_READWRITE, &oldProtect)) {
            g_refused.fetch_add(1, std::memory_order_relaxed);
            logger::warn("[Patch] {}: VirtualProtect failed at {:#x} — patch skipped (fail-open)",
                a_name, address);
            return false;
        }
        const bool wrote =
            SehWrite(reinterpret_cast<void*>(address), a_patch.data(), a_patch.size());
        DWORD ignored = 0;
        ::VirtualProtect(reinterpret_cast<void*>(address), a_patch.size(), oldProtect, &ignored);
        ::FlushInstructionCache(::GetCurrentProcess(), reinterpret_cast<void*>(address), a_patch.size());

        if (!wrote) {
            g_refused.fetch_add(1, std::memory_order_relaxed);
            logger::warn("[Patch] {}: write to {:#x} faulted — patch skipped (fail-open)", a_name, address);
            return false;
        }
        g_applied.fetch_add(1, std::memory_order_relaxed);
        logger::info("[Patch] {}: REL({})+{:#x} @{:#x} [{}] -> [{}]", a_name, a_id, a_offset, address,
            HexOf(current, count), HexOf(a_patch.data(), a_patch.size()));
        return true;
    }

    bool EnginePatch::WriteByte(std::uint64_t a_id, std::ptrdiff_t a_offset, std::uint8_t a_value,
        const char* a_name) noexcept
    {
        const std::uint8_t one[1]{ a_value };
        return WriteBytes(a_id, a_offset, one, {}, a_name);
    }

    bool EnginePatch::FillBytes(std::uint64_t a_id, std::ptrdiff_t a_offset, std::uint8_t a_value,
        std::size_t a_count, const char* a_name) noexcept
    {
        if (a_count == 0U || a_count > 512U) {
            g_refused.fetch_add(1, std::memory_order_relaxed);
            logger::warn("[Patch] {}: refusing a {}-byte fill (out of sane range)", a_name, a_count);
            return false;
        }
        try {
            const std::vector<std::uint8_t> fill(a_count, a_value);
            return WriteBytes(a_id, a_offset, fill, {}, a_name);
        } catch (...) {
            g_refused.fetch_add(1, std::memory_order_relaxed);
            logger::warn("[Patch] {}: fill allocation failed — patch skipped (fail-open)", a_name);
            return false;
        }
    }

    void EnginePatch::LogSummary(const char* a_batchName) noexcept
    {
        const auto applied = g_applied.load(std::memory_order_relaxed);
        const auto refused = g_refused.load(std::memory_order_relaxed);
        if (refused == 0U) {
            logger::info("[Patch] {}: {} patches applied, none refused", a_batchName, applied);
        } else {
            logger::warn("[Patch] {}: {} applied, {} REFUSED — the refused features are NOT active (see the lines above)",
                a_batchName, applied, refused);
        }
    }
}
