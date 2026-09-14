#include "PCH.h"

#include "Platform/EngineStub.h"

#include <atomic>
#include <string>

namespace
{

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
        return a_address >= start && (a_address + a_length) <= (start + nt->OptionalHeader.SizeOfImage);
    }
}

namespace Platform
{
    bool EngineStub::InstallBranch(std::uint64_t a_id, std::ptrdiff_t a_offset,
        const std::uint8_t* a_code, std::size_t a_size, const char* a_name) noexcept
    {
        if (!a_code || a_size == 0U) {
            logger::warn("[Stub] {}: empty generated code — skipped (fail-open)", a_name);
            return false;
        }
        try {
            const REL::Relocation<std::uintptr_t> reloc{ REL::ID(static_cast<std::uint32_t>(a_id)),
                a_offset };
            const std::uintptr_t site = reloc.address();
            if (site == 0U || !WithinGameImage(site, 5U)) {
                logger::warn("[Stub] {}: REL({})+{:#x} resolved to {:#x}, outside the game image — skipped (fail-open)",
                    a_name, a_id, a_offset, site);
                return false;
            }

            std::uint8_t original[5]{};
            if (!SehRead(original, reinterpret_cast<const void*>(site), sizeof(original))) {
                logger::warn("[Stub] {}: reading {:#x} faulted — skipped (fail-open)", a_name, site);
                return false;
            }
            if (original[0] == 0xE9) {
                logger::warn("[Stub] {}: {:#x} already holds a jump [{}] — refusing to overwrite another patch (fail-open)",
                    a_name, site, HexOf(original, sizeof(original)));
                return false;
            }

            auto& trampoline = F4SE::GetTrampoline();
            void* const allocated = trampoline.allocate(a_size);
            if (!allocated) {
                logger::warn("[Stub] {}: trampoline allocation of {} bytes failed — skipped (fail-open)",
                    a_name, a_size);
                return false;
            }
            std::memcpy(allocated, a_code, a_size);

            (void)trampoline.write_branch<5>(site, reinterpret_cast<std::uintptr_t>(allocated));

            logger::info("[Stub] {}: REL({})+{:#x} @{:#x} replaced [{}] with a branch to a {}-byte stub",
                a_name, a_id, a_offset, site, HexOf(original, sizeof(original)), a_size);
            return true;
        } catch (const std::exception& e) {
            logger::warn("[Stub] {}: install threw ({}) — skipped (fail-open)", a_name, e.what());
            return false;
        } catch (...) {
            logger::warn("[Stub] {}: install threw — skipped (fail-open)", a_name);
            return false;
        }
    }

    std::uintptr_t EngineStub::ResolveSite(std::uint64_t a_id, std::ptrdiff_t a_offset,
        const char* a_name) noexcept
    {
        try {
            const REL::Relocation<std::uintptr_t> reloc{ REL::ID(static_cast<std::uint32_t>(a_id)),
                a_offset };
            const std::uintptr_t address = reloc.address();
            if (address == 0U || !WithinGameImage(address, 5U)) {
                logger::warn("[Stub] {}: REL({})+{:#x} resolved to {:#x}, outside the game image — stub not generated (fail-open)",
                    a_name, a_id, a_offset, address);
                return 0U;
            }
            return address;
        } catch (...) {
            logger::warn("[Stub] {}: REL({}) did not resolve — stub not generated (fail-open)", a_name, a_id);
            return 0U;
        }
    }

}
