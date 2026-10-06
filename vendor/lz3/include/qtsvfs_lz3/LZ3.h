#pragma once

/**
 * qtsvfs_lz3 — 自研 LZ3 压缩算法（纯 C++17 重实现）
 *
 * 依据 reference/libQtsVFS.so（ARM64 Ghidra 反编译输出）逐字节还原：
 *   - 压缩端：FUN_003a15ec / FUN_003a1904（小块路径）
 *             FUN_003a218c（LZ3 压缩核心）/ FUN_003a6c88（LZ3HC 变体）
 *   - 解压端：FUN_003a6634（LZ3 块解压核心）
 *   - 流式解压：FUN_003af668 / FUN_003afb60
 *
 * 数据布局（原始单块格式，与反编译解压端逐字节一致）：
 *
 *   [symCount:1B]                偏移符号表条目数 N（0..128）
 *   [symbol:1B 或 2B] xN         符号表：值 <0x80 占 1 字节；
 *                                 >=0x80 占 2 字节：0x80|(v&0x7f)，(v>>7)^1
 *   [token:2B LE]                bit0-3=字面量长度(0xf 触发扩展)
 *                                 bit4-7=匹配长度-3(0xe=17 固定,0xf 触发扩展)
 *                                 bit8-14=bit15=0 时内联偏移高 7 位 /
 *                                          bit15=1 时符号表索引
 *                                 bit15=0 内联偏移；=1 查符号表
 *   [litExt:1B xN]               字面量长度扩展（连续 0xff 累加）
 *   [literal bytes]              字面量字节
 *   [offsetExtra:1B]             bit15=0 时：偏移低 8 位 = extra ^ (token&0xff)
 *   [matchExt:1B xN]             匹配长度扩展（连续 0xff 累加）
 *
 * 块间结构：每个记录 = [token][字面量扩展][字面量][偏移修正][匹配扩展]，
 * 流末尾为纯字面量 token（无匹配、无偏移）。
 *
 * 设计说明：
 *   - 解压端与反编译 FUN_003a6634 语义完全一致（含重叠匹配、0xff 级联扩展）。
 *   - 压缩端为“DP 最优解析 + 频率符号表 + HC 变体”完整压缩器：
 *       · 频率符号表：按偏移出现频率降序取前 128 个进符号表（对齐
 *         FUN_003a42c4 / FUN_003a4704 频率/前缀思想），高频偏移 2 字节
 *         编码，低频/未收录偏移内联 3 字节；
 *       · DP 最优解析：以输出字节数为代价（token + 偏移 + 0xff 级联扩展
 *         精确建模），符号表感知偏移代价，从后向前回推最优 op 序列
 *         （对齐 FUN_003a218c 内部代价表搜索）；
 *       · HC 变体（level>=1，对齐 FUN_003a6c88）：候选搜索更宽，压缩比
 *         更高，解压端不变（字节格式兼容）。
 *   - 输出可被原始语义解压器解码（单块格式逐字节兼容）；原始压缩器输出
 *     也可被本实现解压端解码。
 *   - 单块输入上限 0xff81 字节（对齐反编译 FUN_003a15ec 小块阈值）。
 *   - 便捷 compress()/decompress() 支持任意长度（自动分块 + 帧头）。
 *
 * 命名空间：qtsvfs_lz3，零外部依赖；物理位置 gui/vfs/src/compression/src/lz3，随 QtsVFS 库构建。
 */

#include <cstdint>
#include <cstddef>
#include <vector>

namespace qtsvfs_lz3 {

// ===== 常量（对齐反编译） =====
constexpr size_t kLZ3MaxBlockSize = 0xff81;  // 65409，单块输入上限
constexpr uint32_t kLZ3MaxDist = 0x7fff;     // 32767，滑动窗口（偏移 1..0x7fff）
// 对齐反编译：FUN_003a15ec/FUN_003a1f30 对匹配距离统一钳位到 0x7fff（
// if (0x7fff < dist) dist = 0x7fff），因此压缩端从不产生 dist==0x8000。
// 若允许 dist==0x8000：符号表 2B 编码 (0x8000>>7)^1 回卷成 0、内联路径
// (0x8000>>8)&0x7f==0，均会在解压端还原成 dist=0 → 无法解码。
constexpr uint32_t kLZ3MinMatch = 3;         // 最小匹配长度
constexpr uint32_t kLZ3MaxMatch = 0x10000;   // 匹配长度上限（64KB）
constexpr uint32_t kLZ3MaxSym = 128;         // 符号表条目上限（token 7 位索引）

// 多块帧头魔数 "QL3\x01"（仅便捷接口使用；原始单块格式无帧头）
constexpr uint8_t kLZ3FrameMagic[4] = {0x51, 0x4C, 0x33, 0x01};

/**
 * 压缩单个原始 LZ3 块（srcSize <= kLZ3MaxBlockSize）。
 * @param level 压缩级别：0 = LZ3（FUN_003a218c，DP 标准搜索）；
 *              >=1 = LZ3HC（FUN_003a6c88，更宽候选搜索，更高压缩比）。
 * @return 压缩后字节数；失败（输入超限 / 缓冲区不足）返回 0。
 */
size_t compressBlock(const uint8_t* src, size_t srcSize,
                     uint8_t* dst, size_t dstCapacity, int level = 0);

/**
 * 解压单个原始 LZ3 块。
 * @param expectedSize 期望的解压后字节数（原始块格式不含输出长度，
 *                     需由调用方提供，对应反编译 FUN_003a6634 的 param_3）。
 * @param[out] consumed 实际消耗的压缩字节数，可为 nullptr。
 * @return 成功返回 expectedSize；失败返回 0。
 */
size_t decompressBlock(const uint8_t* src, size_t srcSize,
                       uint8_t* dst, size_t expectedSize,
                       size_t* consumed = nullptr);

/** 单块最坏情况压缩后大小（保证 compressBlock 不溢出的安全上界） */
size_t maxCompressedSizeBlock(size_t srcSize);

/**
 * 便捷压缩：任意长度。
 *  - srcSize <= kLZ3MaxBlockSize：直接输出原始单块格式（与反编译一致）。
 *  - 更大输入：输出帧格式
 *      [magic:4B "QL3\x01"][numBlocks:4B LE][size_i:4B LE xN][块数据...]
 * @param level 压缩级别，透传 compressBlock（0 = LZ3，>=1 = LZ3HC）。
 * @return 压缩后的字节序列；失败返回空 vector。
 */
std::vector<uint8_t> compress(const uint8_t* src, size_t srcSize, int level = 0);

/**
 * 便捷解压：自动识别帧格式 / 原始单块格式。
 * @param rawExpectedSize 原始单块格式（无帧头）时的期望输出长度；帧格式忽略。
 * @return 解压后的字节序列；失败返回空 vector。
 */
std::vector<uint8_t> decompress(const uint8_t* src, size_t srcSize,
                                size_t rawExpectedSize = 0);

/**
 * 压缩等级（对齐反编译 LZ3 / LZ3HC 两个变体）：
 *   LZ3    = FUN_003a218c（DP 最优解析 + 频率符号表）
 *   LZ3HC  = FUN_003a6c88（DP 更宽搜索 + HC 变体，压缩比更高）
 */
enum class Level : int {
    LZ3 = 0,
    LZ3HC = 1,
};

} // namespace qtsvfs_lz3
