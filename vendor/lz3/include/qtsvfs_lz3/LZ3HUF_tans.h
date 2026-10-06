#pragma once

/**
 * qtsvfs_lz3::LZ3HUF_tans — tANS/FSE 熵编码器组件（路线4）
 *
 * 依据反编译 reference/libQtsVFS.so 中 method==14（LZ3HUF）所用熵编码
 * 子系统重实现。**Issue #72 互通整改已完成**：
 *
 * ┌──────────────────────────────────────────────────────────────────┐
 * │ 能力边界（接入前必读）                                            │
 * │                                                                  │
 * │   自洽 roundtrip（本组件编 → 本组件解） ......... ✅ 可用        │
 * │   与原库 FUN_003b4918 位级互通 .................. ✅ 已达成      │
 * │   作为 bit4 段嵌入容器层供原库解析 .............. ✅ 已接入      │
 * │     （Issue #86：LZ3HUF.cpp 容器层已消费/产出 bit4 段）         │
 * │                                                                  │
 * │ 位流已改为标准 zstd 反向流（BIT_CStream/BIT_DStream），终止标记   │
 * │ 位为末字节最高置位位（clz32-23）；权重表支持 raw（hdr>=128）与    │
 * │ FSE 压缩（hdr<128）双模式，覆盖全 256 字母表；bit4 段头为单个     │
 * │ u16（载荷字节数-1），逐字节对齐 FUN_003ad3c0。                    │
 * │                                                                  │
 * │ 交叉验证：tests/test_lz3huf_tans_golden.cpp 用按反编译语义**独立  │
 * │ 实现**的参考解码器回测本组件产出，互通率 100%。                   │
 * └──────────────────────────────────────────────────────────────────┘
 *
 * 以下反编译对应关系为实现依据（非「已互通」之保证）：
 * （下表行号为 reference/libQtsVFS.so.c 实测定义行，行号随反编译文件再生成
 *  可能漂移；旧值系上一轮再生成前的快照，整体漂移约 -20200 行，已订正。）
 *
 *   | 环节        | 反编译函数    | 行号     | 语义                          |
 *   |-------------|---------------|----------|-------------------------------|
 *   | 熵编码      | FUN_003b69ac  | 399611   | 频次统计→建表→写表→写位流     |
 *   | 建表        | FUN_003b5be8  | 398596   | 频次→规范 Huffman/tANS 权重表 |
 *   | 写表        | FUN_003b55e8  | 398175   | 码表序列化（FSE 或 raw 权重） |
 *   | 写位流      | FUN_003b66f0  | 399429   | 单流反向位写入                |
 *   |             | FUN_003b6890  | 399570   | 四流交织反向位写入            |
 *   | 熵解码      | FUN_003b4918  | 397252   | 读表头→建 DTable→逐符号解码   |
 *   | 读表头      | FUN_003b42b8  | 396739   | 权重表反序列化（FSE / raw）   |
 *   | 容器层消费  | FUN_003ad3c0  | 390566   | bit4=tANS 段（调 FUN_003b4918）|
 *
 * === 反编译关键证据（决定字节格式的核心事实） ===
 *
 * 1) `FUN_003b47a8` 的错误字符串表（"tableLog requires too much memory"、
 *    "Unsupported max Symbol Value : too large" 等）表明该熵编码子系统
 *    是 **zstd 系 HUF（Huffman + 权重表）** 编解码器，而非纯 tANS 状态机；
 *    位流层为 zstd 风格「反向位流（从尾部向前读，末字节含终止标记位）」。
 *
 * 2) `FUN_003b45c8`（表头解析，被 FUN_003b42b8 调用）首字节 `hdr` 分流：
 *      - `hdr >= 128`：**raw 权重模式**，权重个数 = hdr - 127，
 *        每字节打包 2 个 4bit 权重（高 nibble 在前），
 *        消耗字节数 = (nbSymbols + 1) / 2 + 1；
 *      - `hdr < 128`：**FSE 压缩权重模式**，hdr 为压缩权重区字节数。
 *      解析后按 `sum(2^(w-1))` 累计得 `total`，
 *      `tableLog = 32 - clz(total)`，末符号权重
 *      `lastW = log2((1<<tableLog) - total) + 1`。
 *
 * 3) `FUN_003b4918` 校验：`tableLog <= 12`（`0xc < local_64` → 错误
 *    0xffffffffffffffd4 = -44「tableLog too large」）、
 *    `maxSymbolValue <= 255`；调用方 `FUN_003ad3c0` 传入 `param_5 = 0xc`
 *    （最大允许 tableLog = 12）、工作区 0x5410 字节。
 *
 * 4) 位流读取：`uVar11 = uVar15 >> (-uVar31 & 0x3f) & kMask[nbBits]`，
 *    其中 `kMask` 即 `DAT_0017e7b0`（= real::kMask，本仓库已提取），
 *    读取自 8 字节小端窗口、从缓冲尾部向低地址滑动 → 标准 zstd
 *    `BIT_DStream` 反向位流。起始位置由末字节最高置位位确定：
 *    `bitsConsumed = clz32(lastByte) - 23`（跳过终止标记位）。
 *
 * === 本组件对外格式（tansCompress 产出 / tansDecompress 消费） ===
 *
 *   [权重表头（FUN_003b45c8 语义）][反向位流（FUN_003b4918 语义）]
 *
 * 该字节布局按 `FUN_003ad3c0` bit4 段内 `FUN_003b4918` 的语义建模，
 * 并已通过独立参考解码器交叉验证（Issue #72 已整改：位流方向、
 * FSE 压缩权重模式、bit4 段头均已对齐）。
 *
 * 【Issue #86】路线2 容器层（LZ3HUF.cpp）已完成接入：
 *   - 解码：decodeRealArray 的 bit4 分支调 tansDecompressBounded；
 *   - 编码：RealArrayEncoder 优先尝试 tANS 段，不划算则回退；
 *   - 验证：tests/test_lz3huf_bit4_container.cpp 用独立参考**容器**
 *     解码器（按 FUN_003ad3c0 重写）回测容器层完整产出。
 *
 * 注意：bit4 段**不携带符号个数**，容器层消费时应用
 * `tansDecompressBounded`（容量驱动）而非 `tansDecompress`（精确个数）。
 *
 * 命名空间 qtsvfs_lz3，零外部依赖，C++11 兼容。
 */

#include <cstddef>
#include <cstdint>
#include <vector>

namespace qtsvfs_lz3 {

// ===== 常量（对齐反编译校验分支） =====
/// tableLog 上限：FUN_003b4918 `if (0xc < local_64) return -44;`
constexpr uint32_t kTansMaxTableLog = 12;
/// 符号值上限：FUN_003b4918 `if (0xff < local_68) return -46;`
constexpr uint32_t kTansMaxSymbolValue = 255;
/// 权重上限：FUN_003b45c8 `if (0xb < weight) → 错误`（权重 <= 11）
constexpr uint32_t kTansMaxWeight = 11;
/// 输入上限：FUN_003b69ac `if (0x20000 < param_4) return -72;`
constexpr size_t kTansMaxInputSize = 0x20000;

// =====================================================================
// 表构建：FUN_003b5be8 语义
//
// 由符号频次构建规范 Huffman 码长表（nbBits[]），满足
//   sum(2^(maxNbBits - nbBits[s])) == 2^maxNbBits（Kraft 等式）
// 且所有 nbBits[s] <= maxNbBits。权重 w[s] = maxNbBits + 1 - nbBits[s]，
// 未出现符号 nbBits = 0 / 权重 0。
//
// @param freq            长度 maxSymbolValue+1 的频次数组
// @param maxSymbolValue  最大符号值（<= 255）
// @param maxNbBits       目标 tableLog（1..12；传 0 取默认 11）
// @param[out] nbBits     长度 maxSymbolValue+1 的码长输出
// @return 实际 tableLog；失败返回 0
// =====================================================================
uint32_t tansBuildTable(const uint32_t* freq, uint32_t maxSymbolValue,
                        uint32_t maxNbBits, uint8_t* nbBits);

// =====================================================================
// 熵编码：FUN_003b69ac（+ FUN_003b55e8 写表 / FUN_003b66f0 写流）语义
//
// 输出 = [权重表头][反向位流]，追加写入 out 尾部。
// @return 写入 out 的字节数；不可压缩 / 失败返回 0（调用方应回退到
//         原始段或 RLE 段，与 FUN_003ab048 段选择语义一致）。
// =====================================================================
size_t tansCompress(const uint8_t* src, size_t n, std::vector<uint8_t>& out);

// =====================================================================
// 熵解码：FUN_003b4918（+ FUN_003b42b8 / FUN_003b45c8 读表头）语义
//
// @param src       [权重表头][反向位流]
// @param srcSize   可读字节数（可大于实际载荷，多余部分被忽略）
// @param expected  期望输出符号数（容器层由段头 n-1 给出）
// @param[out] out  解码符号追加写入 out 尾部
// @return 实际解码字节数（== expected）；失败返回 0
// =====================================================================
size_t tansDecompress(const uint8_t* src, size_t srcSize, size_t expected,
                      std::vector<uint8_t>& out);

// =====================================================================
// 熵解码（容量驱动版）：FUN_003b4918 的**真实调用契约**
//
// 【Issue #86】容器层 bit4 段不携带符号个数（段头只有一个 u16 =
// 载荷字节数-1），因此原库解码器只能拿到「剩余输出容量」：
//
//   uVar13 = FUN_003b4918(*param_2, param_3, ptr, uVar17, 0xc, ...);
//   param_3 = param_3 - uVar13;   // 按**实际解出字节数**递减容量
//
// 故解码终止条件是「位流耗尽」而非「计数达标」。
// tansDecompress（精确个数版）保留给已知长度的场景。
//
// @param capacity  最多允许解出的符号数（外层剩余输出容量）
// @param[out] out  解码符号追加写入 out 尾部
// @return 实际解码字节数；失败 / 超出 capacity 返回 0
// =====================================================================
size_t tansDecompressBounded(const uint8_t* src, size_t srcSize, size_t capacity,
                             std::vector<uint8_t>& out);

// =====================================================================
// 辅助：容器层段编解码（供路线2 复用，语义对齐 FUN_003ad3c0）
// =====================================================================

/// RLE 段（bit3）编码：[flag][n-1:2B LE][value:1B]
void tansWriteRleSegment(std::vector<uint8_t>& out, uint32_t count,
                         uint8_t value, bool last);

/// 反向位打包段（bit2）编码：[flag][n-1:2B LE][width:1B][位数据]
/// 位数据从段尾反向、每项高位先出（与 FUN_003ad3c0 bit2 分支对齐）。
/// @return true 成功；width 非法（0 或 >24）返回 false
bool tansWriteBitpackSegment(std::vector<uint8_t>& out,
                             const uint32_t* vals, size_t n,
                             uint32_t width, bool last);

/// tANS 段（bit4）编码：[flag][payloadLen-1:2B LE][tansCompress 载荷]
///
/// 【Issue #72 整改③】段头为**单个 u16**，语义是「载荷字节数 - 1」，
/// 逐字节对齐 FUN_003ad3c0 bit4 分支：
///     uVar7  = *(ushort *)*param_1;   // 只读一个 u16
///     uVar17 = (ulong)uVar7 + 1;      // 载荷字节数
///     FUN_003b4918(dst, ..., ptr, uVar17, 0xc, ...);
///     *param_1 += uVar17;
/// 旧格式多写了 2 字节的符号个数字段，导致原库无法解析。
/// 解码所需的符号个数由外层剩余输出空间（param_3）控制。
/// @return true 成功；n==0 或熵编码不划算返回 false（调用方回退其他段）
bool tansWriteTansSegment(std::vector<uint8_t>& out,
                          const uint8_t* src, size_t n, bool last);

} // namespace qtsvfs_lz3
