#pragma once

#include <cstddef>
#include <cstdint>

struct ID3D11Resource;
struct ID3D12Resource;
struct ID3D12Device;
struct ID3D12GraphicsCommandList;

namespace Platform::Ngx
{
    using Result = int;
    inline constexpr Result kSuccess = 1;
    inline constexpr std::uint32_t kFailBase = 0xBAD00000U;

    [[nodiscard]] constexpr bool Succeeded(Result a_result) noexcept { return a_result == kSuccess; }
    [[nodiscard]] constexpr std::uint32_t Code(Result a_result) noexcept
    {
        return static_cast<std::uint32_t>(a_result);
    }

    [[nodiscard]] constexpr const char* ResultName(Result a_result) noexcept
    {
        if (a_result == kSuccess) {
            return "Success";
        }
        const std::uint32_t code = Code(a_result);
        if ((code & 0xFFFFFF00U) != kFailBase) {
            return "unknown (not an NGX result)";
        }
        switch (code & 0xFFU) {
        case 0x00: return "Fail";
        case 0x01: return "FeatureNotSupported";
        case 0x02: return "PlatformError (the caller-identity check, when it comes from the snippet)";
        case 0x03: return "FeatureAlreadyExists";
        case 0x04: return "FeatureNotFound";
        case 0x05: return "InvalidParameter";
        case 0x06: return "ScratchBufferTooSmall";
        case 0x07: return "NotInitialized";
        case 0x08: return "UnsupportedInputFormat";
        case 0x09: return "RWFlagMissing";
        case 0x0A: return "MissingInput";
        case 0x0B: return "UnableToInitializeFeature";
        case 0x0C: return "OutOfDate";
        case 0x0D: return "OutOfGPUMemory";
        case 0x0E: return "UnsupportedFormat";
        case 0x0F: return "UnableToWriteToAppDataPath";
        case 0x10: return "UnsupportedParameter";
        case 0x11: return "Denied";
        default:   return "Fail (unlisted code)";
        }
    }

    inline constexpr int kFeatureSuperSampling = 1;
    inline constexpr int kFeatureFrameGeneration = 11;
    inline constexpr int kFeatureRayReconstruction = 13;
    inline constexpr int kFeatureNeuralRendering = 18;

    inline constexpr int kEngineTypeCustom = 0;

    struct Handle
    {
        unsigned int Id;
    };

    struct Parameter
    {
        virtual void Set(const char* a_name, unsigned long long a_value) = 0;
        virtual void Set(const char* a_name, float a_value) = 0;
        virtual void Set(const char* a_name, double a_value) = 0;
        virtual void Set(const char* a_name, unsigned int a_value) = 0;
        virtual void Set(const char* a_name, int a_value) = 0;
        virtual void Set(const char* a_name, ID3D11Resource* a_value) = 0;
        virtual void Set(const char* a_name, ID3D12Resource* a_value) = 0;
        virtual void Set(const char* a_name, void* a_value) = 0;
        virtual Result Get(const char* a_name, unsigned long long* a_value) const = 0;
        virtual Result Get(const char* a_name, float* a_value) const = 0;
        virtual Result Get(const char* a_name, double* a_value) const = 0;
        virtual Result Get(const char* a_name, unsigned int* a_value) const = 0;
        virtual Result Get(const char* a_name, int* a_value) const = 0;
        virtual Result Get(const char* a_name, ID3D11Resource** a_value) const = 0;
        virtual Result Get(const char* a_name, ID3D12Resource** a_value) const = 0;
        virtual Result Get(const char* a_name, void** a_value) const = 0;
        virtual void Reset() = 0;
    };

    struct PathListInfo
    {
        const wchar_t* const* Path;
        unsigned int Length;
    };

    enum LoggingLevel : int
    {
        kLogOff = 0,
        kLogOn = 1,
        kLogVerbose = 2,
    };

    using AppLogCallback = void(__cdecl*)(const char* a_message, LoggingLevel a_level, int a_feature);

    struct LoggingInfo
    {
        AppLogCallback Callback;
        LoggingLevel Level;
        bool DisableOtherLoggingSinks;
    };

    struct FeatureCommonInfo
    {
        PathListInfo PathListInfo;
        void* InternalData;
        LoggingInfo LoggingInfo;
    };

    static_assert(offsetof(LoggingInfo, Callback) == 0 && offsetof(LoggingInfo, Level) == 8
                      && offsetof(LoggingInfo, DisableOtherLoggingSinks) == 12 && sizeof(LoggingInfo) == 16,
        "NVSDK_NGX_LoggingInfo layout");
    static_assert(offsetof(FeatureCommonInfo, InternalData) == 16 && offsetof(FeatureCommonInfo, LoggingInfo) == 24
                      && sizeof(FeatureCommonInfo) == 40,
        "NVSDK_NGX_FeatureCommonInfo layout");

    using PFN_D3D12_Init_ProjectID = Result(__cdecl*)(const char* a_projectId, int a_engineType,
        const char* a_engineVersion, const wchar_t* a_appDataPath, ID3D12Device* a_device,
        int a_sdkVersion, const FeatureCommonInfo* a_info);
    using PFN_D3D12_Init_Ext = Result(__cdecl*)(unsigned long long a_appId,
        const wchar_t* a_appDataPath, ID3D12Device* a_device, int a_sdkVersion,
        const FeatureCommonInfo* a_info);
    using PFN_D3D12_AllocateParameters = Result(__cdecl*)(Parameter** a_out);
    using PFN_D3D12_DestroyParameters = Result(__cdecl*)(Parameter* a_params);
    using PFN_D3D12_GetCapabilityParameters = Result(__cdecl*)(Parameter** a_out);
    using PFN_D3D12_CreateFeature = Result(__cdecl*)(ID3D12GraphicsCommandList* a_list,
        int a_feature, Parameter* a_params, Handle** a_out);
    using PFN_D3D12_EvaluateFeature = Result(__cdecl*)(ID3D12GraphicsCommandList* a_list,
        const Handle* a_handle, const Parameter* a_params, void* a_progressCallback);
    using PFN_D3D12_ReleaseFeature = Result(__cdecl*)(Handle* a_handle);
    using PFN_D3D12_Shutdown = Result(__cdecl*)();
    using PFN_D3D12_Shutdown1 = Result(__cdecl*)(ID3D12Device* a_device);

    using PFN_Snippet_Init_Ext = Result(__cdecl*)(unsigned long long a_appId,
        const wchar_t* a_appDataPath, ID3D12Device* a_device, const FeatureCommonInfo* a_info,
        int a_sdkVersion);

    using PFN_Shim_CallInit = Result(__cdecl*)(void* a_realFn, unsigned long long a_appId,
        const wchar_t* a_appDataPath, ID3D12Device* a_device, int a_sdkVersion,
        const FeatureCommonInfo* a_info);
    using PFN_Shim_CallCreate = Result(__cdecl*)(void* a_realFn, ID3D12GraphicsCommandList* a_list,
        int a_feature, Parameter* a_params, Handle** a_out);
    using PFN_Shim_CallEvaluate = Result(__cdecl*)(void* a_realFn,
        ID3D12GraphicsCommandList* a_list, const Handle* a_handle, const Parameter* a_params,
        void* a_progressCallback);
    using PFN_Shim_CallRelease = Result(__cdecl*)(void* a_realFn, Handle* a_handle);
    using PFN_Shim_CallShutdown = Result(__cdecl*)(void* a_realFn);
}
