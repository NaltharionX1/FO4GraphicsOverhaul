function(add_cxx_files TARGET)
    file(GLOB_RECURSE INCLUDE_FILES
        LIST_DIRECTORIES false
        CONFIGURE_DEPENDS
        "include/*.h"
        "include/*.hpp"
        "include/*.inl"
    )
    file(GLOB_RECURSE SOURCE_FILES
        LIST_DIRECTORIES false
        CONFIGURE_DEPENDS
        "src/*.h"
        "src/*.hpp"
        "src/*.cpp"
    )
    source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/include" PREFIX "Header Files" FILES ${INCLUDE_FILES})
    source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/src" PREFIX "Source Files" FILES ${SOURCE_FILES})
    target_sources("${TARGET}" PRIVATE ${INCLUDE_FILES} ${SOURCE_FILES})
endfunction()
