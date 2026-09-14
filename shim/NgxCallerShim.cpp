// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from ComfyUI-DLSS5-NR caller_shim.cpp, Copyright (c) 2026 ComfyUI-DLSS5-NR contributors, MIT License.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

struct ID3D12Device;
struct ID3D12GraphicsCommandList;

namespace
{
    using Result = int;
    struct NgxHandle;
    struct NgxParameter;

    using SnippetInitFn = Result(__cdecl*)(unsigned long long, const wchar_t*, ID3D12Device*, const void*, int);
    using CreateFn = Result(__cdecl*)(ID3D12GraphicsCommandList*, int, NgxParameter*, NgxHandle**);
    using EvaluateFn = Result(__cdecl*)(ID3D12GraphicsCommandList*, const NgxHandle*, const NgxParameter*, void*);
    using ReleaseFn = Result(__cdecl*)(NgxHandle*);
    using ShutdownFn = Result(__cdecl*)();

    volatile LONG g_postCallSink = 0;

    __forceinline Result Finish(Result a_result)
    {
        g_postCallSink = static_cast<LONG>(a_result);
        return a_result;
    }
}

extern "C" {

__declspec(dllexport) __declspec(noinline) Result __cdecl DLSSNR_CallInit(void* a_realFn,
    unsigned long long a_appId, const wchar_t* a_path, ID3D12Device* a_device, int a_version,
    const void* a_commonInfo)
{
    const Result r = reinterpret_cast<SnippetInitFn>(a_realFn)(a_appId, a_path, a_device, a_commonInfo, a_version);
    return Finish(r);
}

__declspec(dllexport) __declspec(noinline) Result __cdecl DLSSNR_CallCreate(void* a_realFn,
    ID3D12GraphicsCommandList* a_list, int a_feature, NgxParameter* a_params, NgxHandle** a_handle)
{
    const Result r = reinterpret_cast<CreateFn>(a_realFn)(a_list, a_feature, a_params, a_handle);
    return Finish(r);
}

__declspec(dllexport) __declspec(noinline) Result __cdecl DLSSNR_CallEvaluate(void* a_realFn,
    ID3D12GraphicsCommandList* a_list, const NgxHandle* a_handle, const NgxParameter* a_params,
    void* a_callback)
{
    const Result r = reinterpret_cast<EvaluateFn>(a_realFn)(a_list, a_handle, a_params, a_callback);
    return Finish(r);
}

__declspec(dllexport) __declspec(noinline) Result __cdecl DLSSNR_CallRelease(void* a_realFn, NgxHandle* a_handle)
{
    const Result r = reinterpret_cast<ReleaseFn>(a_realFn)(a_handle);
    return Finish(r);
}

__declspec(dllexport) __declspec(noinline) Result __cdecl DLSSNR_CallShutdown(void* a_realFn)
{
    const Result r = reinterpret_cast<ShutdownFn>(a_realFn)();
    return Finish(r);
}

}

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID)
{
    return TRUE;
}
