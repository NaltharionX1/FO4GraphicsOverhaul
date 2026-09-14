#include "Platform/MfgKernelJit.h"
#include "Platform/MfgTemporalFix.h"

#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#    define NOMINMAX
#endif
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#ifndef MFG_TEST_DLL
#    error "MFG_TEST_DLL (the staged nvngx_dlssg.dll) was not defined by the build"
#endif

namespace
{
    int g_checks = 0;
    int g_failures = 0;

    void Check(bool a_condition, const char* a_message)
    {
        ++g_checks;
        if (!a_condition) {
            ++g_failures;
            std::cout << "  - FAIL: " << a_message << '\n';
        }
    }

    [[nodiscard]] std::wstring Widen(const char* a_text)
    {
        const int needed = ::MultiByteToWideChar(CP_UTF8, 0, a_text, -1, nullptr, 0);
        std::wstring out(needed > 0 ? static_cast<std::size_t>(needed - 1) : 0, L'\0');
        if (needed > 1) {
            ::MultiByteToWideChar(CP_UTF8, 0, a_text, -1, out.data(), needed);
        }
        return out;
    }

    void JitProgram(Platform::MfgKernelJit::Session& a_session, const char* a_label, const std::vector<char>& a_ptx, const char* a_entry,
        std::uint32_t a_expectedShared)
    {
        Platform::MfgKernelJit::Built built{};
        char why[200]{};
        const bool ok = a_session.Build(a_ptx.data(), a_ptx.size(), a_entry, built, why, sizeof(why));
        char message[240]{};
        std::snprintf(message, sizeof(message), "%s: the driver builds the program for this device and resolves '%s'", a_label, a_entry);
        Check(ok, message);
        if (!ok) {
            std::cout << "    (" << why << ")\n";
            return;
        }
        std::snprintf(message, sizeof(message), "%s: static shared memory is the %u bytes pinned for the role", a_label, a_expectedShared);
        Check(built.sharedBytes == static_cast<int>(a_expectedShared), message);
        std::cout << "  " << a_label << ": built; entry " << a_entry << " resolved, " << built.registers << " registers, " << built.sharedBytes
                  << " B static shared\n";
    }
}

int main()
{
    using namespace Platform;
    using MfgTemporalFix::Program;
    using MfgTemporalFix::Role;

    std::cout << "MfgKernelJitTests: " << MFG_TEST_DLL << '\n';
    Platform::MfgKernelJit::Session session;
    char why[200]{};
    if (!session.Open(why, sizeof(why))) {
        std::cout << "  SKIPPED: " << why << " — the driver-build proof did NOT run here\n";
        std::cout << "MfgKernelJitTests: SKIPPED (no NVIDIA driver / device)\n";
        return EXIT_SUCCESS;
    }
    std::cout << "  device: " << session.DeviceName() << ", compute capability " << session.ComputeMajor() << '.' << session.ComputeMinor()
              << ", driver API " << session.DriverVersion() << '\n';
    if (session.ComputeMajor() != 8 || session.ComputeMinor() != 9) {
        std::cout << "  NOTE: this is not an Ada (sm_89) device; the programs are built for THIS device, which is not the RTX 40 case these programs target\n";
    }

    const std::wstring path = Widen(MFG_TEST_DLL);
    const HMODULE runtime = ::LoadLibraryExW(path.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
    Check(runtime != nullptr, "the staged runtime maps as an image (no code runs)");
    if (runtime == nullptr) {
        std::cout << "MfgKernelJitTests: " << g_failures << " failure(s) of " << g_checks << '\n';
        return EXIT_FAILURE;
    }

    struct Case
    {
        Program program;
        Role role;
        const char* label;
    };
    static constexpr Case kCases[]{
        { Program::kMidpoint, Role::kMotionVector, "midpoint motion-vector program" },
        { Program::kBlackwell, Role::kMotionVector, "Blackwell motion-vector program" },
        { Program::kBlackwell, Role::kInpaint, "Blackwell inpaint program" },
        { Program::kBlackwell, Role::kInpaintDecision, "Blackwell inpaint-decision program" },
    };
    for (const auto& c : kCases) {
        std::vector<char> ptx;
        const char* entry = nullptr;
        std::uint32_t shared = 0;
        const bool extracted = MfgTemporalFix::ExtractProgram(runtime, c.program, c.role, ptx, entry, shared, why, sizeof(why));
        char message[240]{};
        std::snprintf(message, sizeof(message), "%s: rebuilt from the staged runtime", c.label);
        Check(extracted, message);
        if (!extracted) {
            std::cout << "    (reason: " << why << ")\n";
            continue;
        }
        JitProgram(session, c.label, ptx, entry, shared);
    }

    (void)::FreeLibrary(runtime);
    session.Close();
    if (g_failures != 0) {
        std::cout << "MfgKernelJitTests: " << g_failures << " failure(s) of " << g_checks << '\n';
        return EXIT_FAILURE;
    }
    std::cout << "MfgKernelJitTests: all checks passed (" << g_checks << ")\n";
    return EXIT_SUCCESS;
}
