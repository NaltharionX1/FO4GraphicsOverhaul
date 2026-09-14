cmake_minimum_required(VERSION 3.21)

project(
    FO4GraphicsOverhaul
    VERSION 1.0.0
    LANGUAGES CXX
)

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")

include(XSEPlugin)
include(CTest)

file(GLOB _fxc_candidates "C:/Program Files (x86)/Windows Kits/10/bin/*/x64/fxc.exe")
list(SORT _fxc_candidates)
list(REVERSE _fxc_candidates)
list(GET _fxc_candidates 0 FXC_EXECUTABLE)
if(NOT EXISTS "${FXC_EXECUTABLE}")
    message(FATAL_ERROR "fxc.exe not found in any Windows Kits bin directory — required for the GTAO shaders")
endif()
message(STATUS "fxc: ${FXC_EXECUTABLE}")

set(GTAO_SHADER_SRC "${PROJECT_SOURCE_DIR}/shaders/GtaoPasses.hlsl")
set(GTAO_INTEGRATE_SRC "${PROJECT_SOURCE_DIR}/shaders/AoIntegrateCS.hlsl")
set(GTAO_DECODE_SRC "${PROJECT_SOURCE_DIR}/shaders/GtaoDecodeCS.hlsl")
set(GTAO_XEGTAO_DIR "${PROJECT_SOURCE_DIR}/extern/XeGTAO/Source/Rendering/Shaders")
set(GTAO_SHADER_OUT "${CMAKE_CURRENT_BINARY_DIR}/shaders")
file(MAKE_DIRECTORY "${GTAO_SHADER_OUT}")

set(GTAO_ENTRY_POINTS
    "CSPrefilterDepths16x16|cs_5_0|g_csPrefilterDepths"
    "CSGenerateNormals|cs_5_0|g_csGenerateNormals"
    "CSGTAOLow|cs_5_0|g_csGtaoLow"
    "CSGTAOMedium|cs_5_0|g_csGtaoMedium"
    "CSGTAOHigh|cs_5_0|g_csGtaoHigh"
    "CSGTAOUltra|cs_5_0|g_csGtaoUltra"
    "CSGTAOExtreme|cs_5_0|g_csGtaoExtreme"
    "CSDenoisePass|cs_5_0|g_csDenoise"
    "CSDenoiseLastPass|cs_5_0|g_csDenoiseLast"
)

set(GTAO_SHADER_HEADERS "")
foreach(_entry_spec IN LISTS GTAO_ENTRY_POINTS)
    string(REPLACE "|" ";" _entry_parts "${_entry_spec}")
    list(GET _entry_parts 0 _entry)
    list(GET _entry_parts 1 _profile)
    list(GET _entry_parts 2 _variable)
    list(LENGTH _entry_parts _parts_len)
    set(_extra_flags "")
    if(_parts_len GREATER 3)
        list(GET _entry_parts 3 _extra_flags)
    endif()
    set(_out "${GTAO_SHADER_OUT}/${_entry}.h")
    add_custom_command(
        OUTPUT "${_out}"
        COMMAND "${FXC_EXECUTABLE}" /nologo /O3 /WX
                /T ${_profile} /E ${_entry}
                /I "${GTAO_XEGTAO_DIR}"
                ${_extra_flags}
                /Fh "${_out}" /Vn ${_variable}
                "${GTAO_SHADER_SRC}"
        DEPENDS "${GTAO_SHADER_SRC}"
                "${PROJECT_SOURCE_DIR}/shaders/XeGTAO_fxc.hlsli"
                "${GTAO_XEGTAO_DIR}/XeGTAO.h"
        COMMENT "fxc ${_profile} ${_entry}"
        VERBATIM
    )
    list(APPEND GTAO_SHADER_HEADERS "${_out}")
endforeach()

foreach(_components RANGE 1 4)
    set(_out "${GTAO_SHADER_OUT}/CSAoIntegrate${_components}.h")
    add_custom_command(
        OUTPUT "${_out}"
        COMMAND "${FXC_EXECUTABLE}" /nologo /O3 /WX
                /T cs_5_0 /E CSAoIntegrate
                /D TARGET_COMPONENTS=${_components}
                /Fh "${_out}" /Vn g_csAoIntegrate${_components}
                "${GTAO_INTEGRATE_SRC}"
        DEPENDS "${GTAO_INTEGRATE_SRC}"
        COMMENT "fxc cs_5_0 CSAoIntegrate (TARGET_COMPONENTS=${_components})"
        VERBATIM
    )
    list(APPEND GTAO_SHADER_HEADERS "${_out}")
endforeach()

add_custom_command(
    OUTPUT "${GTAO_SHADER_OUT}/CSDecodeGBuffer.h"
    COMMAND "${FXC_EXECUTABLE}" /nologo /O3 /WX
            /T cs_5_0 /E CSDecodeGBuffer
            /Fh "${GTAO_SHADER_OUT}/CSDecodeGBuffer.h" /Vn g_csDecodeGBuffer
            "${GTAO_DECODE_SRC}"
    DEPENDS "${GTAO_DECODE_SRC}"
    COMMENT "fxc cs_5_0 CSDecodeGBuffer"
    VERBATIM
)
list(APPEND GTAO_SHADER_HEADERS "${GTAO_SHADER_OUT}/CSDecodeGBuffer.h")

add_custom_command(
    OUTPUT "${GTAO_SHADER_OUT}/CSGrading.h"
    COMMAND "${FXC_EXECUTABLE}" /nologo /O3 /WX
            /T cs_5_0 /E CSGrading
            /Fh "${GTAO_SHADER_OUT}/CSGrading.h" /Vn g_csGrading
            "${PROJECT_SOURCE_DIR}/shaders/GradingCS.hlsl"
    DEPENDS "${PROJECT_SOURCE_DIR}/shaders/GradingCS.hlsl"
    COMMENT "fxc cs_5_0 CSGrading"
    VERBATIM
)
list(APPEND GTAO_SHADER_HEADERS "${GTAO_SHADER_OUT}/CSGrading.h")

set(BLOOM_ENTRY_POINTS
    "CSBloomPrefilter|BloomPrefilterCS|g_csBloomPrefilter"
    "CSBloomDown|BloomDownCS|g_csBloomDown"
    "CSBloomUp|BloomUpCS|g_csBloomUp"
    "CSBloomComposite|BloomCompositeCS|g_csBloomComposite"
)
foreach(_bloom_entry IN LISTS BLOOM_ENTRY_POINTS)
    string(REPLACE "|" ";" _bloom_parts "${_bloom_entry}")
    list(GET _bloom_parts 0 _entry)
    list(GET _bloom_parts 1 _source)
    list(GET _bloom_parts 2 _variable)
    add_custom_command(
        OUTPUT "${GTAO_SHADER_OUT}/${_entry}.h"
        COMMAND "${FXC_EXECUTABLE}" /nologo /O3 /WX
                /T cs_5_0 /E ${_entry}
                /Fh "${GTAO_SHADER_OUT}/${_entry}.h" /Vn ${_variable}
                "${PROJECT_SOURCE_DIR}/shaders/${_source}.hlsl"
        DEPENDS "${PROJECT_SOURCE_DIR}/shaders/${_source}.hlsl"
        COMMENT "fxc cs_5_0 ${_entry}"
        VERBATIM
    )
    list(APPEND GTAO_SHADER_HEADERS "${GTAO_SHADER_OUT}/${_entry}.h")
endforeach()

set(NEURAL_ENTRY_POINTS
    "CSNeuralDepth|g_csNeuralDepth"
    "CSNeuralColor|g_csNeuralColor"
    "CSUiAlpha|g_csUiAlpha"
    "CSFirstPersonMask|g_csFirstPersonMask"
    "CSFgDepthConditioned|g_csFgDepthConditioned"
    "CSFgMotionConditioned|g_csFgMotionConditioned"
    "CSDilateMotion|g_csDilateMotion"
)
foreach(_neural_entry IN LISTS NEURAL_ENTRY_POINTS)
    string(REPLACE "|" ";" _neural_parts "${_neural_entry}")
    list(GET _neural_parts 0 _entry)
    list(GET _neural_parts 1 _variable)
    add_custom_command(
        OUTPUT "${GTAO_SHADER_OUT}/${_entry}.h"
        COMMAND "${FXC_EXECUTABLE}" /nologo /O3 /WX
                /T cs_5_0 /E ${_entry}
                /Fh "${GTAO_SHADER_OUT}/${_entry}.h" /Vn ${_variable}
                "${PROJECT_SOURCE_DIR}/shaders/NeuralGuidesCS.hlsl"
        DEPENDS "${PROJECT_SOURCE_DIR}/shaders/NeuralGuidesCS.hlsl"
        COMMENT "fxc cs_5_0 ${_entry}"
        VERBATIM
    )
    list(APPEND GTAO_SHADER_HEADERS "${GTAO_SHADER_OUT}/${_entry}.h")
endforeach()

add_custom_command(
    OUTPUT "${GTAO_SHADER_OUT}/CSFingerprint.h"
    COMMAND "${FXC_EXECUTABLE}" /nologo /O3 /WX
            /T cs_5_0 /E CSFingerprint
            /Fh "${GTAO_SHADER_OUT}/CSFingerprint.h" /Vn g_csFingerprint
            "${PROJECT_SOURCE_DIR}/shaders/FingerprintCS.hlsl"
    DEPENDS "${PROJECT_SOURCE_DIR}/shaders/FingerprintCS.hlsl"
    COMMENT "fxc cs_5_0 CSFingerprint"
    VERBATIM
)
list(APPEND GTAO_SHADER_HEADERS "${GTAO_SHADER_OUT}/CSFingerprint.h")

add_custom_command(
    OUTPUT "${GTAO_SHADER_OUT}/CSFsrTransparency.h"
    COMMAND "${FXC_EXECUTABLE}" /nologo /O3 /WX
            /T cs_5_0 /E CSFsrTransparency
            /Fh "${GTAO_SHADER_OUT}/CSFsrTransparency.h" /Vn g_csFsrTransparency
            "${PROJECT_SOURCE_DIR}/shaders/FsrTransparencyCS.hlsl"
    DEPENDS "${PROJECT_SOURCE_DIR}/shaders/FsrTransparencyCS.hlsl"
    COMMENT "fxc cs_5_0 CSFsrTransparency"
    VERBATIM
)
list(APPEND GTAO_SHADER_HEADERS "${GTAO_SHADER_OUT}/CSFsrTransparency.h")

set(NEURAL_RESAMPLE_ENTRY_POINTS
    "CSNeuralResample|g_csNeuralResample"
    "CSNeuralCompose|g_csNeuralCompose"
)
foreach(_resample_entry IN LISTS NEURAL_RESAMPLE_ENTRY_POINTS)
    string(REPLACE "|" ";" _resample_parts "${_resample_entry}")
    list(GET _resample_parts 0 _entry)
    list(GET _resample_parts 1 _variable)
    add_custom_command(
        OUTPUT "${GTAO_SHADER_OUT}/${_entry}.h"
        COMMAND "${FXC_EXECUTABLE}" /nologo /O3 /WX
                /T cs_5_0 /E ${_entry}
                /Fh "${GTAO_SHADER_OUT}/${_entry}.h" /Vn ${_variable}
                "${PROJECT_SOURCE_DIR}/shaders/NeuralResampleCS.hlsl"
        DEPENDS "${PROJECT_SOURCE_DIR}/shaders/NeuralResampleCS.hlsl"
        COMMENT "fxc cs_5_0 ${_entry}"
        VERBATIM
    )
    list(APPEND GTAO_SHADER_HEADERS "${GTAO_SHADER_OUT}/${_entry}.h")
endforeach()

add_custom_target(GtaoShaders DEPENDS ${GTAO_SHADER_HEADERS})
add_dependencies("${PROJECT_NAME}" GtaoShaders)
target_include_directories("${PROJECT_NAME}" PRIVATE
    "${GTAO_SHADER_OUT}" "${GTAO_XEGTAO_DIR}")

add_custom_command(TARGET FO4GraphicsOverhaul POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_FILE:FO4GraphicsOverhaul>"
            "${CMAKE_CURRENT_SOURCE_DIR}/dist/Data/F4SE/Plugins/"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/dist/Data/F4SE/Plugins/FO4GraphicsOverhaul/FidelityFX"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/extern/FidelityFX-SDK/Kits/FidelityFX/signedbin/amd_fidelityfx_loader_dx12.dll"
            "${CMAKE_CURRENT_SOURCE_DIR}/dist/Data/F4SE/Plugins/FO4GraphicsOverhaul/FidelityFX/"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/extern/FidelityFX-SDK/Kits/FidelityFX/signedbin/amd_fidelityfx_framegeneration_dx12.dll"
            "${CMAKE_CURRENT_SOURCE_DIR}/dist/Data/F4SE/Plugins/FO4GraphicsOverhaul/FidelityFX/"
    COMMENT "staging FO4GraphicsOverhaul (dll) to dist"
    VERBATIM
)

add_library(FO4GraphicsOverhaul-nvngx SHARED shim/NgxCallerShim.cpp)
set_target_properties(FO4GraphicsOverhaul-nvngx PROPERTIES OUTPUT_NAME "FO4GraphicsOverhaul-nvngx" PREFIX "")
target_compile_features(FO4GraphicsOverhaul-nvngx PRIVATE cxx_std_23)
if(MSVC)
    target_compile_options(FO4GraphicsOverhaul-nvngx PRIVATE /W4 /WX /permissive- "$<$<CONFIG:Release>:/O2>")
    target_link_options(FO4GraphicsOverhaul-nvngx PRIVATE /WX)
endif()
add_dependencies("${PROJECT_NAME}" FO4GraphicsOverhaul-nvngx)

set(NGX_SHIM_DIR "${CMAKE_CURRENT_SOURCE_DIR}/dist/Data/F4SE/Plugins/FO4GraphicsOverhaul/NGX")
set(NGX_PAYLOAD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/dist/Data/F4SE/Plugins/FO4GraphicsOverhaul/Streamline")
set(DLSS_RUNTIME_PACKAGE "${CMAKE_CURRENT_SOURCE_DIR}/runtime/nvidia")
set(DLSS_FG_RUNTIME "${DLSS_RUNTIME_PACKAGE}/nvngx_dlssg.dll")
if(NOT EXISTS "${DLSS_FG_RUNTIME}")
    message(WARNING "DLSS FG runtime not found at ${DLSS_FG_RUNTIME}; frame generation will report unavailable")
    set(DLSS_FG_RUNTIME_STAGE "")
else()
    set(DLSS_FG_RUNTIME_STAGE COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${DLSS_FG_RUNTIME}" "${NGX_PAYLOAD_DIR}/")
endif()
set(DLSS_SR_RUNTIME "${DLSS_RUNTIME_PACKAGE}/nvngx_dlss.dll")
if(NOT EXISTS "${DLSS_SR_RUNTIME}")
    message(WARNING "DLSS SR runtime not found at ${DLSS_SR_RUNTIME}; DX12 mode will rely on the driver's own DLSS")
    set(DLSS_SR_RUNTIME_STAGE "")
else()
    set(DLSS_SR_RUNTIME_STAGE COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${DLSS_SR_RUNTIME}" "${NGX_PAYLOAD_DIR}/")
endif()
set(DLSS_LICENSE_TEXT "${DLSS_RUNTIME_PACKAGE}/nvngx_dlss.license.txt")
if(NOT EXISTS "${DLSS_LICENSE_TEXT}")
    message(WARNING "DLSS licence text not found at ${DLSS_LICENSE_TEXT}; NVIDIA's runtimes will be staged without it (it must travel with them)")
    set(DLSS_LICENSE_STAGE "")
else()
    set(DLSS_LICENSE_STAGE COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${DLSS_LICENSE_TEXT}" "${NGX_PAYLOAD_DIR}/")
endif()
add_custom_command(TARGET FO4GraphicsOverhaul-nvngx POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${NGX_SHIM_DIR}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${NGX_PAYLOAD_DIR}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_FILE:FO4GraphicsOverhaul-nvngx>"
            "${NGX_SHIM_DIR}/"
    ${DLSS_SR_RUNTIME_STAGE}
    ${DLSS_FG_RUNTIME_STAGE}
    ${DLSS_LICENSE_STAGE}
    COMMENT "staging the DLSS 5 caller shim to dist/.../NGX and NVIDIA's runtimes to dist/.../Streamline"
    VERBATIM
)

if(BUILD_TESTING)
    add_executable(
        MenuSettingsTests
        tests/MenuSettingsTests.cpp
        src/Settings/MenuSettingsIO.cpp
        src/Platform/NeuralContract.cpp
    )
    target_include_directories(
        MenuSettingsTests
        PRIVATE
        "${PROJECT_SOURCE_DIR}/include"
    )
    target_compile_features(MenuSettingsTests PRIVATE cxx_std_23)
    if(MSVC)
        target_compile_options(MenuSettingsTests PRIVATE /W4 /WX /permissive-)
    endif()

    add_test(NAME MenuSettingsTests COMMAND MenuSettingsTests)

    add_executable(
        NeuralContractTests
        tests/NeuralContractTests.cpp
        src/Platform/NeuralContract.cpp
    )
    target_include_directories(
        NeuralContractTests
        PRIVATE
        "${PROJECT_SOURCE_DIR}/include"
    )
    target_compile_features(NeuralContractTests PRIVATE cxx_std_23)
    if(MSVC)
        target_compile_options(NeuralContractTests PRIVATE /W4 /WX /permissive-)
    endif()

    add_test(NAME NeuralContractTests COMMAND NeuralContractTests)

    add_executable(
        FingerprintMathTests
        tests/FingerprintMathTests.cpp
    )
    target_include_directories(
        FingerprintMathTests
        PRIVATE
        "${PROJECT_SOURCE_DIR}/include"
    )
    target_compile_features(FingerprintMathTests PRIVATE cxx_std_23)
    if(MSVC)
        target_compile_options(FingerprintMathTests PRIVATE /W4 /WX /permissive-)
    endif()

    add_test(NAME FingerprintMathTests COMMAND FingerprintMathTests)

    add_executable(
        DlaaSettingsIOTests
        tests/DlaaSettingsIOTests.cpp
        src/Platform/DlaaSettingsIO.cpp
    )
    target_include_directories(
        DlaaSettingsIOTests
        PRIVATE
        "${PROJECT_SOURCE_DIR}/include"
    )
    target_compile_features(DlaaSettingsIOTests PRIVATE cxx_std_23)
    target_compile_definitions(DlaaSettingsIOTests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    if(MSVC)
        target_compile_options(DlaaSettingsIOTests PRIVATE /W4 /WX /permissive-)
    endif()

    add_test(NAME DlaaSettingsIOTests COMMAND DlaaSettingsIOTests)

    add_executable(DynamicMultiplierTests tests/DynamicMultiplierTests.cpp)
    target_include_directories(DynamicMultiplierTests PRIVATE "${PROJECT_SOURCE_DIR}/include")
    target_compile_features(DynamicMultiplierTests PRIVATE cxx_std_23)
    target_compile_definitions(DynamicMultiplierTests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    if(MSVC)
        target_compile_options(DynamicMultiplierTests PRIVATE /W4 /WX /permissive-)
    endif()
    add_test(NAME DynamicMultiplierTests COMMAND DynamicMultiplierTests)

    add_executable(
        SidecarFrameTests
        tests/SidecarFrameTests.cpp
        src/Platform/SidecarFrame.cpp
    )
    target_include_directories(
        SidecarFrameTests
        PRIVATE
        "${PROJECT_SOURCE_DIR}/include"
        "${PROJECT_SOURCE_DIR}/src"
    )
    target_compile_features(SidecarFrameTests PRIVATE cxx_std_23)
    target_compile_definitions(SidecarFrameTests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    if(MSVC)
        target_compile_options(SidecarFrameTests PRIVATE /W4 /WX /permissive-)
    endif()

    add_test(NAME SidecarFrameTests COMMAND SidecarFrameTests)

    add_executable(
        NeuralCascadeGpuTests
        tests/NeuralCascadeGpuTests.cpp
        tests/NeuralRendererStub.cpp
        src/Platform/NeuralPass.cpp
        src/Platform/NeuralContract.cpp
        src/Platform/NeuralMailbox.cpp
        src/Platform/SidecarFrame.cpp
        src/Platform/D3D12Sidecar.cpp
        src/Platform/SidecarCompute.cpp
    )
    add_dependencies(NeuralCascadeGpuTests GtaoShaders)
    target_include_directories(
        NeuralCascadeGpuTests
        PRIVATE
        "${PROJECT_SOURCE_DIR}/tests/stubs/neural"
        "${PROJECT_SOURCE_DIR}/tests"
        "${PROJECT_SOURCE_DIR}/include"
        "${PROJECT_SOURCE_DIR}/src"
        "${GTAO_SHADER_OUT}"
        "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/include"
    )
    target_compile_features(NeuralCascadeGpuTests PRIVATE cxx_std_23)
    target_compile_definitions(NeuralCascadeGpuTests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX FMT_HEADER_ONLY)
    target_link_libraries(NeuralCascadeGpuTests PRIVATE d3d11 d3d12 dxgi dxguid)
    if(MSVC)
        target_compile_options(NeuralCascadeGpuTests PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
    endif()

    add_test(NAME NeuralCascadeGpuTests COMMAND NeuralCascadeGpuTests)
    set_tests_properties(NeuralCascadeGpuTests PROPERTIES TIMEOUT 240)

    add_executable(
        NeuralMailboxTests
        tests/NeuralMailboxTests.cpp
        src/Platform/NeuralMailbox.cpp
        src/Platform/NeuralContract.cpp
    )
    target_include_directories(
        NeuralMailboxTests
        PRIVATE
        "${PROJECT_SOURCE_DIR}/include"
    )
    target_compile_features(NeuralMailboxTests PRIVATE cxx_std_23)
    if(MSVC)
        target_compile_options(NeuralMailboxTests PRIVATE /W4 /WX /permissive-)
    endif()

    target_link_libraries(NeuralMailboxTests PRIVATE Threads::Threads)

    add_test(NAME NeuralMailboxTests COMMAND NeuralMailboxTests)

    add_executable(
        GtaoOcclusionTests
        tests/GtaoOcclusionTests.cpp
    )
    target_include_directories(
        GtaoOcclusionTests
        PRIVATE
        "${PROJECT_SOURCE_DIR}/include"
    )
    target_compile_features(GtaoOcclusionTests PRIVATE cxx_std_23)
    if(MSVC)
        target_compile_options(GtaoOcclusionTests PRIVATE /W4 /WX /permissive-)
    endif()

    add_test(NAME GtaoOcclusionTests COMMAND GtaoOcclusionTests)

    find_package(Threads REQUIRED)

    add_executable(
        TelemetryTests
        tests/TelemetryTests.cpp
        src/Telemetry/FrameTimings.cpp
    )
    target_include_directories(TelemetryTests PRIVATE "${PROJECT_SOURCE_DIR}/include")
    target_compile_features(TelemetryTests PRIVATE cxx_std_23)
    if(MSVC)
        target_compile_options(TelemetryTests PRIVATE /W4 /WX /permissive-)
    endif()
    target_link_libraries(TelemetryTests PRIVATE Threads::Threads)
    add_test(NAME TelemetryTests COMMAND TelemetryTests)

    add_executable(
        AaMailboxTests
        tests/AaMailboxTests.cpp
        src/Platform/AaMailbox.cpp
        src/Platform/AaEngineResolution.cpp
    )
    target_include_directories(AaMailboxTests PRIVATE "${PROJECT_SOURCE_DIR}/include")
    target_compile_features(AaMailboxTests PRIVATE cxx_std_23)
    if(MSVC)
        target_compile_options(AaMailboxTests PRIVATE /W4 /WX /permissive-)
    endif()
    target_link_libraries(AaMailboxTests PRIVATE Threads::Threads)
    add_test(NAME AaMailboxTests COMMAND AaMailboxTests)

    add_executable(
        SubNativeMathTests
        tests/SubNativeMathTests.cpp
    )
    target_include_directories(SubNativeMathTests PRIVATE include)
    target_compile_features(SubNativeMathTests PRIVATE cxx_std_23)
    if(MSVC)
        target_compile_options(SubNativeMathTests PRIVATE /W4 /WX /permissive-)
    endif()
    add_test(NAME SubNativeMathTests COMMAND SubNativeMathTests)

    add_executable(
        PresentMathTests
        tests/PresentMathTests.cpp
    )
    target_include_directories(PresentMathTests PRIVATE include)
    target_compile_features(PresentMathTests PRIVATE cxx_std_23)
    if(MSVC)
        target_compile_options(PresentMathTests PRIVATE /W4 /WX /permissive-)
    endif()
    add_test(NAME PresentMathTests COMMAND PresentMathTests)

    if(EXISTS "${DLSS_FG_RUNTIME}")
        add_executable(
            MfgTemporalFixTests
            tests/MfgTemporalFixTests.cpp
            src/Platform/MfgTemporalFix.cpp
        )
        target_include_directories(MfgTemporalFixTests PRIVATE "${PROJECT_SOURCE_DIR}/src")
        target_compile_features(MfgTemporalFixTests PRIVATE cxx_std_23)
        target_compile_definitions(MfgTemporalFixTests PRIVATE "MFG_TEST_DLL=\"${DLSS_FG_RUNTIME}\"")
        target_link_libraries(MfgTemporalFixTests PRIVATE bcrypt.lib)
        if(MSVC)
            target_compile_options(MfgTemporalFixTests PRIVATE /W4 /WX /permissive-)
        endif()
        add_test(NAME MfgTemporalFixTests COMMAND MfgTemporalFixTests)

        add_executable(
            MfgTemporalFixFaultTests
            tests/MfgTemporalFixFaultTests.cpp
        )
        target_include_directories(MfgTemporalFixFaultTests PRIVATE "${PROJECT_SOURCE_DIR}/src")
        target_compile_features(MfgTemporalFixFaultTests PRIVATE cxx_std_23)
        target_compile_definitions(MfgTemporalFixFaultTests PRIVATE "MFG_TEST_DLL=\"${DLSS_FG_RUNTIME}\"")
        target_link_libraries(MfgTemporalFixFaultTests PRIVATE bcrypt.lib)
        if(MSVC)
            target_compile_options(MfgTemporalFixFaultTests PRIVATE /W4 /WX /permissive- /EHsc)
        endif()
        add_test(NAME MfgTemporalFixFaultTests COMMAND MfgTemporalFixFaultTests)

        add_executable(
            MfgKernelJitTests
            tests/MfgKernelJitTests.cpp
            src/Platform/MfgTemporalFix.cpp
            src/Platform/MfgKernelJit.cpp
        )
        target_include_directories(MfgKernelJitTests PRIVATE "${PROJECT_SOURCE_DIR}/src")
        target_compile_features(MfgKernelJitTests PRIVATE cxx_std_23)
        target_compile_definitions(MfgKernelJitTests PRIVATE "MFG_TEST_DLL=\"${DLSS_FG_RUNTIME}\"")
        target_link_libraries(MfgKernelJitTests PRIVATE bcrypt.lib)
        if(MSVC)
            target_compile_options(MfgKernelJitTests PRIVATE /W4 /WX /permissive- /EHsc)
        endif()
        add_test(NAME MfgKernelJitTests COMMAND MfgKernelJitTests)
        set_tests_properties(MfgKernelJitTests PROPERTIES TIMEOUT 180 SKIP_REGULAR_EXPRESSION "SKIPPED")

        add_executable(
            MfgCuModuleTests
            tests/MfgCuModuleTests.cpp
            src/Platform/MfgTemporalFix.cpp
            src/Platform/MfgCuModuleProbe.cpp
        )
        target_include_directories(MfgCuModuleTests PRIVATE "${PROJECT_SOURCE_DIR}/src" "${PROJECT_SOURCE_DIR}/extern/nvapi")
        target_compile_features(MfgCuModuleTests PRIVATE cxx_std_23)
        target_compile_definitions(MfgCuModuleTests PRIVATE "MFG_TEST_DLL=\"${DLSS_FG_RUNTIME}\"")
        target_link_libraries(MfgCuModuleTests PRIVATE bcrypt.lib d3d12 dxgi "${PROJECT_SOURCE_DIR}/extern/nvapi/amd64/nvapi64.lib")
        if(MSVC)
            target_compile_options(MfgCuModuleTests PRIVATE /W4 /WX /permissive- /EHsc)
        endif()
        add_test(NAME MfgCuModuleTests COMMAND MfgCuModuleTests)
        set_tests_properties(MfgCuModuleTests PROPERTIES TIMEOUT 180 SKIP_REGULAR_EXPRESSION "SKIPPED")
    else()
        message(WARNING "DLSS FG runtime not found at ${DLSS_FG_RUNTIME}; MfgTemporalFixTests is not registered")
    endif()

    add_executable(
        AaEngineResolutionTests
        tests/AaEngineResolutionTests.cpp
        src/Platform/AaEngineResolution.cpp
    )
    target_include_directories(AaEngineResolutionTests PRIVATE "${PROJECT_SOURCE_DIR}/include")
    target_compile_features(AaEngineResolutionTests PRIVATE cxx_std_23)
    if(MSVC)
        target_compile_options(AaEngineResolutionTests PRIVATE /W4 /WX /permissive-)
    endif()
    add_test(NAME AaEngineResolutionTests COMMAND AaEngineResolutionTests)

    add_executable(
        WiringAdapterTests
        tests/WiringAdapterTests.cpp
        src/Platform/WiringAdapters.cpp
    )
    target_include_directories(WiringAdapterTests PRIVATE "${PROJECT_SOURCE_DIR}/include")
    target_compile_features(WiringAdapterTests PRIVATE cxx_std_23)
    if(MSVC)
        target_compile_options(WiringAdapterTests PRIVATE /W4 /WX /permissive-)
    endif()
    target_link_libraries(WiringAdapterTests PRIVATE Threads::Threads)
    add_test(NAME WiringAdapterTests COMMAND WiringAdapterTests)

    add_executable(
        SamplerGenerationTests
        tests/SamplerGenerationTests.cpp
        src/Platform/SamplerGeneration.cpp
    )
    target_include_directories(SamplerGenerationTests PRIVATE "${PROJECT_SOURCE_DIR}/include")
    target_compile_features(SamplerGenerationTests PRIVATE cxx_std_23)
    if(MSVC)
        target_compile_options(SamplerGenerationTests PRIVATE /W4 /WX /permissive-)
    endif()
    add_test(NAME SamplerGenerationTests COMMAND SamplerGenerationTests)

    add_executable(CommandCatalogueTests tests/CommandCatalogueTests.cpp)
    target_include_directories(CommandCatalogueTests PRIVATE "${PROJECT_SOURCE_DIR}/include")
    target_compile_features(CommandCatalogueTests PRIVATE cxx_std_23)
    if(MSVC)
        target_compile_options(CommandCatalogueTests PRIVATE /W4 /WX /permissive-)
    endif()
    add_test(NAME CommandCatalogueTests COMMAND CommandCatalogueTests)

    add_executable(
        CommandComposerTests
        tests/CommandComposerTests.cpp
        src/Platform/CommandComposer.cpp
    )
    target_include_directories(CommandComposerTests PRIVATE "${PROJECT_SOURCE_DIR}/include")
    target_compile_features(CommandComposerTests PRIVATE cxx_std_23)
    if(MSVC)
        target_compile_options(CommandComposerTests PRIVATE /W4 /WX /permissive-)
    endif()
    add_test(NAME CommandComposerTests COMMAND CommandComposerTests)

    add_executable(DeviceGenerationTrackerTests tests/DeviceGenerationTrackerTests.cpp)
    target_include_directories(DeviceGenerationTrackerTests PRIVATE "${PROJECT_SOURCE_DIR}/include")
    target_compile_features(DeviceGenerationTrackerTests PRIVATE cxx_std_23)
    if(MSVC)
        target_compile_options(DeviceGenerationTrackerTests PRIVATE /W4 /WX /permissive-)
    endif()
    target_link_libraries(DeviceGenerationTrackerTests PRIVATE Threads::Threads)
    add_test(NAME DeviceGenerationTrackerTests COMMAND DeviceGenerationTrackerTests)
endif()

include(Package)
