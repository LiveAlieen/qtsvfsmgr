// LZ3HUF_tans.cpp — tANS/FSE 熵编码器组件（Issue #63 路线4）
//
// 依据反编译 reference/libQtsVFS.so 中 method==14 熵编码子系统实现：
//   编码 FUN_003b69ac / 建表 FUN_003b5be8 / 写表 FUN_003b55e8 /
//   写流 FUN_003b66f0、FUN_003b6890 / 解码 FUN_003b4918 /
//   读表头 FUN_003b42b8、FUN_003b45c8 / 容器层消费 FUN_003ad3c0。
//
// 反编译中 FUN_003b47a8 的错误字符串表（"tableLog requires too much
// memory"、"Unsupported max Symbol Value : too large"、"Corrupted block
// detected" 等）确认该子系统为 **zstd 系 HUF 编解码器**：规范 Huffman
// 码 + 权重表头 + 反向位流（BIT_DStream）。本实现按其位级语义重建。
//
// 命名空间 qtsvfs_lz3，零外部依赖，C++11 兼容。

#include "qtsvfs_lz3/LZ3HUF_tans.h"

#include <algorithm>
#include <cstring>

namespace qtsvfs_lz3 {

// 前向声明：匿名命名空间内的 writeWeightsFse 需调用公开接口 tansBuildTable。
uint32_t tansBuildTable(const uint32_t* freq, uint32_t maxSymbolValue,
                        uint32_t maxNbBits, uint8_t* nbBits);

namespace {

// ---------------------------------------------------------------------
// 位运算小工具
// ---------------------------------------------------------------------

/// 32 位前导零计数（对齐反编译中的 LZCOUNT）。x 必须非 0。
inline uint32_t clz32(uint32_t x) {
    uint32_t n = 0;
    while ((x & 0x80000000u) == 0) {
        x <<= 1;
        ++n;
    }
    return n;
}

/// floor(log2(x))，x 必须非 0。
inline uint32_t log2Floor(uint32_t x) { return 31u - clz32(x); }

/// 位掩码：等价 real::kMask / 反编译 DAT_0017e7b0。
inline uint32_t bitMask(uint32_t nbBits) {
    return nbBits >= 32 ? 0xffffffffu : ((1u << nbBits) - 1u);
}

// ---------------------------------------------------------------------
// 反向位流写入器（对齐 FUN_003b66f0 / zstd BIT_CStream）
//
// 【Issue #72 整改①】旧实现为「正向 MSB-first」，仅能自洽；现改为真正的
// zstd 反向流，使原库 FUN_003b4918 可直接解码本组件产出。
//
// zstd BIT_CStream 语义：
//   - bitContainer 为 64 位，新位从**低位侧**插入：
//         container |= value << bitPos;  bitPos += nbBits;
//   - flush 时把已满的整字节以**小端**顺序追加到输出尾部；
//   - 于是「先写入的位」落在输出缓冲的**低地址**，而解码端从**高地址
//     （尾部）**起向低地址滑动、自高位侧取位，故编码端必须按符号
//     **逆序**写入（最后一个符号最先写）。
//   - 收流写入终止标记位 1，使末字节非 0；该标记位是末字节的
//     **最高置位位**，解码端用 clz32(lastByte)-23 定位（整改④）。
// ---------------------------------------------------------------------
class RevBitWriter {
public:
    /// 写入 nbBits 位（取 value 的低 nbBits 位）。
    /// 注意：调用方需按**符号逆序**调用（见类头说明）。
    void addBits(uint32_t value, uint32_t nbBits) {
        if (nbBits == 0) return;
        container_ |= static_cast<uint64_t>(value & bitMask(nbBits)) << bitPos_;
        bitPos_ += nbBits;
        flushFull();
    }

    /// 收流：追加终止标记位并刷出剩余位。
    ///
    /// 终止标记位（zstd BIT_closeCStream 语义）：在已写数据位**之上**再插入一个 1。
    /// 它在末字节中位于比所有数据位更高的位置，即末字节的**最高置位位**，
    /// 故解码端 clz32(lastByte)-23 可唯一定位它（与 FUN_003b4918 一致）。
    ///
    /// @return 位流字节序列（末字节必非 0）
    std::vector<uint8_t> finish() {
        addBits(1, 1);  // 终止标记位（位于最高位）
        // 刷出剩余不足一字节的尾巴（高位自然补 0）
        if (bitPos_ > 0) {
            bytes_.push_back(static_cast<uint8_t>(container_ & 0xffu));
            container_ = 0;
            bitPos_ = 0;
        }
        return bytes_;
    }

private:
    void flushFull() {
        while (bitPos_ >= 8) {
            bytes_.push_back(static_cast<uint8_t>(container_ & 0xffu));
            container_ >>= 8;
            bitPos_ -= 8;
        }
    }

    uint64_t container_ = 0;
    uint32_t bitPos_ = 0;
    std::vector<uint8_t> bytes_;
};

// ---------------------------------------------------------------------
// 反向位流读取器（逐条对齐 FUN_003b4918 位读取语义）
//
// 反编译核心（reference/libQtsVFS.so.c FUN_003b4918：定义 @行397252，位读取器
// @行397481-397543；行号随反编译文件再生成可能漂移）：
//   lVar24 = uVar12 - 8;                                   //@397481 窗口起点在**尾部**
//   uVar16 = *(ulong *)(pbVar4 + lVar24);                  //@397516 8 字节小端窗口
//   iVar25 = (int)LZCOUNT((uint)末字节) + -0x17;           //@397517 = clz32(last) - 23
//   uVar32 = iVar25 + nbBits;                              //@397520
//   uVar12 = uVar16 >> (-uVar32 & 0x3f) & kMask[nbBits];   //@397522 自**高位侧**取位
//                                                          //       （DAT_0017e7b0 = kMask 位宽掩码表）
//   lVar24 -= uVar32 >> 3;  uVar32 &= 7;                   //@397537-397539 窗口向**低地址**滑动
//
// 即：从缓冲尾部向低地址滑动的反向位流。本实现以等价、无 UB 的方式重建：
// 维护一个从流尾向前递减的全局位游标 pos_，取位时自 pos_-1 向低地址读，
// 每字节内按「低位 = 先进」的小端语义定位。
//
// 终止标记位：写入端在数据位**之上**插入 1 位标记，因此它是末字节的
// **最高置位位**（非最低位）。有效数据位数：
//   nbBits_ = size*8 - (clz32(last) - 23)
// 其中 clz32(last)-23 已包含末字节的高位填充零与标记位本身（共 1..8 位）。
// ---------------------------------------------------------------------
class RevBitReader {
public:
    /// @return false 表示位流非法（空 / 末字节为 0）
    bool init(const uint8_t* src, size_t size) {
        if (src == nullptr || size == 0) return false;
        const uint8_t last = src[size - 1];
        if (last == 0) return false;  // FUN_003b4918: pbVar3[n-1]==0 → 错误
        src_ = src;
        size_ = size;
        // 整改④：终止标记位 = 末字节**最高置位位**。
        // FUN_003b4918: iVar24 = LZCOUNT(last) + -0x17，即 clz32(last)-23，
        // 它已同时包含「高位填充零」与「终止标记位」本身，故不得再减 1。
        const uint32_t skipTop = clz32(last) - 23u;  // ∈ [1,8]
        nbBits_ = size * 8u - skipTop;               // 有效数据位数
        pos_ = nbBits_;                              // 从尾部起向前消费
        return true;
    }

    /// 剩余可读位数
    size_t remaining() const { return pos_; }

    /// 读取 nbBits 位（自当前游标向低地址）。位数不足返回 false。
    bool read(uint32_t nbBits, uint32_t& out) {
        if (nbBits == 0) {
            out = 0;
            return true;
        }
        if (nbBits > 24 || nbBits > pos_) return false;
        out = peek(nbBits);
        pos_ -= nbBits;
        return true;
    }

    /// 预览 nbBits 位而不前进（不足时低位补 0，用于查表解码）。
    ///
    /// 返回值的高位对应流中更靠近 pos_ 的位（即更先被消费的位），
    /// 与 FUN_003b4918 `uVar15 >> (-uVar31 & 0x3f) & kMask[nbBits]` 等价。
    uint32_t peek(uint32_t nbBits) const {
        uint32_t v = 0;
        for (uint32_t i = 0; i < nbBits; ++i) {
            uint32_t b = 0;
            if (i < pos_) {
                const size_t bit = pos_ - 1u - i;  // 自游标向低地址
                b = (src_[bit >> 3] >> (bit & 7u)) & 1u;
            }
            v = (v << 1) | b;
        }
        return v;
    }

    void skip(uint32_t nbBits) { pos_ = (nbBits > pos_) ? 0u : (pos_ - nbBits); }

private:
    const uint8_t* src_ = nullptr;
    size_t size_ = 0;
    size_t nbBits_ = 0;
    size_t pos_ = 0;
};

// ---------------------------------------------------------------------
// 频次统计
// ---------------------------------------------------------------------
uint32_t countFreq(const uint8_t* src, size_t n, uint32_t* freq /*[256]*/) {
    std::memset(freq, 0, sizeof(uint32_t) * 256);
    for (size_t i = 0; i < n; ++i) ++freq[src[i]];
    uint32_t maxSym = 0;
    for (uint32_t s = 0; s < 256; ++s)
        if (freq[s]) maxSym = s;
    return maxSym;
}

// ---------------------------------------------------------------------
// 通用 HUF 位流编解码核心（供数据流与「FSE 权重流」复用）
//
// 【Issue #72 整改②】原库 FUN_003b45c8 在 hdr < 128 分支中直接调用
// FUN_003b4918（即 HUF 解码器本身）解压权重序列：
//
//   uVar4 = FUN_003b4918(param_1, param_2-1, param_6+1, uVar9, 6, ...);
//                         权重输出   上限     压缩数据  hdr  tableLog<=6
//
// 即「FSE 压缩权重模式」= 用一层 tableLog ≤ 6 的 HUF 对权重字节序列
// 自身做熵编码，hdr 为该压缩块的字节数，总消耗 = hdr + 1。
// 该模式不受 raw 模式 hdr = 127+nbSymbols ≤ 255 的限制，
// 因而可覆盖**全 256 字母表**。
// ---------------------------------------------------------------------

/// 规范 Huffman 码值分配（前向声明，定义见下）。
void assignCodes(const uint8_t* nbBits, uint32_t maxSymbolValue, uint32_t tableLog,
                 uint16_t* codes /*[256]*/);

/// 由码长表构建解码查找表（每个 tableLog 位前缀 → (symbol, nbBits)）。
/// @return false 表示码表损坏（前缀冲突 / 未填满 / 越界）
bool buildDecodeTable(const uint8_t* nbBits, uint32_t maxSymbolValue, uint32_t tableLog,
                      std::vector<uint8_t>& dtSymbol, std::vector<uint8_t>& dtNbBits) {
    if (tableLog == 0 || tableLog > kTansMaxTableLog) return false;
    uint16_t codes[256];
    assignCodes(nbBits, maxSymbolValue, tableLog, codes);

    const uint32_t tableSize = 1u << tableLog;
    dtSymbol.assign(tableSize, 0);
    dtNbBits.assign(tableSize, 0);
    for (uint32_t s = 0; s <= maxSymbolValue; ++s) {
        const uint32_t nb = nbBits[s];
        if (nb == 0) continue;
        if (nb > tableLog) return false;
        const uint32_t start = static_cast<uint32_t>(codes[s]) << (tableLog - nb);
        const uint32_t span = 1u << (tableLog - nb);
        if (start + span > tableSize) return false;  // 码表损坏
        for (uint32_t k = 0; k < span; ++k) {
            if (dtNbBits[start + k] != 0) return false;  // 前缀冲突 → 损坏
            dtSymbol[start + k] = static_cast<uint8_t>(s);
            dtNbBits[start + k] = static_cast<uint8_t>(nb);
        }
    }
    for (uint32_t k = 0; k < tableSize; ++k)
        if (dtNbBits[k] == 0) return false;  // 表未填满 → Kraft 不成立
    return true;
}

/// 用给定码表把 src[0..n) 编码为反向位流（zstd BIT_CStream 语义）。
/// 按符号**逆序**写入（见 RevBitWriter 类头）。
/// @return false 表示存在无码长符号（调用方保证不会发生）
bool encodeStream(const uint8_t* src, size_t n, const uint8_t* nbBits,
                  const uint16_t* codes, std::vector<uint8_t>& stream) {
    RevBitWriter bw;
    for (size_t i = n; i-- > 0;) {
        const uint8_t s = src[i];
        if (nbBits[s] == 0) return false;
        bw.addBits(codes[s], nbBits[s]);
    }
    stream = bw.finish();
    return !stream.empty();
}

/// 从反向位流解出 expected 个符号（zstd BIT_DStream 语义）。
/// @return false 表示位流损坏 / 长度不匹配
bool decodeStream(const uint8_t* src, size_t size, size_t expected,
                  const std::vector<uint8_t>& dtSymbol,
                  const std::vector<uint8_t>& dtNbBits, uint32_t tableLog,
                  uint8_t* out) {
    RevBitReader br;
    if (!br.init(src, size)) return false;
    for (size_t i = 0; i < expected; ++i) {
        if (br.remaining() == 0) return false;  // 位流提前耗尽
        const uint32_t idx = br.peek(tableLog);
        const uint32_t nb = dtNbBits[idx];
        if (nb == 0 || nb > br.remaining()) return false;
        br.skip(nb);
        out[i] = dtSymbol[idx];
    }
    return br.remaining() == 0;  // 应恰好耗尽
}

/// 从反向位流「解到流耗尽为止」，最多 capacity 个符号。
///
/// 【Issue #86】对齐 FUN_003b4918 的真实语义：容器层 bit4 段**不携带
/// 符号个数**，反编译中解码器只收到一个「剩余输出容量」(param_3)，
/// 解完后返回**实际解出的字节数**（调用方据此递减 param_3）。
/// 因此读取终止条件是「位流耗尽」而非「计数达标」。
///
/// @return 实际解出符号数；0 表示位流损坏 / 超出容量
size_t decodeStreamToEnd(const uint8_t* src, size_t size, size_t capacity,
                         const std::vector<uint8_t>& dtSymbol,
                         const std::vector<uint8_t>& dtNbBits, uint32_t tableLog,
                         uint8_t* out) {
    RevBitReader br;
    if (!br.init(src, size)) return 0;
    size_t i = 0;
    while (br.remaining() != 0) {
        if (i >= capacity) return 0;            // 超出剩余容量 → 数据不可信
        const uint32_t idx = br.peek(tableLog);
        const uint32_t nb = dtNbBits[idx];
        if (nb == 0 || nb > br.remaining()) return 0;
        br.skip(nb);
        out[i++] = dtSymbol[idx];
    }
    return i;
}
// ---------------------------------------------------------------------
// 权重表写出（对齐 FUN_003b55e8 / FUN_003b45c8 raw 模式）
//
// 格式（hdr >= 128 分支）：
//   [hdr = 127 + nbSymbols][权重 nibble 打包，高 nibble 在前]
// 其中 nbSymbols = maxSymbolValue（末符号权重可由 Kraft 余量推出，
// 故不写入），消耗字节数 = (nbSymbols + 1) / 2 + 1。
//
// 权重定义：w[s] = tableLog + 1 - nbBits[s]（nbBits[s]==0 → w=0）。
// ---------------------------------------------------------------------
bool writeWeightsRaw(const uint8_t* nbBits, uint32_t maxSymbolValue,
                     uint32_t tableLog, std::vector<uint8_t>& out) {
    // 末符号必须有码长（其权重由余量推出，不写入）
    if (nbBits[maxSymbolValue] == 0) return false;
    const uint32_t nbSymbols = maxSymbolValue;  // 写出 [0, maxSymbolValue) 的权重
    // 【Issue #72 整改②】raw 模式的 hdr = 127 + nbSymbols 必须 ≤ 255，
    // 故此处 nbSymbols ≤ 128 是**格式固有约束**（非人为硬限制）；
    // 超出部分改由 FSE 权重模式（hdr < 128）承担，见 writeWeightsFse。
    if (nbSymbols == 0 || nbSymbols > 128) return false;

    out.push_back(static_cast<uint8_t>(127u + nbSymbols));
    const size_t weightBytes = (nbSymbols + 1) / 2;
    const size_t base = out.size();
    out.resize(base + weightBytes, 0);
    for (uint32_t i = 0; i < nbSymbols; ++i) {
        const uint32_t w = nbBits[i] ? (tableLog + 1u - nbBits[i]) : 0u;
        if (w > kTansMaxWeight) return false;  // FUN_003b45c8: 0xb < w → 错误
        if ((i & 1) == 0)
            out[base + i / 2] |= static_cast<uint8_t>(w << 4);  // 高 nibble 在前
        else
            out[base + i / 2] |= static_cast<uint8_t>(w & 0xf);
    }
    return true;
}

// ---------------------------------------------------------------------
// raw 模式权重表读入（仅 hdr >= 128 分支）
//
// 供 FSE 权重模式的**内层**表头解析使用（内层必为 raw 模式，
// 不允许再嵌套，避免无限递归）。
//
// @return 消耗字节数；失败返回 0
// ---------------------------------------------------------------------
bool weightsToNbBits(const uint32_t* weights, uint32_t nbSymbols,
                     uint8_t* nbBits, uint32_t& maxSymbolValue, uint32_t& tableLog);

size_t readWeightsRawOnly(const uint8_t* src, size_t srcSize, uint8_t* nbBits /*[256]*/,
                          uint32_t& maxSymbolValue, uint32_t& tableLog) {
    if (srcSize == 0) return 0;
    const uint8_t hdr = src[0];
    if (hdr < 128) return 0;  // 内层不允许再为压缩模式
    const uint32_t nbSymbols = static_cast<uint32_t>(hdr) - 127u;
    if (nbSymbols == 0 || nbSymbols > kTansMaxSymbolValue) return 0;
    const size_t weightBytes = (nbSymbols + 1) / 2;
    if (srcSize < 1 + weightBytes) return 0;

    uint32_t weights[256];
    std::memset(weights, 0, sizeof(weights));
    for (uint32_t i = 0; i < nbSymbols; ++i) {
        const uint8_t byteVal = src[1 + i / 2];
        weights[i] = (i & 1) ? (byteVal & 0xfu) : (byteVal >> 4);
    }
    if (!weightsToNbBits(weights, nbSymbols, nbBits, maxSymbolValue, tableLog)) return 0;
    return 1 + weightBytes;
}

// ---------------------------------------------------------------------
// FSE（HUF 压缩）权重模式写出 —— 【Issue #72 整改②】
//
// 对齐 FUN_003b45c8 的 hdr < 128 分支：
//   [hdr = 压缩字节数(<128)][对权重序列做 tableLog<=6 的 HUF 压缩块]
// 权重序列本身即 w[0..nbSymbols)（每个 ∈ [0,11]），末符号权重仍由
// Kraft 余量推出、不写入。
//
// 该模式不受 raw 模式「hdr = 127 + nbSymbols ≤ 255」的约束，
// 因此可支持**全 256 字母表**（nbSymbols 最大 255）。
//
// @return true 成功写出；false 表示不适用（调用方回退 raw 模式）
// ---------------------------------------------------------------------
bool writeWeightsFse(const uint8_t* nbBits, uint32_t maxSymbolValue,
                     uint32_t tableLog, std::vector<uint8_t>& out) {
    if (nbBits[maxSymbolValue] == 0) return false;  // 末符号须有码长
    const uint32_t nbSymbols = maxSymbolValue;
    if (nbSymbols == 0 || nbSymbols > kTansMaxSymbolValue) return false;

    // 1) 生成权重序列 w[0..nbSymbols)
    std::vector<uint8_t> weights(nbSymbols);
    for (uint32_t i = 0; i < nbSymbols; ++i) {
        const uint32_t w = nbBits[i] ? (tableLog + 1u - nbBits[i]) : 0u;
        if (w > kTansMaxWeight) return false;  // FUN_003b45c8: 0xb < w → 错误
        weights[i] = static_cast<uint8_t>(w);
    }

    // 2) 对权重序列自身建 HUF 表（tableLog 上限 6，对齐 FUN_003b4918 param_5=6）
    uint32_t wfreq[256];
    const uint32_t wMaxSym = countFreq(weights.data(), weights.size(), wfreq);
    uint8_t wNbBits[256];
    const uint32_t wTableLog = tansBuildTable(wfreq, wMaxSym, 6u, wNbBits);
    if (wTableLog == 0) return false;  // 权重序列只有 0/1 种取值 → 用 raw 模式

    // 3) 权重表头本身用 raw 模式写出（其字母表 ≤ 12 个取值，必然 ≤ 128）
    std::vector<uint8_t> inner;
    if (!writeWeightsRaw(wNbBits, wMaxSym, wTableLog, inner)) return false;

    // 4) 权重序列的 HUF 位流
    uint16_t wCodes[256];
    assignCodes(wNbBits, wMaxSym, wTableLog, wCodes);
    std::vector<uint8_t> wStream;
    if (!encodeStream(weights.data(), weights.size(), wNbBits, wCodes, wStream)) return false;

    const size_t compressedSize = inner.size() + wStream.size();
    if (compressedSize == 0 || compressedSize >= 128) return false;  // hdr 须 < 128

    out.push_back(static_cast<uint8_t>(compressedSize));
    out.insert(out.end(), inner.begin(), inner.end());
    out.insert(out.end(), wStream.begin(), wStream.end());
    return true;
}

// ---------------------------------------------------------------------
// 由权重序列推导码长表（FUN_003b45c8 尾部公共逻辑）
//
// 1) total = sum((1 << w) >> 1)；校验 0 < total < 0x1000；
// 2) tableLog = 32 - clz(total)；rest = (1<<tableLog) - total；
//    要求 rest 为 2 的幂 → 末符号权重 = log2(rest) + 1；
// 3) nbBits[s] = w ? (tableLog + 1 - w) : 0。
//
// @return false 表示权重序列非法
// ---------------------------------------------------------------------
bool weightsToNbBits(const uint32_t* weights, uint32_t nbSymbols,
                     uint8_t* nbBits /*[256]*/, uint32_t& maxSymbolValue,
                     uint32_t& tableLog) {
    uint32_t total = 0;
    for (uint32_t i = 0; i < nbSymbols; ++i) {
        if (weights[i] > kTansMaxWeight) return false;  // FUN_003b45c8: 0xb < w → 错误
        total += (1u << weights[i]) >> 1;
    }
    if (total == 0 || total >= 0x1000u) return false;  // FUN_003b45c8 校验

    tableLog = 32u - clz32(total);  // = ceil(log2(total+1))
    const uint32_t rest = (1u << tableLog) - total;
    if (rest == 0 || (rest & (rest - 1)) != 0) return false;  // 须为 2 的幂
    const uint32_t lastW = log2Floor(rest) + 1u;
    if (lastW > kTansMaxWeight) return false;
    if (tableLog == 0 || tableLog > kTansMaxTableLog) return false;  // FUN_003b4918

    maxSymbolValue = nbSymbols;
    if (maxSymbolValue > kTansMaxSymbolValue) return false;

    std::memset(nbBits, 0, 256);
    for (uint32_t s = 0; s < nbSymbols; ++s)
        nbBits[s] = weights[s] ? static_cast<uint8_t>(tableLog + 1u - weights[s]) : 0;
    nbBits[nbSymbols] = static_cast<uint8_t>(tableLog + 1u - lastW);
    return true;
}

// ---------------------------------------------------------------------
// 权重表读入（对齐 FUN_003b45c8 + FUN_003b42b8）
//
// 1) hdr >= 128：raw 模式，nbSymbols = hdr - 127，nibble 解包
//    （高 nibble 在前），消耗 = (nbSymbols+1)/2 + 1；
// 2) hdr <  128：FSE 权重模式（整改②），hdr 为压缩块字节数，
//    压缩块本身是一层 tableLog<=6 的 HUF，消耗 = hdr + 1；
// 3) 两模式解出权重序列后，走公共的 weightsToNbBits 推导码长。
//
// @return 消耗的表头字节数；失败返回 0
// ---------------------------------------------------------------------
size_t readWeights(const uint8_t* src, size_t srcSize, uint8_t* nbBits /*[256]*/,
                   uint32_t& maxSymbolValue, uint32_t& tableLog) {
    if (srcSize == 0) return 0;
    const uint8_t hdr = src[0];

    uint32_t weights[256];
    std::memset(weights, 0, sizeof(weights));
    uint32_t nbSymbols = 0;
    size_t consumed = 0;

    if (hdr >= 128) {
        // ---- raw 模式 ----
        nbSymbols = static_cast<uint32_t>(hdr) - 127u;
        if (nbSymbols == 0 || nbSymbols > kTansMaxSymbolValue) return 0;
        const size_t weightBytes = (nbSymbols + 1) / 2;
        if (srcSize < 1 + weightBytes) return 0;
        for (uint32_t i = 0; i < nbSymbols; ++i) {
            const uint8_t byteVal = src[1 + i / 2];
            weights[i] = (i & 1) ? (byteVal & 0xfu) : (byteVal >> 4);
        }
        consumed = 1 + weightBytes;
    } else {
        // ---- FSE（HUF 压缩）权重模式：整改② ----
        const size_t blockSize = hdr;
        if (blockSize == 0 || srcSize < 1 + blockSize) return 0;
        const uint8_t* blk = src + 1;

        // 内层权重表（raw 模式），描述「权重字节」自身的码长
        uint8_t wNbBits[256];
        uint32_t wMaxSym = 0;
        uint32_t wTableLog = 0;
        const size_t innerHdr = readWeightsRawOnly(blk, blockSize, wNbBits, wMaxSym, wTableLog);
        if (innerHdr == 0 || innerHdr >= blockSize) return 0;
        if (wTableLog > 6u) return 0;  // FUN_003b4918 param_5 = 6

        std::vector<uint8_t> dtSymbol, dtNbBits;
        if (!buildDecodeTable(wNbBits, wMaxSym, wTableLog, dtSymbol, dtNbBits)) return 0;

        // 逐符号解出权重序列；数量由位流自然长度决定，上限 255
        const uint8_t* wsrc = blk + innerHdr;
        const size_t wsize = blockSize - innerHdr;
        RevBitReader br;
        if (!br.init(wsrc, wsize)) return 0;
        uint32_t cnt = 0;
        while (br.remaining() != 0) {
            if (cnt >= kTansMaxSymbolValue) return 0;
            const uint32_t idx = br.peek(wTableLog);
            const uint32_t nb = dtNbBits[idx];
            if (nb == 0 || nb > br.remaining()) return 0;
            br.skip(nb);
            weights[cnt++] = dtSymbol[idx];
        }
        if (cnt == 0) return 0;
        nbSymbols = cnt;
        consumed = 1 + blockSize;
    }

    if (!weightsToNbBits(weights, nbSymbols, nbBits, maxSymbolValue, tableLog)) return 0;
    return consumed;
}

// ---------------------------------------------------------------------
// 规范 Huffman 码值分配
//
// 按 (码长升序, 符号升序) 分配前缀码；同一码长内码值递增。
// 与解码端 DTable 构建（FUN_003b4918 中 `param_6 + 0x200` 区域填充）
// 使用同一顺序，保证位级一致。
// ---------------------------------------------------------------------
void assignCodes(const uint8_t* nbBits, uint32_t maxSymbolValue, uint32_t tableLog,
                 uint16_t* codes /*[256]*/) {
    uint32_t nextCode[16];
    std::memset(nextCode, 0, sizeof(nextCode));
    uint32_t countPerLen[16];
    std::memset(countPerLen, 0, sizeof(countPerLen));
    for (uint32_t s = 0; s <= maxSymbolValue; ++s)
        if (nbBits[s]) ++countPerLen[nbBits[s]];

    uint32_t code = 0;
    for (uint32_t len = 1; len <= tableLog; ++len) {
        code = (code + countPerLen[len - 1]) << 1;
        nextCode[len] = code;
    }
    std::memset(codes, 0, sizeof(uint16_t) * 256);
    for (uint32_t s = 0; s <= maxSymbolValue; ++s)
        if (nbBits[s]) codes[s] = static_cast<uint16_t>(nextCode[nbBits[s]]++);
}

}  // namespace

// =====================================================================
// tansBuildTable — 对齐 FUN_003b5be8
//
// 算法：包合并（package-merge 的等价简化）——先用堆式 Huffman 求最优
// 码长，若超过 maxNbBits 则做码长上限规整（zstd HUF_setMaxHeight 语义），
// 保证 Kraft 等式 sum(2^(tableLog - nbBits[s])) == 2^tableLog。
// =====================================================================
uint32_t tansBuildTable(const uint32_t* freq, uint32_t maxSymbolValue,
                        uint32_t maxNbBits, uint8_t* nbBits) {
    if (freq == nullptr || nbBits == nullptr) return 0;
    if (maxSymbolValue > kTansMaxSymbolValue) return 0;
    if (maxNbBits == 0) maxNbBits = 11;                    // FUN_003b69ac: param_6==0 → 0xb
    if (maxNbBits > kTansMaxTableLog) return 0;            // FUN_003b69ac: 0xc < param_6 → -44

    std::memset(nbBits, 0, maxSymbolValue + 1);

    // 收集出现过的符号
    uint32_t symbols[256];
    uint32_t nbSym = 0;
    for (uint32_t s = 0; s <= maxSymbolValue; ++s)
        if (freq[s]) symbols[nbSym++] = s;
    if (nbSym < 2) return 0;  // 0/1 个符号：Huffman 无法表达（由 RLE 段承担）

    // --- Huffman 树：节点数组 (weight, parent) ---
    // 节点 0..nbSym-1 为叶子；nbSym.. 为内部节点。
    const uint32_t maxNodes = 2 * nbSym;
    std::vector<uint64_t> weight(maxNodes, 0);
    std::vector<uint32_t> parent(maxNodes, 0xffffffffu);
    std::vector<uint32_t> alive;
    alive.reserve(nbSym);
    for (uint32_t i = 0; i < nbSym; ++i) {
        weight[i] = freq[symbols[i]];
        alive.push_back(i);
    }
    uint32_t nextNode = nbSym;
    while (alive.size() > 1) {
        // 取两个最小权重（nbSym <= 256，线性扫描足够）
        size_t i1 = 0;
        for (size_t k = 1; k < alive.size(); ++k)
            if (weight[alive[k]] < weight[alive[i1]]) i1 = k;
        const uint32_t n1 = alive[i1];
        alive.erase(alive.begin() + static_cast<long>(i1));
        size_t i2 = 0;
        for (size_t k = 1; k < alive.size(); ++k)
            if (weight[alive[k]] < weight[alive[i2]]) i2 = k;
        const uint32_t n2 = alive[i2];
        alive.erase(alive.begin() + static_cast<long>(i2));

        const uint32_t np = nextNode++;
        weight[np] = weight[n1] + weight[n2];
        parent[n1] = np;
        parent[n2] = np;
        alive.push_back(np);
    }

    // 叶子深度 = 码长
    std::vector<uint32_t> len(nbSym, 0);
    uint32_t maxLen = 0;
    for (uint32_t i = 0; i < nbSym; ++i) {
        uint32_t d = 0;
        uint32_t cur = i;
        while (parent[cur] != 0xffffffffu) {
            cur = parent[cur];
            ++d;
        }
        if (d == 0) d = 1;  // 退化保护
        len[i] = d;
        maxLen = std::max(maxLen, d);
    }

    // --- 码长上限规整（zstd HUF_setMaxHeight 语义）---
    // 若最长码超过 maxNbBits，则截断到 maxNbBits，再通过延长短码
    // 补回 Kraft 亏空，使 sum(2^(maxNbBits - len)) == 2^maxNbBits。
    if (maxLen > maxNbBits) {
        for (uint32_t i = 0; i < nbSym; ++i) len[i] = std::min(len[i], maxNbBits);
        maxLen = maxNbBits;
        // 当前 Kraft 和（以 2^maxNbBits 为单位）
        uint64_t total = 0;
        for (uint32_t i = 0; i < nbSym; ++i) total += 1ull << (maxNbBits - len[i]);
        const uint64_t target = 1ull << maxNbBits;
        // 截断后 total 只会 >= target；逐步延长「最短码中频次最低者」
        while (total > target) {
            // 找一个可延长（len < maxNbBits）且频次最小的符号
            uint32_t best = 0xffffffffu;
            for (uint32_t i = 0; i < nbSym; ++i) {
                if (len[i] >= maxNbBits) continue;
                if (best == 0xffffffffu || len[i] > len[best] ||
                    (len[i] == len[best] && freq[symbols[i]] < freq[symbols[best]]))
                    best = i;
            }
            if (best == 0xffffffffu) return 0;  // 无法规整（理论不可达）
            const uint64_t delta = (1ull << (maxNbBits - len[best])) -
                                   (1ull << (maxNbBits - len[best] - 1));
            if (total - delta < target) break;
            total -= delta;
            ++len[best];
        }
        // 若出现亏空（total < target），缩短最高频符号补齐
        while (total < target) {
            uint32_t best = 0xffffffffu;
            for (uint32_t i = 0; i < nbSym; ++i) {
                if (len[i] <= 1) continue;
                const uint64_t gain = 1ull << (maxNbBits - len[i] + 1);
                if (total - (1ull << (maxNbBits - len[i])) + gain > target) continue;
                if (best == 0xffffffffu || freq[symbols[i]] > freq[symbols[best]]) best = i;
            }
            if (best == 0xffffffffu) return 0;
            total += 1ull << (maxNbBits - len[best] + 1);
            total -= 1ull << (maxNbBits - len[best]);
            --len[best];
        }
        if (total != target) return 0;
    }

    // 实际 tableLog = 最长码长（须满足 Kraft 等式）
    uint32_t tableLog = 0;
    for (uint32_t i = 0; i < nbSym; ++i) tableLog = std::max(tableLog, len[i]);
    if (tableLog == 0 || tableLog > maxNbBits) return 0;

    uint64_t kraft = 0;
    for (uint32_t i = 0; i < nbSym; ++i) kraft += 1ull << (tableLog - len[i]);
    if (kraft != (1ull << tableLog)) return 0;

    for (uint32_t i = 0; i < nbSym; ++i)
        nbBits[symbols[i]] = static_cast<uint8_t>(len[i]);
    return tableLog;
}

// =====================================================================
// tansCompress — 对齐 FUN_003b69ac（+ FUN_003b55e8 / FUN_003b66f0）
// =====================================================================
size_t tansCompress(const uint8_t* src, size_t n, std::vector<uint8_t>& out) {
    if (src == nullptr || n == 0) return 0;                 // FUN_003b69ac: param_4==0 → 0
    if (n > kTansMaxInputSize) return 0;                    // FUN_003b69ac: 0x20000 < param_4 → -72

    uint32_t freq[256];
    const uint32_t maxSymbolValue = countFreq(src, n, freq);

    uint8_t nbBits[256];
    const uint32_t tableLog = tansBuildTable(freq, maxSymbolValue, kTansMaxTableLog - 1, nbBits);
    if (tableLog == 0) return 0;  // 单符号 / 建表失败 → 交由 RLE / 原始段

    // 权重表头：【整改②】优先选更短者，raw 不适用时（nbSymbols > 128，
    // 即最大字节值 > 0x80 的全 256 字母表场景）自动回退到 FSE 权重模式。
    // 旧实现仅有 raw 一条路，故 nbSymbols > 128 一律返回 0（100% 被拒）。
    std::vector<uint8_t> payload;
    {
        std::vector<uint8_t> rawHdr;
        const bool rawOk = writeWeightsRaw(nbBits, maxSymbolValue, tableLog, rawHdr);
        std::vector<uint8_t> fseHdr;
        const bool fseOk = writeWeightsFse(nbBits, maxSymbolValue, tableLog, fseHdr);
        if (rawOk && (!fseOk || rawHdr.size() <= fseHdr.size())) {
            payload.swap(rawHdr);
        } else if (fseOk) {
            payload.swap(fseHdr);
        } else {
            return 0;  // 两种模式均不适用 → 交由 RLE / 原始段
        }
    }
    const size_t headerSize = payload.size();

    // 码值分配 + 位流写入
    uint16_t codes[256];
    assignCodes(nbBits, maxSymbolValue, tableLog, codes);

    // 整改①：zstd 反向流要求按符号**逆序**写入——先写的位落在输出低地址，
    // 而解码端从尾部向低地址消费，故最后一个符号必须最先写入。
    RevBitWriter bw;
    for (size_t i = n; i-- > 0;) {
        const uint8_t s = src[i];
        if (nbBits[s] == 0) return 0;  // 理论不可达（频次统计保证）
        bw.addBits(codes[s], nbBits[s]);
    }
    const std::vector<uint8_t> stream = bw.finish();
    if (stream.empty()) return 0;

    const size_t totalSize = headerSize + stream.size();
    // 不划算则回退（对齐 FUN_003ab048 段选择：熵编码不小于原始则用原始段）
    if (totalSize >= n) return 0;

    out.insert(out.end(), payload.begin(), payload.end());
    out.insert(out.end(), stream.begin(), stream.end());
    return totalSize;
}

// =====================================================================
// tansDecompress — 对齐 FUN_003b4918（+ FUN_003b42b8 / FUN_003b45c8）
// =====================================================================
size_t tansDecompress(const uint8_t* src, size_t srcSize, size_t expected,
                      std::vector<uint8_t>& out) {
    if (src == nullptr || srcSize == 0 || expected == 0) return 0;

    uint8_t nbBits[256];
    uint32_t maxSymbolValue = 0;
    uint32_t tableLog = 0;
    const size_t headerSize = readWeights(src, srcSize, nbBits, maxSymbolValue, tableLog);
    if (headerSize == 0 || headerSize >= srcSize) return 0;

    // 构建解码表（对齐 FUN_003b4918 `param_6 + 0x200` 区域：
    // 每个 tableLog 位前缀映射到 (symbol, nbBits)）
    std::vector<uint8_t> dtSymbol, dtNbBits;
    if (!buildDecodeTable(nbBits, maxSymbolValue, tableLog, dtSymbol, dtNbBits)) return 0;

    const size_t outBase = out.size();
    out.resize(outBase + expected);
    if (!decodeStream(src + headerSize, srcSize - headerSize, expected, dtSymbol, dtNbBits,
                      tableLog, out.data() + outBase)) {
        out.resize(outBase);
        return 0;  // 位流损坏 / 长度不匹配（Corrupted block detected）
    }
    return expected;
}

size_t tansDecompressBounded(const uint8_t* src, size_t srcSize, size_t capacity,
                             std::vector<uint8_t>& out) {
    if (src == nullptr || srcSize == 0 || capacity == 0) return 0;

    uint8_t nbBits[256];
    uint32_t maxSymbolValue = 0;
    uint32_t tableLog = 0;
    const size_t headerSize = readWeights(src, srcSize, nbBits, maxSymbolValue, tableLog);
    if (headerSize == 0 || headerSize >= srcSize) return 0;

    std::vector<uint8_t> dtSymbol, dtNbBits;
    if (!buildDecodeTable(nbBits, maxSymbolValue, tableLog, dtSymbol, dtNbBits)) return 0;

    // 按剩余容量预留，解完后按实际符号数回收（对齐 FUN_003b4918
    // 「写入不超 param_3，返回实际字节数」的语义）。
    const size_t outBase = out.size();
    out.resize(outBase + capacity);
    const size_t n = decodeStreamToEnd(src + headerSize, srcSize - headerSize, capacity,
                                       dtSymbol, dtNbBits, tableLog, out.data() + outBase);
    out.resize(outBase + n);   // n==0 时回退到原长度
    return n;
}

// =====================================================================
// 容器段辅助（对齐 FUN_003ad3c0 段格式）
// =====================================================================

void tansWriteRleSegment(std::vector<uint8_t>& out, uint32_t count, uint8_t value,
                         bool last) {
    if (count == 0) return;
    const uint32_t nm1 = count - 1;
    out.push_back(static_cast<uint8_t>(0x08u | (last ? 0x01u : 0x00u)));
    out.push_back(static_cast<uint8_t>(nm1 & 0xff));
    out.push_back(static_cast<uint8_t>((nm1 >> 8) & 0xff));
    out.push_back(value);
}

bool tansWriteBitpackSegment(std::vector<uint8_t>& out, const uint32_t* vals, size_t n,
                             uint32_t width, bool last) {
    if (vals == nullptr || n == 0) return false;
    if (width == 0 || width > 24) return false;
    const uint32_t nm1 = static_cast<uint32_t>(n - 1);
    if (n - 1 > 0xffff) return false;

    const size_t totalBits = n * width;
    const size_t byteLen = (totalBits + 7) / 8;
    const size_t base = out.size();
    out.push_back(static_cast<uint8_t>(0x04u | (last ? 0x01u : 0x00u)));
    out.push_back(static_cast<uint8_t>(nm1 & 0xff));
    out.push_back(static_cast<uint8_t>((nm1 >> 8) & 0xff));
    out.push_back(static_cast<uint8_t>(width));
    out.resize(out.size() + byteLen, 0);
    uint8_t* data = out.data() + base + 4;

    // 反向位打包：从段尾字节起、每项高位先出（与 FUN_003ad3c0 bit2 对齐）
    size_t bitCursor = totalBits;
    for (size_t i = 0; i < n; ++i) {
        const uint32_t v = vals[i];
        if (width < 32 && v > bitMask(width)) {
            out.resize(base);
            return false;
        }
        // 解码端（FUN_003ad3c0 bit2 / LZ3HUF.cpp decodeRealArray）对第 i 项：
        //   for (b = 0..width-1) { --cursor; v = (v << 1) | bit(cursor); }
        // 即最先取出的位是 v 的**最高位**、最后取出的是最低位。
        // 写入端需逐位镜像该顺序：第 b 步（游标递减）写入 v 的 bit(width-1-b)。
        for (uint32_t b = 0; b < width; ++b) {
            --bitCursor;
            const uint32_t bitVal = (v >> (width - 1u - b)) & 1u;
            if (bitVal) data[bitCursor >> 3] |= static_cast<uint8_t>(1u << (7u - (bitCursor & 7u)));
        }
    }
    return true;
}

bool tansWriteTansSegment(std::vector<uint8_t>& out, const uint8_t* src, size_t n,
                          bool last) {
    if (src == nullptr || n == 0 || n - 1 > 0xffff) return false;

    std::vector<uint8_t> payload;
    const size_t ps = tansCompress(src, n, payload);
    if (ps == 0) return false;  // 不划算 → 回退到其他段
    // 【整改③】段头只有一个 u16，且语义为「载荷字节数 - 1」，故上限是
    // payload 字节数 ≤ 0x10000（非符号数量）。
    if (ps > 0x10000) return false;

    // 【Issue #72 整改③】对齐 FUN_003ad3c0 bit4 分支：
    //     uVar7  = *(ushort *)*param_1;   // 只读**一个** u16
    //     *param_1 += 2;
    //     uVar17 = (ulong)uVar7 + 1;      // 载荷字节数 = u16 + 1
    //     FUN_003b4918(dst, ..., ptr, uVar17, 0xc, ...);
    //     *param_1 += uVar17;             // 跳过载荷
    // 即段格式为 [flag][payloadLen-1 : u16 LE][payload]，
    // **没有**单独的符号个数字段（解码长度由外层 param_3 控制）。
    // 旧实现写了 [nm1:2B][ps:2B] 共 4 字节，多出 2 字节 → 原库无法解析。
    const uint32_t psm1 = static_cast<uint32_t>(ps - 1);
    out.push_back(static_cast<uint8_t>(0x10u | (last ? 0x01u : 0x00u)));
    out.push_back(static_cast<uint8_t>(psm1 & 0xff));
    out.push_back(static_cast<uint8_t>((psm1 >> 8) & 0xff));
    out.insert(out.end(), payload.begin(), payload.end());
    return true;
}

}  // namespace qtsvfs_lz3
