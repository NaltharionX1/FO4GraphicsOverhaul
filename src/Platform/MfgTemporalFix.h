// SPDX-License-Identifier: GPL-3.0-or-later
// Portions ported from MFGAdaUnlock-RenoDx, Copyright (c) 2026 ImDreamt, MIT License.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

struct ID3D12Device;

namespace Platform::MfgTemporalFix
{
    struct VerifiedBuild
    {
        unsigned long long size;
        const char* sha256;
        const char* version;
        const char* profile;
        std::size_t ptxBytes;
        std::uint32_t slots;
        std::uint32_t gateAdvertiseRva;
        std::uint32_t gateCapabilityRva;
    };
    inline constexpr VerifiedBuild kVerifiedBuilds[]{
        { 7460976ULL, "ff6e90eb78b827927dff5b4ecc6b1c870c2e9bca29ed9f48c7d348cc9e170b82", "310.9.1.0", "EstimateIntermMvecsScatter", 99626, 8, 0x016862U, 0x035E4FU },
    };
    [[nodiscard]] inline const VerifiedBuild* FindVerifiedBuild(unsigned long long a_size, const char* a_sha256) noexcept
    {
        if (a_sha256 == nullptr) {
            return nullptr;
        }
        for (const auto& build : kVerifiedBuilds) {
            if (build.size == a_size && std::strcmp(build.sha256, a_sha256) == 0) {
                return &build;
            }
        }
        return nullptr;
    }

    enum class Program : int
    {
        kMidpoint = 0,
        kBlackwell = 1,
    };
    enum class Role : int
    {
        kMotionVector = 0,
        kInpaint = 1,
        kInpaintDecision = 2,
    };
    inline constexpr std::size_t kRoleCount = 3;
    [[nodiscard]] const char* ProgramName(Program a_program) noexcept;
    [[nodiscard]] const char* RoleName(Role a_role) noexcept;

    struct Report
    {
        bool ok{ false };
        Program program{ Program::kMidpoint };
        std::uint32_t kernels{ 0 };
        char profile[40]{};
        std::size_t ptxBytes{ 0 };
        std::size_t fatbinBytes{ 0 };
        std::size_t rebuiltBytes{ 0 };
        std::uint32_t slots{ 0 };
        std::uint32_t kernelSlots[3]{};
        std::uint32_t constants{ 0 };
        char fallbackReason[200]{};
        char detail[200]{};
    };

    [[nodiscard]] Report Probe(void* a_module, Program a_program = Program::kMidpoint) noexcept;
    [[nodiscard]] Report Apply(void* a_module, Program a_program = Program::kMidpoint) noexcept;
    void Restore() noexcept;
    [[nodiscard]] bool ExtractProgram(void* a_module, Program a_program, Role a_role, std::vector<char>& a_ptxOut,
        const char*& a_entryName, std::uint32_t& a_sharedBytes, char* a_why, std::size_t a_whySize) noexcept;
    [[nodiscard]] bool ExtractFatbin(void* a_module, Program a_program, Role a_role, std::vector<std::uint8_t>& a_out,
        const char*& a_entryName, char* a_why, std::size_t a_whySize) noexcept;
    [[nodiscard]] bool ExtractOriginalFatbin(void* a_module, Role a_role, std::vector<std::uint8_t>& a_out,
        const char*& a_entryName, char* a_why, std::size_t a_whySize) noexcept;

    struct GateReport
    {
        bool ok{ false };
        std::uint32_t advertiseRva{ 0 };
        std::uint32_t capabilityRva{ 0 };
        std::uint32_t rewritten{ 0 };
        char detail[200]{};
    };
    [[nodiscard]] GateReport ProbeGates(void* a_module, std::uint32_t a_expectedAdvertiseRva,
        std::uint32_t a_expectedCapabilityRva) noexcept;
    [[nodiscard]] GateReport ApplyGates(void* a_module, std::uint32_t a_expectedAdvertiseRva,
        std::uint32_t a_expectedCapabilityRva) noexcept;
    void RestoreGates() noexcept;
    [[nodiscard]] bool HashFile(const wchar_t* a_path, char* a_sha256Out, std::size_t a_outSize,
        unsigned long long& a_size) noexcept;

    void Install(Program a_program) noexcept;
    void PrepareProgram(ID3D12Device* a_device) noexcept;
    void FlushPendingLog() noexcept;

    struct State
    {
        bool installed{ false };
        bool applied{ false };
        bool refused{ false };
        std::uint32_t slots{ 0 };
        std::uint32_t constants{ 0 };
        char program[16]{};
        char applied_program[16]{};
        std::uint32_t kernels{ 0 };
        char fallbackReason[200]{};
        char reason[200]{};
        bool gatesApplied{ false };
        bool gatesRefused{ false };
        std::uint32_t gateSites{ 0 };
        char gateReason[200]{};
        char build[16]{};
    };
    [[nodiscard]] State Snapshot() noexcept;
}
