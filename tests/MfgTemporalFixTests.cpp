#include "Platform/MfgTemporalFix.h"

#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#    define NOMINMAX
#endif
#include <windows.h>

#include <cstdlib>
#include <cstring>
#include <iostream>
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

    constexpr std::uint32_t kExpectedConstants = 104;

    [[nodiscard]] std::wstring Widen(const char* a_text)
    {
        const int needed = ::MultiByteToWideChar(CP_UTF8, 0, a_text, -1, nullptr, 0);
        std::wstring out(needed > 0 ? static_cast<std::size_t>(needed - 1) : 0, L'\0');
        if (needed > 1) {
            ::MultiByteToWideChar(CP_UTF8, 0, a_text, -1, out.data(), needed);
        }
        return out;
    }
}

int main()
{
    using namespace Platform;

    const std::wstring path = Widen(MFG_TEST_DLL);
    std::cout << "MfgTemporalFixTests: " << MFG_TEST_DLL << '\n';

    char sha[65]{};
    unsigned long long size = 0;
    const bool hashed = MfgTemporalFix::HashFile(path.c_str(), sha, sizeof(sha), size);
    Check(hashed, "the staged frame-generation runtime can be read and hashed");
    if (!hashed) {
        std::cout << "MfgTemporalFixTests: " << g_failures << " failure(s) of " << g_checks << '\n';
        return EXIT_FAILURE;
    }
    const auto* const build = MfgTemporalFix::FindVerifiedBuild(size, sha);
    Check(build != nullptr, "the staged runtime is one of the verified builds (size + SHA-256)");
    if (build == nullptr) {
        std::cout << "  (staged: " << size << " bytes, sha256 " << sha << ")\n";
        std::cout << "MfgTemporalFixTests: " << g_failures << " failure(s) of " << g_checks << '\n';
        return EXIT_FAILURE;
    }
    std::cout << "  staged build " << build->version << " (" << size << " bytes): expecting " << build->profile
              << ", a " << build->ptxBytes << "-byte PTX, " << build->slots << " descriptor slot(s)\n";

    const HMODULE module = ::LoadLibraryExW(path.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
    Check(module != nullptr, "the staged runtime maps as an image");
    if (module == nullptr) {
        std::cout << "MfgTemporalFixTests: " << g_failures << " failure(s) of " << g_checks << '\n';
        return EXIT_FAILURE;
    }

    const auto probe = MfgTemporalFix::Probe(module);
    Check(probe.ok, "the temporal kernel is found, decompressed, rewritten and the rebuild re-parses");
    if (!probe.ok) {
        std::cout << "  (reason: " << probe.detail << ")\n";
        std::cout << "MfgTemporalFixTests: " << g_failures << " failure(s) of " << g_checks << '\n';
        (void)::FreeLibrary(module);
        return EXIT_FAILURE;
    }
    Check(std::strcmp(probe.profile, build->profile) == 0, "the descriptor family is the pinned build's");
    Check(probe.ptxBytes == build->ptxBytes, "the decompressed sm_89 PTX is the pinned size");
    Check(probe.constants == kExpectedConstants, "all 104 midpoint constants were rewritten");
    Check(probe.slots == build->slots, "every descriptor slot the pinned build carries is found");
    Check(probe.rebuiltBytes != probe.fatbinBytes, "the fatbin was rebuilt, not copied");
    Check(probe.rebuiltBytes > probe.ptxBytes,
        "the rebuild carries the whole uncompressed PTX (plus the entries ahead of it and their headers)");
    std::cout << "  " << probe.detail << '\n';

    const auto applied = MfgTemporalFix::Apply(module);
    Check(applied.ok, "the correction applies to the mapped image");
    if (!applied.ok) {
        std::cout << "  (reason: " << applied.detail << ")\n";
    }
    Check(applied.slots == build->slots, "every pinned slot was redirected (all or nothing)");
    Check(applied.constants == kExpectedConstants, "the applied rebuild rewrote the same 104 constants");

    const auto second = MfgTemporalFix::Apply(module);
    Check(!second.ok && std::strcmp(second.detail, "already applied") == 0,
        "a second apply is refused rather than doubling the patch");

    const auto afterApply = MfgTemporalFix::Probe(module);
    Check(!afterApply.ok, "with the descriptors redirected, the in-image scan no longer finds the kernel");

    MfgTemporalFix::Restore();
    const auto afterRestore = MfgTemporalFix::Probe(module);
    Check(afterRestore.ok, "after Restore the original descriptors are back");
    Check(afterRestore.slots == probe.slots, "the same slot count is found again");
    Check(afterRestore.ptxBytes == probe.ptxBytes && afterRestore.constants == probe.constants,
        "the restored kernel is byte-for-byte the one the probe first read");

    MfgTemporalFix::Restore();

    const auto gates = MfgTemporalFix::ProbeGates(module, build->gateAdvertiseRva, build->gateCapabilityRva);
    Check(gates.ok, "the two architecture-gate compares are found in .text and no other compare against 0x1b0 exists");
    if (!gates.ok) {
        std::cout << "  (reason: " << gates.detail << ")\n";
        std::cout << "MfgTemporalFixTests: " << g_failures << " failure(s) of " << g_checks << '\n';
        (void)::FreeLibrary(module);
        return EXIT_FAILURE;
    }
    std::cout << "  " << gates.detail << '\n';
    Check(gates.advertiseRva == build->gateAdvertiseRva, "the advertising gate is at the RVA pinned for this build");
    Check(gates.capabilityRva == build->gateCapabilityRva, "the capability gate is at the RVA pinned for this build");
    const auto* const image = reinterpret_cast<const std::uint8_t*>(module);
    const std::uint8_t* const advertiseImm = image + build->gateAdvertiseRva + 2;
    const std::uint8_t* const capabilityImm = image + build->gateCapabilityRva + 1;
    Check(*advertiseImm == 0xB0 && *capabilityImm == 0xB0, "both gates compare against 0x1b0 before the rewrite");
    const auto mispinned = MfgTemporalFix::ProbeGates(module, build->gateAdvertiseRva + 1U, build->gateCapabilityRva);
    Check(!mispinned.ok && std::strstr(mispinned.detail, "pins them") != nullptr,
        "a pinned RVA that does not match the scan refuses the gates before anything is written");
    const auto gatesApplied = MfgTemporalFix::ApplyGates(module, build->gateAdvertiseRva, build->gateCapabilityRva);
    Check(gatesApplied.ok && gatesApplied.rewritten == 2, "both gates are rewritten, all or nothing");
    Check(*advertiseImm == 0x90 && *capabilityImm == 0x90, "both gates now compare against 0x190 (Ada)");
    const auto gatesAgain = MfgTemporalFix::ApplyGates(module, build->gateAdvertiseRva, build->gateCapabilityRva);
    Check(!gatesAgain.ok && std::strcmp(gatesAgain.detail, "already applied") == 0, "a second gate apply is refused");
    Check(!MfgTemporalFix::ProbeGates(module, 0, 0).ok, "with the gates rewritten, the pristine scan no longer finds them");
    MfgTemporalFix::RestoreGates();
    Check(*advertiseImm == 0xB0 && *capabilityImm == 0xB0, "restore puts the original compares back");
    Check(MfgTemporalFix::ProbeGates(module, build->gateAdvertiseRva, build->gateCapabilityRva).ok,
        "after restore the gates are found again, at the pinned RVAs");
    MfgTemporalFix::RestoreGates();

    {
        using MfgTemporalFix::Program;
        using MfgTemporalFix::Role;
        const auto bw = MfgTemporalFix::Probe(module, Program::kBlackwell);
        Check(bw.ok, "the three framework kernels are found and their sm_120 programs rebuild for sm_89");
        if (!bw.ok) std::cout << "  (reason: " << bw.detail << ")\n";
        Check(bw.program == Program::kBlackwell && bw.fallbackReason[0] == '\0', "no fallback to the midpoint was needed");
        Check(bw.kernels == 3U, "the Blackwell program touches three kernels");
        Check(bw.slots == build->slots * 3U, "eight descriptor slots per kernel, all found");
        Check(bw.kernelSlots[0] == build->slots && bw.kernelSlots[1] == build->slots && bw.kernelSlots[2] == build->slots,
            "each of the three kernels has exactly the pinned slot count (a sum would hide an uneven split)");
        Check(bw.constants == 0U, "no midpoint constant is rewritten on the Blackwell route");
        std::cout << "  " << bw.detail << '\n';
        const auto bwApplied = MfgTemporalFix::Apply(module, Program::kBlackwell);
        Check(bwApplied.ok && bwApplied.slots == build->slots * 3U && bwApplied.program == Program::kBlackwell,
            "the Blackwell route redirects every slot of all three kernels (all or nothing)");
        Check(!MfgTemporalFix::Probe(module, Program::kMidpoint).ok,
            "with the kernels redirected, the in-image scan no longer finds the motion-vector kernel");
        Check(bwApplied.kernelSlots[0] == build->slots && bwApplied.kernelSlots[1] == build->slots && bwApplied.kernelSlots[2] == build->slots,
            "the apply redirected exactly the pinned slot count of each kernel");
        MfgTemporalFix::Restore();
        const auto again = MfgTemporalFix::Probe(module, Program::kMidpoint);
        Check(again.ok && again.slots == build->slots, "after Restore the midpoint route finds the original descriptors again");
        const auto bwAgain = MfgTemporalFix::Probe(module, Program::kBlackwell);
        Check(bwAgain.ok && bwAgain.slots == build->slots * 3U, "after Restore the Blackwell route finds all three kernels again");
        const auto midpointAfter = MfgTemporalFix::Apply(module, Program::kMidpoint);
        Check(midpointAfter.ok && midpointAfter.slots == build->slots && midpointAfter.program == Program::kMidpoint,
            "the midpoint route applies after a Blackwell apply and restore");
        MfgTemporalFix::Restore();
        Check(MfgTemporalFix::Probe(module, Program::kBlackwell).ok, "and restores cleanly for the Blackwell route again");
        static constexpr Role kRoles[]{ Role::kMotionVector, Role::kInpaint, Role::kInpaintDecision };
        static constexpr std::uint32_t kShared[]{ 7776U, 3920U, 784U };
        for (std::size_t i = 0; i < 3; ++i) {
            std::vector<char> text;
            const char* entry = nullptr;
            std::uint32_t shared = 0;
            char why[200]{};
            const bool extracted = MfgTemporalFix::ExtractProgram(module, Program::kBlackwell, kRoles[i], text, entry, shared, why, sizeof(why));
            Check(extracted, "the Blackwell program text extracts for the role");
            if (!extracted) { std::cout << "  (reason: " << why << ")\n"; continue; }
            const std::string_view ptx(text.data(), text.size());
            Check(shared == kShared[i], "the role's pinned shared-memory size is the fork's discriminant");
            Check(ptx.find("\n.target sm_89") != std::string_view::npos && ptx.find(".target sm_120") == std::string_view::npos,
                "the extracted program targets sm_89 and no longer sm_120");
            Check(ptx.find(std::string(".entry ") + entry + "(") != std::string_view::npos, "the extracted program carries the kernel's entry");
            if (kRoles[i] == Role::kMotionVector) {
                Check(ptx.find("_param_0+32]") != std::string_view::npos, "the Blackwell motion-vector program reads the frame's time itself");
                bool midpointMultiply = false;
                for (std::size_t pos = ptx.find("0f3F000000;"); pos != std::string_view::npos; pos = ptx.find("0f3F000000;", pos + 1)) {
                    std::size_t line = pos;
                    while (line > 0 && ptx[line - 1] != '\n') --line;
                    if (ptx.substr(line, 12) == "mul.ftz.f32 ") midpointMultiply = true;
                }
                Check(!midpointMultiply, "the Blackwell motion-vector program carries no midpoint multiply");
            }
            std::cout << "  " << MfgTemporalFix::RoleName(kRoles[i]) << ": " << entry << ", " << text.size() << " bytes of PTX for the driver\n";
        }
        std::vector<char> midpointText;
        const char* midpointEntry = nullptr;
        std::uint32_t midpointShared = 0;
        char why[200]{};
        const bool midpointExtracted = MfgTemporalFix::ExtractProgram(module, Program::kMidpoint, Role::kMotionVector, midpointText,
            midpointEntry, midpointShared, why, sizeof(why));
        Check(midpointExtracted && std::string_view(midpointText.data(), midpointText.size()).find("sub.ftz.f32 %f136, %f135, %f134;") != std::string_view::npos,
            "the midpoint program text extracts too, carrying the injected temporal sequence");
        Check(!MfgTemporalFix::ExtractProgram(module, Program::kMidpoint, Role::kInpaint, midpointText, midpointEntry, midpointShared, why, sizeof(why)),
            "the midpoint route has no inpaint program to extract");
    }

    (void)::FreeLibrary(module);

    if (g_failures != 0) {
        std::cout << "MfgTemporalFixTests: " << g_failures << " failure(s) of " << g_checks << '\n';
        return EXIT_FAILURE;
    }
    std::cout << "MfgTemporalFixTests: all checks passed (" << g_checks << ")\n";
    return EXIT_SUCCESS;
}
