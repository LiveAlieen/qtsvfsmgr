# QtsVFS 内置 zstd 1.5.5（第三方依赖内嵌）
#
# 构建动态库（核心 compress/decompress/dictBuilder，目标名 zstd_static，产物 libzstd.so），
# 供 QtsVFS 压缩抽象层 ZstdCompressor 使用（QTSVFS_USE_ZSTD），由 libqtsvfs_compression.so
# 动态链接。源码来自 facebook/zstd v1.5.5。
#
# 内嵌目的：避免依赖系统安装的 libzstd 开发包。
#
# 说明：
#  - 仅启用核心 API（ZSTD_compress/ZSTD_decompress 等），不编译 legacy/deprecated。
#  - 关闭多线程（ZSTD_MULTITHREAD），保持纯单线程静态链接（与反编译存储管线一致）。

cmake_minimum_required(VERSION 3.14)
project(qtsvfs_zstd LANGUAGES C)

set(ZSTD_SOURCES
    lib/common/entropy_common.c
    lib/common/error_private.c
    lib/common/fse_decompress.c
    lib/common/pool.c
    lib/common/threading.c
    lib/common/xxhash.c
    lib/common/zstd_common.c
    lib/compress/fse_compress.c
    lib/compress/hist.c
    lib/compress/huf_compress.c
    lib/compress/zstd_compress.c
    lib/compress/zstd_compress_literals.c
    lib/compress/zstd_compress_sequences.c
    lib/compress/zstd_compress_superblock.c
    lib/compress/zstd_double_fast.c
    lib/compress/zstd_fast.c
    lib/compress/zstd_lazy.c
    lib/compress/zstd_ldm.c
    lib/compress/zstd_opt.c
    lib/compress/zstdmt_compress.c
    lib/decompress/huf_decompress.c
    lib/decompress/zstd_ddict.c
    lib/decompress/zstd_decompress.c
    lib/decompress/zstd_decompress_block.c
    lib/dictBuilder/cover.c
    lib/dictBuilder/divsufsort.c
    lib/dictBuilder/fastcover.c
    lib/dictBuilder/zdict.c
)

add_library(zstd_static SHARED ${ZSTD_SOURCES})
add_library(zstd::zstd_static ALIAS zstd_static)

# 公共头（zstd.h / zstd_errors.h 位于 lib/ 根目录，库代码通过 common/... 内部头访问）
target_include_directories(zstd_static PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/lib>
    $<INSTALL_INTERFACE:include>)
# 内部头（common/ compress/ decompress/ dictBuilder/）
target_include_directories(zstd_static PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/common
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/compress
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/decompress
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/dictBuilder)

# 不启用多线程，避免依赖 pthread
target_compile_definitions(zstd_static PRIVATE
    ZSTD_DISABLE_ASM
    ZSTD_DISABLE_DEPRECATE_WARNINGS)

set_target_properties(zstd_static PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    OUTPUT_NAME zstd)
# MSVC 默认不自动导出 DLL 符号（Linux/MinGW 默认全导出），补此属性以生成导入库
set_target_properties(zstd_static PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)

# 随 qtsvfsTargets 导出集一起安装（与 ooz 一致）
include(GNUInstallDirs)
install(TARGETS zstd_static
    EXPORT qtsvfsTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
