add_library("${PROJECT_NAME}" SHARED)

target_compile_features("${PROJECT_NAME}" PRIVATE cxx_std_23)

include(AddCXXFiles)
add_cxx_files("${PROJECT_NAME}")

target_include_directories("${PROJECT_NAME}" PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/extern/FidelityFX-SDK/Kits/FidelityFX/api/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/extern/FidelityFX-SDK/Kits/FidelityFX/framegeneration/include")

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Plugin.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/cmake/Plugin.h"
    @ONLY
)
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Version.rc.in"
    "${CMAKE_CURRENT_BINARY_DIR}/cmake/Version.rc"
    @ONLY
)

target_sources(
    "${PROJECT_NAME}"
    PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}/cmake/Plugin.h"
    "${CMAKE_CURRENT_BINARY_DIR}/cmake/Version.rc"
)

target_precompile_headers("${PROJECT_NAME}" PRIVATE include/PCH.h)

target_compile_definitions(
    "${PROJECT_NAME}"
    PRIVATE
    FALLOUT_PRE_NG
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

if(MSVC)
    target_compile_options(
        "${PROJECT_NAME}"
        PRIVATE
        /MP
        /W4
        /WX
        /permissive-
        /Zc:__cplusplus
        /Zc:preprocessor
        /arch:AVX
        "$<$<CONFIG:Release>:/Zi;/O2;/Ob2;/Oi;/Ot>"
    )
    target_link_options(
        "${PROJECT_NAME}"
        PRIVATE
        /WX
        "$<$<CONFIG:Release>:/INCREMENTAL:NO;/OPT:REF;/OPT:ICF;/DEBUG:FULL>"
    )
endif()

add_subdirectory(
    "${CMAKE_CURRENT_SOURCE_DIR}/extern/CommonLibF4/CommonLibF4"
    "${CMAKE_CURRENT_BINARY_DIR}/CommonLibF4"
    EXCLUDE_FROM_ALL
)

include(Detours)

target_include_directories(
    "${PROJECT_NAME}"
    PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}/cmake"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/extern/nvapi"
)

target_link_libraries(
    "${PROJECT_NAME}"
    PUBLIC
    CommonLibF4::CommonLibF4
    d3d11.lib
    dxgi.lib
    d3d12.lib
    bcrypt.lib
    wintrust.lib
    version.lib
    detours
    "${CMAKE_CURRENT_SOURCE_DIR}/extern/nvapi/amd64/nvapi64.lib"
)

target_include_directories(
    "${PROJECT_NAME}"
    PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/extern/detours/detours"
)

set(IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/extern/imgui")
set(IMGUI_SOURCES
    "${IMGUI_DIR}/imgui.cpp"
    "${IMGUI_DIR}/imgui_draw.cpp"
    "${IMGUI_DIR}/imgui_tables.cpp"
    "${IMGUI_DIR}/imgui_widgets.cpp"
    "${IMGUI_DIR}/imgui_demo.cpp"
    "${IMGUI_DIR}/imgui.h"
    "${IMGUI_DIR}/imgui_internal.h"
    "${IMGUI_DIR}/imstb_rectpack.h"
    "${IMGUI_DIR}/imstb_textedit.h"
    "${IMGUI_DIR}/imstb_truetype.h"
    "${IMGUI_DIR}/imconfig.h"
    "${IMGUI_DIR}/backends/imgui_impl_win32.h"
    "${IMGUI_DIR}/backends/imgui_impl_win32.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_dx11.h"
    "${IMGUI_DIR}/backends/imgui_impl_dx11.cpp"
)
source_group(TREE "${IMGUI_DIR}" PREFIX "extern/imgui" FILES ${IMGUI_SOURCES})
target_sources("${PROJECT_NAME}" PRIVATE ${IMGUI_SOURCES})

target_include_directories(
    "${PROJECT_NAME}"
    PRIVATE
    "${IMGUI_DIR}"
    "${IMGUI_DIR}/backends"
)

include(FidelityFX-SDK)

set_target_properties("${PROJECT_NAME}" PROPERTIES INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
