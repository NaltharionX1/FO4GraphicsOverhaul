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
#include <exception>
#include <iostream>
#include <new>
#include <string>
#include <string_view>

#ifndef MFG_TEST_DLL
#    error "MFG_TEST_DLL (the staged nvngx_dlssg.dll) was not defined by the build"
#endif

namespace
{
    int g_checks = 0;
    int g_failures = 0;

    void Check(bool a_condition, std::string_view a_message)
    {
        ++g_checks;
        if (!a_condition) {
            ++g_failures;
            std::cout << "  - FAIL: " << a_message << '\n';
        }
    }

    unsigned g_allocationCalls = 0;
    unsigned g_failAllocationAt = 0;

    unsigned g_protectCalls = 0;
    unsigned g_failProtectAt = 0;
    unsigned g_failProtectAlso = 0;

    void ArmNothing()
    {
        g_allocationCalls = 0;
        g_failAllocationAt = 0;
        g_protectCalls = 0;
        g_failProtectAt = 0;
        g_failProtectAlso = 0;
    }
}

void* operator new(std::size_t a_size)
{
    ++g_allocationCalls;
    if (g_failAllocationAt != 0 && g_allocationCalls == g_failAllocationAt) {
        g_failAllocationAt = 0;
        throw std::bad_alloc{};
    }
    if (void* const memory = std::malloc(a_size == 0 ? 1 : a_size)) {
        return memory;
    }
    throw std::bad_alloc{};
}

void operator delete(void* a_memory) noexcept { std::free(a_memory); }
void operator delete(void* a_memory, std::size_t) noexcept { std::free(a_memory); }

static BOOL WINAPI ShimVirtualProtect(LPVOID a_address, SIZE_T a_size, DWORD a_protect, PDWORD a_old)
{
    ++g_protectCalls;
    if ((g_failProtectAt != 0 && g_protectCalls == g_failProtectAt) ||
        (g_failProtectAlso != 0 && g_protectCalls == g_failProtectAlso)) {
        ::SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    return ::VirtualProtect(a_address, a_size, a_protect, a_old);
}

#define VirtualProtect ShimVirtualProtect
#include "Platform/MfgTemporalFix.cpp"
#undef VirtualProtect

namespace
{
    std::uint32_t g_expectedSlots = 0;
    const Platform::MfgTemporalFix::VerifiedBuild* g_build = nullptr;

    [[nodiscard]] std::wstring Widen(const char* a_text)
    {
        const int needed = ::MultiByteToWideChar(CP_UTF8, 0, a_text, -1, nullptr, 0);
        std::wstring out(needed > 0 ? static_cast<std::size_t>(needed - 1) : 0, L'\0');
        if (needed > 1) {
            ::MultiByteToWideChar(CP_UTF8, 0, a_text, -1, out.data(), needed);
        }
        return out;
    }

    [[nodiscard]] bool ModuleIsPristine(HMODULE a_module)
    {
        ArmNothing();
        const auto probe = Platform::MfgTemporalFix::Probe(a_module);
        const auto blackwell = Platform::MfgTemporalFix::Probe(a_module, Platform::MfgTemporalFix::Program::kBlackwell);
        return probe.ok && probe.slots == g_expectedSlots && blackwell.ok && blackwell.slots == g_expectedSlots * 3U &&
               blackwell.fallbackReason[0] == '\0';
    }
}

int main()
{
    using namespace Platform;

    std::set_terminate([]() noexcept {
        std::puts("  - FAIL: an exception escaped the correction and reached std::terminate");
        std::puts("MfgTemporalFixFaultTests: FAILED (terminate)");
        std::fflush(stdout);
        ::ExitProcess(EXIT_FAILURE);
    });

    const std::wstring path = Widen(MFG_TEST_DLL);
    const HMODULE module = ::LoadLibraryExW(path.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (module == nullptr) {
        std::cout << "  - FAIL: the staged runtime did not map\nMfgTemporalFixFaultTests: 1 failure(s)\n";
        return EXIT_FAILURE;
    }

    {
        char sha[65]{};
        unsigned long long size = 0;
        const auto* const build = MfgTemporalFix::HashFile(path.c_str(), sha, sizeof(sha), size)
                                      ? MfgTemporalFix::FindVerifiedBuild(size, sha)
                                      : nullptr;
        if (build == nullptr) {
            std::cout << "  - FAIL: the staged runtime is not a verified build\nMfgTemporalFixFaultTests: 1 failure(s)\n";
            (void)::FreeLibrary(module);
            return EXIT_FAILURE;
        }
        g_expectedSlots = build->slots;
        g_build = build;
    }

    Check(ModuleIsPristine(module), "the module starts pristine");

    unsigned allocationsSeen = 0;
    {
        ArmNothing();
        const auto baseline = MfgTemporalFix::Apply(module);
        allocationsSeen = g_allocationCalls;
        Check(baseline.ok && baseline.slots == g_expectedSlots, "the baseline apply redirects every pinned slot");
        MfgTemporalFix::Restore();
        Check(ModuleIsPristine(module), "restore puts the module back");
    }

    unsigned allocationRefusals = 0;
    for (unsigned at = 1; at <= allocationsSeen; ++at) {
        ArmNothing();
        g_failAllocationAt = at;
        const auto report = MfgTemporalFix::Apply(module);
        if (!report.ok) {
            ++allocationRefusals;
        } else {
            MfgTemporalFix::Restore();
        }
        if (!ModuleIsPristine(module)) {
            std::cout << "  (allocation #" << at << " left the module modified: " << report.detail << ")\n";
            Check(false, "an allocation failure never leaves a half-redirected kernel");
            break;
        }
    }
    Check(allocationRefusals > 0, "failing an allocation makes the correction refuse rather than crash");
    std::cout << "  allocation sweep: " << allocationsSeen << " site(s), " << allocationRefusals
              << " refusal(s), the module pristine after every one\n";

    unsigned protectsSeen = 0;
    {
        ArmNothing();
        const auto baseline = MfgTemporalFix::Apply(module);
        protectsSeen = g_protectCalls;
        Check(baseline.ok, "the baseline apply succeeds (protection sweep setup)");
        MfgTemporalFix::Restore();
    }

    unsigned protectRefusals = 0;
    for (unsigned at = 1; at <= protectsSeen; ++at) {
        ArmNothing();
        g_failProtectAt = at;
        const auto report = MfgTemporalFix::Apply(module);
        if (!report.ok) {
            ++protectRefusals;
        } else {
            ArmNothing();
            MfgTemporalFix::Restore();
        }
        ArmNothing();
        MfgTemporalFix::Restore();
        if (!ModuleIsPristine(module)) {
            std::cout << "  (protection call #" << at << " left the module modified: " << report.detail << ")\n";
            Check(false, "a protection failure never leaves a half-redirected kernel");
            break;
        }
    }
    Check(protectRefusals > 0, "failing a page-protection call makes the correction refuse");
    std::cout << "  protection sweep: " << protectsSeen << " call(s), " << protectRefusals
              << " refusal(s), the module pristine after every one\n";

    {
        ArmNothing();
        const auto applied = MfgTemporalFix::Apply(module);
        Check(applied.ok, "apply succeeds before the restore-failure case");
        ArmNothing();
        g_failProtectAt = 1;
        MfgTemporalFix::Restore();
        Check(!ModuleIsPristine(module), "a stuck descriptor leaves the module still redirected, as it must");
        ArmNothing();
        MfgTemporalFix::Restore();
        Check(ModuleIsPristine(module), "a later restore completes the revert");
    }

    {
        using MfgTemporalFix::Program;
        unsigned seen = 0;
        {
            ArmNothing();
            const auto baseline = MfgTemporalFix::Apply(module, Program::kBlackwell);
            seen = g_allocationCalls;
            Check(baseline.ok && baseline.slots == g_expectedSlots * 3U && baseline.program == Program::kBlackwell,
                "the baseline Blackwell apply redirects all three kernels' slots");
            MfgTemporalFix::Restore();
            Check(ModuleIsPristine(module), "restore puts the module back after the Blackwell apply");
        }
        unsigned refusals = 0;
        for (unsigned at = 1; at <= seen; ++at) {
            ArmNothing();
            g_failAllocationAt = at;
            const auto report = MfgTemporalFix::Apply(module, Program::kBlackwell);
            ArmNothing();
            if (report.ok) {
                MfgTemporalFix::Restore();
            } else {
                ++refusals;
            }
            if (!ModuleIsPristine(module)) {
                Check(false, "an allocation failure never leaves a half-redirected kernel (Blackwell route)");
                MfgTemporalFix::Restore();
            }
        }
        Check(refusals > 0, "failing an allocation makes the Blackwell route refuse rather than crash");
        std::cout << "  Blackwell allocation sweep: " << seen << " allocation(s), " << refusals << " refusal(s), the module pristine after every one\n";

        unsigned protects = 0;
        {
            ArmNothing();
            const auto baseline = MfgTemporalFix::Apply(module, Program::kBlackwell);
            protects = g_protectCalls;
            Check(baseline.ok, "the baseline Blackwell apply succeeds (protection sweep setup)");
            MfgTemporalFix::Restore();
        }
        unsigned protectRefusalsBw = 0;
        for (unsigned at = 1; at <= protects; ++at) {
            ArmNothing();
            g_failProtectAt = at;
            const auto report = MfgTemporalFix::Apply(module, Program::kBlackwell);
            ArmNothing();
            if (!report.ok) {
                ++protectRefusalsBw;
            }
            MfgTemporalFix::Restore();
            if (!ModuleIsPristine(module)) {
                Check(false, "a protection failure never leaves a half-redirected kernel (Blackwell route)");
            }
        }
        Check(protectRefusalsBw > 0, "failing a page-protection call makes the Blackwell route refuse");
        std::cout << "  Blackwell protection sweep: " << protects << " call(s), " << protectRefusalsBw << " refusal(s), the module pristine after every one\n";
    }

    {
        const auto* const image = reinterpret_cast<const std::uint8_t*>(module);
        const std::uint8_t* const advertiseImm = image + g_build->gateAdvertiseRva + 2;
        const std::uint8_t* const capabilityImm = image + g_build->gateCapabilityRva + 1;
        const auto pristineGates = [&]() { return *advertiseImm == 0xB0 && *capabilityImm == 0xB0; };
        Check(pristineGates(), "the gates start pristine");

        ArmNothing();
        const auto baseline = MfgTemporalFix::ApplyGates(module, g_build->gateAdvertiseRva, g_build->gateCapabilityRva);
        const unsigned gateProtects = g_protectCalls;
        Check(baseline.ok && baseline.rewritten == 2, "the baseline gate apply rewrites both sites");
        Check(gateProtects == 4, "a clean gate apply is exactly four protection calls (make writable + restore, per site)");
        ArmNothing();
        MfgTemporalFix::RestoreGates();
        Check(pristineGates(), "restore puts both gates back");

        unsigned gateRefusals = 0;
        for (unsigned at = 1; at <= gateProtects; ++at) {
            ArmNothing();
            g_failProtectAt = at;
            const auto report = MfgTemporalFix::ApplyGates(module, g_build->gateAdvertiseRva, g_build->gateCapabilityRva);
            if (!report.ok) {
                ++gateRefusals;
                Check(!(*advertiseImm == 0x90 && *capabilityImm == 0xB0), "a refused gate apply never advertises without the capability");
            }
            ArmNothing();
            MfgTemporalFix::RestoreGates();
            if (!pristineGates()) {
                std::cout << "  (gate protection call #" << at << " left the gates modified: " << report.detail << ")\n";
                Check(false, "a protection failure never leaves the gates modified after restore");
                break;
            }
        }
        Check(gateRefusals > 0, "failing a protection call makes the gate apply refuse");

        ArmNothing();
        g_failProtectAt = 3;
        g_failProtectAlso = 4;
        const auto stuck = MfgTemporalFix::ApplyGates(module, g_build->gateAdvertiseRva, g_build->gateCapabilityRva);
        Check(!stuck.ok && std::strstr(stuck.detail, "INCOMPLETELY") != nullptr, "a failed rollback is reported as incomplete, never as clean");
        Check(*capabilityImm == 0x90 && *advertiseImm == 0xB0, "the incomplete state is capable-but-advertising-2x, the harmless one");
        ArmNothing();
        const auto afterStuck = MfgTemporalFix::ApplyGates(module, g_build->gateAdvertiseRva, g_build->gateCapabilityRva);
        Check(!afterStuck.ok && std::strstr(afterStuck.detail, "could not put it back") != nullptr,
            "with a site stuck, another apply refuses rather than writing on top");
        MfgTemporalFix::RestoreGates();
        Check(pristineGates(), "RestoreGates finishes the rollback the apply could not");
        std::cout << "  gate sweep: " << gateProtects << " protection call(s), " << gateRefusals
                  << " refusal(s), the gates pristine after every one; the stuck-rollback case recovered\n";
    }

    ArmNothing();
    (void)::FreeLibrary(module);

    if (g_failures != 0) {
        std::cout << "MfgTemporalFixFaultTests: " << g_failures << " failure(s) of " << g_checks << '\n';
        return EXIT_FAILURE;
    }
    std::cout << "MfgTemporalFixFaultTests: all checks passed (" << g_checks << ")\n";
    return EXIT_SUCCESS;
}
