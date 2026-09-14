#include "PCH.h"

#include "Platform/CharLightParams.h"

#include "Platform/EngineMemory.h"

#include <atomic>
#include <cstring>

namespace
{
    struct Param
    {
        const char* command;
        std::size_t readOffset;
        std::uint8_t readBytes[4];
        std::size_t writeOffset;
        std::uint8_t writeBytes[4];
    };

    constexpr Param kParams[]{
        { "cl rim", 0x325, { 0xF3, 0x0F, 0x10, 0x35 }, 0x35B, { 0xF3, 0x0F, 0x11, 0x0D } },
        { "cl fill", 0x396, { 0xF3, 0x0F, 0x10, 0x35 }, 0x3CC, { 0xF3, 0x0F, 0x11, 0x0D } },
    };
    constexpr std::size_t kParamCount = std::size(kParams);
    constexpr std::size_t kInstructionLength = 8;

    std::atomic<float*> g_value[kParamCount]{};

    [[nodiscard]] std::ptrdiff_t IndexOf(const char* a_command) noexcept
    {
        if (a_command == nullptr) {
            return -1;
        }
        for (std::size_t i = 0; i < kParamCount; ++i) {
            if (::_stricmp(a_command, kParams[i].command) == 0) {
                return static_cast<std::ptrdiff_t>(i);
            }
        }
        return -1;
    }

    [[nodiscard]] std::uintptr_t RipTarget(const std::uint8_t* a_handler,
        std::size_t a_offset) noexcept
    {
        std::int32_t disp = 0;
        std::memcpy(&disp, a_handler + a_offset + kInstructionLength - 4, sizeof(disp));
        return reinterpret_cast<std::uintptr_t>(a_handler) + a_offset + kInstructionLength +
               static_cast<std::intptr_t>(disp);
    }
}

namespace Platform::CharLightParams
{
    void Bind(std::span<RE::SCRIPT_FUNCTION> a_functions) noexcept
    {
        const std::uint8_t* handler = nullptr;
        for (const RE::SCRIPT_FUNCTION& fn : a_functions) {
            if (fn.shortName != nullptr && ::_stricmp(fn.shortName, "cl") == 0) {
                handler = reinterpret_cast<const std::uint8_t*>(fn.executeFunction);
                break;
            }
        }
        if (handler == nullptr) {
            logger::warn("[CharLight] 'cl' is not in the console function table — rim/fill not "
                         "owned");
            return;
        }

        for (std::size_t i = 0; i < kParamCount; ++i) {
            if (g_value[i].load(std::memory_order_acquire) != nullptr) {
                continue;
            }
            const Param& param = kParams[i];
            if (std::memcmp(handler + param.readOffset, param.readBytes, 4) != 0 ||
                std::memcmp(handler + param.writeOffset, param.writeBytes, 4) != 0) {
                logger::warn("[CharLight] '{}' handler bytes mismatch — REFUSED; `cl` stays "
                             "unblocked so this control keeps its console route",
                    param.command);
                continue;
            }
            const std::uintptr_t readTarget = RipTarget(handler, param.readOffset);
            const std::uintptr_t writeTarget = RipTarget(handler, param.writeOffset);
            if (readTarget != writeTarget) {
                logger::warn("[CharLight] '{}' read/write targets disagree (0x{:X} vs 0x{:X}) — "
                             "REFUSED",
                    param.command, readTarget, writeTarget);
                continue;
            }
            if (!EngineMemory::WithinGameImage(writeTarget, sizeof(float))) {
                logger::warn("[CharLight] '{}' derives 0x{:X}, outside the game image — REFUSED",
                    param.command, writeTarget);
                continue;
            }

            float current = 0.0f;
            (void)EngineMemory::SafeRead(&current, reinterpret_cast<const void*>(writeTarget),
                sizeof(current));
            g_value[i].store(reinterpret_cast<float*>(writeTarget), std::memory_order_release);
            logger::info("[CharLight] {:<7} OWNED — static float derived from 'cl's own bytes, "
                         "engine value {:.4f}; direct writes from here on, no console",
                param.command, current);
        }
    }

    bool Owns(const char* a_command) noexcept
    {
        const std::ptrdiff_t index = IndexOf(a_command);
        return index >= 0 &&
               g_value[static_cast<std::size_t>(index)].load(std::memory_order_acquire) != nullptr;
    }

    bool Write(const char* a_command, float a_value) noexcept
    {
        const std::ptrdiff_t index = IndexOf(a_command);
        if (index < 0) {
            return false;
        }
        float* const target =
            g_value[static_cast<std::size_t>(index)].load(std::memory_order_acquire);
        if (target == nullptr) {
            return false;
        }
        if (!EngineMemory::SafeWrite(target, &a_value, sizeof(a_value))) {
            return false;
        }
        float readBack = 0.0f;
        if (!EngineMemory::SafeRead(&readBack, target, sizeof(readBack)) || readBack != a_value) {
            return false;
        }
        logger::info("[CharLight] {} = {:.4f} — direct write, no console", a_command, a_value);
        return true;
    }
}
