set(_V "${CMAKE_SOURCE_DIR}/vendor")

file(GLOB _ZSTD_SRC CONFIGURE_DEPENDS
    ${_V}/zstd/lib/common/*.c
    ${_V}/zstd/lib/compress/*.c
    ${_V}/zstd/lib/decompress/*.c
    ${_V}/zstd/lib/dictBuilder/*.c
)
list(FILTER _ZSTD_SRC EXCLUDE REGEX "\\.[Ss]$")

add_library(qtsvfs_core STATIC
    # ── 核心库 ──
    qtsvfs/Alias.cpp
    qtsvfs/Catalog.cpp
    qtsvfs/Export.cpp
    qtsvfs/Harvest.cpp
    qtsvfs/Live.cpp
    qtsvfs/Names.cpp
    qtsvfs/NodeTree.cpp
    qtsvfs/Normalize.cpp
    qtsvfs/Package.cpp
    qtsvfs/Roots.cpp
    qtsvfs/Tree.cpp
    qtsvfs/codec/Codec.cpp
    qtsvfs/format/GlobalIndex.cpp
    qtsvfs/format/Kdb.cpp
    qtsvfs/format/PathHash.cpp
    qtsvfs/format/QtsfNode.cpp
    qtsvfs/format/QtsfStream.cpp
    qtsvfs/io/FileReader.cpp
    qtsvfs/probe/Probe.cpp
    # ── zlib 1.3.1 ──
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
    # ── lz4 1.9.4 ──
    ${_V}/lz4/lib/lz4.c
    ${_V}/lz4/lib/lz4hc.c
    ${_V}/lz4/lib/xxhash.c
    # ── zstd 1.5.5 ──
    ${_ZSTD_SRC}
    # ── ooz（Kraken/Mermaid/Selkie/Hydra/Leviathan + LZNA/Bitknit）──
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
    # ── lz3（自研 method 7 / method 14）──
    ${_V}/lz3/src/LZ3.cpp
    ${_V}/lz3/src/LZ3HUF.cpp
    ${_V}/lz3/src/LZ3HUF_tans.cpp
)

target_include_directories(qtsvfs_core PUBLIC
    "${CMAKE_SOURCE_DIR}/include"
    ${_V}/zlib
    ${_V}/lz4/lib
    ${_V}/zstd/lib
    ${_V}/ooz
    ${_V}/lz3/include
)
target_include_directories(qtsvfs_core PRIVATE
    ${_V}/zstd/lib/common
    ${_V}/zstd/lib/compress
    ${_V}/zstd/lib/decompress
    ${_V}/zstd/lib/dictBuilder
)
target_compile_definitions(qtsvfs_core PRIVATE
    _CRT_SECURE_NO_WARNINGS
    _CRT_SECURE_NO_DEPRECATE
    ZSTD_DISABLE_ASM
    ZSTD_DISABLE_DEPRECATE_WARNINGS
)

if(MSVC)
    target_compile_options(qtsvfs_core PRIVATE /W4 /utf-8)
else()
    target_compile_options(qtsvfs_core PRIVATE -w -O2)
endif()
