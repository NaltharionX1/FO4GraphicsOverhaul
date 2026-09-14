#pragma once

#include <cstddef>
#include <cstdint>

namespace Platform::MfgKernelJit
{
    struct Built
    {
        int registers{ -1 };
        int sharedBytes{ -1 };
    };

    class Session
    {
    public:
        Session() = default;
        ~Session();
        Session(const Session&) = delete;
        Session& operator=(const Session&) = delete;

        [[nodiscard]] bool Open(char* a_why, std::size_t a_whySize) noexcept;
        void Close() noexcept;
        [[nodiscard]] bool IsOpen() const noexcept { return m_context != nullptr; }
        [[nodiscard]] const char* DeviceName() const noexcept { return m_name; }
        [[nodiscard]] int ComputeMajor() const noexcept { return m_major; }
        [[nodiscard]] int ComputeMinor() const noexcept { return m_minor; }
        [[nodiscard]] int DriverVersion() const noexcept { return m_driverVersion; }

        [[nodiscard]] bool Build(const char* a_ptx, std::size_t a_ptxBytes, const char* a_entry, Built& a_out, char* a_why,
            std::size_t a_whySize) noexcept;

    private:
        void* m_module{ nullptr };
        void* m_context{ nullptr };
        char m_name[128]{};
        int m_major{ 0 };
        int m_minor{ 0 };
        int m_driverVersion{ 0 };
        void* m_fn[12]{};
    };
}
