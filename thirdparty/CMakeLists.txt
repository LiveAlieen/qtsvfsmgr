# vendor 压缩库：直接编译 ${QTSVFS_VENDOR_CODECS_DIR} 下的源码为静态库。
# 源列表与各 vendor 目录自己的 CMakeLists.txt 保持一致（那些产出 SHARED 库，
# 本工程要单文件 CLI，所以在此重述一次并改 STATIC）。vendor 目录保持只读。

set(_V "${QTSVFS_VENDOR_CODECS_DIR}")

function(qtsvfs_3rd target)
    add_library(${target} STATIC ${ARGN})
    add_library(qtsvfs::${target} ALIAS ${target})
    if(MSVC)
        target_compile_options(${target} PRIVATE /W3 /utf-8)
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS _CRT_SECURE_NO_DEPRECATE)
    else()
        target_compile_options(${target} PRIVATE -w -O2)
    endif()
endfunction()

# ── zlib 1.3.1 ──
qtsvfs_3rd(3rd_zlib
    ${_V}/zlib/adler32.c
    ${_V}/zlib/compress.c
    ${_V}/zlib/crc32.c
    ${_V}/zlib/deflate.c
    ${_V}/zlib/infback.c
    ${_V}/zlib/inffast.c
    ${_V}/zlib/inflate.c
    ${_V}/zlib/inftrees.c
    ${_V}/zlib/trees.c
    ${_V}/zlib/uncompr.c
    ${_V}/zlib/zutil.c
)
target_include_directories(3rd_zlib PUBLIC ${_V}/zlib)

# ── lz4 1.9.4 ──
qtsvfs_3rd(3rd_lz4
    ${_V}/lz4/lib/lz4.c
    ${_V}/lz4/lib/lz4hc.c
    ${_V}/lz4/lib/xxhash.c
)
target_include_directories(3rd_lz4 PUBLIC ${_V}/lz4/lib)

# ── zstd 1.5.5（核心 API，关多线程与手写汇编，与 vendor 配置一致）──
file(GLOB _ZSTD_SRC CONFIGURE_DEPENDS
    ${_V}/zstd/lib/common/*.c
    ${_V}/zstd/lib/compress/*.c
    ${_V}/zstd/lib/decompress/*.c
    ${_V}/zstd/lib/dictBuilder/*.c
)
list(FILTER _ZSTD_SRC EXCLUDE REGEX "\\.[Ss]$")
qtsvfs_3rd(3rd_zstd ${_ZSTD_SRC})
target_include_directories(3rd_zstd PUBLIC ${_V}/zstd/lib)
target_include_directories(3rd_zstd PRIVATE
    ${_V}/zstd/lib/common ${_V}/zstd/lib/compress
    ${_V}/zstd/lib/decompress ${_V}/zstd/lib/dictBuilder)
target_compile_definitions(3rd_zstd PRIVATE ZSTD_DISABLE_ASM ZSTD_DISABLE_DEPRECATE_WARNINGS)

# ── ooz（真实 OodleLZ 容器：Kraken/Mermaid/Selkie/Hydra/Leviathan + LZNA/Bitknit）──
qtsvfs_3rd(3rd_ooz
    ${_V}/ooz/kraken.cpp
    ${_V}/ooz/lzna.cpp
    ${_V}/ooz/bitknit.cpp
    ${_V}/ooz/compress.cpp
    ${_V}/ooz/compr_entropy.cpp
    ${_V}/ooz/compr_kraken.cpp
    ${_V}/ooz/compr_leviathan.cpp
    ${_V}/ooz/compr_mermaid.cpp
    ${_V}/ooz/compr_match_finder.cpp
    ${_V}/ooz/compr_multiarray.cpp
    ${_V}/ooz/compr_tans.cpp
    ${_V}/ooz/stdafx.cpp
    ${_V}/ooz/ooz_wrapper.cpp
)
target_include_directories(3rd_ooz PUBLIC ${_V}/ooz)

# ── qtsvfs_lz3（自研 LZ3=method 7 / LZ3HUF=method 14，源自 libQtsVFS.so 反编译）──
qtsvfs_3rd(3rd_lz3
    ${_V}/lz3/src/LZ3.cpp
    ${_V}/lz3/src/LZ3HUF.cpp
    ${_V}/lz3/src/LZ3HUF_tans.cpp
)
target_include_directories(3rd_lz3 PUBLIC ${_V}/lz3/include)
