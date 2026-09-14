#include "Platform/MfgKernelJit.h"

#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#    define NOMINMAX
#endif
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace
{
    using CUresult = int;
    using CUdevice = int;
    using CUcontext = void*;
    using CUmodule = void*;
    using CUfunction = void*;

    constexpr int kJitInfoLogBuffer = 3;
    constexpr int kJitInfoLogBufferSize = 4;
    constexpr int kJitErrorLogBuffer = 5;
    constexpr int kJitErrorLogBufferSize = 6;
    constexpr int kFuncAttributeSharedSizeBytes = 1;
    constexpr int kFuncAttributeNumRegs = 4;
    constexpr int kDeviceAttributeComputeCapabilityMajor = 75;
    constexpr int kDeviceAttributeComputeCapabilityMinor = 76;

    enum Fn : int
    {
        kInit, kDeviceGet, kDeviceGetName, kDeviceGetAttribute, kDriverGetVersion, kCtxCreate, kCtxDestroy,
        kModuleLoadDataEx, kModuleGetFunction, kFuncGetAttribute, kModuleUnload, kGetErrorString
    };
    constexpr const char* kNames[]{ "cuInit", "cuDeviceGet", "cuDeviceGetName", "cuDeviceGetAttribute", "cuDriverGetVersion",
        "cuCtxCreate_v2", "cuCtxDestroy_v2", "cuModuleLoadDataEx", "cuModuleGetFunction", "cuFuncGetAttribute", "cuModuleUnload",
        "cuGetErrorString" };

    using InitFn = CUresult (*)(unsigned);
    using DeviceGetFn = CUresult (*)(CUdevice*, int);
    using DeviceGetNameFn = CUresult (*)(char*, int, CUdevice);
    using DeviceGetAttributeFn = CUresult (*)(int*, int, CUdevice);
    using DriverGetVersionFn = CUresult (*)(int*);
    using CtxCreateFn = CUresult (*)(CUcontext*, unsigned, CUdevice);
    using CtxDestroyFn = CUresult (*)(CUcontext);
    using ModuleLoadDataExFn = CUresult (*)(CUmodule*, const void*, unsigned, int*, void**);
    using ModuleGetFunctionFn = CUresult (*)(CUfunction*, CUmodule, const char*);
    using FuncGetAttributeFn = CUresult (*)(int*, int, CUfunction);
    using ModuleUnloadFn = CUresult (*)(CUmodule);
    using GetErrorStringFn = CUresult (*)(CUresult, const char**);

    template <class F>
    [[nodiscard]] F As(void* a_p) noexcept
    {
        return reinterpret_cast<F>(a_p);
    }

    void Say(char* a_out, std::size_t a_size, const char* a_text) noexcept
    {
        std::snprintf(a_out, a_size, "%s", a_text);
    }
}

namespace Platform::MfgKernelJit
{
    Session::~Session()
    {
        Close();
    }

    bool Session::Open(char* a_why, std::size_t a_whySize) noexcept
    {
        Close();
        const HMODULE module = ::LoadLibraryW(L"nvcuda.dll");
        if (module == nullptr) {
            Say(a_why, a_whySize, "nvcuda.dll (the NVIDIA driver API) is not on this machine");
            return false;
        }
        m_module = module;
        for (int i = 0; i < 12; ++i) {
            m_fn[i] = reinterpret_cast<void*>(::GetProcAddress(module, kNames[i]));
            if (m_fn[i] == nullptr) {
                std::snprintf(a_why, a_whySize, "the driver API lacks %s", kNames[i]);
                Close();
                return false;
            }
        }
        if (As<InitFn>(m_fn[kInit])(0) != 0) {
            Say(a_why, a_whySize, "the NVIDIA driver API would not initialise (no usable device)");
            Close();
            return false;
        }
        CUdevice device = 0;
        if (As<DeviceGetFn>(m_fn[kDeviceGet])(&device, 0) != 0) {
            Say(a_why, a_whySize, "no NVIDIA device 0");
            Close();
            return false;
        }
        (void)As<DeviceGetNameFn>(m_fn[kDeviceGetName])(m_name, sizeof(m_name), device);
        (void)As<DeviceGetAttributeFn>(m_fn[kDeviceGetAttribute])(&m_major, kDeviceAttributeComputeCapabilityMajor, device);
        (void)As<DeviceGetAttributeFn>(m_fn[kDeviceGetAttribute])(&m_minor, kDeviceAttributeComputeCapabilityMinor, device);
        (void)As<DriverGetVersionFn>(m_fn[kDriverGetVersion])(&m_driverVersion);
        CUcontext context = nullptr;
        const CUresult created = As<CtxCreateFn>(m_fn[kCtxCreate])(&context, 0, device);
        if (created != 0 || context == nullptr) {
            const char* text = nullptr;
            (void)As<GetErrorStringFn>(m_fn[kGetErrorString])(created, &text);
            std::snprintf(a_why, a_whySize, "a driver context could not be created on device 0: %d (%s)", created, text != nullptr ? text : "?");
            Close();
            return false;
        }
        m_context = context;
        return true;
    }

    void Session::Close() noexcept
    {
        if (m_context != nullptr && m_fn[kCtxDestroy] != nullptr) {
            (void)As<CtxDestroyFn>(m_fn[kCtxDestroy])(m_context);
        }
        m_context = nullptr;
        if (m_module != nullptr) {
            (void)::FreeLibrary(static_cast<HMODULE>(m_module));
        }
        m_module = nullptr;
        std::memset(m_fn, 0, sizeof(m_fn));
    }

    bool Session::Build(const char* a_ptx, std::size_t a_ptxBytes, const char* a_entry, Built& a_out, char* a_why,
        std::size_t a_whySize) noexcept
    {
        a_out = Built{};
        if (!IsOpen() || a_ptx == nullptr || a_entry == nullptr) {
            Say(a_why, a_whySize, "no driver session");
            return false;
        }
        try {
            std::string text(a_ptx, a_ptxBytes);
            text.push_back('\0');
            std::vector<char> errorLog(8192, '\0');
            std::vector<char> infoLog(8192, '\0');
            int options[]{ kJitErrorLogBuffer, kJitErrorLogBufferSize, kJitInfoLogBuffer, kJitInfoLogBufferSize };
            void* values[]{ errorLog.data(), reinterpret_cast<void*>(static_cast<std::uintptr_t>(errorLog.size())), infoLog.data(),
                reinterpret_cast<void*>(static_cast<std::uintptr_t>(infoLog.size())) };
            CUmodule module = nullptr;
            const CUresult loaded = As<ModuleLoadDataExFn>(m_fn[kModuleLoadDataEx])(&module, text.data(), 4, options, values);
            if (loaded != 0 || module == nullptr) {
                const char* name = nullptr;
                (void)As<GetErrorStringFn>(m_fn[kGetErrorString])(loaded, &name);
                char* const newline = std::strchr(errorLog.data(), '\n');
                if (newline != nullptr) *newline = '\0';
                std::snprintf(a_why, a_whySize, "the driver would not build it: %d (%s) %s", loaded, name != nullptr ? name : "?", errorLog.data());
                return false;
            }
            CUfunction function = nullptr;
            const CUresult resolved = As<ModuleGetFunctionFn>(m_fn[kModuleGetFunction])(&function, module, a_entry);
            if (resolved == 0 && function != nullptr) {
                (void)As<FuncGetAttributeFn>(m_fn[kFuncGetAttribute])(&a_out.sharedBytes, kFuncAttributeSharedSizeBytes, function);
                (void)As<FuncGetAttributeFn>(m_fn[kFuncGetAttribute])(&a_out.registers, kFuncAttributeNumRegs, function);
            }
            (void)As<ModuleUnloadFn>(m_fn[kModuleUnload])(module);
            if (resolved != 0 || function == nullptr) {
                std::snprintf(a_why, a_whySize, "the built module has no entry '%s'", a_entry);
                return false;
            }
            return true;
        } catch (...) {
            Say(a_why, a_whySize, "an exception while asking the driver to build the program");
            return false;
        }
    }
}
