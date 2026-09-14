#include "PCH.h"

#include "Platform/ConsoleCommand.h"

#include "RE/Bethesda/Console.h"
#include "RE/Bethesda/IMenu.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace
{
    void LogExecutedThrottled(const std::string& a_command, bool a_hasReply,
        const std::string& a_reply = {})
    {
        static std::mutex mutex;
        static std::string lastCommand;
        static std::uint64_t repeats = 0;
        const std::scoped_lock lock(mutex);
        if (a_command == lastCommand) {
            ++repeats;
            if ((repeats % 250U) != 0U) {
                return;
            }
            logger::info("[Console] '{}' executed {} more times (repeats collapsed)", a_command,
                repeats);
            return;
        }
        if (repeats != 0U) {
            logger::info("[Console] '{}' repeated {} time(s) in total", lastCommand, repeats);
        }
        lastCommand = a_command;
        repeats = 0;
        if (a_hasReply) {
            logger::info("[Console] executed '{}' — engine replied: {}", a_command, a_reply);
        } else {
            logger::info("[Console] executed '{}'", a_command);
        }
    }

    constexpr std::size_t kMaxCommand = 128U;

    [[nodiscard]] std::size_t ConsoleBufferSize() noexcept
    {
        try {
            if (auto* const log = RE::ConsoleLog::GetSingleton()) {
                return static_cast<std::size_t>(log->buffer.size());
            }
        } catch (...) {
        }
        return 0U;
    }

    [[nodiscard]] std::string ReadConsoleTail(std::size_t a_fromOffset) noexcept
    {
        try {
            auto* const log = RE::ConsoleLog::GetSingleton();
            if (!log) {
                return {};
            }
            const char* const data = log->buffer.c_str();
            const auto size = static_cast<std::size_t>(log->buffer.size());
            if (!data || size <= a_fromOffset) {
                return {};
            }
            std::string tail{ data + a_fromOffset, size - a_fromOffset };
            while (!tail.empty() && (tail.back() == '\n' || tail.back() == '\r')) {
                tail.pop_back();
            }
            return tail;
        } catch (...) {
            return {};
        }
    }

    [[nodiscard]] std::string StripEcho(std::string a_tail, const std::string& a_command) noexcept
    {
        try {
            const auto trim = [](std::string& text) noexcept {
                const auto notSpace = [](unsigned char c) noexcept {
                    return c != ' ' && c != '\t' && c != '\n' && c != '\r';
                };
                while (!text.empty() && !notSpace(static_cast<unsigned char>(text.front()))) {
                    text.erase(text.begin());
                }
                while (!text.empty() && !notSpace(static_cast<unsigned char>(text.back()))) {
                    text.pop_back();
                }
            };
            trim(a_tail);
            if (a_tail.size() >= a_command.size() &&
                a_tail.compare(0U, a_command.size(), a_command) == 0) {
                a_tail.erase(0U, a_command.size());
                trim(a_tail);
            }
            return a_tail;
        } catch (...) {
            return {};
        }
    }

    [[nodiscard]] bool QueueImpl(const char* a_command) noexcept
    {
        if (!a_command || a_command[0] == '\0') {
            return false;
        }
        std::size_t length = 0U;
        while (length <= kMaxCommand && a_command[length] != '\0') {
            ++length;
        }
        if (length > kMaxCommand) {
            logger::warn("[Console] refusing a {}+ character command — over the {} cap", length,
                kMaxCommand);
            return false;
        }
        const auto* const tasks = F4SE::GetTaskInterface();
        if (!tasks) {
            logger::warn("[Console] task interface unavailable — '{}' NOT issued", a_command);
            return false;
        }
        try {
            std::string command{ a_command, length };
            tasks->AddTask([command]() noexcept {
                try {
                    const std::size_t before = ConsoleBufferSize();
                    RE::Console::ExecuteCommand(command.c_str());
                    const std::string reply = StripEcho(ReadConsoleTail(before), command);
                    if (reply.empty()) {
                        LogExecutedThrottled(command, false);
                        logger::debug("[Console] executed '{}' — accepted (echo only, no complaint)",
                            command);
                    } else {
                        LogExecutedThrottled(command, true, reply);
                    }
                } catch (...) {
                    logger::error("[Console] executing '{}' threw — command dropped", command);
                }
            });
        } catch (...) {
            logger::warn("[Console] queuing '{}' threw — not issued (fail-open)", a_command);
            return false;
        }
        static std::atomic<std::uint64_t> queuedCount{ 0 };
        const auto queuedSoFar = queuedCount.fetch_add(1, std::memory_order_relaxed) + 1U;
        if (queuedSoFar <= 5U || (queuedSoFar % 250U) == 0U) {
            logger::info("[Console] queued '{}' (#{} this session)", a_command, queuedSoFar);
        }
        return true;
    }

}

namespace Platform
{
    bool ConsoleCommand::Queue(const char* a_command) noexcept
    {
        return QueueImpl(a_command);
    }

}
