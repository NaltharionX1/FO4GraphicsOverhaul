#include "Platform/MfgCuModuleProbe.h"

#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#    define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include <dxgi.h>
#include <nvapi.h>

#include <cstdio>
#include <cstring>

namespace
{
    unsigned long SehCreateModule(ID3D12Device* a_device, const void* a_blob, NvU32 a_size, NVDX_ObjectHandle* a_module, NvAPI_Status* a_status) noexcept
    {
        __try {
            *a_status = NvAPI_D3D12_CreateCuModule(a_device, a_blob, a_size, a_module);
            return 0;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return GetExceptionCode();
        }
    }

    unsigned long SehEnumFunctions(ID3D12Device* a_device, NVDX_ObjectHandle a_module, NvU32* a_count, const char** a_names, NvAPI_Status* a_status) noexcept
    {
        __try {
            *a_status = NvAPI_D3D12_EnumFunctionsInModule(a_device, a_module, a_count, a_names);
            return 0;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return GetExceptionCode();
        }
    }

    unsigned long SehCreateFunction(ID3D12Device* a_device, NVDX_ObjectHandle a_module, const char* a_name, NVDX_ObjectHandle* a_function, NvAPI_Status* a_status) noexcept
    {
        __try {
            *a_status = NvAPI_D3D12_CreateCuFunction(a_device, a_module, a_name, a_function);
            return 0;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return GetExceptionCode();
        }
    }

    void SehDestroy(ID3D12Device* a_device, NVDX_ObjectHandle a_module, NVDX_ObjectHandle a_function) noexcept
    {
        __try {
            if (a_function != nullptr) (void)NvAPI_D3D12_DestroyCuFunction(a_device, a_function);
            if (a_module != nullptr) (void)NvAPI_D3D12_DestroyCuModule(a_device, a_module);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }

    unsigned long SehPtxSupported(ID3D12Device* a_device, bool* a_supported, NvAPI_Status* a_status) noexcept
    {
        __try {
            *a_status = NvAPI_D3D12_IsFatbinPTXSupported(a_device, a_supported);
            return 0;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return GetExceptionCode();
        }
    }

    unsigned long SehInitialize(NvAPI_Status* a_status) noexcept
    {
        __try {
            *a_status = NvAPI_Initialize();
            return 0;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return GetExceptionCode();
        }
    }

    unsigned long SehErrorMessage(NvAPI_Status a_status, char* a_text) noexcept
    {
        __try {
            (void)NvAPI_GetErrorMessage(a_status, a_text);
            return 0;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return GetExceptionCode();
        }
    }

    [[nodiscard]] bool Initialised(char* a_why, std::size_t a_whySize) noexcept
    {
        NvAPI_Status status = NVAPI_OK;
        const unsigned long raised = SehInitialize(&status);
        if (raised != 0) {
            std::snprintf(a_why, a_whySize, "NvAPI_Initialize raised 0x%08lx", raised);
            return false;
        }
        if (status != NVAPI_OK) {
            std::snprintf(a_why, a_whySize, "NVAPI would not initialise (%d)", static_cast<int>(status));
            return false;
        }
        return true;
    }

    void StatusText(NvAPI_Status a_status, char* a_out, std::size_t a_size) noexcept
    {
        NvAPI_ShortString text{};
        if (SehErrorMessage(a_status, text) != 0) {
            text[0] = '\0';
        }
        std::snprintf(a_out, a_size, "%d (%s)", static_cast<int>(a_status), text[0] != '\0' ? text : "?");
    }
}

namespace Platform::MfgCuModuleProbe
{
    bool PtxSupported(ID3D12Device* a_device, char* a_why, std::size_t a_whySize) noexcept
    {
        if (a_device == nullptr) {
            std::snprintf(a_why, a_whySize, "no device");
            return false;
        }
        if (!Initialised(a_why, a_whySize)) {
            return false;
        }
        bool supported = false;
        NvAPI_Status status = NVAPI_OK;
        const unsigned long raised = SehPtxSupported(a_device, &supported, &status);
        if (raised != 0) {
            std::snprintf(a_why, a_whySize, "IsFatbinPTXSupported raised 0x%08lx", raised);
            return false;
        }
        if (status != NVAPI_OK) {
            char text[80]{};
            StatusText(status, text, sizeof(text));
            std::snprintf(a_why, a_whySize, "IsFatbinPTXSupported refused: %s", text);
            return false;
        }
        if (!supported) {
            std::snprintf(a_why, a_whySize, "the driver's D3D12 module does not build PTX from a fatbin on this device");
        }
        return supported;
    }

    bool Load(ID3D12Device* a_device, const void* a_blob, std::size_t a_size, const char* a_entry, char* a_why, std::size_t a_whySize) noexcept
    {
        if (a_device == nullptr || a_blob == nullptr || a_size == 0 || a_entry == nullptr) {
            std::snprintf(a_why, a_whySize, "nothing to load");
            return false;
        }
        if (!Initialised(a_why, a_whySize)) {
            return false;
        }
        NVDX_ObjectHandle module = nullptr;
        NvAPI_Status status = NVAPI_OK;
        const unsigned long created = SehCreateModule(a_device, a_blob, static_cast<NvU32>(a_size), &module, &status);
        if (created != 0) {
            std::snprintf(a_why, a_whySize, "CreateCuModule raised 0x%08lx (%zu bytes)", created, a_size);
            return false;
        }
        if (status != NVAPI_OK || module == nullptr) {
            char text[80]{};
            StatusText(status, text, sizeof(text));
            std::snprintf(a_why, a_whySize, "CreateCuModule refused: %s (%zu bytes)", text, a_size);
            return false;
        }
        const char* names[32]{};
        NvU32 count = 32;
        NvAPI_Status enumStatus = NVAPI_OK;
        const unsigned long enumerated = SehEnumFunctions(a_device, module, &count, names, &enumStatus);
        NVDX_ObjectHandle function = nullptr;
        NvAPI_Status functionStatus = NVAPI_OK;
        const unsigned long resolved = SehCreateFunction(a_device, module, a_entry, &function, &functionStatus);
        const bool functionMade = resolved == 0 && functionStatus == NVAPI_OK && function != nullptr;
        const bool ok = enumerated == 0 && enumStatus == NVAPI_OK && functionMade;
        if (!ok) {
            if (enumerated != 0 || resolved != 0) {
                std::snprintf(a_why, a_whySize, "the driver raised 0x%08lx resolving '%s'", enumerated != 0 ? enumerated : resolved, a_entry);
            } else {
                char text[80]{};
                StatusText(enumStatus != NVAPI_OK ? enumStatus : functionStatus, text, sizeof(text));
                std::snprintf(a_why, a_whySize, "'%s' did not resolve: %s", a_entry, text);
            }
        }
        SehDestroy(a_device, module, functionMade ? function : nullptr);
        return ok;
    }
}
