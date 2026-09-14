#pragma once
#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include <fmt/format.h>

namespace logger
{
    inline std::mutex g_logMutex;

    template <class... T>
    void info(fmt::format_string<T...> a_format, T&&... a_args)
    {
        const std::scoped_lock lock(g_logMutex);
        const auto text = fmt::format(a_format, std::forward<T>(a_args)...);
        std::printf("  [log] %s\n", text.c_str());
        std::fflush(stdout);
    }
    template <class... T>
    void warn(fmt::format_string<T...> a_format, T&&... a_args) { info(a_format, std::forward<T>(a_args)...); }
    template <class... T>
    void error(fmt::format_string<T...> a_format, T&&... a_args) { info(a_format, std::forward<T>(a_args)...); }
    template <class... T>
    void debug(fmt::format_string<T...> a_format, T&&... a_args) { info(a_format, std::forward<T>(a_args)...); }
    template <class... T>
    void trace(fmt::format_string<T...> a_format, T&&... a_args) { info(a_format, std::forward<T>(a_args)...); }
}
