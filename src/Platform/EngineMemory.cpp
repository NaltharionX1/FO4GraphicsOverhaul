#include "PCH.h"

#include "Platform/EngineMemory.h"

#include <cstring>

namespace Platform::EngineMemory
{
    bool WithinGameImage(std::uintptr_t a_address, std::size_t a_length) noexcept
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
        return a_address >= start && a_length <= (end - start) && (a_address + a_length) <= end;
    }

    bool SafeRead(void* a_dst, const void* a_src, std::size_t a_count) noexcept
    {
        __try {
            std::memcpy(a_dst, a_src, a_count);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    bool SafeWrite(void* a_dst, const void* a_src, std::size_t a_count) noexcept
    {
        __try {
            std::memcpy(a_dst, a_src, a_count);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }
}
