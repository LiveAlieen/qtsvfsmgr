#include "qtsvfs_lz3/LZ3HUF.h"
#include "qtsvfs_lz3/LZ3HUF_tables.h"
#include "qtsvfs_lz3/LZ3HUF_tans.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <vector>

namespace qtsvfs_lz3 {
namespace {

// =====================================================================
// 位流写入/读取（低位在前）
// =====================================================================
class BitWriter {
public:
    explicit BitWriter(std::vector<uint8_t>& buf) : buf_(buf) {}

    void put(uint32_t v, uint32_t n) {
        for (uint32_t i = 0; i < n; ++i) {
            acc_ |= ((v >> i) & 1) << bits_;
            ++bits_;
            if (bits_ == 8) {
                buf_.push_back(static_cast<uint8_t>(acc_));
                acc_ = 0;
                bits_ = 0;
            }
        }
    }

    void finish() {
        if (bits_ > 0) {
            buf_.push_back(static_cast<uint8_t>(acc_));
            acc_ = 0;
            bits_ = 0;
        }
    }

private:
    std::vector<uint8_t>& buf_;
    uint32_t acc_ = 0;
    int bits_ = 0;
};

class BitReader {
public:
    BitReader(const uint8_t* data, size_t size) : data_(data), size_(size) {}

    uint32_t read(size_t n) {
        uint32_t v = 0;
        for (size_t i = 0; i < n; ++i) {
            if (bitPos_ >= size_ * 8) return 0;
            const size_t byte = bitPos_ >> 3;
            const size_t bit = bitPos_ & 7;
            v |= static_cast<uint32_t>((data_[byte] >> bit) & 1) << i;
            ++bitPos_;
        }
        return v;
    }

    size_t bitPos() const { return bitPos_; }

private:
    const uint8_t* data_;
    size_t size_;
    size_t bitPos_ = 0;
};

// =====================================================================
// 距离码表（DEFLATE 风格 30 表：base + 扩展位，最大 0x8000）
// =====================================================================
struct DistEntry { uint32_t base; uint32_t extra; };
const DistEntry kDistTable[30] = {
    {1,0},{2,0},{3,0},{4,0},{5,1},{7,1},{9,2},{13,2},
    {17,3},{25,3},{33,4},{49,4},{65,5},{97,5},{129,6},{193,6},
    {257,7},{385,7},{513,8},{769,8},{1025,9},{1537,9},{2049,10},{3073,10},
    {4097,11},{6145,11},{8193,12},{12289,12},{16385,13},{24577,13},
};

uint32_t distExtraBits(uint32_t code) {
    if (code >= 30) return 0;
    return kDistTable[code].extra;
}

// =====================================================================
// 数组容器编码/解码（对齐反编译 FUN_003ab048 / FUN_003ad3c0 的段格式）
//
// 段序列：
//   [flag:1B]
//     bit0=1 收尾段（无数据）
//     bit1=1 原始段：  [n-1:2B][n 项原始字节]
//     bit2=1 位打包段：[n-1:2B][width:1B][ceil(n*width/8) 字节位数据，低位在前]
//   位打包段数据就地存储（段内连续），不跨段共享位流（自洽简化，
//   架构仍对齐反编译的「原始段 + 位打包段 + 收尾段」容器）。
// =====================================================================
class ArrayEncoder {
public:
    explicit ArrayEncoder(std::vector<uint8_t>& out) : out_(out) {}

    // 原始段（值 <= 255 时直接逐字节）
    // 每写一个数组自动追加收尾段（0x01），保证多个数组在同一个字节流中
    // 可被 decodeArray 逐个独立读取（对齐反编译 FUN_003acb58 连续 4 次
    // FUN_003ad3c0 读 4 个数组）。
    void writeRaw(const std::vector<uint32_t>& vals) {
        if (!vals.empty()) {
            const uint32_t n = static_cast<uint32_t>(vals.size());
            out_.push_back(0x02);  // bit1
            out_.push_back(static_cast<uint8_t>((n - 1) & 0xff));
            out_.push_back(static_cast<uint8_t>(((n - 1) >> 8) & 0xff));
            for (uint32_t v : vals) {
                out_.push_back(static_cast<uint8_t>(v & 0xff));
                if (v > 0xff) out_.push_back(static_cast<uint8_t>((v >> 8) & 0xff));
            }
        }
        out_.push_back(0x01);  // 收尾
    }

    // 位打包段（每项 width 位）
    void writeBits(const std::vector<uint32_t>& vals, uint32_t width) {
        if (!vals.empty()) {
            const uint32_t n = static_cast<uint32_t>(vals.size());
            out_.push_back(0x04);  // bit2
            out_.push_back(static_cast<uint8_t>((n - 1) & 0xff));
            out_.push_back(static_cast<uint8_t>(((n - 1) >> 8) & 0xff));
            out_.push_back(static_cast<uint8_t>(width));
            uint32_t acc = 0;
            int bits = 0;
            for (uint32_t v : vals) {
                for (uint32_t b = 0; b < width; ++b) {
                    acc |= ((v >> b) & 1) << bits;
                    ++bits;
                    if (bits == 8) {
                        out_.push_back(static_cast<uint8_t>(acc));
                        acc = 0;
                        bits = 0;
                    }
                }
            }
            if (bits > 0) out_.push_back(static_cast<uint8_t>(acc));
        }
        out_.push_back(0x01);  // 收尾
    }

private:
    std::vector<uint8_t>& out_;
};

// 解码单个容器段序列；返回项数；失败返回 (size_t)-1。
size_t decodeArray(const uint8_t* src, size_t srcSize, size_t& pos,
                   std::vector<uint32_t>& out) {
    out.clear();
    while (true) {
        if (pos >= srcSize) return static_cast<size_t>(-1);
        const uint8_t flag = src[pos++];
        if (flag & 1) break;  // 收尾
        if (flag & 2) {
            // 原始段
            if (pos + 2 > srcSize) return static_cast<size_t>(-1);
            const uint32_t n = static_cast<uint16_t>(src[pos]) |
                               (static_cast<uint16_t>(src[pos + 1]) << 8);
            pos += 2;
            const uint32_t count = n + 1;
            if (pos + count > srcSize) return static_cast<size_t>(-1);
            for (uint32_t i = 0; i < count; ++i) {
                out.push_back(src[pos++]);
            }
        } else if (flag & 4) {
            // 位打包段
            if (pos + 3 > srcSize) return static_cast<size_t>(-1);
            const uint32_t n = static_cast<uint16_t>(src[pos]) |
                               (static_cast<uint16_t>(src[pos + 1]) << 8);
            const uint32_t width = src[pos + 2];
            pos += 3;
            if (width == 0 || width > 24) return static_cast<size_t>(-1);
            const uint32_t count = n + 1;
            const size_t totalBits = static_cast<size_t>(count) * width;
            const size_t byteLen = (totalBits + 7) / 8;
            if (pos + byteLen > srcSize) return static_cast<size_t>(-1);
            for (uint32_t i = 0; i < count; ++i) {
                uint32_t v = 0;
                const size_t startBit = static_cast<size_t>(i) * width;
                for (uint32_t b = 0; b < width; ++b) {
                    const size_t bp = startBit + b;
                    const size_t byte = bp >> 3;
                    const size_t bit = bp & 7;
                    v |= static_cast<uint32_t>((src[pos + byte] >> bit) & 1) << b;
                }
                out.push_back(v);
            }
            pos += byteLen;
        } else {
            return static_cast<size_t>(-1);
        }
    }
    return out.size();
}

// =====================================================================
// LZ77 贪心解析（哈希链）
// =====================================================================
class HashChainHUF {
public:
    static constexpr size_t kHashBits = 16;
    static constexpr size_t kHashSize = 1u << kHashBits;
    static constexpr size_t kNone = std::numeric_limits<size_t>::max();

    HashChainHUF(const uint8_t* d, size_t n) : data_(d), size_(n) {
        head_.assign(kHashSize, kNone);
        next_.assign(n, kNone);
    }

    // 上界守卫：尾部不足 3 字节时用 0 补齐，避免越界读（ASan heap-buffer-overflow）。
    // 对 pos + 2 < size_ 的位置哈希值与旧实现完全一致，故不影响既有压缩产物。
    uint32_t hash3(size_t pos) const {
        const uint32_t b0 = pos < size_ ? data_[pos] : 0u;
        const uint32_t b1 = pos + 1 < size_ ? data_[pos + 1] : 0u;
        const uint32_t b2 = pos + 2 < size_ ? data_[pos + 2] : 0u;
        return ((b0 * 0x1E35A7BDu) ^ (b1 * 0x1E35A7BDu >> 7) ^ (b2 << 5)) &
               (kHashSize - 1);
    }

    void insert(size_t pos) {
        const uint32_t h = hash3(pos);
        next_[pos] = head_[h];
        head_[h] = pos;
    }

    const uint8_t* data_;
    size_t size_;
    std::vector<size_t> head_;
    std::vector<size_t> next_;
};

struct OpHUF {
    uint32_t litLen;
    uint32_t dist;
    uint32_t matchLen;
};

std::vector<OpHUF> parseGreedyHUF(const uint8_t* src, size_t n, int level) {
    std::vector<OpHUF> ops;
    if (n == 0) return ops;
    HashChainHUF parser(src, n);
    size_t pos = 0;
    size_t litStart = 0;
    const size_t kMaxCand = level >= 1 ? 256u : 64u;
    while (pos < n) {
        const size_t maxLen = std::min(n - pos, static_cast<size_t>(kLZ3HUFMaxMatch));
        uint32_t bestDist = 0;
        size_t bestLen = 0;
        const size_t winMin = pos > kLZ3HUFMaxDist ? pos - kLZ3HUFMaxDist : 0;
        const uint32_t h = parser.hash3(pos);
        size_t cand = parser.head_[h];
        size_t checked = 0;
        while (cand != HashChainHUF::kNone && checked < kMaxCand) {
            if (cand < winMin) break;
            const uint32_t dist = static_cast<uint32_t>(pos - cand);
            if (dist != 0) {
                size_t len = 0;
                while (len < maxLen && src[cand + len] == src[pos + len]) ++len;
                if (len > bestLen) {
                    bestLen = len;
                    bestDist = dist;
                    if (len == maxLen) break;
                }
            }
            cand = parser.next_[cand];
            ++checked;
        }
        if (bestLen >= kLZ3HUFMinMatch) {
            ops.push_back({static_cast<uint32_t>(pos - litStart), bestDist,
                           static_cast<uint32_t>(bestLen)});
            for (size_t i = pos; i < pos + bestLen && i + kLZ3HUFMinMatch <= n; ++i) {
                parser.insert(i);
            }
            pos += bestLen;
            litStart = pos;
        } else {
            if (pos + kLZ3HUFMinMatch <= n) parser.insert(pos);
            ++pos;
        }
    }
    if (litStart < n) {
        ops.push_back({static_cast<uint32_t>(n - litStart), 0, 0});
    }
    return ops;
}

// 距离编码（前向声明，供 DP 解析使用；定义见下）
uint32_t distEncode(uint32_t dist, uint32_t& extra);

// =====================================================================
// LZ77 最优（DP）解析
//
// 以实际编码位代价为模型做全局最优解析（对齐 LZ3 的 DP 思路）：
//   每 op 代价 = 字面量字节(8bit/字节)
//              + 字面量长度码 8bit + (litLen>=255 时 +16bit)
//              + 距离码 8bit + distExtraBits(dCode)bit（无匹配则 0）
//              + 匹配长度码 8bit + (matchLen>17 时 +16bit)（无匹配则 0）
// 状态：e[i]=位置 i 刚结束某 op 的最小代价；r[i]=以 i 结尾的开放字面量
// 运行代价（运行起点记于 runStart）。中间 op 必须带匹配；块尾开放运行
// 以纯字面量 op 免费闭合。
// =====================================================================
std::vector<OpHUF> parseOptimalHUF(const uint8_t* src, size_t n, int level) {
    std::vector<OpHUF> ops;
    if (n == 0) return ops;
    if (n < kLZ3HUFMinMatch) {
        ops.push_back({static_cast<uint32_t>(n), 0, 0});
        return ops;
    }

    constexpr int32_t kLitByte = 8;    // 每字面量字节 8bit
    constexpr int32_t kCode = 8;       // 每码表项 8bit（原始段字节）
    constexpr int32_t kExt16 = 16;     // 255 级联 / 0xf 级联扩展位
    constexpr int32_t kInf = 0x3fffffff;

    const size_t kMaxCand = level >= 2 ? 512u : (level >= 1 ? 256u : 96u);

    auto litLenExtra = [kExt16](uint32_t litLen) {
        return litLen >= 255 ? kExt16 : 0;
    };
    auto matchExtra = [kExt16](uint32_t mlen) {
        return mlen > 17 ? kExt16 : 0;
    };
    auto distCostBits = [](uint32_t dist) {
        uint32_t ex = 0;
        const uint32_t code = distEncode(dist, ex);
        (void)ex;
        // 返回真实扩展位 bit 数（kDistTable[code].extra），而非扩展值 ex
        return distExtraBits(code);
    };

    std::vector<int32_t> e(n + 1, kInf);
    std::vector<int32_t> r(n + 1, kInf);
    std::vector<int32_t> runStart(n + 1, -1);
    struct Back { int32_t start; uint32_t dist; uint32_t len; };
    std::vector<Back> decE(n + 1);

    HashChainHUF parser(src, n);

    e[0] = 0;
    for (size_t i = 0; i < n; ++i) {
        // 0) 续写字面量运行（每字节 +8bit）
        if (i >= 1 && r[i - 1] < kInf) {
            const int32_t rs = runStart[i - 1];
            const int32_t c = r[i - 1] + kLitByte;
            if (c < r[i]) {
                r[i] = c;
                runStart[i] = rs;
            }
        }
        if (e[i] >= kInf && r[i] >= kInf) continue;

        if (i + kLZ3HUFMinMatch <= n) parser.insert(i);
        const int32_t ei = e[i];

        // 1) 起新字面量运行（仅从 e[i]）
        if (e[i] < kInf && ei + kLitByte < r[i + 1]) {
            r[i + 1] = ei + kLitByte;
            runStart[i + 1] = static_cast<int32_t>(i);
        }

        // 2) 匹配转移（中间 op 必须带匹配）
        const size_t rem = n - i;
        if (rem >= kLZ3HUFMinMatch) {
            const size_t maxLen = std::min(rem, static_cast<size_t>(kLZ3HUFMaxMatch));
            const size_t winMin = i > kLZ3HUFMaxDist ? i - kLZ3HUFMaxDist : 0;
            const uint32_t h = parser.hash3(i);
            size_t cand = parser.head_[h];
            size_t checked = 0;
            while (cand != HashChainHUF::kNone && checked < kMaxCand) {
                if (cand < winMin) break;
                const uint32_t dist = static_cast<uint32_t>(i - cand);
                if (dist != 0 && dist <= kLZ3HUFMaxDist) {
                    size_t len = 0;
                    while (len < maxLen && src[cand + len] == src[i + len]) ++len;
                    if (len >= kLZ3HUFMinMatch) {
                        const int32_t matchCost =
                            kCode + distCostBits(dist) + kCode + matchExtra(static_cast<uint32_t>(len));
                        // 每 op 恒含 1 个字面量长度码（即使 litLen==0）
                        const int32_t opLitCode = kCode;
                        // 从 e[i]：op=[字面量长度码][匹配]（litLen=0）
                        if (e[i] < kInf) {
                            const int32_t c = ei + opLitCode + matchCost;
                            if (c < e[i + len]) {
                                e[i + len] = c;
                                decE[i + len] = {static_cast<int32_t>(i), dist, static_cast<uint32_t>(len)};
                            }
                        }
                        // 从 r[i]：op=[字面量运行][字面量长度码][匹配]
                        if (r[i] < kInf) {
                            const int32_t runLen = static_cast<int32_t>(i) - runStart[i];
                            const int32_t c = r[i] + opLitCode + litLenExtra(static_cast<uint32_t>(runLen)) + matchCost;
                            if (c < e[i + len]) {
                                e[i + len] = c;
                                decE[i + len] = {runStart[i], dist, static_cast<uint32_t>(len)};
                            }
                        }
                        if (len >= (level >= 1 ? 128u : 64u)) break;  // 高质量匹配提前终止
                    }
                }
                cand = parser.next_[cand];
                ++checked;
            }
        }
    }

    // 收尾：r[n] 闭合为块尾纯字面量 op（含字面量长度码）
    if (r[n] < kInf) {
        const int32_t runLen = static_cast<int32_t>(n) - runStart[n];
        const int32_t c = r[n] + kCode + litLenExtra(static_cast<uint32_t>(runLen));
        if (c < e[n]) {
            e[n] = c;
            decE[n] = {runStart[n], 0, 0};
        }
    }
    if (e[n] >= kInf) {
        ops.push_back({static_cast<uint32_t>(n), 0, 0});
        return ops;
    }

    // 回推最优 op 序列
    int32_t pos = static_cast<int32_t>(n);
    while (pos > 0) {
        const Back& b = decE[pos];
        if (b.len > 0) {
            const size_t runEnd = static_cast<size_t>(pos) - b.len;
            ops.push_back({static_cast<uint32_t>(runEnd - static_cast<size_t>(b.start)),
                           b.dist, b.len});
            pos = b.start;
        } else {
            const size_t litLen = static_cast<size_t>(pos) - static_cast<size_t>(b.start);
            if (litLen > 0) ops.push_back({static_cast<uint32_t>(litLen), 0, 0});
            pos = b.start;
        }
    }
    std::reverse(ops.begin(), ops.end());
    return ops;
}

// 距离编码
uint32_t distEncode(uint32_t dist, uint32_t& extra) {
    for (int i = 0; i < 30; ++i) {
        if (dist <= kDistTable[i].base + (1u << kDistTable[i].extra) - 1) {
            extra = dist - kDistTable[i].base;
            return static_cast<uint32_t>(i);
        }
    }
    extra = dist - kDistTable[29].base;
    return 29;
}

void copyMatch(uint8_t* dst, size_t& outPos, uint32_t dist, uint32_t len) {
    for (uint32_t i = 0; i < len; ++i) {
        dst[outPos] = dst[outPos - dist];
        ++outPos;
    }
}

// 写帧：头部存每块**解压后**大小（对齐 LZ3 帧语义，解压端以它作为
// 期望输出大小驱动逐块解压）。
// 参数：rawSizes[i] = 第 i 块解压后大小；blocks[i] = 第 i 块压缩数据。
size_t writeFrame(const std::vector<size_t>& rawSizes,
                  const std::vector<std::vector<uint8_t>>& blocks,
                  std::vector<uint8_t>& out) {
    out.clear();
    out.reserve(blocks.size() * 24 + 8);
    out.insert(out.end(), kLZ3HUFFrameMagic, kLZ3HUFFrameMagic + 4);
    const uint32_t numBlocks = static_cast<uint32_t>(blocks.size());
    for (int shift = 0; shift < 32; shift += 8) {
        out.push_back(static_cast<uint8_t>((numBlocks >> shift) & 0xff));
    }
    for (size_t i = 0; i < blocks.size(); ++i) {
        const uint32_t len = static_cast<uint32_t>(rawSizes[i]);
        for (int shift = 0; shift < 32; shift += 8) {
            out.push_back(static_cast<uint8_t>((len >> shift) & 0xff));
        }
    }
    for (const auto& b : blocks) {
        out.insert(out.end(), b.begin(), b.end());
    }
    return out.size();
}

}  // namespace

// =====================================================================
// 解压单块
//
// 块格式：
//   [字面量字节容器][字面量长度码容器][距离码容器][匹配长度码容器][位流长:2B][位流]
//
// 码表（自洽）：
//   字面量长度码：0..254 = 长度；255 = 长度 = 255 + 位流16位
//   距离码：30 表（DEFLATE 风格），码>=4 时读 (code-4)/2 位扩展
//   匹配长度码：0..14 = 长度 = 码+3（3..17）；15 = 17 + 位流16位
// =====================================================================
size_t lz3hufDecompressBlock(const uint8_t* src, size_t srcSize,
                             uint8_t* dst, size_t expectedSize,
                             size_t* consumed) {
    if (src == nullptr || dst == nullptr || srcSize == 0) return 0;

    size_t p = 0;
    std::vector<uint32_t> litArr, litLenArr, distArr, matchArr;
    if (decodeArray(src, srcSize, p, litArr) == static_cast<size_t>(-1)) return 0;
    if (decodeArray(src, srcSize, p, litLenArr) == static_cast<size_t>(-1)) return 0;
    if (decodeArray(src, srcSize, p, distArr) == static_cast<size_t>(-1)) return 0;
    if (decodeArray(src, srcSize, p, matchArr) == static_cast<size_t>(-1)) return 0;

    if (p + 2 > srcSize) return 0;
    const uint16_t bitLen = static_cast<uint16_t>(src[p]) |
                            (static_cast<uint16_t>(src[p + 1]) << 8);
    p += 2;
    if (p + bitLen > srcSize) return 0;
    BitReader br(src + p, bitLen);

    size_t outPos = 0;
    size_t iLit = 0, iLitLen = 0, iDist = 0, iMatch = 0;

    while (outPos < expectedSize) {
        // ---- 字面量 ----
        if (iLitLen >= litLenArr.size()) return 0;
        uint32_t litLen = litLenArr[iLitLen++];
        if (litLen == 255) {
            litLen = 255 + br.read(16);
        }
        for (uint32_t k = 0; k < litLen; ++k) {
            if (iLit >= litArr.size()) return 0;
            if (outPos >= expectedSize) return 0;
            dst[outPos++] = static_cast<uint8_t>(litArr[iLit++]);
        }
        if (outPos == expectedSize) break;  // 块尾

        // ---- 距离 ----
        if (iDist >= distArr.size()) return 0;
        const uint32_t distCode = distArr[iDist++];
        if (distCode >= 30) return 0;
        uint32_t distExtra = 0;
        const uint32_t ebits = distExtraBits(distCode);
        if (ebits > 0) distExtra = br.read(ebits);
        const uint32_t dist = kDistTable[distCode].base + distExtra;
        if (dist == 0 || dist > outPos) return 0;

        // ---- 匹配长度 ----
        if (iMatch >= matchArr.size()) return 0;
        uint32_t mlen;
        const uint32_t matchCode = matchArr[iMatch++];
        if (matchCode < 0xe) {
            mlen = matchCode + kLZ3HUFMinMatch;
        } else if (matchCode == 0xe) {
            mlen = 17;
        } else {
            mlen = 17 + br.read(16);
        }
        if (mlen > expectedSize - outPos) return 0;
        copyMatch(dst, outPos, dist, mlen);
    }

    if (outPos != expectedSize) return 0;
    if (consumed) *consumed = p + bitLen;
    return outPos;
}

// =====================================================================
// 压缩单块
// =====================================================================
size_t lz3hufCompressBlock(const uint8_t* src, size_t srcSize,
                           uint8_t* dst, size_t dstCapacity, int level) {
    if (src == nullptr || dst == nullptr) return 0;
    if (srcSize == 0 || srcSize > kLZ3HUFMaxBlockSize) return 0;

    // 1) 解析：level>=1 用 DP 最优解析（压缩比更优），否则贪心
    std::vector<OpHUF> ops =
        level >= 1 ? parseOptimalHUF(src, srcSize, level) : parseGreedyHUF(src, srcSize, level);

    // 2) 生成 4 路数据流 + 位流
    std::vector<uint32_t> litArr, litLenArr, distArr, matchArr;
    std::vector<uint8_t> bitBuf;
    BitWriter bw(bitBuf);

    size_t litPos = 0;
    for (const OpHUF& op : ops) {
        // 字面量长度码
        if (op.litLen < 255) {
            litLenArr.push_back(op.litLen);
        } else {
            litLenArr.push_back(255);
            bw.put(op.litLen - 255, 16);
        }
        // 字面量字节（字面量运行位于源中 [litPos, litPos+litLen)）
        for (uint32_t k = 0; k < op.litLen; ++k) {
            litArr.push_back(src[litPos + k]);
        }
        litPos += op.litLen + op.matchLen;

        if (op.matchLen > 0) {
            // 距离
            uint32_t dExtra = 0;
            const uint32_t dCode = distEncode(op.dist, dExtra);
            distArr.push_back(dCode);
            const uint32_t ebits = distExtraBits(dCode);
            if (ebits > 0) bw.put(dExtra, ebits);
            // 匹配长度
            if (op.matchLen <= 17) {
                matchArr.push_back(op.matchLen - kLZ3HUFMinMatch);
            } else {
                matchArr.push_back(0xf);
                bw.put(op.matchLen - 17, 16);
            }
        } else {
            distArr.push_back(0);
            matchArr.push_back(0);
        }
    }
    bw.finish();

    // 3) 容器编码（每个数组独立容器，以收尾段结束）
    std::vector<uint8_t> tmp;
    ArrayEncoder enc(tmp);
    enc.writeBits(litArr, 8);
    enc.writeRaw(litLenArr);
    enc.writeRaw(distArr);
    enc.writeRaw(matchArr);

    // 4) 位流
    const uint32_t bitLen = static_cast<uint32_t>(bitBuf.size());
    if (bitLen > 0xffff) return 0;
    tmp.push_back(static_cast<uint8_t>(bitLen & 0xff));
    tmp.push_back(static_cast<uint8_t>((bitLen >> 8) & 0xff));
    tmp.insert(tmp.end(), bitBuf.begin(), bitBuf.end());

    if (tmp.size() > dstCapacity) return 0;
    std::memcpy(dst, tmp.data(), tmp.size());
    return tmp.size();
}

// =====================================================================
// 便捷接口
// =====================================================================

size_t lz3hufMaxCompressedSize(size_t srcSize) {
    if (srcSize > kLZ3HUFMaxBlockSize) return 0;
    // 最坏上界：每字节 1 个 op → 4 容器（字面量 1B + 长度 1B + 距离 1B + 匹配 1B）
    // + 位流（每 op 最多 16+13+16 位）+ 容器头 + 收尾 + 安全余量。
    return srcSize * 4 + srcSize * 6 + 512;
}

std::vector<uint8_t> lz3hufCompress(const uint8_t* src, size_t srcSize, int level) {
    if (src == nullptr && srcSize != 0) return {};
    if (srcSize == 0) {
        return {kLZ3HUFFrameMagic[0], kLZ3HUFFrameMagic[1], kLZ3HUFFrameMagic[2],
                kLZ3HUFFrameMagic[3], 0, 0, 0, 0};
    }
    const size_t numBlocks = (srcSize + kLZ3HUFMaxBlockSize - 1) / kLZ3HUFMaxBlockSize;
    std::vector<size_t> rawSizes;
    std::vector<std::vector<uint8_t>> blocks;
    rawSizes.reserve(numBlocks);
    blocks.reserve(numBlocks);
    for (size_t i = 0; i < numBlocks; ++i) {
        const size_t off = i * kLZ3HUFMaxBlockSize;
        const size_t len = std::min(kLZ3HUFMaxBlockSize, srcSize - off);
        std::vector<uint8_t> block(lz3hufMaxCompressedSize(len));
        const size_t n = lz3hufCompressBlock(src + off, len, block.data(), block.size(), level);
        if (n == 0) return {};
        block.resize(n);
        rawSizes.push_back(len);
        blocks.push_back(std::move(block));
    }
    std::vector<uint8_t> out;
    writeFrame(rawSizes, blocks, out);
    return out;
}

std::vector<uint8_t> lz3hufDecompress(const uint8_t* src, size_t srcSize,
                                      size_t rawExpectedSize) {
    if (src == nullptr || srcSize == 0) return {};
    const bool isFrame =
        srcSize >= 8 && src[0] == kLZ3HUFFrameMagic[0] && src[1] == kLZ3HUFFrameMagic[1] &&
        src[2] == kLZ3HUFFrameMagic[2] && src[3] == kLZ3HUFFrameMagic[3];
    if (isFrame) {
        uint32_t numBlocks = 0;
        for (int shift = 0; shift < 32; shift += 8) {
            numBlocks |= static_cast<uint32_t>(src[4 + shift / 8]) << shift;
        }
        size_t p = 8;
        std::vector<uint32_t> sizes(numBlocks);
        size_t total = 0;
        for (size_t i = 0; i < numBlocks; ++i) {
            if (p + 4 > srcSize) return {};
            uint32_t len = 0;
            for (int shift = 0; shift < 32; shift += 8) {
                len |= static_cast<uint32_t>(src[p + shift / 8]) << shift;
            }
            p += 4;
            sizes[i] = len;
            total += len;
        }
        std::vector<uint8_t> out(total);
        size_t o = 0;
        for (size_t i = 0; i < numBlocks; ++i) {
            size_t consumed = 0;
            const size_t n =
                lz3hufDecompressBlock(src + p, srcSize - p, out.data() + o, sizes[i], &consumed);
            if (n == 0 || n != sizes[i] || consumed == 0) return {};
            p += consumed;
            o += n;
        }
        return out;
    }
    if (rawExpectedSize == 0) return {};
    std::vector<uint8_t> out(rawExpectedSize);
    const size_t n = lz3hufDecompressBlock(src, srcSize, out.data(), rawExpectedSize, nullptr);
    if (n == 0) return {};
    return out;
}

} // namespace qtsvfs_lz3

// =====================================================================
// 真实反编译格式（FUN_003acb58）解压
//
// 本实现按反编译 reference/libQtsVFS.so 中 FUN_003acb58 语义解析真实
// method==14 单块格式。与当前自洽格式字节不互通（见 README / docs）。
//
// 已支持容器段：原始段(bit1)、收尾段(bit0)、反向位打包段(bit2)、
// RLE 段(bit3)、tANS/FSE 段(bit4)。
//
// 【Issue #86】bit4 段已由容器层消费（此前返回 unsupported 哨兵）：
// 载荷交由 LZ3HUF_tans 组件（tansCompress/tansDecompress，#72 已与原库
// FUN_003b4918 位级互通）编解码。
// =====================================================================
namespace qtsvfs_lz3 {
namespace {

// =====================================================================
// 真实容器：读 flag 并解析一段序列，直至收尾段。
// 返回 0 成功 / -1 失败 / 1 遇到 tANS/FSE 段（bit4，待路线4 接入）。
// out：解码出的元素（每项为一个码/字节值）。
//
// 段类型（对齐反编译 FUN_003ad3c0）：
//   bit0(0x01)=收尾段（无数据）
//   bit1(0x02)=原始段：       [n-1:2B][n 项原始字节]
//   bit2(0x04)=反向位打包段： [n-1:2B][width:1B][位数据，从段尾反向读取]
//   bit3(0x08)=RLE 段：       [n-1:2B][value:1B]（n 项均为 value，
//                             对齐 FUN_003ad3c0 uVar7==0 memset 分支）
//   bit4(0x10)=tANS/FSE 段：  [payloadLen-1:2B][tANS 载荷]
//                             载荷经 FUN_003b4918（= tansDecompress）解码。
//                             注意段头是**单个 u16 = 载荷字节数-1**，无符号
//                             个数字段；解码符号数由外层剩余容量决定，对齐
//                             反编译：
//                               uVar7  = *(ushort *)*param_1;   // 单 u16
//                               uVar17 = (ulong)uVar7 + 1;      // 载荷字节数
//                               uVar13 = FUN_003b4918(dst, param_3, ptr, uVar17, 0xc, ..);
//                               param_3 -= uVar13;              // 容量递减
//                               *param_1 += uVar17;
// =====================================================================
// 单路数组产出总量上限（防放大 OOM/DoS）：
// 合法块的四路数组均源于一个 <= kLZ3HUFMaxBlockSize 的原始块，
// 字面量最多 kLZ3HUFMaxBlockSize 项，其余三路更少，因此以块上限
// 为界对合法数据无损；超限即不可信数据，必须在**分配前**拒绝。
constexpr size_t kMaxRealArrayElems = kLZ3HUFMaxBlockSize;

// @param capacity 外层剩余输出容量（对齐反编译 param_3）。bit4 段无符号
//                 个数字段，解码长度必须由它驱动；其余段类型也以它
//                 作为上限兼顾防放大。
int decodeRealArray(const uint8_t* src, size_t srcSize, size_t& pos,
                    std::vector<uint32_t>& out, size_t capacity) {
    out.clear();
    // 单路上限：取「外层剩余容量」与「块上限」的较小者。
    const size_t limit = std::min(capacity, kMaxRealArrayElems);
    while (true) {
        if (pos >= srcSize) return -1;
        const uint8_t flag = src[pos++];
        if (flag & 0x10) {                            // bit4: tANS/FSE 段
            // 段头 = 单个 u16，语义为「载荷字节数 - 1」（#72 已固化契约）。
            if (pos + 2 > srcSize) return -1;
            const uint32_t u16 = static_cast<uint32_t>(src[pos]) |
                                 (static_cast<uint32_t>(src[pos + 1]) << 8);
            pos += 2;
            const size_t payloadLen = static_cast<size_t>(u16) + 1;
            if (payloadLen > srcSize - pos) return -1;
            // 解码符号数由剩余容量驱动（对齐 FUN_003ad3c0 传 param_3）：
            // bit4 段无个数字段，解码至位流耗尽，返回实际符号数。
            // 容量已耗尽却仍出现 bit4 段 → 数据不可信，拒绝。
            if (out.size() >= limit) return -1;
            const size_t remain = limit - out.size();
            std::vector<uint8_t> syms;
            const size_t n = tansDecompressBounded(src + pos, payloadLen, remain, syms);
            if (n == 0) return -1;                    // 载荷损坏 / 超容量
            out.insert(out.end(), syms.begin(), syms.end());
            pos += payloadLen;
        } else if (flag & 8) {                        // bit3: RLE 段（memset 语义）
            if (pos + 3 > srcSize) return -1;
            const uint32_t n = static_cast<uint16_t>(src[pos]) |
                               (static_cast<uint16_t>(src[pos + 1]) << 8);
            const uint8_t value = src[pos + 2];
            pos += 3;
            const uint32_t count = n + 1;
            // 先校验后分配：RLE 无对应输入字节，是唯一可无限放大的段
            // 类型（每 4B 段头产出最多 65536 项），必须先卡上限再写入。
            if (count > kMaxRealArrayElems - out.size()) return -1;
            out.insert(out.end(), count, static_cast<uint32_t>(value));
        } else if (flag & 4) {                        // bit2: 反向位打包段
            if (pos + 3 > srcSize) return -1;
            const uint32_t n = static_cast<uint16_t>(src[pos]) |
                               (static_cast<uint16_t>(src[pos + 1]) << 8);
            const uint32_t width = src[pos + 2];
            pos += 3;
            if (width == 0 || width > 24) return -1;
            const uint32_t count = n + 1;
            if (count > kMaxRealArrayElems - out.size()) return -1;
            const size_t totalBits = static_cast<size_t>(count) * width;
            const size_t byteLen = (totalBits + 7) / 8;
            if (pos + byteLen > srcSize) return -1;
            // 反向读取：从段尾字节起，按位从高到低反向消费（对齐真实
            // FUN_003ad3c0 bit2 分支「从段尾反向读位」语义）。
            size_t bitCursor = totalBits;             // 已消费到的反向位游标
            for (uint32_t i = 0; i < count; ++i) {
                uint32_t v = 0;
                for (uint32_t b = 0; b < width; ++b) {
                    --bitCursor;
                    const size_t byteIdx = bitCursor >> 3;
                    const size_t bitIdx = 7 - (bitCursor & 7);
                    const uint32_t bitVal =
                        (src[pos + byteIdx] >> bitIdx) & 1;
                    v = (v << 1) | bitVal;            // 高位先出
                }
                out.push_back(v);
            }
            pos += byteLen;
        } else if (flag & 2) {                        // bit1: 原始段
            if (pos + 2 > srcSize) return -1;
            const uint32_t n = static_cast<uint16_t>(src[pos]) |
                               (static_cast<uint16_t>(src[pos + 1]) << 8);
            pos += 2;
            const uint32_t count = n + 1;
            if (count > kMaxRealArrayElems - out.size()) return -1;
            if (pos + count > srcSize) return -1;
            for (uint32_t i = 0; i < count; ++i) out.push_back(src[pos++]);
        }
        if (flag & 1) return 0;                       // bit0: 收尾段
    }
}

// =====================================================================
// 真实容器编码端（与 decodeRealArray 对齐；对齐反编译 FUN_003ab048
// 的段选择语义：优先 RLE，其次 tANS，最后原始/反向位打包）。
//
// 【Issue #86】tANS 段（bit4）已接入：当数组为字节值（<= 0xff）且
// 熵编码确实比原始段更短时选用；否则回退到原有段类型，
// 保证永不因接入而变大。
// =====================================================================
class RealArrayEncoder {
public:
    explicit RealArrayEncoder(std::vector<uint8_t>& out) : out_(out) {}

    // 写入一路数组，自动在末尾追加收尾段（bit0）。
    // 段选择：全同值 → RLE 段；否则按最大值位宽选原始/反向位打包段。
    void write(const std::vector<uint32_t>& vals) {
        if (vals.empty()) {
            out_.push_back(0x01);                     // 空数组：仅收尾段
            return;
        }
        // 全同值 → RLE 段（memset 语义），需值 <= 0xff。
        bool allSame = true;
        for (size_t i = 1; i < vals.size(); ++i) {
            if (vals[i] != vals[0]) { allSame = false; break; }
        }
        if (allSame && vals[0] <= 0xff && vals.size() >= 2) {
            writeRLE(vals);
            return;
        }
        // 计算最大值位宽，决定原始段 vs 反向位打包段。
        uint32_t maxVal = 0;
        for (uint32_t v : vals) maxVal = std::max(maxVal, v);
        if (maxVal <= 0xff) {
            // 【Issue #86】优先尝试 tANS 段（bit4）：仅当实际更短时采用。
            if (tryWriteTans(vals)) return;
            writeRaw(vals);
        } else {
            uint32_t width = 1;
            while ((1u << width) <= maxVal && width < 24) ++width;
            writeBitsReverse(vals, width);
        }
    }

private:
    // 尝试以 tANS 段（bit4）编码。仅在严格优于原始段时写入。
    // 原始段开销 = 3B 段头 + n 字节；tANS 段开销 = 3B 段头 + 载荷。
    // 因两者段头等长，直接比较「载荷 < n」即可。
    bool tryWriteTans(const std::vector<uint32_t>& vals) {
        const size_t n = vals.size();
        // tansCompress 输入上限（FUN_003b69ac）与段头 u16 容量限制。
        if (n < kTansMinElems || n > kTansMaxInputSize) return false;

        std::vector<uint8_t> bytes;
        bytes.reserve(n);
        for (uint32_t v : vals) bytes.push_back(static_cast<uint8_t>(v & 0xff));

        // 先写到临时缓冲，确认确实更短再提交（避免膨胀）。
        std::vector<uint8_t> seg;
        if (!tansWriteTansSegment(seg, bytes.data(), bytes.size(), true)) return false;
        // 原始段总长 = 1B flag + 2B n-1 + n；seg 已含自身段头。
        if (seg.size() >= 3 + n) return false;         // 不划算 → 回退
        out_.insert(out_.end(), seg.begin(), seg.end());
        return true;
    }

    // 小数组用 tANS 得不偿失（权重表固定开销），且增加无谓风险。
    static constexpr size_t kTansMinElems = 64;

    void writeRLE(const std::vector<uint32_t>& vals) {
        const uint32_t n = static_cast<uint32_t>(vals.size());
        out_.push_back(0x08 | 0x01);                  // bit3 + 收尾
        out_.push_back(static_cast<uint8_t>((n - 1) & 0xff));
        out_.push_back(static_cast<uint8_t>(((n - 1) >> 8) & 0xff));
        out_.push_back(static_cast<uint8_t>(vals[0] & 0xff));
    }

    void writeRaw(const std::vector<uint32_t>& vals) {
        const uint32_t n = static_cast<uint32_t>(vals.size());
        out_.push_back(0x02 | 0x01);                  // bit1 + 收尾
        out_.push_back(static_cast<uint8_t>((n - 1) & 0xff));
        out_.push_back(static_cast<uint8_t>(((n - 1) >> 8) & 0xff));
        for (uint32_t v : vals) out_.push_back(static_cast<uint8_t>(v & 0xff));
    }

    // 反向位打包：与 decodeRealArray bit2 对齐（高位先出、从段尾反向）。
    void writeBitsReverse(const std::vector<uint32_t>& vals, uint32_t width) {
        const uint32_t n = static_cast<uint32_t>(vals.size());
        out_.push_back(0x04 | 0x01);                  // bit2 + 收尾
        out_.push_back(static_cast<uint8_t>((n - 1) & 0xff));
        out_.push_back(static_cast<uint8_t>(((n - 1) >> 8) & 0xff));
        out_.push_back(static_cast<uint8_t>(width));
        const size_t totalBits = static_cast<size_t>(n) * width;
        const size_t byteLen = (totalBits + 7) / 8;
        std::vector<uint8_t> seg(byteLen, 0);
        // 与解码端逆运算：写入位游标从段尾反向递减，高位先写。
        size_t bitCursor = totalBits;
        for (uint32_t i = 0; i < n; ++i) {
            const uint32_t v = vals[i];
            for (uint32_t b = 0; b < width; ++b) {
                --bitCursor;
                const uint32_t bitVal = (v >> (width - 1 - b)) & 1;
                const size_t byteIdx = bitCursor >> 3;
                const size_t bitIdx = 7 - (bitCursor & 7);
                seg[byteIdx] |= static_cast<uint8_t>(bitVal << bitIdx);
            }
        }
        out_.insert(out_.end(), seg.begin(), seg.end());
    }

    std::vector<uint8_t>& out_;
};

// =====================================================================
// 真实码表编码端（与 lz3hufRealDecompressBlock 逐项对齐 real:: 码表）
// =====================================================================

// 字面量长度编码：
//   litLen < 0x10 → 直通（code = litLen，无扩展位）
//   否则找 code 使 kLitLenBase[code] <= litLen < base + (1<<extra)，
//        写扩展位 = litLen - base（低位在前）。
// 返回 code；extraBits/extraVal 输出扩展位。
uint32_t realLitLenEncode(uint32_t litLen, uint32_t& extraBits, uint32_t& extraVal) {
    extraBits = 0; extraVal = 0;
    if (litLen < 0x10) return litLen;
    for (uint32_t code = 0x10; code < 64; ++code) {
        const uint32_t base = real::kLitLenBase[code];
        const uint32_t eb = real::kLitLenExtra[code];
        if (base <= litLen && litLen < base + (1u << eb)) {
            extraBits = eb;
            extraVal = litLen - base;
            return code;
        }
    }
    return 0xffffffff;  // 超出可编码范围
}

// 距离编码：
//   dist 命中 dCode<=4 的直通基值时用直通码；
//   否则找 code 使 kDistBase[code] <= dist < base + (1<<extra)。
uint32_t realDistEncode(uint32_t dist, uint32_t& extraBits, uint32_t& extraVal) {
    extraBits = 0; extraVal = 0;
    // 直通码：dCode<=4 且扩展位为 0 → 需 dist == kDistBase[dCode]
    for (uint32_t code = 0; code <= 4; ++code) {
        if (real::kDistExtra[code] == 0 && real::kDistBase[code] == dist &&
            real::kDistBase[code] != 0) {
            return code;
        }
    }
    for (uint32_t code = 5; code < 36; ++code) {
        const uint32_t base = real::kDistBase[code];
        const uint32_t eb = real::kDistExtra[code];
        if (base != 0 && base <= dist && dist < base + (1u << eb)) {
            extraBits = eb;
            extraVal = dist - base;
            return code;
        }
    }
    return 0xffffffff;
}

// 匹配长度编码：
//   mlen 在 3..16（mCode 0..0xd）→ 直通 code = mlen - 3；
//   否则找 code 使 kMatchLenBase[code] <= mlen < base + (1<<extra)。
uint32_t realMatchLenEncode(uint32_t mlen, uint32_t& extraBits, uint32_t& extraVal) {
    extraBits = 0; extraVal = 0;
    if (mlen >= kLZ3HUFMinMatch && mlen < kLZ3HUFMinMatch + 0xe) {
        return mlen - kLZ3HUFMinMatch;  // mCode < 0xe
    }
    for (uint32_t code = 0xe; code < 52; ++code) {
        const uint32_t base = real::kMatchLenBase[code];
        const uint32_t eb = real::kMatchLenExtraB[code];
        if (base <= mlen && mlen < base + (1u << eb)) {
            extraBits = eb;
            extraVal = mlen - base;
            return code;
        }
    }
    return 0xffffffff;
}

} // namespace

// =====================================================================
// 压缩单块（真实反编译格式，对齐 FUN_003a6c88 / FUN_003acb58）
//
//   [头部flag:1B]  bit1=有 bVar14 直接表  bit2=有符号表
//   [bVar14:1B]（bit1）
//   [符号表:2B]（bit2）
//   数组1: 字面量字节   （真实容器：RLE/原始/反向位打包段）
//   数组2: 字面量长度码 （real:: 码表）
//   数组3: 距离码       （real:: 码表）
//   数组4: 匹配长度码   （real:: 码表）
//   [位流长:2B LE][扩展位流数据]
//
// 本实现头部 flag=0（无 bVar14 直接表 / 无符号表），走 FUN_003acb58
// 的默认码字构建分支（LAB_003acbe0）。tANS 段（bit4）已由 RealArrayEncoder
// 收尾接入（Issue #157 / 支线5）：字节值数组且熵编码更短时产出 bit4 段。
// =====================================================================
size_t lz3hufRealCompressBlock(const uint8_t* src, size_t srcSize,
                               uint8_t* dst, size_t dstCapacity, int level) {
    if (src == nullptr || dst == nullptr) return 0;
    if (srcSize == 0 || srcSize > kLZ3HUFMaxBlockSize) return 0;

    // 1) 解析：level>=1 用 DP 最优解析（压缩比更优），否则贪心
    std::vector<OpHUF> ops =
        level >= 1 ? parseOptimalHUF(src, srcSize, level) : parseGreedyHUF(src, srcSize, level);

    // 2) 生成 4 路码流 + 扩展位流（real:: 码表）
    std::vector<uint32_t> litArr, litLenArr, distArr, matchArr;
    std::vector<uint8_t> bitBuf;
    BitWriter bw(bitBuf);

    size_t litPos = 0;
    for (const OpHUF& op : ops) {
        // 字面量长度码
        uint32_t eb = 0, ev = 0;
        const uint32_t llCode = realLitLenEncode(op.litLen, eb, ev);
        if (llCode == 0xffffffff) return 0;
        litLenArr.push_back(llCode);
        if (eb > 0) bw.put(ev, eb);
        // 字面量字节
        for (uint32_t k = 0; k < op.litLen; ++k) {
            litArr.push_back(src[litPos + k]);
        }
        litPos += op.litLen + op.matchLen;

        if (op.matchLen > 0) {
            // 距离码
            const uint32_t dCode = realDistEncode(op.dist, eb, ev);
            if (dCode == 0xffffffff) return 0;
            distArr.push_back(dCode);
            if (eb > 0) bw.put(ev, eb);
            // 匹配长度码
            const uint32_t mCode = realMatchLenEncode(op.matchLen, eb, ev);
            if (mCode == 0xffffffff) return 0;
            matchArr.push_back(mCode);
            if (eb > 0) bw.put(ev, eb);
        }
        // 注：块尾纯字面量 op（matchLen==0）不产出距离/匹配码，
        // 与 FUN_003acb58 主循环「outPos 到达 expectedSize 即结束」对齐。
    }
    bw.finish();

    // 3) 组装真实块
    std::vector<uint8_t> tmp;
    tmp.push_back(0x00);  // 头部 flag：bit1=0（无 bVar14），bit2=0（无符号表）

    RealArrayEncoder enc(tmp);
    enc.write(litArr);
    enc.write(litLenArr);
    enc.write(distArr);
    enc.write(matchArr);

    // 4) 位流长 + 位流
    const uint32_t bitLen = static_cast<uint32_t>(bitBuf.size());
    if (bitLen > 0xffff) return 0;
    tmp.push_back(static_cast<uint8_t>(bitLen & 0xff));
    tmp.push_back(static_cast<uint8_t>((bitLen >> 8) & 0xff));
    tmp.insert(tmp.end(), bitBuf.begin(), bitBuf.end());

    if (tmp.size() > dstCapacity) return 0;
    std::memcpy(dst, tmp.data(), tmp.size());
    return tmp.size();
}

size_t lz3hufRealDecompressBlock(const uint8_t* src, size_t srcSize,
                                 uint8_t* dst, size_t expectedSize,
                                 int* unsupportedSegment, size_t* consumed) {
    if (src == nullptr || dst == nullptr || srcSize == 0) return 0;
    if (unsupportedSegment) *unsupportedSegment = 0;

    size_t p = 0;
    const uint8_t flag = src[p++];
    uint8_t bVar14 = 0;
    if (flag & 2) {                                   // bit1: bVar14 直接表
        if (p >= srcSize) return 0;
        bVar14 = src[p++];
    }
    (void)bVar14;  // 头部 bVar14 直接表暂未用于本路径（默认码字构建分支）
    uint16_t sym = 0;
    if (flag & 4) {                                   // bit2: 符号表
        if (p + 2 > srcSize) return 0;
        sym = static_cast<uint16_t>(src[p]) | (static_cast<uint16_t>(src[p + 1]) << 8);
        p += 2;
    }
    (void)sym;     // 符号表读法保留，供 tANS 段接入时使用

    std::vector<uint32_t> litArr, litLenArr, distArr, matchArr;
    // 四路数组的容量上限：对齐反编译 FUN_003acb58 向 FUN_003ad3c0 传入的
    // param_3（剩余输出容量）。字面量最多产出 expectedSize 字节；码流
    // 每项至少对应 1 字节输出，同样以 expectedSize 为界。
    int rc;
    if ((rc = decodeRealArray(src, srcSize, p, litArr, expectedSize)) != 0) {
        if (unsupportedSegment) *unsupportedSegment = rc;
        return 0;
    }
    if ((rc = decodeRealArray(src, srcSize, p, litLenArr, expectedSize)) != 0) {
        if (unsupportedSegment) *unsupportedSegment = rc;
        return 0;
    }
    if ((rc = decodeRealArray(src, srcSize, p, distArr, expectedSize)) != 0) {
        if (unsupportedSegment) *unsupportedSegment = rc;
        return 0;
    }
    if ((rc = decodeRealArray(src, srcSize, p, matchArr, expectedSize)) != 0) {
        if (unsupportedSegment) *unsupportedSegment = rc;
        return 0;
    }

    if (p + 2 > srcSize) return 0;
    const uint16_t bitLen = static_cast<uint16_t>(src[p]) |
                            (static_cast<uint16_t>(src[p + 1]) << 8);
    p += 2;
    if (p + bitLen > srcSize) return 0;
    BitReader br(src + p, bitLen);

    size_t outPos = 0;
    size_t iLit = 0, iLitLen = 0, iDist = 0, iMatch = 0;
    while (outPos < expectedSize) {
        // ---- 字面量长度 ----
        if (iLitLen >= litLenArr.size()) return 0;
        uint32_t code = litLenArr[iLitLen++];
        uint32_t litLen;
        if (code < 0x10) {
            litLen = code;
        } else {
            // 安全设界：kLitLenBase 仅 64 项（kLitLenExtra 为 256 项），
            // 必须按两表的**较小**尺寸设界，否则 code ∈ [64,255] 会越界读
            // kLitLenBase（不可信数据可控 → 静默错误解码）。
            if (code >= 64) return 0;
            const uint32_t ebits = real::kLitLenExtra[code];
            litLen = real::kLitLenBase[code] + br.read(ebits);
        }
        for (uint32_t k = 0; k < litLen; ++k) {
            if (iLit >= litArr.size() || outPos >= expectedSize) return 0;
            dst[outPos++] = static_cast<uint8_t>(litArr[iLit++]);
        }
        if (outPos == expectedSize) break;            // 块尾

        // ---- 距离 ----
        if (iDist >= distArr.size()) return 0;
        const uint32_t dCode = distArr[iDist++];
        uint32_t dist;
        if (dCode <= 4) {
            dist = real::kDistBase[dCode];
        } else {
            if (dCode >= 36) return 0;
            const uint32_t ebits = real::kDistExtra[dCode];
            dist = real::kDistBase[dCode] + br.read(ebits);
        }
        if (dist == 0 || dist > outPos) return 0;

        // ---- 匹配长度 ----
        if (iMatch >= matchArr.size()) return 0;
        const uint32_t mCode = matchArr[iMatch++];
        uint32_t mlen;
        if (mCode < 0xe) {
            mlen = mCode + kLZ3HUFMinMatch;
        } else {
            if (mCode >= 52) return 0;
            const uint32_t ebits = real::kMatchLenExtraB[mCode];
            mlen = real::kMatchLenBase[mCode] + br.read(ebits);
        }
        if (mlen > expectedSize - outPos) return 0;
        copyMatch(dst, outPos, dist, mlen);
    }

    if (outPos != expectedSize) return 0;
    if (consumed) *consumed = p + bitLen;
    return outPos;
}

} // namespace qtsvfs_lz3
