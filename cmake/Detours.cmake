set(DETOURS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/extern/detours/detours")

if(NOT EXISTS "${DETOURS_DIR}/Detours.h")
    message(FATAL_ERROR "extern/detours is missing. Run scripts/setup.ps1 first.")
endif()

add_library(detours STATIC
    "${DETOURS_DIR}/Detours.cpp"
    "${DETOURS_DIR}/Detours32.cpp"
    "${DETOURS_DIR}/Detours64.cpp"
    "${DETOURS_DIR}/stdafx.cpp"
    "${DETOURS_DIR}/HideStaticLibSymbols.c"
)

target_include_directories(detours
    PUBLIC
    "${DETOURS_DIR}"
    PRIVATE
    "${DETOURS_DIR}/zydis/include"
    "${DETOURS_DIR}/zydis/src"
    "${DETOURS_DIR}/zydis/dependencies/zycore/include"
    "${DETOURS_DIR}/zydis/msvc"
)

target_compile_definitions(detours PRIVATE ZYDIS_STATIC_DEFINE ZYCORE_STATIC_DEFINE WIN32 _LIB)

if(MSVC)
    target_compile_options(detours PRIVATE /W0)
endif()
