# QtsVFS 内置 zlib 1.3.1（第三方依赖内嵌）
#
# 构建动态库 zlib（目标名 zlib_static，产物 libzlib.so），供 QtsVFS 压缩抽象层
# ZlibCompressor 使用（QTSVFS_USE_ZLIB），由 libqtsvfs_compression.so 动态链接。
# 源码来自 madler/zlib v1.3.1。
#
# 内嵌目的：避免依赖系统安装的 libz 开发包，实现"所有依赖集中到项目内"。

cmake_minimum_required(VERSION 3.14)
project(qtsvfs_zlib LANGUAGES C)

set(ZLIB_SOURCES
    adler32.c
    compress.c
    crc32.c
    deflate.c
    infback.c
    inffast.c
    inflate.c
    inftrees.c
    trees.c
    uncompr.c
    zutil.c
)

# zlib 需要生成 zconf.h（由 zconf.h.in 通过 configure 生成）。
# 我们直接提供一份预生成的 zconf.h（构建期由 CMake 拷贝生成，避免冲突）。
set(ZLIB_PUBLIC_HDRS
    zlib.h
    zconf.h
)

add_library(zlib_static SHARED ${ZLIB_SOURCES})
add_library(zlib::zlib_static ALIAS zlib_static)

target_include_directories(zlib_static PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:include>)

# zutil.h / crc32.h / deflate.h 等内部头
target_include_directories(zlib_static PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR})

if(MSVC)
    target_compile_definitions(zlib_static PRIVATE _CRT_SECURE_NO_DEPRECATE)
endif()

set_target_properties(zlib_static PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    OUTPUT_NAME zlib)
# MSVC 默认不自动导出 DLL 符号（Linux/MinGW 默认全导出），补此属性以生成导入库
set_target_properties(zlib_static PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)

# 随 qtsvfsTargets 导出集一起安装（与 ooz 一致）
include(GNUInstallDirs)
install(TARGETS zlib_static
    EXPORT qtsvfsTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
