add_library(qtsvfs_core STATIC
    qtsvfs/io/FileReader.cpp
    qtsvfs/probe/Probe.cpp
    qtsvfs/format/Kdb.cpp
    qtsvfs/format/PathHash.cpp
    qtsvfs/format/GlobalIndex.cpp
    qtsvfs/format/QtsfNode.cpp
    qtsvfs/codec/Codec.cpp
    qtsvfs/Package.cpp
    qtsvfs/Names.cpp
    qtsvfs/Tree.cpp
    qtsvfs/Export.cpp
    qtsvfs/format/QtsfStream.cpp
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

# 图形界面：Qt Widgets（没找到 Qt6 就只出 CLI，不影响主构建）
option(QTSVFS_BUILD_GUI "构建 Qt Widgets 图形界面" ON)
if(QTSVFS_BUILD_GUI)
    find_package(Qt6 QUIET COMPONENTS Core Gui Widgets)
    if(Qt6_FOUND)
        set(CMAKE_AUTOMOC ON)
        add_executable(qtsvfs-gui WIN32
            gui/main.cpp
            gui/MainWindow.cpp
            gui/MainWindow.h
        )
        target_link_libraries(qtsvfs-gui PRIVATE qtsvfs_core Qt6::Widgets Qt6::Gui Qt6::Core)
        if(MSVC)
            target_compile_options(qtsvfs-gui PRIVATE /W4 /utf-8)
        endif()
        message(STATUS "启用 GUI：qtsvfs-gui（Qt ${Qt6_VERSION}）")
    else()
        message(STATUS "未找到 Qt6，跳过 qtsvfs-gui")
    endif()
endif()
