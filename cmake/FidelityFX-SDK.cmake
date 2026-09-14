set(FFX_API_VK OFF)
set(FFX_API_DX12 OFF)
set(FFX_ALL OFF)
set(FFX_FSR3 ON)
set(FFX_FSR ON)
set(FFX_AUTO_COMPILE_SHADERS 1)

get_filename_component(_ffx_sdk_root "${CMAKE_CURRENT_LIST_DIR}/../extern/FidelityFX-SDK-DX11" ABSOLUTE)
if(NOT EXISTS "${_ffx_sdk_root}/sdk/CMakeLists.txt")
    message(FATAL_ERROR "extern/FidelityFX-SDK-DX11 is missing. Run scripts/setup.ps1 first.")
endif()
file(READ "${_ffx_sdk_root}/sdk/include/FidelityFX/host/backends/dx11/ffx_dx11.h" _ffx_dx11_header)
string(FIND "${_ffx_dx11_header}" "ffxDx11SetFailureLog" _ffx_patched)
if(_ffx_patched EQUAL -1)
    message(FATAL_ERROR "extern/FidelityFX-SDK-DX11 is not patched. Run scripts/setup.ps1 -Force.")
endif()

set(_dlss_addon_saved_cxx_flags "${CMAKE_CXX_FLAGS}")
string(REPLACE "/WX" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
string(REPLACE "/W4" "/W0" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")

add_subdirectory("${_ffx_sdk_root}/sdk" "${CMAKE_CURRENT_BINARY_DIR}/FidelityFX-SDK" EXCLUDE_FROM_ALL)

set(CMAKE_CXX_FLAGS "${_dlss_addon_saved_cxx_flags}")

target_include_directories(
    "${PROJECT_NAME}"
    PRIVATE
    "${_ffx_sdk_root}/sdk/include"
)

target_link_libraries(
    "${PROJECT_NAME}"
    PUBLIC
    ffx_backend_dx11_x64
    ffx_fsr3upscaler_x64
    ffx_fsr3_x64
)
