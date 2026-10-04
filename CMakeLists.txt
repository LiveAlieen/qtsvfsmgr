cmake_minimum_required(VERSION 3.25)
project(qtsvfs LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

option(QTSVFS_BUILD_TESTS "Build unit tests" ON)

# H:\tool\压缩算法 是只读的 vendor 目录（zlib/lz4/zstd/ooz/lz3 已按 QtsVFS 内嵌版本整理）。
# 本文件只用其源码与头文件，不复制、不修改，构建产物全部落在本工程 build/ 下。
set(QTSVFS_VENDOR_CODECS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../tool/压缩算法"
    CACHE PATH "只读引用的压缩算法源码目录")

if(NOT EXISTS "${QTSVFS_VENDOR_CODECS_DIR}/zlib/zlib.h")
    message(FATAL_ERROR
        "找不到 vendor 压缩算法目录，请用 -DQTSVFS_VENDOR_CODECS_DIR=<path> 指定: ${QTSVFS_VENDOR_CODECS_DIR}")
endif()

add_subdirectory(thirdparty)
add_subdirectory(src)

if(QTSVFS_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    enable_testing()
    add_subdirectory(tests)
endif()
