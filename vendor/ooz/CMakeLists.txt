# ooz — 真实 OodleLZ 压缩/解压实现（第三方库）
#
# 来源：https://github.com/rarten/ooz （powzix/ooz 的增强 fork）
#   - 解压：Kraken / Mermaid / Selkie / Leviathan / LZNA / Bitknit（官方位精确）
#   - 压缩：Kraken / Mermaid / Selkie / Leviathan（真实 Oodle 编码器）
# 许可：kraken.cpp/bitknit.cpp/lzna.cpp 为 GPLv3；compr_*.cpp 仅供教育用途（非 GPL）。
#
# 本模块是 qtsvfs「第二条主线」的第三方替代实现，与反编译重实现的 oodle/ 模块并行。

cmake_minimum_required(VERSION 3.14)
project(ooz LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/W3 /utf-8)
    add_definitions(-D_CRT_SECURE_NO_WARNINGS)
else()
    # 重要：ooz 解码器含未对齐访问（x86 下正常工作），但 GCC -O3 的
    # aggressive-loop-optimizations/严格别名优化会触发 LZNA 解码崩溃，
    # 因此固定使用 -O2（与上游 CLI 验证通过的优化级别一致）。
    add_compile_options(-O2)
endif()

# 第三方库告警降噪（与 MSVC 分支的 #pragma warning(disable:4244/4530/4018) 对齐）。
# 必须放在 add_library 之前：add_compile_options 只作用于其后创建的目标
# （原实现位于 sources 列表之前但在 add_library 之后，实际未生效）。
#   -Wno-unused-function / -Wno-sign-compare：上游既有
#   -Wno-unused-parameter / -Wno-unused-variable / -Wno-unused-but-set-variable：
#     上游压缩器大量保留未用形参/局部（反编译式移植的固有形态）
#   -Wno-missing-field-initializers：聚合体部分初始化是上游既有写法
#   -Wno-implicit-fallthrough：compr_tans.cpp 的刻意贯穿（原仅 MSVC 有 pragma 抑制）
#   -Wno-unknown-pragmas：stdafx.h/compr_entropy.cpp 残留的 MSVC #pragma warning
#   -Wno-parentheses / -Wno-shift-negative-value：kraken.cpp / bitknit.cpp 的
#     上游位运算写法（算术优先级歧义、对负值左移），MinGW/GCC 特有告警
# 本库为第三方实现，仅做告警降噪，不改动其源码语义。
if(NOT MSVC)
    add_compile_options(-Wall
        -Wno-unused-function -Wno-sign-compare
        -Wno-unused-parameter -Wno-unused-variable -Wno-unused-but-set-variable
        -Wno-missing-field-initializers -Wno-implicit-fallthrough
        -Wno-unknown-pragmas -Wno-parentheses -Wno-shift-negative-value)
endif()

set(OOZ_SOURCES
    kraken.cpp
    lzna.cpp
    bitknit.cpp
    compress.cpp
    compr_entropy.cpp
    compr_kraken.cpp
    compr_leviathan.cpp
    compr_mermaid.cpp
    compr_match_finder.cpp
    compr_multiarray.cpp
    compr_tans.cpp
    stdafx.cpp
    ooz_wrapper.cpp
)

# ooz 独立为动态库 libooz.so（OodleLZ 后端动态化），由 libqtsvfs_compression.so 动态链接。
add_library(ooz SHARED ${OOZ_SOURCES})
add_library(ooz::ooz ALIAS ooz)
set_target_properties(ooz PROPERTIES POSITION_INDEPENDENT_CODE ON)
# MSVC 默认不自动导出 DLL 符号（Linux/MinGW 默认全导出），补此属性以生成导入库
set_target_properties(ooz PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
target_include_directories(ooz PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:include>)

# ── 第三方库：从 ASan/UBSan 插桩中排除 ──
# ooz 为第三方 OodleLZ 实现：解码器含未对齐访问（x86 下按设计正常工作，见上方注释），
# 且反编译式压缩器存在 new/delete 类型不匹配等既有问题。这些均为第三方库固有行为，
# 不属于本工程自研代码，纳入 ASan/UBSan 门禁只会产生无法修复的噪音告警。
# 故在此剥离 -fsanitize* 编译旗标（含其配套的 -fno-sanitize-recover 等），
# 仅对工程自研代码做内存安全门禁。
# （自研压缩后端 QtsVFSCompression 与 LZ3 仍随顶层工程完整参与 ASan/UBSan 门禁。）
# 注意：add_subdirectory 会为本目录建立独立变量作用域，此处覆盖 CMAKE_CXX_FLAGS
# 只影响 ooz 及其子目录，不会污染工程其它模块。
string(REGEX REPLACE "-fsanitize[^ ]*" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
string(REGEX REPLACE "-fno-sanitize-recover[^ ]*" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")


# ---- 安装规则（ooz 已随 vfs 归纳，随 qtsvfsTargets 导出集一起安装） ----
include(GNUInstallDirs)
install(TARGETS ooz
    EXPORT qtsvfsTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
