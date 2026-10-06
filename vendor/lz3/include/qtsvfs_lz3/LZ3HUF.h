#pragma once

/**
 * qtsvfs_lz3::LZ3HUF — method==14 独立 Huffman 熵编码压缩族
 *
 * 依据反编译 reference/libQtsVFS.so（ARM64 Ghidra 输出）中 method==14
 * （LZ3HC）存储管线重实现。反编译证据表明 method==14 是一套**独立**于
 * method==7（符号表 LZ3，FUN_003a6634）的 Huffman 熵编码压缩器：
 *
 *   - 压缩核心：FUN_003a6c88（生成 4 路数据流 + 共享位流）
 *   - 数组容器：FUN_003ab048（压缩）/ FUN_003ad3c0（解压）
 *   - tANS 熵编码器：FUN_003b69ac（编码）/ FUN_003b4918（解码）
 *   - 单块解压：FUN_003acb58（头部 + 4 路数组 + 位流重建 LZ77）
 *   - 流式续接：FUN_003afb60 / FUN_003a2094
 *
 * 本实现与反编译在**容器/块级架构上逐字节对齐**：
 *
 *   块格式（对齐 FUN_003acb58 读法）：
 *     [数组1: 字面量数据]   容器编码（逐字节）
 *     [数组2: 字面量长度码] 容器编码（每 op 1 码）
 *     [数组3: 距离码]       容器编码（每 op 1 码）
 *     [数组4: 匹配长度码]   容器编码（每 op 1 码）
 *     [位流长:2B LE][扩展位流数据]
 *
 *   数组容器格式（对齐 FUN_003ad3c0 读法）：
 *     段序列，直至收尾段：
 *       [flag:1B]  bit0=收尾  bit1=原始段  bit2=位打包段
 *       原始段:   [len-1:2B][原始数据 len 项]
 *       位打包段: [len-1:2B][bitwidth:1B][位打包数据]
 *
 *   主循环（对齐 FUN_003acb58 语义）：
 *     每 op：读字面量长度码(+扩展) → 复制字面量 →
 *            读距离码(+扩展) → 读匹配长度码(+扩展) → 复制匹配（重叠）；
 *     输出位置到达期望大小即结束（块尾为纯字面量 op）。
 *
 * 设计说明：
 *   - method==7（符号表 LZ3）字节格式完全不动，本族为独立新格式；
 *   - LZ3HUF 压缩输出可被本族解压端解码；解压端按容器/主循环语义
 *     对齐反编译 FUN_003acb58；
 *   - 距离码采用 DEFLATE 风格 30 码表（base + 扩展位，最大 0x8000）；
 *     字面量长度 0..254 直接、>=255 用 0xff 级联；匹配长度 3..17 直接、
 *     >=18 用 0xff 级联；
 *   - 反编译 `.rodata` 中 tANS/距离/长度码表（DAT_0017e19c 等）在
 *     Ghidra 输出中仅有符号声明、无数据内容，故本实现按架构语义自洽
 *     重建码表；容器/块级格式与反编译一致，位级 tANS 码表对齐需补充
 *     原始 .so 的 .rodata（见 docs/COMPARISON_REPORT.md 备注）。
 *
 * 命名空间：qtsvfs_lz3，零外部依赖。
 */

#include <cstddef>
#include <cstdint>
#include <vector>

namespace qtsvfs_lz3 {

// ===== 常量（对齐反编译 LZ3HUF / 与 LZ3 一致） =====
constexpr size_t kLZ3HUFMaxBlockSize = 0xff81;  // 65409，单块输入上限
constexpr uint32_t kLZ3HUFMaxDist = 0x7fff;     // 32767，滑动窗口（对齐反编译压缩端钳位）
constexpr uint32_t kLZ3HUFMinMatch = 3;         // 最小匹配长度
constexpr uint32_t kLZ3HUFMaxMatch = 0x10000;   // 匹配长度上限（64KB）
constexpr uint32_t kLZ3HUFMaxLitLen = 0x10000;  // 字面量运行长度上限

// 多块帧头魔数 "QH1\x01"（仅便捷接口使用；原始单块格式无帧头）
constexpr uint8_t kLZ3HUFFrameMagic[4] = {0x51, 0x48, 0x31, 0x01};

/**
 * 压缩单个原始 LZ3HUF 块（srcSize <= kLZ3HUFMaxBlockSize）。
 * @param level 压缩级别：越大候选搜索越宽、压缩比越高（默认 0）。
 *   - level 0：贪心解析（快速，候选搜索 64）
 *   - level >=1：DP 最优解析（以实际编码位代价为模型做全局最优，
 *     结构化数据上压缩比优于贪心 ~5–8%），候选搜索 256/512
 * @return 压缩后字节数；失败（输入超限 / 缓冲区不足）返回 0。
 */
size_t lz3hufCompressBlock(const uint8_t* src, size_t srcSize,
                           uint8_t* dst, size_t dstCapacity, int level = 0);

/**
 * 解压单个原始 LZ3HUF 块。
 * @param expectedSize 期望的解压后字节数（块格式不含输出长度，由调用方提供）。
 * @param[out] consumed 实际消耗的压缩字节数，可为 nullptr。
 * @return 成功返回 expectedSize；失败返回 0。
 */
size_t lz3hufDecompressBlock(const uint8_t* src, size_t srcSize,
                             uint8_t* dst, size_t expectedSize,
                             size_t* consumed = nullptr);

/** 单块最坏情况压缩后大小（安全上界） */
size_t lz3hufMaxCompressedSize(size_t srcSize);

/**
 * 便捷压缩：任意长度。
 *  - srcSize <= kLZ3HUFMaxBlockSize：直接输出原始单块格式。
 *  - 更大输入：输出帧格式
 *      [magic:4B "QH1\x01"][numBlocks:4B LE][size_i:4B LE xN][块数据...]
 */
std::vector<uint8_t> lz3hufCompress(const uint8_t* src, size_t srcSize, int level = 0);

/**
 * 便捷解压：自动识别帧格式 / 原始单块格式。
 * @param rawExpectedSize 原始单块格式（无帧头）时的期望输出长度；帧格式忽略。
 */
std::vector<uint8_t> lz3hufDecompress(const uint8_t* src, size_t srcSize,
                                      size_t rawExpectedSize = 0);

// =====================================================================
// 真实反编译格式（FUN_003acb58）解压支持
//
// 反编译真实 method==14 单块格式与当前自洽格式**字节不互通**（见
// docs/COMPARISON_REPORT.md 生死线分析）。以下接口按反编译语义实现
// 真实格式的解析/解压：
//
//   块格式（FUN_003acb58 读法）：
//     [头部:1B]  bit1=有 bVar14 直接表  bit2=有符号表
//     [bVar14:1B]（bit1）
//     [符号表:2B]（bit2）
//     数组1..4：各自容器（原始段/位打包段/收尾段/tANS段）
//     [位流长:2B LE][扩展位流数据]
//
// @return 成功返回 expectedSize；失败返回 0。FSE/tANS 段（bit4）已由容器
//         层消费（Issue #86/#157），不再返回 unsupported 哨兵；
//         *unsupportedSegment 仅在段载荷损坏（rc=-1）时置位，保留向后兼容。
// =====================================================================
size_t lz3hufRealDecompressBlock(const uint8_t* src, size_t srcSize,
                                 uint8_t* dst, size_t expectedSize,
                                 int* unsupportedSegment = nullptr,
                                 size_t* consumed = nullptr);

/**
 * 压缩单个 LZ3HUF 块为**真实反编译格式**（FUN_003a6c88 / FUN_003acb58 对齐）。
 *
 *   块格式：
 *     [头部flag:1B]  bit1=有 bVar14 直接表  bit2=有符号表（本实现取 0/0）
 *     [bVar14:1B]（bit1）  [符号表:2B]（bit2）
 *     数组1..4：各自真实容器（原始段/RLE 段/反向位打包段/tANS 段）
 *     [位流长:2B LE][扩展位流数据]
 *
 * 码表采用 real::（kDistBase/kMatchLenBase/kLitLenBase 等），与
 * lz3hufRealDecompressBlock 逐项对齐。
 *
 * 【Issue #157 / 支线5】FSE/tANS 熵编码段（bit4）已收尾接入：当某路数组
 * 为字节值且熵编码严格更短时，RealArrayEncoder 产出 bit4 段（载荷经
 * tansCompress，与 FUN_003b4918 位级互通）；否则回退原始/位打包/RLE 段，
 * 保证永不因接入而变大。真实块级 compress→decompress round-trip 已由
 * tests/test_lz3huf_real_tans.cpp 用独立参考块解码器逐字节回测验收。
 *
 * @return 压缩后字节数；失败（输入超限 / 缓冲区不足 / 值超码表范围）返回 0。
 */
size_t lz3hufRealCompressBlock(const uint8_t* src, size_t srcSize,
                               uint8_t* dst, size_t dstCapacity, int level = 0);

} // namespace qtsvfs_lz3
