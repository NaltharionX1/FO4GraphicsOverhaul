#include "PCH.h"

#include "Platform/MfgTemporalFix.h"
#include "Platform/MfgCuModuleProbe.h"

#include <Detours.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <filesystem>

namespace
{
    using LoadLibraryExWFn = HMODULE(WINAPI*)(LPCWSTR, HANDLE, DWORD);

    struct
    {
        std::atomic<bool> installed{ false };
        std::atomic<bool> identityOk{ false };
        std::atomic<bool> applied{ false };
        std::atomic<bool> refused{ false };
        std::atomic<bool> acting{ false };
        std::atomic<bool> sawOther{ false };
        std::atomic<bool> toldNeverFired{ false };
        std::atomic<std::uint32_t> slots{ 0 };
        std::atomic<std::uint32_t> constants{ 0 };
        std::atomic<int> requested{ 0 };
        std::atomic<int> program{ 0 };
        std::atomic<int> appliedProgram{ -1 };
        std::atomic<int> preflight{ 0 };
        std::atomic<DWORD> preflightThread{ 0 };
        std::atomic<std::uint32_t> kernels{ 0 };
        std::atomic<bool> gatesApplied{ false };
        std::atomic<bool> gatesRefused{ false };
        std::atomic<std::uint32_t> gateSites{ 0 };
        std::atomic<const Platform::MfgTemporalFix::VerifiedBuild*> build{ nullptr };
        std::atomic<LoadLibraryExWFn> realLoadLibraryExW{ nullptr };
        wchar_t stagedPath[MAX_PATH]{};
        char reason[200]{};
        char gateReason[200]{};
        char fallbackReason[200]{};
    } g;

    void Say(const char* a_reason) noexcept
    {
        std::snprintf(g.reason, sizeof(g.reason), "%s", a_reason != nullptr ? a_reason : "unknown");
    }

    enum class Pending : int { kNone = 0, kApplied, kRefused, kNote };
    std::atomic<Pending> g_pending{ Pending::kNone };
    std::atomic<Pending> g_gatePending{ Pending::kNone };

    void Note(const char* a_text) noexcept
    {
        Say(a_text);
        g_pending.store(Pending::kNote, std::memory_order_release);
    }

    void Refuse(const char* a_reason) noexcept
    {
        Say(a_reason);
        g.refused.store(true, std::memory_order_relaxed);
        g_pending.store(Pending::kRefused, std::memory_order_release);
    }

    [[nodiscard]] bool PreflightBlackwell(ID3D12Device* a_device, const wchar_t* a_stagedPath, char* a_why, std::size_t a_whySize,
        char* a_summary, std::size_t a_summarySize) noexcept
    {
        using Platform::MfgTemporalFix::Program;
        using Platform::MfgTemporalFix::Role;
        if (a_device == nullptr) {
            std::snprintf(a_why, a_whySize, "no D3D12 device for the pre-flight");
            return false;
        }
        char why[200]{};
        if (!Platform::MfgCuModuleProbe::PtxSupported(a_device, why, sizeof(why))) {
            std::snprintf(a_why, a_whySize, "%s", why);
            return false;
        }
        g.preflightThread.store(::GetCurrentThreadId(), std::memory_order_release);
        const HMODULE image = ::LoadLibraryExW(a_stagedPath, nullptr, DONT_RESOLVE_DLL_REFERENCES);
        g.preflightThread.store(0, std::memory_order_release);
        if (image == nullptr) {
            std::snprintf(a_why, a_whySize, "the staged runtime could not be mapped for the pre-flight (%lu)", ::GetLastError());
            return false;
        }
        static constexpr Role kRoles[]{ Role::kMotionVector, Role::kInpaint, Role::kInpaintDecision };
        std::size_t sizes[3]{};
        bool ok = true;
        for (std::size_t i = 0; i < 3 && ok; ++i) {
            std::vector<std::uint8_t> blob;
            std::vector<std::uint8_t> original;
            const char* entry = nullptr;
            const char* originalEntry = nullptr;
            if (!Platform::MfgTemporalFix::ExtractFatbin(image, Program::kBlackwell, kRoles[i], blob, entry, why, sizeof(why)) ||
                !Platform::MfgTemporalFix::ExtractOriginalFatbin(image, kRoles[i], original, originalEntry, why, sizeof(why))) {
                std::snprintf(a_why, a_whySize, "%s", why);
                ok = false;
                break;
            }
            const std::size_t handed = original.size() > blob.size() ? blob.size() : original.size();
            if (!Platform::MfgCuModuleProbe::Load(a_device, blob.data(), handed, entry, why, sizeof(why))) {
                std::snprintf(a_why, a_whySize, "the %s container: %s", Platform::MfgTemporalFix::RoleName(kRoles[i]), why);
                ok = false;
                break;
            }
            sizes[i] = handed;
        }
        (void)::FreeLibrary(image);
        if (ok) {
            std::snprintf(a_summary, a_summarySize, "containers loaded at their originals' sizes (%zu, %zu and %zu bytes) and their entries resolved on the runtime's device",
                sizes[0], sizes[1], sizes[2]);
        }
        return ok;
    }

    [[nodiscard]] std::filesystem::path SelfModuleDirectory()
    {
        wchar_t buffer[MAX_PATH]{};
        HMODULE module = nullptr;
        if (::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&g), &module) == 0 || module == nullptr) {
            return {};
        }
        ::GetModuleFileNameW(module, buffer, MAX_PATH);
        return std::filesystem::path{ buffer }.parent_path();
    }

    [[nodiscard]] bool IsFrameGenRuntimeName(const wchar_t* a_path) noexcept
    {
        if (a_path == nullptr) {
            return false;
        }
        const wchar_t* name = a_path;
        for (const wchar_t* c = a_path; *c != L'\0'; ++c) {
            if (*c == L'\\' || *c == L'/') {
                name = c + 1;
            }
        }
        static constexpr wchar_t kName[] = L"nvngx_dlssg.dll";
        std::size_t i = 0;
        for (; kName[i] != L'\0'; ++i) {
            const wchar_t ch = name[i];
            if (ch == L'\0') {
                return false;
            }
            const wchar_t lower = (ch >= L'A' && ch <= L'Z') ? static_cast<wchar_t>(ch - L'A' + L'a') : ch;
            if (lower != kName[i]) {
                return false;
            }
        }
        return name[i] == L'\0';
    }

    [[nodiscard]] bool SamePath(const wchar_t* a_left, const wchar_t* a_right) noexcept
    {
        return ::CompareStringOrdinal(a_left, -1, a_right, -1, TRUE) == CSTR_EQUAL;
    }

    void OnRuntimeMapped(HMODULE a_module) noexcept
    {
        if (a_module == nullptr || !g.identityOk.load(std::memory_order_relaxed)) {
            return;
        }
        wchar_t resolved[MAX_PATH]{};
        const DWORD length = ::GetModuleFileNameW(a_module, resolved, MAX_PATH);
        if (length == 0 || length >= MAX_PATH || !SamePath(resolved, g.stagedPath)) {
            if (!g.sawOther.exchange(true, std::memory_order_relaxed)) {
                Note("a frame-generation runtime other than the staged copy was mapped; it was left alone "
                     "(the staged copy is still the one this correction waits for)");
            }
            return;
        }
        if (g.acting.exchange(true, std::memory_order_relaxed)) {
            return;
        }
        auto program = static_cast<Platform::MfgTemporalFix::Program>(g.program.load(std::memory_order_relaxed));
        if (program == Platform::MfgTemporalFix::Program::kBlackwell && g.preflight.load(std::memory_order_acquire) != 1) {
            program = Platform::MfgTemporalFix::Program::kMidpoint;
            g.program.store(static_cast<int>(program), std::memory_order_relaxed);
            if (g.fallbackReason[0] == '\0') {
                std::snprintf(g.fallbackReason, sizeof(g.fallbackReason),
                    "the runtime mapped before the Blackwell pre-flight ran, so the driver never built the programs on this machine");
            }
        }
        const auto report = Platform::MfgTemporalFix::Apply(a_module, program);
        if (!report.ok) {
            Refuse(report.detail);
        } else {
            g.slots.store(report.slots, std::memory_order_relaxed);
            g.constants.store(report.constants, std::memory_order_relaxed);
            g.kernels.store(report.kernels, std::memory_order_relaxed);
            g.appliedProgram.store(static_cast<int>(report.program), std::memory_order_relaxed);
            if (report.fallbackReason[0] != '\0') {
                std::snprintf(g.fallbackReason, sizeof(g.fallbackReason), "%s", report.fallbackReason);
            }
            g.applied.store(true, std::memory_order_relaxed);
            Say(report.detail);
            g_pending.store(Pending::kApplied, std::memory_order_release);
        }

        const auto* const build = g.build.load(std::memory_order_acquire);
        const auto gates = Platform::MfgTemporalFix::ApplyGates(a_module,
            build != nullptr ? build->gateAdvertiseRva : 0U, build != nullptr ? build->gateCapabilityRva : 0U);
        std::snprintf(g.gateReason, sizeof(g.gateReason), "%s", gates.detail);
        if (gates.ok) {
            g.gateSites.store(gates.rewritten, std::memory_order_relaxed);
            g.gatesApplied.store(true, std::memory_order_relaxed);
            g_gatePending.store(Pending::kApplied, std::memory_order_release);
        } else {
            g.gatesRefused.store(true, std::memory_order_relaxed);
            g_gatePending.store(Pending::kRefused, std::memory_order_release);
        }
    }

    [[nodiscard]] bool Mentions(const wchar_t* a_path, const wchar_t* a_needle) noexcept
    {
        if (a_path == nullptr) {
            return false;
        }
        for (const wchar_t* p = a_path; *p != L'\0'; ++p) {
            std::size_t i = 0;
            while (a_needle[i] != L'\0') {
                const wchar_t ch = p[i];
                if (ch == L'\0') {
                    break;
                }
                const wchar_t lower = (ch >= L'A' && ch <= L'Z') ? static_cast<wchar_t>(ch - L'A' + L'a') : ch;
                if (lower != a_needle[i]) {
                    break;
                }
                ++i;
            }
            if (a_needle[i] == L'\0') {
                return true;
            }
        }
        return false;
    }

    void Inspect(HMODULE a_module, const wchar_t* a_requested) noexcept
    {
        if (a_module == nullptr || g.applied.load(std::memory_order_relaxed)) {
            return;
        }
        if (IsFrameGenRuntimeName(a_requested)) {
            OnRuntimeMapped(a_module);
            return;
        }
        if (!Mentions(a_requested, L"nvngx_dlssg")) {
            return;
        }
        wchar_t resolved[MAX_PATH]{};
        const DWORD length = ::GetModuleFileNameW(a_module, resolved, MAX_PATH);
        if (length != 0 && length < MAX_PATH && IsFrameGenRuntimeName(resolved)) {
            OnRuntimeMapped(a_module);
        }
    }

    HMODULE WINAPI Hook_LoadLibraryExW(LPCWSTR a_fileName, HANDLE a_file, DWORD a_flags)
    {
        LoadLibraryExWFn real = g.realLoadLibraryExW.load(std::memory_order_acquire);
        for (int spin = 0; real == nullptr && spin < 1000; ++spin) {
            ::Sleep(0);
            real = g.realLoadLibraryExW.load(std::memory_order_acquire);
        }
        if (real == nullptr) {
            return nullptr;
        }
        HMODULE module = real(a_fileName, a_file, a_flags);
        constexpr DWORD kDataOnly = LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE |
                                    LOAD_LIBRARY_AS_IMAGE_RESOURCE;
        if ((a_flags & kDataOnly) == 0 && g.preflightThread.load(std::memory_order_acquire) != ::GetCurrentThreadId()) {
            Inspect(module, a_fileName);
        }
        return module;
    }

    [[nodiscard]] bool Detour(const char* a_name, void* a_hook, void** a_realOut, char* a_why, std::size_t a_whySize) noexcept
    {
        const HMODULE kernelbase = ::GetModuleHandleW(L"kernelbase.dll");
        if (kernelbase == nullptr) {
            std::snprintf(a_why, a_whySize, "kernelbase.dll is not in the process");
            return false;
        }
        const FARPROC target = ::GetProcAddress(kernelbase, a_name);
        if (target == nullptr) {
            std::snprintf(a_why, a_whySize, "kernelbase.dll has no %s export", a_name);
            return false;
        }
        const auto* const bytes = reinterpret_cast<const std::uint8_t*>(target);
        if (bytes[0] == 0xE8 || bytes[0] == 0xE9) {
            std::snprintf(a_why, a_whySize, "%s already starts with a call/jmp (someone else is there) - not splicing over it", a_name);
            return false;
        }
        if (bytes[0] == 0xFF || (bytes[0] == 0x48 && bytes[1] == 0xFF)) {
            std::snprintf(a_why, a_whySize, "%s is an indirect-jump stub, which cannot be prologue-copied", a_name);
            return false;
        }
        const std::uintptr_t trampoline = Detours::X64::DetourFunction(
            reinterpret_cast<std::uintptr_t>(target), reinterpret_cast<std::uintptr_t>(a_hook));
        if (trampoline == 0) {
            std::snprintf(a_why, a_whySize, "the detour helper refused %s (range or prologue)", a_name);
            return false;
        }
        *a_realOut = reinterpret_cast<void*>(trampoline);
        return true;
    }
}

namespace Platform::MfgTemporalFix
{
    void Install(Program a_program) noexcept
    {
        if (g.installed.load(std::memory_order_relaxed) || g.refused.load(std::memory_order_relaxed)) {
            return;
        }
        g.requested.store(static_cast<int>(a_program), std::memory_order_relaxed);
        g.program.store(static_cast<int>(a_program), std::memory_order_relaxed);
        try {
            const auto self = SelfModuleDirectory();
            if (self.empty()) {
                Refuse("this plugin's own folder could not be resolved, so the staged runtime cannot be pinned");
                logger::warn("[MFG] temporal correction NOT armed: {}", g.reason);
                return;
            }
            const auto staged = self / L"FO4GraphicsOverhaul" / L"Streamline" / L"nvngx_dlssg.dll";
            ::wcsncpy_s(g.stagedPath, staged.c_str(), _TRUNCATE);

            char sha[65]{};
            unsigned long long size = 0;
            if (!HashFile(g.stagedPath, sha, sizeof(sha), size)) {
                Refuse("the staged frame-generation runtime could not be read for its identity");
                logger::warn("[MFG] temporal correction NOT armed: {}", g.reason);
                return;
            }
            const VerifiedBuild* const build = FindVerifiedBuild(size, sha);
            if (build == nullptr) {
                char reason[200]{};
                std::snprintf(reason, sizeof(reason),
                    "the staged frame-generation runtime is not a build this correction is verified against "
                    "(%llu bytes, sha256 %.16s...)", size, sha);
                Refuse(reason);
                logger::warn("[MFG] temporal correction NOT armed: {}", g.reason);
                return;
            }
            g.build.store(build, std::memory_order_release);
            g.identityOk.store(true, std::memory_order_relaxed);

            void* real = nullptr;
            char why[160]{};
            if (!Detour("LoadLibraryExW", reinterpret_cast<void*>(&Hook_LoadLibraryExW), &real, why, sizeof(why))) {
                char reason[200]{};
                std::snprintf(reason, sizeof(reason), "the runtime cannot be caught as it is mapped: %s", why);
                Refuse(reason);
                logger::warn("[MFG] temporal correction NOT armed: {}", g.reason);
                return;
            }
            g.realLoadLibraryExW.store(reinterpret_cast<LoadLibraryExWFn>(real), std::memory_order_release);
            g.installed.store(true, std::memory_order_release);
            logger::info("[MFG] temporal correction armed: the staged nvngx_dlssg.dll is the verified {} build "
                         "({} bytes, {} descriptor family), and it is corrected the moment the NGX core maps it — program: {}{}",
                build->version, size, build->profile, ProgramName(a_program),
                a_program == Program::kBlackwell ? " (the runtime's own sm_120 framework kernels, retargeted for the driver to build for Ada — "
                                                    "the driver's D3D12 module is asked to load them first, on the runtime's device before the NGX session; "
                                                    "the midpoint correction is the fallback)"
                                                  : " (the Ada kernel with its midpoint weights rewritten)");

            const HMODULE already = ::GetModuleHandleW(L"nvngx_dlssg.dll");
            if (already != nullptr) {
                OnRuntimeMapped(already);
            }
        } catch (...) {
            Refuse("an exception while arming the temporal correction");
            logger::warn("[MFG] temporal correction NOT armed: {}", g.reason);
        }
    }

    void PrepareProgram(ID3D12Device* a_device) noexcept
    {
        if (static_cast<Program>(g.program.load(std::memory_order_relaxed)) != Program::kBlackwell ||
            g.preflight.load(std::memory_order_acquire) != 0 || !g.identityOk.load(std::memory_order_relaxed)) {
            return;
        }
        char why[200]{};
        char summary[240]{};
        const ULONGLONG t0 = ::GetTickCount64();
        if (PreflightBlackwell(a_device, g.stagedPath, why, sizeof(why), summary, sizeof(summary))) {
            g.preflight.store(1, std::memory_order_release);
            logger::info("[MFG] Blackwell framework kernels pre-flight: the driver's D3D12 module loaded all three containers on the runtime's "
                         "device in {} ms — {}", ::GetTickCount64() - t0, summary);
        } else {
            std::snprintf(g.fallbackReason, sizeof(g.fallbackReason), "%s", why);
            g.program.store(static_cast<int>(Program::kMidpoint), std::memory_order_relaxed);
            g.preflight.store(2, std::memory_order_release);
            logger::warn("[MFG] Blackwell framework kernels NOT armed: {} — the midpoint correction is armed instead "
                         "for this session (there is no setting; the driver's answer decides)", why);
        }
    }

    void FlushPendingLog() noexcept
    {
        switch (g_pending.exchange(Pending::kNone, std::memory_order_acq_rel)) {
        case Pending::kApplied:
            if (g.appliedProgram.load(std::memory_order_relaxed) == static_cast<int>(Program::kBlackwell)) {
                logger::info("[MFG] Blackwell framework kernels applied to nvngx_dlssg.dll: {} — the driver builds the Blackwell "
                             "programs for Ada when it creates the kernels; the motion-vector program reads the frame's own time "
                             "itself, so no midpoint rewrite is needed", g.reason);
            } else if (g.fallbackReason[0] != '\0') {
                logger::warn("[MFG] Blackwell framework kernels NOT applied ({}); the midpoint correction applied instead: {} — "
                             "the generated frames carry the kernel's own time instead of a fixed midpoint", g.fallbackReason, g.reason);
            } else {
                logger::info("[MFG] temporal correction applied to nvngx_dlssg.dll: {} — the generated frames "
                             "carry the kernel's own time instead of a fixed midpoint", g.reason);
            }
            break;
        case Pending::kRefused:
            logger::warn("[MFG] temporal correction NOT applied: {} — multi-frame generation stays as NVIDIA ships it",
                g.reason);
            break;
        case Pending::kNote:
            logger::info("[MFG] {}", g.reason);
            break;
        case Pending::kNone:
        default:
            break;
        }
        switch (g_gatePending.exchange(Pending::kNone, std::memory_order_acq_rel)) {
        case Pending::kApplied:
            logger::info("[MFG] architecture gates rewritten in nvngx_dlssg.dll: {} — both compares now read Ada's own id "
                         "instead of Blackwell's; whether the runtime then advertises more than 2x is the [NGX] capabilities "
                         "line that follows, not this one", g.gateReason);
            break;
        case Pending::kRefused:
            logger::warn("[MFG] architecture gates NOT rewritten: {} — multi-frame stays as NVIDIA ships it (2x)", g.gateReason);
            break;
        case Pending::kNote:
        case Pending::kNone:
        default:
            break;
        }
        if (g.installed.load(std::memory_order_relaxed) && !g.applied.load(std::memory_order_relaxed) &&
            !g.refused.load(std::memory_order_relaxed) &&
            !g.toldNeverFired.exchange(true, std::memory_order_relaxed)) {
            Say("the frame-generation runtime came up without passing through the loader hook, so nothing was "
                "corrected (multi-frame runs as NVIDIA ships it)");
            logger::warn("[MFG] {}", g.reason);
        }
    }

    State Snapshot() noexcept
    {
        State state{};
        state.installed = g.installed.load(std::memory_order_relaxed);
        state.applied = g.applied.load(std::memory_order_relaxed);
        state.refused = g.refused.load(std::memory_order_relaxed);
        state.slots = g.slots.load(std::memory_order_relaxed);
        state.constants = g.constants.load(std::memory_order_relaxed);
        state.kernels = g.kernels.load(std::memory_order_relaxed);
        std::snprintf(state.program, sizeof(state.program), "%s",
            ProgramName(static_cast<Program>(g.requested.load(std::memory_order_relaxed))));
        if (const int applied = g.appliedProgram.load(std::memory_order_relaxed); applied >= 0) {
            std::snprintf(state.applied_program, sizeof(state.applied_program), "%s", ProgramName(static_cast<Program>(applied)));
        }
        std::snprintf(state.fallbackReason, sizeof(state.fallbackReason), "%s", g.fallbackReason);
        std::snprintf(state.reason, sizeof(state.reason), "%s", g.reason);
        state.gatesApplied = g.gatesApplied.load(std::memory_order_relaxed);
        state.gatesRefused = g.gatesRefused.load(std::memory_order_relaxed);
        state.gateSites = g.gateSites.load(std::memory_order_relaxed);
        std::snprintf(state.gateReason, sizeof(state.gateReason), "%s", g.gateReason);
        if (const auto* const build = g.build.load(std::memory_order_acquire); build != nullptr) {
            std::snprintf(state.build, sizeof(state.build), "%s", build->version);
        }
        return state;
    }
}
