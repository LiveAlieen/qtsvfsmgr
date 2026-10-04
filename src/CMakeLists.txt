add_library(qtsvfs_core STATIC
    qtsvfs/io/FileReader.cpp
    qtsvfs/probe/Probe.cpp
    qtsvfs/format/Kdb.cpp
    qtsvfs/format/PathHash.cpp
)
target_include_directories(qtsvfs_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(qtsvfs_core PUBLIC
    qtsvfs::3rd_zlib
    qtsvfs::3rd_lz4
    qtsvfs::3rd_zstd
    qtsvfs::3rd_ooz
    qtsvfs::3rd_lz3
)
if(MSVC)
    target_compile_options(qtsvfs_core PRIVATE /W4 /utf-8)
    target_compile_definitions(qtsvfs_core PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

add_executable(qtsvfs cli/main.cpp)
target_link_libraries(qtsvfs PRIVATE qtsvfs_core)
if(MSVC)
    target_compile_options(qtsvfs PRIVATE /W4 /utf-8)
    target_compile_definitions(qtsvfs PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
