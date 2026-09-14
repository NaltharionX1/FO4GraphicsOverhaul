#include "PCH.h"

#include "Platform/NgxD3D12.h"

#include "Platform/MfgTemporalFix.h"

#include <bcrypt.h>
#include <softpub.h>
#include <wintrust.h>

#include <atomic>
#include <cstdio>
#include <cstring>

namespace
{
    using namespace Platform;
    using namespace Platform::NgxD3D12;

    struct CoreState
    {
        HMODULE module{ nullptr };
        bool ownsModule{ false };
        bool faulted{ false };
        ID3D12Device* sessionDevice{ nullptr };
        int sdkVersion{ 0 };
        Ngx::PFN_D3D12_Init_ProjectID initProjectId{ nullptr };
        Ngx::PFN_D3D12_Init_Ext initExt{ nullptr };
        Ngx::PFN_D3D12_AllocateParameters allocateParameters{ nullptr };
        Ngx::PFN_D3D12_DestroyParameters destroyParameters{ nullptr };
        Ngx::PFN_D3D12_GetCapabilityParameters getCapabilityParameters{ nullptr };
        Ngx::PFN_D3D12_Shutdown shutdown{ nullptr };
        Ngx::PFN_D3D12_Shutdown1 shutdown1{ nullptr };
        Ngx::PFN_D3D12_CreateFeature createFeature{ nullptr };
        Ngx::PFN_D3D12_EvaluateFeature evaluateFeature{ nullptr };
        Ngx::PFN_D3D12_ReleaseFeature releaseFeature{ nullptr };
        std::uint64_t sessionSerial{ 0 };
        int superSamplingAvailable{ -1 };
        int frameGenerationAvailable{ -1 };
        wchar_t pathList[MAX_PATH]{};
        const wchar_t* pathListEntries[1]{};
        Ngx::FeatureCommonInfo commonInfo{};
    };

    struct SnippetState
    {
        HMODULE module{ nullptr };
        HMODULE shim{ nullptr };
        bool initialised{ false };
        bool faulted{ false };
        Ngx::PFN_Snippet_Init_Ext initExt{ nullptr };
        Ngx::PFN_D3D12_CreateFeature createFeature{ nullptr };
        Ngx::PFN_D3D12_EvaluateFeature evaluateFeature{ nullptr };
        Ngx::PFN_D3D12_ReleaseFeature releaseFeature{ nullptr };
        Ngx::PFN_Shim_CallInit shimInit{ nullptr };
        Ngx::PFN_Shim_CallCreate shimCreate{ nullptr };
        Ngx::PFN_Shim_CallEvaluate shimEvaluate{ nullptr };
        Ngx::PFN_Shim_CallRelease shimRelease{ nullptr };
        wchar_t pathList[MAX_PATH]{};
        const wchar_t* pathListEntries[1]{};
        Ngx::FeatureCommonInfo commonInfo{};
        bool identityKnown{ false };
        SnippetIdentity identity{};
    };

    CoreState g_core;
    SnippetState g_snippet;

    [[nodiscard]] std::string Narrow(const wchar_t* a_text)
    {
        if (a_text == nullptr || *a_text == L'\0') {
            return {};
        }
        const int needed = ::WideCharToMultiByte(CP_UTF8, 0, a_text, -1, nullptr, 0, nullptr, nullptr);
        if (needed <= 1) {
            return {};
        }
        std::string out(static_cast<std::size_t>(needed - 1), '\0');
        ::WideCharToMultiByte(CP_UTF8, 0, a_text, -1, out.data(), needed, nullptr, nullptr);
        return out;
    }

    constexpr unsigned long long kAppId = 0x4F34474FULL;
    constexpr const char* kProjectId = "7c1c5e0a-4f0d-4c56-9a6b-3f8a0d3b2e11";
    constexpr const char* kEngineVersion = "FO4GraphicsOverhaul";

    std::atomic<std::uint32_t> g_ngxLogLines{ 0 };
    std::atomic<std::uint32_t> g_ngxVerboseLines{ 0 };

    void __cdecl NgxLogCallback(const char* a_message, Ngx::LoggingLevel a_level, int a_feature)
    {
        if (a_message == nullptr) {
            return;
        }
        const bool verbose = a_level == Ngx::kLogVerbose;
        const std::uint32_t n = (verbose ? g_ngxVerboseLines : g_ngxLogLines).fetch_add(1, std::memory_order_relaxed);
        if (n < (verbose ? 400U : 2000U) || (n % 200) == 0) {
            std::string text(a_message);
            while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
                text.pop_back();
            }
            logger::info("[NGX f{}] {}", a_feature, text);
        }
    }

    char g_lastFaultSite[192]{};

    int CaptureFault(EXCEPTION_POINTERS* a_pointers, unsigned long* a_out) noexcept
    {
        const EXCEPTION_RECORD* const record = a_pointers != nullptr ? a_pointers->ExceptionRecord : nullptr;
        *a_out = record != nullptr ? record->ExceptionCode : 0xE0000001UL;
        void* const address = record != nullptr ? record->ExceptionAddress : nullptr;
        if (*a_out == 0xC00000FDUL) {
            std::snprintf(g_lastFaultSite, sizeof(g_lastFaultSite), "at %p (stack overflow: the module is not looked up)", address);
            return EXCEPTION_EXECUTE_HANDLER;
        }
        HMODULE module = nullptr;
        wchar_t path[MAX_PATH]{};
        if (address != nullptr &&
            ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                static_cast<LPCWSTR>(address), &module) && module != nullptr) {
            ::GetModuleFileNameW(module, path, MAX_PATH);
        }
        const wchar_t* base = path;
        for (const wchar_t* p = path; *p != L'\0'; ++p) {
            if (*p == L'\\' || *p == L'/') base = p + 1;
        }
        const unsigned long long offset = module != nullptr
            ? static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(address) - reinterpret_cast<std::uintptr_t>(module))
            : 0ULL;
        std::snprintf(g_lastFaultSite, sizeof(g_lastFaultSite), "at %p (%.24ls+0x%llx)", address,
            module != nullptr && base[0] != L'\0' ? base : L"unknown module", offset);
        return EXCEPTION_EXECUTE_HANDLER;
    }

    Ngx::Result SehInitProjectId(Ngx::PFN_D3D12_Init_ProjectID a_fn, const char* a_project,
        const wchar_t* a_path, ID3D12Device* a_device, int a_version, const Ngx::FeatureCommonInfo* a_info,
        unsigned long* a_code) noexcept
    {
        __try {
            return a_fn(a_project, Ngx::kEngineTypeCustom, kEngineVersion, a_path, a_device, a_version, a_info);
        } __except (CaptureFault(GetExceptionInformation(), a_code)) {
            return 0;
        }
    }

    Ngx::Result SehCoreCreate(Ngx::PFN_D3D12_CreateFeature a_fn, ID3D12GraphicsCommandList* a_list,
        int a_feature, Ngx::Parameter* a_params, Ngx::Handle** a_handle, unsigned long* a_code) noexcept
    {
        __try {
            return a_fn(a_list, a_feature, a_params, a_handle);
        } __except (CaptureFault(GetExceptionInformation(), a_code)) {
            return 0;
        }
    }

    Ngx::Result SehCoreEvaluate(Ngx::PFN_D3D12_EvaluateFeature a_fn, ID3D12GraphicsCommandList* a_list,
        const Ngx::Handle* a_handle, const Ngx::Parameter* a_params, unsigned long* a_code) noexcept
    {
        __try {
            return a_fn(a_list, a_handle, a_params, nullptr);
        } __except (CaptureFault(GetExceptionInformation(), a_code)) {
            return 0;
        }
    }

    Ngx::Result SehCoreRelease(Ngx::PFN_D3D12_ReleaseFeature a_fn, Ngx::Handle* a_handle,
        unsigned long* a_code) noexcept
    {
        __try {
            return a_fn(a_handle);
        } __except (CaptureFault(GetExceptionInformation(), a_code)) {
            return 0;
        }
    }

    Ngx::Result SehInitExt(Ngx::PFN_D3D12_Init_Ext a_fn, const wchar_t* a_path, ID3D12Device* a_device,
        int a_version, const Ngx::FeatureCommonInfo* a_info, unsigned long* a_code) noexcept
    {
        __try {
            return a_fn(kAppId, a_path, a_device, a_version, a_info);
        } __except (CaptureFault(GetExceptionInformation(), a_code)) {
            return 0;
        }
    }

    Ngx::Result SehAllocate(Ngx::PFN_D3D12_AllocateParameters a_fn, Ngx::Parameter** a_out,
        unsigned long* a_code) noexcept
    {
        __try {
            return a_fn(a_out);
        } __except (CaptureFault(GetExceptionInformation(), a_code)) {
            return 0;
        }
    }

    Ngx::Result SehGetCaps(Ngx::PFN_D3D12_GetCapabilityParameters a_fn, Ngx::Parameter** a_out,
        unsigned long* a_code) noexcept
    {
        __try {
            return a_fn(a_out);
        } __except (CaptureFault(GetExceptionInformation(), a_code)) {
            return 0;
        }
    }

    Ngx::Result SehShimInit(Ngx::PFN_Shim_CallInit a_shim, void* a_real, const wchar_t* a_path,
        ID3D12Device* a_device, int a_version, const Ngx::FeatureCommonInfo* a_info,
        unsigned long* a_code) noexcept
    {
        __try {
            return a_shim(a_real, kAppId, a_path, a_device, a_version, a_info);
        } __except (CaptureFault(GetExceptionInformation(), a_code)) {
            return 0;
        }
    }

    Ngx::Result SehShimCreate(Ngx::PFN_Shim_CallCreate a_shim, void* a_real,
        ID3D12GraphicsCommandList* a_list, int a_feature, Ngx::Parameter* a_params,
        Ngx::Handle** a_handle, unsigned long* a_code) noexcept
    {
        __try {
            return a_shim(a_real, a_list, a_feature, a_params, a_handle);
        } __except (CaptureFault(GetExceptionInformation(), a_code)) {
            return 0;
        }
    }

    Ngx::Result SehShimEvaluate(Ngx::PFN_Shim_CallEvaluate a_shim, void* a_real,
        ID3D12GraphicsCommandList* a_list, const Ngx::Handle* a_handle, const Ngx::Parameter* a_params,
        unsigned long* a_code) noexcept
    {
        __try {
            return a_shim(a_real, a_list, a_handle, a_params, nullptr);
        } __except (CaptureFault(GetExceptionInformation(), a_code)) {
            return 0;
        }
    }

    Ngx::Result SehShimRelease(Ngx::PFN_Shim_CallRelease a_shim, void* a_real, Ngx::Handle* a_handle,
        unsigned long* a_code) noexcept
    {
        __try {
            return a_shim(a_real, a_handle);
        } __except (CaptureFault(GetExceptionInformation(), a_code)) {
            return 0;
        }
    }

    Ngx::Result SehShutdown(Ngx::PFN_D3D12_Shutdown a_fn, unsigned long* a_code) noexcept
    {
        __try {
            return a_fn();
        } __except (CaptureFault(GetExceptionInformation(), a_code)) {
            return 0;
        }
    }

    Ngx::Result SehShutdown1(Ngx::PFN_D3D12_Shutdown1 a_fn, ID3D12Device* a_device, unsigned long* a_code) noexcept
    {
        __try {
            return a_fn(a_device);
        } __except (CaptureFault(GetExceptionInformation(), a_code)) {
            return 0;
        }
    }

    Ngx::Result SehDestroyParameters(Ngx::PFN_D3D12_DestroyParameters a_fn, Ngx::Parameter* a_params,
        unsigned long* a_code) noexcept
    {
        __try {
            return a_fn(a_params);
        } __except (CaptureFault(GetExceptionInformation(), a_code)) {
            return 0;
        }
    }

    [[nodiscard]] HMODULE LoadCoreFromDriverStore() noexcept
    {
        wchar_t system32[MAX_PATH]{};
        if (::GetSystemDirectoryW(system32, MAX_PATH) == 0) {
            return nullptr;
        }
        std::wstring repo = std::wstring(system32) + L"\\DriverStore\\FileRepository\\";
        std::wstring pattern = repo + L"nv*.inf_*";
        WIN32_FIND_DATAW find{};
        HANDLE handle = ::FindFirstFileW(pattern.c_str(), &find);
        if (handle == INVALID_HANDLE_VALUE) {
            return nullptr;
        }
        std::wstring best;
        FILETIME bestTime{};
        do {
            if ((find.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
                continue;
            }
            std::wstring candidate = repo + find.cFileName + L"\\_nvngx.dll";
            WIN32_FILE_ATTRIBUTE_DATA data{};
            if (!::GetFileAttributesExW(candidate.c_str(), GetFileExInfoStandard, &data)) {
                continue;
            }
            if (best.empty() || ::CompareFileTime(&data.ftLastWriteTime, &bestTime) > 0) {
                best = candidate;
                bestTime = data.ftLastWriteTime;
            }
        } while (::FindNextFileW(handle, &find));
        ::FindClose(handle);
        if (best.empty()) {
            return nullptr;
        }
        HMODULE module = ::LoadLibraryW(best.c_str());
        if (module != nullptr) {
            logger::info("[NGX] core loaded from the DriverStore: {}", Narrow(best.c_str()));
        }
        return module;
    }

    void FileVersionString(const wchar_t* a_path, char* a_out, std::size_t a_size) noexcept
    {
        std::snprintf(a_out, a_size, "%s", "unknown");
        DWORD handle = 0;
        const DWORD size = ::GetFileVersionInfoSizeW(a_path, &handle);
        if (size == 0) {
            return;
        }
        std::string buffer(size, '\0');
        if (!::GetFileVersionInfoW(a_path, 0, size, buffer.data())) {
            return;
        }
        VS_FIXEDFILEINFO* info = nullptr;
        UINT length = 0;
        if (::VerQueryValueW(buffer.data(), L"\\", reinterpret_cast<void**>(&info), &length) && info != nullptr) {
            std::snprintf(a_out, a_size, "%u.%u.%u.%u", HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS),
                HIWORD(info->dwFileVersionLS), LOWORD(info->dwFileVersionLS));
        }
    }

    void Sha256Hex(const wchar_t* a_path, char* a_out, std::size_t a_size) noexcept
    {
        std::snprintf(a_out, a_size, "%s", "unavailable");
        BCRYPT_ALG_HANDLE algorithm = nullptr;
        BCRYPT_HASH_HANDLE hash = nullptr;
        if (!BCRYPT_SUCCESS(::BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) {
            return;
        }
        HANDLE file = ::CreateFileW(a_path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (file != INVALID_HANDLE_VALUE && BCRYPT_SUCCESS(::BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0))) {
            std::string chunk(1 << 20, '\0');
            DWORD got = 0;
            bool ok = true;
            while (::ReadFile(file, chunk.data(), static_cast<DWORD>(chunk.size()), &got, nullptr) && got > 0) {
                if (!BCRYPT_SUCCESS(::BCryptHashData(hash, reinterpret_cast<PUCHAR>(chunk.data()), got, 0))) {
                    ok = false;
                    break;
                }
            }
            unsigned char digest[32]{};
            if (ok && BCRYPT_SUCCESS(::BCryptFinishHash(hash, digest, sizeof(digest), 0))) {
                static constexpr char kHex[] = "0123456789abcdef";
                std::size_t n = 0;
                for (unsigned char byte : digest) {
                    if (n + 2 >= a_size) {
                        break;
                    }
                    a_out[n++] = kHex[byte >> 4];
                    a_out[n++] = kHex[byte & 0xF];
                }
                a_out[n] = '\0';
            }
        }
        if (hash != nullptr) {
            ::BCryptDestroyHash(hash);
        }
        if (file != INVALID_HANDLE_VALUE) {
            ::CloseHandle(file);
        }
        ::BCryptCloseAlgorithmProvider(algorithm, 0);
    }

    void SignatureStatus(const wchar_t* a_path, char* a_out, std::size_t a_size) noexcept
    {
        WINTRUST_FILE_INFO fileInfo{};
        fileInfo.cbStruct = sizeof(fileInfo);
        fileInfo.pcwszFilePath = a_path;
        WINTRUST_DATA data{};
        data.cbStruct = sizeof(data);
        data.dwUIChoice = WTD_UI_NONE;
        data.fdwRevocationChecks = WTD_REVOKE_NONE;
        data.dwUnionChoice = WTD_CHOICE_FILE;
        data.pFile = &fileInfo;
        data.dwStateAction = WTD_STATEACTION_VERIFY;
        data.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;
        GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
        const LONG status = ::WinVerifyTrust(nullptr, &action, &data);
        data.dwStateAction = WTD_STATEACTION_CLOSE;
        ::WinVerifyTrust(nullptr, &action, &data);
        switch (static_cast<unsigned long>(status)) {
        case 0: std::snprintf(a_out, a_size, "%s", "Valid"); break;
        case 0x80096010UL: std::snprintf(a_out, a_size, "%s", "HashMismatch (modified build)"); break;
        case 0x800B0100UL: std::snprintf(a_out, a_size, "%s", "NotSigned"); break;
        case 0x800B0109UL: std::snprintf(a_out, a_size, "%s", "UntrustedRoot"); break;
        default: std::snprintf(a_out, a_size, "0x%08lx", static_cast<unsigned long>(status)); break;
        }
    }

    [[nodiscard]] bool ResolveCoreExports() noexcept
    {
        g_core.initProjectId = reinterpret_cast<Ngx::PFN_D3D12_Init_ProjectID>(
            ::GetProcAddress(g_core.module, "NVSDK_NGX_D3D12_Init_ProjectID"));
        g_core.initExt = reinterpret_cast<Ngx::PFN_D3D12_Init_Ext>(
            ::GetProcAddress(g_core.module, "NVSDK_NGX_D3D12_Init_Ext"));
        g_core.allocateParameters = reinterpret_cast<Ngx::PFN_D3D12_AllocateParameters>(
            ::GetProcAddress(g_core.module, "NVSDK_NGX_D3D12_AllocateParameters"));
        g_core.destroyParameters = reinterpret_cast<Ngx::PFN_D3D12_DestroyParameters>(
            ::GetProcAddress(g_core.module, "NVSDK_NGX_D3D12_DestroyParameters"));
        g_core.getCapabilityParameters = reinterpret_cast<Ngx::PFN_D3D12_GetCapabilityParameters>(
            ::GetProcAddress(g_core.module, "NVSDK_NGX_D3D12_GetCapabilityParameters"));
        g_core.createFeature = reinterpret_cast<Ngx::PFN_D3D12_CreateFeature>(
            ::GetProcAddress(g_core.module, "NVSDK_NGX_D3D12_CreateFeature"));
        g_core.evaluateFeature = reinterpret_cast<Ngx::PFN_D3D12_EvaluateFeature>(
            ::GetProcAddress(g_core.module, "NVSDK_NGX_D3D12_EvaluateFeature"));
        g_core.releaseFeature = reinterpret_cast<Ngx::PFN_D3D12_ReleaseFeature>(
            ::GetProcAddress(g_core.module, "NVSDK_NGX_D3D12_ReleaseFeature"));
        g_core.shutdown = reinterpret_cast<Ngx::PFN_D3D12_Shutdown>(
            ::GetProcAddress(g_core.module, "NVSDK_NGX_D3D12_Shutdown"));
        g_core.shutdown1 = reinterpret_cast<Ngx::PFN_D3D12_Shutdown1>(
            ::GetProcAddress(g_core.module, "NVSDK_NGX_D3D12_Shutdown1"));
        return (g_core.initProjectId != nullptr || g_core.initExt != nullptr) &&
               g_core.allocateParameters != nullptr;
    }
}

namespace Platform::NgxD3D12
{
    bool LoadCore() noexcept
    {
        if (g_core.module != nullptr) {
            return true;
        }
        try {
            HMODULE module = ::GetModuleHandleW(L"_nvngx.dll");
            if (module != nullptr) {
                wchar_t path[MAX_PATH]{};
                ::GetModuleFileNameW(module, path, MAX_PATH);
                logger::info("[NGX] core already in the process (Streamline loaded it): {}", Narrow(path));
                g_core.ownsModule = false;
            } else {
                module = ::LoadLibraryW(L"_nvngx.dll");
                if (module == nullptr) {
                    module = LoadCoreFromDriverStore();
                }
                g_core.ownsModule = module != nullptr;
            }
            if (module == nullptr) {
                logger::error("[NGX] the NGX core (_nvngx.dll) was not found: not loaded, not on the search path, "
                              "not in the DriverStore. Is the NVIDIA driver installed?");
                return false;
            }
            g_core.module = module;
            if (!ResolveCoreExports()) {
                logger::error("[NGX] the NGX core exports no usable D3D12 init entry point");
                g_core.module = nullptr;
                return false;
            }
            return true;
        } catch (...) {
            return false;
        }
    }

    bool CoreLoaded() noexcept { return g_core.module != nullptr; }
    bool CoreFaulted() noexcept { return g_core.faulted; }

    Outcome InitSession(ID3D12Device* a_device, const wchar_t* a_appDataPath, const wchar_t* a_snippetFolder) noexcept
    {
        struct FlushCorrectionOutcome
        {
            ~FlushCorrectionOutcome() { MfgTemporalFix::FlushPendingLog(); }
        } const flushCorrectionOutcome;

        Outcome outcome{};
        if (a_device == nullptr || a_appDataPath == nullptr || a_snippetFolder == nullptr) {
            outcome.result = static_cast<Ngx::Result>(0xBAD00005);
            return outcome;
        }
        if (g_core.faulted) {
            outcome.faultCode = 1;
            return outcome;
        }
        if (g_core.sessionDevice == a_device) {
            outcome.result = Ngx::kSuccess;
            return outcome;
        }
        if (!LoadCore()) {
            outcome.result = static_cast<Ngx::Result>(0xBAD00007);
            return outcome;
        }
        if (g_core.sessionDevice != nullptr) {
            ShutdownSession();
        }
        ::wcsncpy_s(g_core.pathList, a_snippetFolder, _TRUNCATE);
        g_core.pathListEntries[0] = g_core.pathList;
        g_core.commonInfo = Ngx::FeatureCommonInfo{};
        g_core.commonInfo.PathListInfo.Path = g_core.pathListEntries;
        g_core.commonInfo.PathListInfo.Length = 1;
        g_core.commonInfo.LoggingInfo.Callback = &NgxLogCallback;
        g_core.commonInfo.LoggingInfo.Level = Ngx::kLogOn;
        g_core.commonInfo.LoggingInfo.DisableOtherLoggingSinks = true;
        MfgTemporalFix::PrepareProgram(a_device);
        static constexpr int kCoreVersions[]{ 0x15, 0x14, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F, 0x20, 0x13 };
        for (const int version : kCoreVersions) {
            unsigned long code = 0;
            if (g_core.initProjectId != nullptr) {
                const Ngx::Result r = SehInitProjectId(g_core.initProjectId, kProjectId, a_appDataPath, a_device, version, &g_core.commonInfo, &code);
                if (code != 0) {
                    g_core.faulted = true;
                    outcome.faultCode = code;
                    logger::error("[NGX] Init_ProjectID(version={:#04x}) raised {:#010x}; the core is off for the session", version, code);
                    return outcome;
                }
                if (Ngx::Succeeded(r)) {
                    g_core.sessionDevice = a_device;
                    g_core.sdkVersion = version;
                    ++g_core.sessionSerial;
                    outcome.result = r;
                    logger::info("[NGX] D3D12 session initialised via Init_ProjectID (sdk version {:#04x}); snippet path {}",
                        version, Narrow(a_snippetFolder));
                    LogCapabilities();
                    return outcome;
                }
                outcome.result = r;
            }
            if (g_core.initExt != nullptr) {
                const Ngx::Result r = SehInitExt(g_core.initExt, a_appDataPath, a_device, version, &g_core.commonInfo, &code);
                if (code != 0) {
                    g_core.faulted = true;
                    outcome.faultCode = code;
                    logger::error("[NGX] Init_Ext(version={:#04x}) raised {:#010x}; the core is off for the session", version, code);
                    return outcome;
                }
                if (Ngx::Succeeded(r)) {
                    g_core.sessionDevice = a_device;
                    g_core.sdkVersion = version;
                    ++g_core.sessionSerial;
                    outcome.result = r;
                    logger::info("[NGX] D3D12 session initialised via Init_Ext (sdk version {:#04x}); snippet path {}",
                        version, Narrow(a_snippetFolder));
                    LogCapabilities();
                    return outcome;
                }
                outcome.result = r;
            }
        }
        logger::error("[NGX] no D3D12 init entry point accepted any SDK version 0x13..0x20 (last result {:#010x} {})",
            Ngx::Code(outcome.result), Ngx::ResultName(outcome.result));
        return outcome;
    }

    void ShutdownSession() noexcept
    {
        if (g_core.sessionDevice == nullptr) {
            return;
        }
        g_snippet.initialised = false;
        if (g_core.faulted) {
            g_core.sessionDevice = nullptr;
            g_core.sdkVersion = 0;
            logger::warn("[NGX] D3D12 session dropped without calling Shutdown: the core faulted earlier this session");
            return;
        }
        unsigned long code = 0;
        if (g_core.shutdown1 != nullptr) {
            SehShutdown1(g_core.shutdown1, g_core.sessionDevice, &code);
        } else if (g_core.shutdown != nullptr) {
            SehShutdown(g_core.shutdown, &code);
        }
        if (code != 0) {
            g_core.faulted = true;
            logger::error("[NGX] Shutdown raised {:#010x}; the core is off for the session", code);
        }
        g_core.sessionDevice = nullptr;
        g_core.sdkVersion = 0;
        g_core.superSamplingAvailable = -1;
        g_core.frameGenerationAvailable = -1;
        logger::info("[NGX] D3D12 session shut down");
    }

    bool SessionReady() noexcept { return g_core.sessionDevice != nullptr && !g_core.faulted; }
    std::uint64_t SessionSerial() noexcept { return g_core.sessionSerial; }
    int NegotiatedSdkVersion() noexcept { return g_core.sdkVersion; }
    int SuperSamplingAvailable() noexcept { return g_core.superSamplingAvailable; }
    int FrameGenerationAvailable() noexcept { return g_core.frameGenerationAvailable; }

    Outcome CoreCreate(ID3D12GraphicsCommandList* a_list, int a_feature, Ngx::Parameter* a_params,
        Ngx::Handle** a_handle) noexcept
    {
        Outcome outcome{};
        if (!SessionReady() || g_core.createFeature == nullptr || a_list == nullptr || a_params == nullptr ||
            a_handle == nullptr) {
            outcome.result = static_cast<Ngx::Result>(0xBAD00007);
            return outcome;
        }
        outcome.result = SehCoreCreate(g_core.createFeature, a_list, a_feature, a_params, a_handle, &outcome.faultCode);
        if (outcome.faultCode != 0) {
            g_core.faulted = true;
            logger::error("[NGX] CreateFeature({}) raised {:#010x}; the core is off for the session", a_feature, outcome.faultCode);
        }
        return outcome;
    }

    Outcome CoreEvaluate(ID3D12GraphicsCommandList* a_list, const Ngx::Handle* a_handle,
        const Ngx::Parameter* a_params) noexcept
    {
        Outcome outcome{};
        if (!SessionReady() || g_core.evaluateFeature == nullptr || a_list == nullptr || a_handle == nullptr ||
            a_params == nullptr) {
            outcome.result = static_cast<Ngx::Result>(0xBAD00007);
            return outcome;
        }
        outcome.result = SehCoreEvaluate(g_core.evaluateFeature, a_list, a_handle, a_params, &outcome.faultCode);
        if (outcome.faultCode != 0) {
            g_core.faulted = true;
            logger::error("[NGX] EvaluateFeature raised {:#010x}; the core is off for the session", outcome.faultCode);
        }
        return outcome;
    }

    Outcome CoreRelease(Ngx::Handle* a_handle) noexcept
    {
        Outcome outcome{};
        if (a_handle == nullptr || !SessionReady() || g_core.releaseFeature == nullptr) {
            outcome.result = Ngx::kSuccess;
            return outcome;
        }
        outcome.result = SehCoreRelease(g_core.releaseFeature, a_handle, &outcome.faultCode);
        if (outcome.faultCode != 0) {
            g_core.faulted = true;
            logger::error("[NGX] ReleaseFeature raised {:#010x}; the core is off for the session", outcome.faultCode);
        }
        return outcome;
    }

    Ngx::Parameter* AllocateParameters() noexcept
    {
        if (!SessionReady() || g_core.allocateParameters == nullptr) {
            return nullptr;
        }
        Ngx::Parameter* params = nullptr;
        unsigned long code = 0;
        const Ngx::Result r = SehAllocate(g_core.allocateParameters, &params, &code);
        if (code != 0) {
            g_core.faulted = true;
            logger::error("[NGX] AllocateParameters raised {:#010x}", code);
            return nullptr;
        }
        if (!Ngx::Succeeded(r) || params == nullptr) {
            logger::error("[NGX] AllocateParameters failed {:#010x} {}", Ngx::Code(r), Ngx::ResultName(r));
            return nullptr;
        }
        return params;
    }

    void DestroyParameters(Ngx::Parameter* a_params) noexcept
    {
        if (a_params == nullptr || g_core.destroyParameters == nullptr || g_core.faulted) {
            return;
        }
        unsigned long code = 0;
        SehDestroyParameters(g_core.destroyParameters, a_params, &code);
        if (code != 0) {
            g_core.faulted = true;
            logger::error("[NGX] DestroyParameters raised {:#010x}; the core is off for the session", code);
        }
    }

    void LogCapabilities() noexcept
    {
        if (!SessionReady() || g_core.getCapabilityParameters == nullptr) {
            return;
        }
        Ngx::Parameter* caps = nullptr;
        unsigned long code = 0;
        const Ngx::Result r = SehGetCaps(g_core.getCapabilityParameters, &caps, &code);
        if (code != 0) {
            g_core.faulted = true;
            logger::error("[NGX] GetCapabilityParameters raised {:#010x}; the core is off for the session", code);
            return;
        }
        if (!Ngx::Succeeded(r) || caps == nullptr) {
            logger::info("[NGX] capability query refused ({:#010x} {})", Ngx::Code(r), Ngx::ResultName(r));
            return;
        }
        unsigned int ssAvailable = 0, needsDriver = 0, minMajor = 0, minMinor = 0;
        const bool ssKnown = Ngx::Succeeded(caps->Get("SuperSampling.Available", &ssAvailable));
        caps->Get("SuperSampling.NeedsUpdatedDriver", &needsDriver);
        caps->Get("SuperSampling.MinDriverVersionMajor", &minMajor);
        caps->Get("SuperSampling.MinDriverVersionMinor", &minMinor);
        g_core.superSamplingAvailable = ssKnown ? (ssAvailable != 0U ? 1 : 0) : -1;
        unsigned int fgAvailable = 0;
        const bool fgKnown = Ngx::Succeeded(caps->Get("FrameGeneration.Available", &fgAvailable));
        g_core.frameGenerationAvailable = fgKnown ? (fgAvailable != 0U ? 1 : 0) : -1;
        logger::info("[NGX] capabilities: FrameGeneration.Available={}{}", fgAvailable, fgKnown ? "" : " (not reported)");
        {
            unsigned int mfgMax = 0;
            const bool mfgKnown = Ngx::Succeeded(caps->Get("DLSSG.MultiFrameCountMax", &mfgMax));
            logger::info("[NGX] capabilities: DLSSG.MultiFrameCountMax={}{} (1 = 2x only; multi-frame needs > 1)", mfgMax,
                mfgKnown ? "" : " (not reported)");
        }
        logger::info("[NGX] capabilities: SuperSampling.Available={} NeedsUpdatedDriver={} MinDriver={}.{}",
            ssAvailable, needsDriver, minMajor, minMinor);
        DestroyParameters(caps);
    }

    bool LoadSnippet(const wchar_t* a_snippetPath, const wchar_t* a_shimPath, SnippetIdentity& a_identity) noexcept
    {
        if (g_snippet.module != nullptr && g_snippet.shim != nullptr && g_snippet.initExt != nullptr &&
            g_snippet.createFeature != nullptr && g_snippet.evaluateFeature != nullptr &&
            g_snippet.releaseFeature != nullptr && g_snippet.shimInit != nullptr &&
            g_snippet.shimCreate != nullptr && g_snippet.shimEvaluate != nullptr &&
            g_snippet.shimRelease != nullptr) {
            a_identity = g_snippet.identity;
            return true;
        }
        try {
            if (!g_snippet.identityKnown) {
                ::wcsncpy_s(g_snippet.identity.path, a_snippetPath, _TRUNCATE);
                WIN32_FILE_ATTRIBUTE_DATA data{};
                if (!::GetFileAttributesExW(a_snippetPath, GetFileExInfoStandard, &data)) {
                    logger::error("[NGX] the DLSS 5 runtime is not at {}", Narrow(a_snippetPath));
                    return false;
                }
                g_snippet.identity.size = (static_cast<unsigned long long>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
                logger::info("[NGX] computing the DLSS 5 runtime's identity (size/version/SHA-256/Authenticode) - a one-time hitch");
                FileVersionString(a_snippetPath, g_snippet.identity.version, sizeof(g_snippet.identity.version));
                Sha256Hex(a_snippetPath, g_snippet.identity.sha256, sizeof(g_snippet.identity.sha256));
                SignatureStatus(a_snippetPath, g_snippet.identity.signature, sizeof(g_snippet.identity.signature));
                g_snippet.identityKnown = true;
                logger::info("[NGX] DLSS 5 runtime identity: {} | {} bytes | version {} | Authenticode {} | sha256 {}",
                    Narrow(a_snippetPath), g_snippet.identity.size,
                    g_snippet.identity.version, g_snippet.identity.signature, g_snippet.identity.sha256);
            }
            a_identity = g_snippet.identity;

            if (g_snippet.shim == nullptr) {
                g_snippet.shim = ::LoadLibraryW(a_shimPath);
                if (g_snippet.shim == nullptr) {
                    logger::error("[NGX] the caller-identity shim did not load ({}, error {:#x})",
                        Narrow(a_shimPath), ::GetLastError());
                    return false;
                }
                g_snippet.shimInit = reinterpret_cast<Ngx::PFN_Shim_CallInit>(::GetProcAddress(g_snippet.shim, "DLSSNR_CallInit"));
                g_snippet.shimCreate = reinterpret_cast<Ngx::PFN_Shim_CallCreate>(::GetProcAddress(g_snippet.shim, "DLSSNR_CallCreate"));
                g_snippet.shimEvaluate = reinterpret_cast<Ngx::PFN_Shim_CallEvaluate>(::GetProcAddress(g_snippet.shim, "DLSSNR_CallEvaluate"));
                g_snippet.shimRelease = reinterpret_cast<Ngx::PFN_Shim_CallRelease>(::GetProcAddress(g_snippet.shim, "DLSSNR_CallRelease"));
                if (g_snippet.shimInit == nullptr || g_snippet.shimCreate == nullptr ||
                    g_snippet.shimEvaluate == nullptr || g_snippet.shimRelease == nullptr) {
                    logger::error("[NGX] the caller-identity shim is missing exports");
                    ::FreeLibrary(g_snippet.shim);
                    g_snippet.shim = nullptr;
                    return false;
                }
            }
            if (g_snippet.module == nullptr) {
                g_snippet.module = ::LoadLibraryW(a_snippetPath);
                if (g_snippet.module == nullptr) {
                    logger::error("[NGX] LoadLibrary(nvngx_dlssnr.dll) failed, error {:#x}", ::GetLastError());
                    return false;
                }
                g_snippet.initExt = reinterpret_cast<Ngx::PFN_Snippet_Init_Ext>(::GetProcAddress(g_snippet.module, "NVSDK_NGX_D3D12_Init_Ext"));
                g_snippet.createFeature = reinterpret_cast<Ngx::PFN_D3D12_CreateFeature>(::GetProcAddress(g_snippet.module, "NVSDK_NGX_D3D12_CreateFeature"));
                g_snippet.evaluateFeature = reinterpret_cast<Ngx::PFN_D3D12_EvaluateFeature>(::GetProcAddress(g_snippet.module, "NVSDK_NGX_D3D12_EvaluateFeature"));
                g_snippet.releaseFeature = reinterpret_cast<Ngx::PFN_D3D12_ReleaseFeature>(::GetProcAddress(g_snippet.module, "NVSDK_NGX_D3D12_ReleaseFeature"));
                if (g_snippet.initExt == nullptr || g_snippet.createFeature == nullptr ||
                    g_snippet.evaluateFeature == nullptr || g_snippet.releaseFeature == nullptr) {
                    logger::error("[NGX] nvngx_dlssnr.dll is missing the D3D12 feature exports - not a DLSS 5 runtime?");
                    ::FreeLibrary(g_snippet.module);
                    g_snippet.module = nullptr;
                    return false;
                }
                logger::info("[NGX] DLSS 5 runtime loaded ({}), shim loaded ({})",
                    fmt::ptr(g_snippet.module), fmt::ptr(g_snippet.shim));
            }
            return true;
        } catch (...) {
            return false;
        }
    }

    bool SnippetLoaded() noexcept { return g_snippet.module != nullptr && g_snippet.shim != nullptr; }

    void UnloadSnippet() noexcept
    {
        if (g_snippet.faulted) {
            logger::warn("[NGX] the DLSS 5 runtime stays loaded: it faulted this session and is not touched again");
            return;
        }
        if (g_snippet.module != nullptr) {
            ::FreeLibrary(g_snippet.module);
            g_snippet.module = nullptr;
            logger::info("[NGX] DLSS 5 runtime unloaded (a new session reloads it)");
        }
        if (g_snippet.shim != nullptr) {
            ::FreeLibrary(g_snippet.shim);
            g_snippet.shim = nullptr;
        }
        g_snippet.initExt = nullptr;
        g_snippet.createFeature = nullptr;
        g_snippet.evaluateFeature = nullptr;
        g_snippet.releaseFeature = nullptr;
        g_snippet.shimInit = nullptr;
        g_snippet.shimCreate = nullptr;
        g_snippet.shimEvaluate = nullptr;
        g_snippet.shimRelease = nullptr;
        g_snippet.initialised = false;
    }

    Outcome SnippetInit(ID3D12Device* a_device, const wchar_t* a_appDataPath) noexcept
    {
        Outcome outcome{};
        if (!SnippetLoaded() || a_device == nullptr || a_appDataPath == nullptr || g_snippet.faulted) {
            outcome.faultCode = g_snippet.faulted ? 1UL : 0UL;
            outcome.result = static_cast<Ngx::Result>(0xBAD00007);
            return outcome;
        }
        if (g_snippet.initialised) {
            outcome.result = Ngx::kSuccess;
            return outcome;
        }
        ::wcsncpy_s(g_snippet.pathList, a_appDataPath, _TRUNCATE);
        g_snippet.pathListEntries[0] = g_snippet.pathList;
        g_snippet.commonInfo = Ngx::FeatureCommonInfo{};
        g_snippet.commonInfo.PathListInfo.Path = g_snippet.pathListEntries;
        g_snippet.commonInfo.PathListInfo.Length = 1;
        g_snippet.commonInfo.LoggingInfo.Callback = &NgxLogCallback;
        g_snippet.commonInfo.LoggingInfo.Level = Ngx::kLogVerbose;
        g_snippet.commonInfo.LoggingInfo.DisableOtherLoggingSinks = true;

        static constexpr int kVersions[]{ 0x15, 0x13, 0x14, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F, 0x20 };
        for (const int version : kVersions) {
            unsigned long code = 0;
            const Ngx::Result r = SehShimInit(g_snippet.shimInit, reinterpret_cast<void*>(g_snippet.initExt),
                a_appDataPath, a_device, version, &g_snippet.commonInfo, &code);
            if (code != 0) {
                g_snippet.faulted = true;
                outcome.faultCode = code;
                logger::error("[NGX] the DLSS 5 runtime's Init_Ext(version={:#04x}) raised {:#010x}; off for the session", version, code);
                return outcome;
            }
            outcome.result = r;
            if (Ngx::Succeeded(r)) {
                g_snippet.initialised = true;
                logger::info("[NGX] DLSS 5 runtime initialised through the shim (version {:#04x})", version);
                return outcome;
            }
            if (Ngx::Code(r) == 0xBAD00002U) {
                break;
            }
        }
        logger::error("[NGX] the DLSS 5 runtime refused to initialise: {:#010x} {}", Ngx::Code(outcome.result),
            Ngx::ResultName(outcome.result));
        return outcome;
    }

    Outcome SnippetCreate(ID3D12GraphicsCommandList* a_list, int a_feature, Ngx::Parameter* a_params,
        Ngx::Handle** a_handle) noexcept
    {
        Outcome outcome{};
        if (!g_snippet.initialised || g_snippet.faulted) {
            outcome.result = static_cast<Ngx::Result>(0xBAD00007);
            return outcome;
        }
        outcome.result = SehShimCreate(g_snippet.shimCreate, reinterpret_cast<void*>(g_snippet.createFeature),
            a_list, a_feature, a_params, a_handle, &outcome.faultCode);
        if (outcome.faultCode != 0) {
            g_snippet.faulted = true;
        }
        return outcome;
    }

    Outcome SnippetEvaluate(ID3D12GraphicsCommandList* a_list, const Ngx::Handle* a_handle,
        const Ngx::Parameter* a_params) noexcept
    {
        Outcome outcome{};
        if (!g_snippet.initialised || g_snippet.faulted) {
            outcome.result = static_cast<Ngx::Result>(0xBAD00007);
            return outcome;
        }
        outcome.result = SehShimEvaluate(g_snippet.shimEvaluate, reinterpret_cast<void*>(g_snippet.evaluateFeature),
            a_list, a_handle, a_params, &outcome.faultCode);
        if (outcome.faultCode != 0) {
            g_snippet.faulted = true;
        }
        return outcome;
    }

    Outcome SnippetRelease(Ngx::Handle* a_handle) noexcept
    {
        Outcome outcome{};
        if (!g_snippet.initialised || g_snippet.faulted || a_handle == nullptr) {
            outcome.result = Ngx::kSuccess;
            return outcome;
        }
        outcome.result = SehShimRelease(g_snippet.shimRelease, reinterpret_cast<void*>(g_snippet.releaseFeature),
            a_handle, &outcome.faultCode);
        if (outcome.faultCode != 0) {
            g_snippet.faulted = true;
        }
        return outcome;
    }

    void DescribeOutcome(const Outcome& a_outcome, char* a_out, std::size_t a_size) noexcept
    {
        if (a_outcome.faultCode != 0) {
            std::snprintf(a_out, a_size, "exception 0x%08lx inside NGX %s", a_outcome.faultCode, g_lastFaultSite);
        } else {
            std::snprintf(a_out, a_size, "0x%08x %s", Ngx::Code(a_outcome.result), Ngx::ResultName(a_outcome.result));
        }
    }
}
