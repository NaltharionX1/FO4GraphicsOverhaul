#pragma once

#include <atomic>
#include <cstdint>
#include <exception>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <typeinfo>

namespace Core::FailOpen
{
    namespace detail
    {
        struct Latch
        {
            std::atomic<bool> tripped{ false };
            std::atomic<bool> logged{ false };
        };

        class Registry
        {
        public:
            [[nodiscard]] static Registry& Get() noexcept
            {
                static Registry instance;
                return instance;
            }

            [[nodiscard]] Latch& Find(std::string_view a_name)
            {
                std::scoped_lock lock(mutex_);
                const auto it = latches_.find(a_name);
                if (it != latches_.end()) {
                    return it->second;
                }
                return latches_.try_emplace(std::string(a_name)).first->second;
            }

        private:
            std::mutex mutex_;
            std::map<std::string, Latch, std::less<>> latches_;
        };
    }

    [[nodiscard]] inline bool Tripped(std::string_view a_name) noexcept
    {
        try {
            return detail::Registry::Get().Find(a_name).tripped.load(std::memory_order_acquire);
        } catch (...) {
            return true;
        }
    }

    inline void Trip(std::string_view a_name, std::string_view a_reason) noexcept
    {
        try {
            auto& latch = detail::Registry::Get().Find(a_name);
            latch.tripped.store(true, std::memory_order_release);
            if (!latch.logged.exchange(true, std::memory_order_acq_rel)) {
                logger::error("[FailOpen] '{}' tripped: {}", a_name, a_reason);
            }
        } catch (...) {
        }
    }

    namespace Guard
    {
        inline std::atomic<std::uint32_t> catchTotal{ 0 };
    }
}

#define GUARD_BEGIN try {
#define GUARD_END_TRIP(site, latch, reason)                                                                 \
    }                                                                                                        \
    catch (const std::exception& e) {                                                                       \
        ::Core::FailOpen::Guard::catchTotal.fetch_add(1, std::memory_order_relaxed);                         \
        ::Core::FailOpen::Trip(latch, reason);                                                               \
        static std::atomic<int> _guardCount{ 0 };                                                            \
        if (_guardCount.fetch_add(1, std::memory_order_relaxed) < 3) {                                       \
            logger::error("[Guard] " site " caught {}: {}", typeid(e).name(), e.what());                     \
        }                                                                                                     \
    }                                                                                                        \
    catch (...) {                                                                                            \
        ::Core::FailOpen::Guard::catchTotal.fetch_add(1, std::memory_order_relaxed);                         \
        ::Core::FailOpen::Trip(latch, reason);                                                               \
        static std::atomic<int> _guardCount2{ 0 };                                                           \
        if (_guardCount2.fetch_add(1, std::memory_order_relaxed) < 3) {                                      \
            logger::error("[Guard] " site " caught an unknown C++ exception");                               \
        }                                                                                                     \
    }
#define GUARD_END(site)                                                                                     \
    }                                                                                                        \
    catch (const std::exception& e) {                                                                       \
        ::Core::FailOpen::Guard::catchTotal.fetch_add(1, std::memory_order_relaxed);                         \
        static std::atomic<int> _guardCount{ 0 };                                                            \
        if (_guardCount.fetch_add(1, std::memory_order_relaxed) < 3) {                                       \
            logger::error("[Guard] " site " caught {}: {}", typeid(e).name(), e.what());                     \
        }                                                                                                     \
    }                                                                                                        \
    catch (...) {                                                                                            \
        ::Core::FailOpen::Guard::catchTotal.fetch_add(1, std::memory_order_relaxed);                         \
        static std::atomic<int> _guardCount2{ 0 };                                                           \
        if (_guardCount2.fetch_add(1, std::memory_order_relaxed) < 3) {                                      \
            logger::error("[Guard] " site " caught an unknown C++ exception");                               \
        }                                                                                                     \
    }
