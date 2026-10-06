find_package(Qt6 QUIET COMPONENTS Core Gui Widgets)
if(NOT Qt6_FOUND)
    message(STATUS "未找到 Qt6，跳过 qtsvfs-gui")
    return()
endif()

set(CMAKE_AUTOMOC ON)

add_executable(qtsvfs-gui WIN32
    main.cpp
    MainWindow.cpp
    MainWindow.h
)

target_link_libraries(qtsvfs-gui PRIVATE qtsvfs_core Qt6::Widgets Qt6::Gui Qt6::Core)

if(MSVC)
    target_compile_options(qtsvfs-gui PRIVATE /W4 /utf-8)
endif()

message(STATUS "启用 GUI：qtsvfs-gui（Qt ${Qt6_VERSION}）")
