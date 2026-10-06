# QtsVFS 内置 lz4 1.9.4（第三方依赖内嵌）
#
# 构建动态库（lz4 + lz4hc，目标名 lz4_static，产物 liblz4.so），供 QtsVFS 压缩抽象层
# LZ4Compressor / LZ4HCCompressor 使用（QTSVFS_USE_LZ4），由 libqtsvfs_compression.so 动态链接。
# 源码来自 lz4/lz4 v1.9.4（lib/ 目录，公共领域/BSD-2-Clause）。
#
# 内嵌目的：避免依赖系统安装的 liblz4 开发包。

cmake_minimum_required(VERSION 3.14)
project(qtsvfs_lz4 LANGUAGES C)

set(LZ4_SOURCES
    lib/lz4.c
    lib/lz4hc.c
    lib/xxhash.c
)

add_library(lz4_static SHARED ${LZ4_SOURCES})
add_library(lz4::lz4_static ALIAS lz4_static)

target_include_directories(lz4_static PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/lib>
    $<INSTALL_INTERFACE:include>)

set_target_properties(lz4_static PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    OUTPUT_NAME lz4)
# MSVC 默认不自动导出 DLL 符号（Linux/MinGW 默认全导出），补此属性以生成导入库
set_target_properties(lz4_static PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)

# 随 qtsvfsTargets 导出集一起安装（与 ooz 一致）
include(GNUInstallDirs)
install(TARGETS lz4_static
    EXPORT qtsvfsTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
