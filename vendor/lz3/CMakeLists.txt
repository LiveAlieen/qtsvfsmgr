# qtsvfs_lz3 — 自研 LZ3 / LZ3HUF 压缩算法动态库
#
# 依据 reference/libQtsVFS.so 反编译重实现的 LZ3（method==7）与 LZ3HUF（method==14）
# 算法族，独立编译为动态库 libqtsvfs_lz3.so，由 libqtsvfs_compression.so 动态链接。
#
# 源码来自 reference/libQtsVFS.so（ARM64 Ghidra 反编译输出）：
#   - LZ3.cpp          —— 符号表 LZ3 算法（FUN_003a218c / FUN_003a6634）
#   - LZ3HUF.cpp       —— method==14 独立 Huffman 熵编码压缩族（FUN_003a6c88 / FUN_003acb58）
#   - LZ3HUF_tans.cpp  —— tANS 熵编码器（FUN_003b69ac / FUN_003b4918）
#
# 命名空间：qtsvfs_lz3，零外部依赖，纯 C++17。

cmake_minimum_required(VERSION 3.14)
project(qtsvfs_lz3 LANGUAGES CXX)

set(QTSVFS_LZ3_SOURCES
    src/LZ3.cpp
    src/LZ3HUF.cpp
    src/LZ3HUF_tans.cpp
)

add_library(qtsvfs_lz3 SHARED ${QTSVFS_LZ3_SOURCES})
add_library(qtsvfs::qtsvfs_lz3 ALIAS qtsvfs_lz3)
# MSVC 默认不自动导出 DLL 符号（Linux/MinGW 默认全导出），补此属性以生成导入库
set_target_properties(qtsvfs_lz3 PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)

target_include_directories(qtsvfs_lz3 PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include/qtsvfs/lz3>)

if(MSVC)
    target_compile_options(qtsvfs_lz3 PRIVATE /W3 /utf-8)
    add_definitions(-D_CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(qtsvfs_lz3 PRIVATE -Wall -Wextra -Wpedantic)
endif()

# 随 qtsvfsTargets 导出集一起安装（与 ooz 一致）
include(GNUInstallDirs)
install(TARGETS qtsvfs_lz3
    EXPORT qtsvfsTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/qtsvfs/lz3)
