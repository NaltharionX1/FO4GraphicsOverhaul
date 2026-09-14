#include "Platform/MfgCuModuleProbe.h"
#include "Platform/MfgTemporalFix.h"

#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#    define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <nvapi.h>

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

    [[nodiscard]] bool LoadContainer(ID3D12Device* a_device, const char* a_label, const std::vector<std::uint8_t>& a_blob, const char* a_entry)
    {
        char why[200]{};
        const bool ok = Platform::MfgCuModuleProbe::Load(a_device, a_blob.data(), a_blob.size(), a_entry, why, sizeof(why));
        std::cout << "  " << a_label << " (" << a_blob.size() << " B): " << (ok ? "module created, entry resolved" : why) << '\n';
        return ok;
    }
}

namespace
{
    [[nodiscard]] bool LoadWithClaimedSize(ID3D12Device* a_device, const char* a_label, const std::vector<std::uint8_t>& a_blob,
        std::size_t a_claimedSize, const char* a_entry)
    {
        const std::size_t page = 4096;
        const std::size_t rounded = (a_blob.size() + page - 1) / page * page;
        auto* const region = static_cast<std::uint8_t*>(::VirtualAlloc(nullptr, rounded + page, MEM_RESERVE, PAGE_NOACCESS));
        if (region == nullptr || ::VirtualAlloc(region, rounded, MEM_COMMIT, PAGE_READWRITE) == nullptr) {
            std::cout << "  " << a_label << ": no region\n";
            if (region != nullptr) (void)::VirtualFree(region, 0, MEM_RELEASE);
            return false;
        }
        std::uint8_t* const at = region + (rounded - a_blob.size());
        std::memcpy(at, a_blob.data(), a_blob.size());
        char why[200]{};
        const bool ok = Platform::MfgCuModuleProbe::Load(a_device, at, a_claimedSize, a_entry, why, sizeof(why));
        std::cout << "  " << a_label << ": " << a_blob.size() << " B handed as " << a_claimedSize << " B before a guard page -> "
                  << (ok ? "loaded, entry resolved" : why) << (std::strstr(why, "raised") != nullptr ? " (the known crash, reproduced)" : "") << "\n";
        (void)::VirtualFree(region, 0, MEM_RELEASE);
        return ok;
    }
}

int main()
{
    using namespace Platform;
    using MfgTemporalFix::Program;
    using MfgTemporalFix::Role;

    std::cout << "MfgCuModuleTests: " << MFG_TEST_DLL << '\n';

    ID3D12Device* device = nullptr;
    {
        IDXGIFactory6* factory = nullptr;
        if (SUCCEEDED(::CreateDXGIFactory1(IID_PPV_ARGS(&factory))) && factory != nullptr) {
            IDXGIAdapter1* adapter = nullptr;
            for (UINT i = 0; factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)) == S_OK; ++i) {
                DXGI_ADAPTER_DESC1 desc{};
                (void)adapter->GetDesc1(&desc);
                if (desc.VendorId == 0x10DE && (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0 &&
                    SUCCEEDED(::D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)))) {
                    std::wcout << L"  device: " << desc.Description << L'\n';
                    adapter->Release();
                    break;
                }
                adapter->Release();
                adapter = nullptr;
            }
            factory->Release();
        }
    }
    if (device == nullptr) {
        std::cout << "  SKIPPED: no NVIDIA D3D12 device on this machine — the CuModule proof did NOT run here\nMfgCuModuleTests: SKIPPED\n";
        return EXIT_SUCCESS;
    }
    if (NvAPI_Initialize() != NVAPI_OK) {
        std::cout << "  SKIPPED: NVAPI would not initialise — the CuModule proof did NOT run here\nMfgCuModuleTests: SKIPPED\n";
        device->Release();
        return EXIT_SUCCESS;
    }
    {
        char why[200]{};
        const bool ptx = Platform::MfgCuModuleProbe::PtxSupported(device, why, sizeof(why));
        std::cout << "  IsFatbinPTXSupported: " << (ptx ? "yes" : why) << '\n';
        Check(ptx, "the driver's D3D12 module builds PTX from a fatbin on this device (both routes rely on it)");
    }

    const std::wstring path = Widen(MFG_TEST_DLL);
    const HMODULE runtime = ::LoadLibraryExW(path.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
    Check(runtime != nullptr, "the staged runtime maps as an image");
    if (runtime == nullptr) {
        std::cout << "MfgCuModuleTests: " << g_failures << " failure(s) of " << g_checks << '\n';
        return EXIT_FAILURE;
    }

    static constexpr Role kRoles[]{ Role::kMotionVector, Role::kInpaint, Role::kInpaintDecision };
    static constexpr const char* kRoleLabels[]{ "motion-vector", "inpaint", "inpaint-decision" };

    for (std::size_t i = 0; i < 3; ++i) {
        std::vector<std::uint8_t> blob;
        const char* entry = nullptr;
        char why[200]{};
        char label[80]{};
        std::snprintf(label, sizeof(label), "original %s container", kRoleLabels[i]);
        const bool extracted = MfgTemporalFix::ExtractOriginalFatbin(runtime, kRoles[i], blob, entry, why, sizeof(why));
        Check(extracted, "the original container extracts");
        if (extracted) {
            Check(LoadContainer(device, label, blob, entry), "the runtime's own container loads through the CuModule path (a control)");
        }
    }
    {
        std::vector<std::uint8_t> blob;
        const char* entry = nullptr;
        char why[200]{};
        const bool extracted = MfgTemporalFix::ExtractFatbin(runtime, Program::kMidpoint, Role::kMotionVector, blob, entry, why, sizeof(why));
        Check(extracted, "the midpoint container extracts");
        if (extracted) {
            Check(LoadContainer(device, "midpoint motion-vector rebuild", blob, entry),
                "the midpoint rebuild loads through the CuModule path (the proven control)");
        }
    }

    for (std::size_t i = 0; i < 3; ++i) {
        std::vector<std::uint8_t> blob;
        const char* entry = nullptr;
        char why[200]{};
        char label[80]{};
        std::snprintf(label, sizeof(label), "Blackwell %s rebuild", kRoleLabels[i]);
        const bool extracted = MfgTemporalFix::ExtractFatbin(runtime, Program::kBlackwell, kRoles[i], blob, entry, why, sizeof(why));
        Check(extracted, "the Blackwell container extracts");
        if (extracted) {
            char message[160]{};
            std::snprintf(message, sizeof(message), "the Blackwell %s rebuild loads through the CuModule path", kRoleLabels[i]);
            Check(LoadContainer(device, label, blob, entry), message);
        }
    }

    std::cout << "  size-mismatch probe (the size beside the descriptor is the original container's):\n";
    {
        std::vector<std::uint8_t> original;
        std::vector<std::uint8_t> blob;
        const char* entry = nullptr;
        char why[200]{};
        if (MfgTemporalFix::ExtractOriginalFatbin(runtime, Role::kMotionVector, original, entry, why, sizeof(why)) &&
            MfgTemporalFix::ExtractFatbin(runtime, Program::kMidpoint, Role::kMotionVector, blob, entry, why, sizeof(why))) {
            Check(LoadWithClaimedSize(device, "midpoint motion-vector rebuild", blob, original.size(), entry),
                "the midpoint rebuild handed as its original size loads");
        }
        for (std::size_t i = 0; i < 3; ++i) {
            if (MfgTemporalFix::ExtractOriginalFatbin(runtime, kRoles[i], original, entry, why, sizeof(why)) &&
                MfgTemporalFix::ExtractFatbin(runtime, Program::kBlackwell, kRoles[i], blob, entry, why, sizeof(why))) {
                char label[80]{};
                std::snprintf(label, sizeof(label), "Blackwell %s rebuild", kRoleLabels[i]);
                Check(blob.size() >= original.size(), "a rebuilt container is never smaller than its original (the padding rule)");
                Check(LoadWithClaimedSize(device, label, blob, original.size(), entry),
                    "the rebuilt container handed as its ORIGINAL size loads (a crash regression, kept red-able)");
            }
        }
    }

    (void)::FreeLibrary(runtime);
    (void)NvAPI_Unload();
    device->Release();
    if (g_failures != 0) {
        std::cout << "MfgCuModuleTests: " << g_failures << " failure(s) of " << g_checks << '\n';
        return EXIT_FAILURE;
    }
    std::cout << "MfgCuModuleTests: all checks passed (" << g_checks << ")\n";
    return EXIT_SUCCESS;
}
