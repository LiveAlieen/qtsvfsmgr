cmake_minimum_required(VERSION 3.25)
project(qtsvfs LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

option(QTSVFS_BUILD_GUI "构建 Qt Widgets 图形界面" ON)
option(QTSVFS_BUILD_TESTS "Build unit tests" ON)

add_subdirectory(lib)
add_subdirectory(cli)

if(QTSVFS_BUILD_GUI)
    add_subdirectory(gui)
endif()

if(QTSVFS_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    enable_testing()
    add_subdirectory(tests)
endif()
